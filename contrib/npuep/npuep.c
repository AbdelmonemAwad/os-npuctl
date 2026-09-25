/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * npuep - bring up the host side of the Marvell CN913x PCIe endpoint on a
 * Sophos XGS appliance, far enough that the NPU starts its own dataplane.
 *
 * WHAT THIS IS FOR
 *
 * On these boards every front port belongs to a Marvell NPU sitting behind a
 * PCIe endpoint (11ab:7080). The NPU boots its own Linux, publishes a small
 * map into its BARs, and then blocks until the host completes a handshake.
 * Its own startup script polls for exactly one condition and does nothing at
 * all until it is met:
 *
 *	handshake >= 0x0b
 *
 * Bit 0 of that word is the NPU's. Bits 1 and 3 are ours. Bit 3 is a
 * heartbeat: the target clears it on every scan, so the host must keep
 * setting it or the far side decides we have gone away and restarts.
 *
 * All of that can be done from userspace through /dev/mem, and was, before
 * this module existed. What userspace cannot do is the step the NPU asks for
 * next: it wants five target-to-host doorbells, and a doorbell is an MSI-X
 * vector. Allocating one is a kernel operation. That is the whole reason this
 * file exists.
 *
 * WHERE THE NUMBERS COME FROM
 *
 * Not from guesswork. Sophos publishes the driver as GPL source in its
 * SFOS_OSS ISO, under pcie_ep_armada/, and every constant below is from
 * barmap.h and facility_conf.h in that tree. The layout was independently
 * recovered by disassembling the shipped module first and matched exactly,
 * which is a reasonable check that these really are the right structures.
 *
 * WHAT THIS DOES NOT DO
 *
 * It does not move packets. It brings up the control facility and the
 * doorbells; the GIU datapath - rings, descriptors, the AGNIC command
 * protocol - is a separate and much larger piece of work. What it gets you is
 * an NPU that has started NetAgent, UMSD and its dataplane, and a host that
 * can interrupt it and be interrupted by it.
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/callout.h>

#include <machine/atomic.h>
#include <machine/bus.h>
#include <machine/resource.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

static int	npuep_detach(device_t);

/* barmap.h */
#define	NPU_BARMAP_COOKIE	0xD0FAC10DU
#define	NPU_BARMAP_VERSION	5U
/*
 * The facilities are at the TOP of BAR2, not the bottom. facility_host.c
 * computes it as resource_size(BAR2) - (SZ_4K + NPU_BARMAP_BAR2_TOTAL_SIZE),
 * and the trailing 4 KB it leaves out is the PCI BOOTCMD page.
 */
#define	NPU_BARMAP_TAIL		0x104000U	/* window start, from the end */
#define	NPU_BARMAP_WINDOW_LEN	0x103000U	/* what the vendor maps      */
#define	NPU_BARMAP_STRUCT_OFF	0x102000U	/* npu_bar_map in that window */

/* facility_conf.h */
#define	ARMADA_FACILITY_COOKIE	0xAFACAFACU
#define	CTRL_FCLT_TRGT_INIT	(1U << 0)	/* target init done   - NPU  */
#define	CTRL_FCLT_HOST_INIT	(1U << 1)	/* host init done     - ours */
#define	CTRL_FCLT_TRGT_H2T_DBELL (1U << 2)	/* target dbell init  - NPU  */
#define	CTRL_FCLT_HOST_ALIVE	(1U << 3)	/* host alive         - ours */

#define	MV_FACILITY_COUNT	5
#define	MV_FACILITY_CONTROL	0
#define	MV_FACILITY_MGMT_NETDEV	1
#define	MV_FACILITY_NW_AGENT	2
#define	MV_FACILITY_RPC		3
#define	MV_FACILITY_GIU		4

/*
 * Doorbell counts per facility, from the `facilities[]` table in
 * facility_conf.h. Note MGMT_NETDEV's t2h count is 1 even though its IRQ_CNT
 * macro is 0 - the vendor source carries the comment "Workaround for MSI NMP
 * zero MSIX ID entry issue". Keep it, or the vector numbering shifts and the
 * NPU asks for a vec_id nobody allocated.
 */
static const int npuep_t2h_dbells[MV_FACILITY_COUNT] = {
	[MV_FACILITY_CONTROL]     = 0,
	[MV_FACILITY_MGMT_NETDEV] = 1,
	[MV_FACILITY_NW_AGENT]    = 0,
	[MV_FACILITY_RPC]         = 0,
	[MV_FACILITY_GIU]         = 4,
};
#define	NPUEP_TOTAL_DBELLS	5

static const char *npuep_facility_name[MV_FACILITY_COUNT] = {
	"ctrl", "mvmgmt", "nwa", "rpc", "giu"
};

#define	NPUEP_HEARTBEAT_HZ	1	/* the target clears HOST_ALIVE each scan */

struct npuep_dbell {
	struct resource	*irq;
	void		*cookie;
	int		 rid;
	int		 facility;
	int		 index;
	uint64_t	 count;
	struct npuep_softc *sc;
};

struct npuep_softc {
	device_t		 dev;

	struct resource		*bar0;		/* GIU + network agent   */
	struct resource		*bar2;		/* control + barmap      */
	int			 bar0_rid;
	int			 bar2_rid;

	bus_size_t		 window;	/* facility window in BAR2 */
	bus_size_t		 ctrl;		/* ctrl_map, absolute in BAR2 */

	int			 nvec;
	int			 busmaster;	/* we turned it on, we turn it off */
	int			 handshaken;	/* HOST_INIT was set by us */
	int			 lost;		/* endpoint stopped decoding */
	struct npuep_dbell	 dbell[NPUEP_TOTAL_DBELLS];

	struct callout		 heartbeat;
	struct mtx		 mtx;
	int			 running;
	uint64_t		 beats;
	uint64_t		 last_handshake;
};

#define	NPUEP_LOCK(sc)		mtx_lock(&(sc)->mtx)
#define	NPUEP_UNLOCK(sc)	mtx_unlock(&(sc)->mtx)

static uint32_t
bar2_read(struct npuep_softc *sc, bus_size_t off)
{
	return (bus_read_4(sc->bar2, off));
}

static void
bar2_write(struct npuep_softc *sc, bus_size_t off, uint32_t val)
{
	bus_write_4(sc->bar2, off, val);
}

/*
 * The doorbell handler. There is nothing to do with these yet - the GIU
 * datapath that would consume them does not exist on this side. Counting them
 * is not laziness: it is the only direct evidence that the NPU can reach us,
 * which is exactly what this module was written to establish.
 */
static void
npuep_intr(void *arg)
{
	struct npuep_dbell *db = arg;

	atomic_add_64(&db->count, 1);
}

/*
 * The callout was created with callout_init_mtx(), so softclock_call_cc() has ALREADY taken
 * sc->mtx before calling this and drops it afterwards. Taking it again here would be a
 * recursive acquire of a non-recursive MTX_DEF mutex: instant panic on any kernel built with
 * INVARIANTS, and on a production kernel it merely happens to balance - which is worse,
 * because it works by accident. An earlier version of this function did exactly that.
 *
 * Calling callout_reset() from inside the body with the lock held is the documented idiom and
 * is correct.
 */
static void
npuep_heartbeat(void *arg)
{
	struct npuep_softc *sc = arg;
	uint32_t hs;

	mtx_assert(&sc->mtx, MA_OWNED);

	if (!sc->running)
		return;

	hs = bar2_read(sc, sc->ctrl + 4);	/* +4 is the handshake; +0 is the cookie */
	if (hs == 0xFFFFFFFFU) {
		/*
		 * The endpoint stopped decoding - almost certainly the NPU was reset underneath
		 * us. Stop writing into an address that is no longer ours and stop the
		 * heartbeat; the far side will time us out, which is the correct outcome.
		 */
		if (!sc->lost) {
			device_printf(sc->dev, "endpoint stopped responding - "
			    "NPU reset? heartbeat stopped\n");
			sc->lost = 1;
		}
		sc->running = 0;
		return;
	}
	bar2_write(sc, sc->ctrl + 4, hs | CTRL_FCLT_HOST_ALIVE);
	sc->beats++;
	sc->last_handshake = hs;
	callout_reset(&sc->heartbeat, hz / NPUEP_HEARTBEAT_HZ, npuep_heartbeat, sc);
}

/*
 * Read npu_bar_map and find where the control facility lives.
 *
 * The compile-time constants would give the same answer today, but the NPU is
 * what publishes this table, so read it rather than assume it. If a later
 * firmware moves a facility, an implementation that assumed would write the
 * handshake into the middle of something else.
 */
static int
npuep_read_barmap(struct npuep_softc *sc)
{
	device_t dev = sc->dev;
	uint32_t version, cookie;
	bus_size_t base;
	int i, found = -1;

	base = sc->window + NPU_BARMAP_STRUCT_OFF;
	version = bar2_read(sc, base);
	cookie = bar2_read(sc, base + 4);

	if (cookie == 0xFFFFFFFFU) {
		device_printf(dev, "BAR2 reads all ones - the endpoint is not "
		    "decoding. If the NPU was just reset, its BARs were cleared "
		    "and need restoring before anything here works.\n");
		return (ENXIO);
	}
	if (cookie != NPU_BARMAP_COOKIE) {
		device_printf(dev, "NPU barmap not configured (cookie 0x%08x) - "
		    "the NPU is not running yet\n", cookie);
		return (EAGAIN);
	}
	if (version != NPU_BARMAP_VERSION) {
		device_printf(dev, "barmap version %u, expected %u\n",
		    version, NPU_BARMAP_VERSION);
		return (EINVAL);
	}

	device_printf(dev, "barmap version %u cookie 0x%08x\n", version, cookie);

	/* struct facility_bar_map { u32 bar; u32 type; u32 offset; u32 size; } */
	for (i = 0; i < MV_FACILITY_COUNT; i++) {
		bus_size_t e = base + 8 + i * 16;
		uint32_t bar = bar2_read(sc, e);
		uint32_t type = bar2_read(sc, e + 4);
		uint32_t off = bar2_read(sc, e + 8);
		uint32_t size = bar2_read(sc, e + 12);

		device_printf(dev, "  facility %-7s bar%u off 0x%06x size %u\n",
		    type < MV_FACILITY_COUNT ? npuep_facility_name[type] : "?",
		    bar == 0 ? 0 : 2, off, size);

		if (type == MV_FACILITY_CONTROL) {
			if (bar != 1) {		/* SHM_BAR2 == 1 */
				device_printf(dev,
				    "control facility is not on BAR2\n");
				return (EINVAL);
			}
			/*
			 * This offset comes out of memory the NPU writes. Trusting it would
			 * hand a coprocessor the ability to steer our bus_read_4 and, worse,
			 * our bus_write_4 - every second, from the heartbeat - to any offset
			 * it liked. Bound it against the window we actually mapped.
			 */
			if (off > NPU_BARMAP_WINDOW_LEN - sizeof(uint32_t) * 3) {
				device_printf(dev, "control facility offset "
				    "0x%x is outside the mapped window\n", off);
				return (EINVAL);
			}
			sc->ctrl = sc->window + off;
			found = i;
		}
	}

	if (found < 0) {
		device_printf(dev, "no control facility in the barmap\n");
		return (EINVAL);
	}
	return (0);
}

static int
npuep_handshake(struct npuep_softc *sc)
{
	device_t dev = sc->dev;
	bus_size_t ctrl = sc->ctrl;
	uint32_t cookie, hs;
	int i;

	cookie = bar2_read(sc, ctrl);
	if (cookie != ARMADA_FACILITY_COOKIE) {
		device_printf(dev, "control facility cookie 0x%08x, expected "
		    "0x%08x - refusing to write\n", cookie,
		    ARMADA_FACILITY_COOKIE);
		return (EINVAL);
	}

	/* The vendor driver spins here forever. Give up instead, and say so. */
	for (i = 0; i < 100; i++) {
		hs = bar2_read(sc, ctrl + 4);
		if (hs & CTRL_FCLT_TRGT_INIT)
			break;
		pause("npuep", hz / 10);
	}
	if (!(hs & CTRL_FCLT_TRGT_INIT)) {
		device_printf(dev, "target never set TRGT_INIT (handshake "
		    "0x%08x)\n", hs);
		return (ETIMEDOUT);
	}

	atomic_thread_fence_rel();
	bar2_write(sc, ctrl + 4, hs | CTRL_FCLT_HOST_INIT);
	sc->handshaken = 1;
	hs = bar2_read(sc, ctrl + 4);
	device_printf(dev, "handshake 0x%08x after HOST_INIT\n", hs);

	bar2_write(sc, ctrl + 4, hs | CTRL_FCLT_HOST_ALIVE);
	hs = bar2_read(sc, ctrl + 4);
	device_printf(dev, "handshake 0x%08x after HOST_ALIVE%s\n", hs,
	    (hs & 0x0b) == 0x0b ? "  - target may proceed" : "");

	return (0);
}

static int
npuep_setup_dbells(struct npuep_softc *sc)
{
	device_t dev = sc->dev;
	int msgs = NPUEP_TOTAL_DBELLS;
	int fcl, i, n = 0, err;

	if (pci_msix_count(dev) < NPUEP_TOTAL_DBELLS) {
		device_printf(dev, "only %d MSI-X messages, need %d\n",
		    pci_msix_count(dev), NPUEP_TOTAL_DBELLS);
		return (ENXIO);
	}

	err = pci_alloc_msix(dev, &msgs);
	if (err != 0) {
		device_printf(dev, "pci_alloc_msix failed: %d\n", err);
		return (err);
	}
	if (msgs != NPUEP_TOTAL_DBELLS) {
		device_printf(dev, "asked for %d MSI-X vectors, got %d\n",
		    NPUEP_TOTAL_DBELLS, msgs);
		pci_release_msi(dev);
		return (ENXIO);
	}
	sc->nvec = msgs;

	/*
	 * Vector order matters. The NPU asks for them as a flat vec_id across
	 * facilities in facility order - mvmgmt first, then the four GIU ones -
	 * which is what walking the table in order produces.
	 */
	for (fcl = 0; fcl < MV_FACILITY_COUNT; fcl++) {
		for (i = 0; i < npuep_t2h_dbells[fcl]; i++) {
			struct npuep_dbell *db = &sc->dbell[n];

			db->sc = sc;
			db->facility = fcl;
			db->index = i;
			db->rid = n + 1;	/* MSI-X rids start at 1 */
			db->irq = bus_alloc_resource_any(dev, SYS_RES_IRQ,
			    &db->rid, RF_ACTIVE);
			if (db->irq == NULL) {
				device_printf(dev, "no IRQ resource for "
				    "%s-db%d (rid %d)\n",
				    npuep_facility_name[fcl], i, db->rid);
				return (ENXIO);
			}
			err = bus_setup_intr(dev, db->irq,
			    INTR_TYPE_MISC | INTR_MPSAFE, NULL, npuep_intr,
			    db, &db->cookie);
			if (err != 0) {
				device_printf(dev, "bus_setup_intr for "
				    "%s-db%d: %d\n", npuep_facility_name[fcl],
				    i, err);
				return (err);
			}
			n++;
		}
	}

	device_printf(dev, "%d MSI-X doorbells armed (mvmgmt x1, giu x4)\n", n);
	return (0);
}

static void
npuep_add_sysctls(struct npuep_softc *sc)
{
	struct sysctl_ctx_list *ctx = device_get_sysctl_ctx(sc->dev);
	struct sysctl_oid *tree = device_get_sysctl_tree(sc->dev);
	struct sysctl_oid_list *child = SYSCTL_CHILDREN(tree);
	char name[32];
	int i;

	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "heartbeats", CTLFLAG_RD,
	    &sc->beats, 0, "HOST_ALIVE writes since attach");
	/*
	 * The heartbeat count only says we kept writing, which a dead endpoint would not
	 * disturb. This is the value actually read back each time, so it is the one that can
	 * distinguish a live target - watch bit 2 appear and bit 3 keep being cleared.
	 */
	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "handshake", CTLFLAG_RD,
	    &sc->last_handshake, 0, "handshake word as last read by the heartbeat");

	for (i = 0; i < NPUEP_TOTAL_DBELLS; i++) {
		struct npuep_dbell *db = &sc->dbell[i];

		snprintf(name, sizeof(name), "%s_db%d",
		    npuep_facility_name[db->facility], db->index);
		SYSCTL_ADD_U64(ctx, child, OID_AUTO, name, CTLFLAG_RD,
		    &db->count, 0, "doorbell interrupts taken");
	}
}

static int
npuep_probe(device_t dev)
{
	if (pci_get_vendor(dev) != 0x11ab || pci_get_device(dev) != 0x7080)
		return (ENXIO);

	device_set_desc(dev, "Marvell CN913x PCIe endpoint (Sophos XGS NPU)");
	return (BUS_PROBE_DEFAULT);
}

static int
npuep_attach(device_t dev)
{
	struct npuep_softc *sc = device_get_softc(dev);
	int err;

	sc->dev = dev;
	mtx_init(&sc->mtx, device_get_nameunit(dev), NULL, MTX_DEF);
	callout_init_mtx(&sc->heartbeat, &sc->mtx, 0);

	/*
	 * Bus mastering is needed before the NPU can deliver an MSI-X write, so it cannot be
	 * left until last - but it is tracked so that detach and every failure path turn it
	 * off again. Leaving an endpoint bus-mastering with no driver behind it is how this
	 * project earned a "general protection fault" panic: a coprocessor that has been told
	 * a host driver is ready, with nothing left to service it, writing into host memory
	 * nobody vetted. See docs/porting-notes.md.
	 */
	pci_enable_busmaster(dev);
	sc->busmaster = 1;

	/*
	 * BAR0 holds the GIU window and, at +0x1000, the MSI-X table. It has
	 * to be mapped before pci_alloc_msix() can write that table.
	 */
	sc->bar0_rid = PCIR_BAR(0);
	sc->bar0 = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &sc->bar0_rid,
	    RF_ACTIVE);
	if (sc->bar0 == NULL) {
		device_printf(dev, "cannot map BAR0\n");
		err = ENXIO;
		goto fail;
	}

	sc->bar2_rid = PCIR_BAR(2);
	sc->bar2 = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &sc->bar2_rid,
	    RF_ACTIVE);
	if (sc->bar2 == NULL) {
		device_printf(dev, "cannot map BAR2\n");
		err = ENXIO;
		goto fail;
	}

	if (rman_get_size(sc->bar2) < NPU_BARMAP_TAIL) {
		/* Without this the subtraction wraps and every offset is nonsense. */
		device_printf(dev, "BAR2 is only 0x%jx bytes, need at least 0x%x\n",
		    (uintmax_t)rman_get_size(sc->bar2), NPU_BARMAP_TAIL);
		err = ENXIO;
		goto fail;
	}
	sc->window = rman_get_size(sc->bar2) - NPU_BARMAP_TAIL;
	device_printf(dev, "BAR2 size 0x%jx, facility window at 0x%jx\n",
	    (uintmax_t)rman_get_size(sc->bar2), (uintmax_t)sc->window);

	/*
	 * Check BAR0 answers before pci_alloc_msix writes the MSI-X table into it. If the NPU
	 * is in reset the endpoint does not respond to config or memory cycles, everything
	 * reads back as ones, and the vectors would be written into a void - which looks like
	 * success here and like "failed to get msi_msg" on the far side.
	 */
	if (bus_read_4(sc->bar0, 0) == 0xFFFFFFFFU &&
	    bus_read_4(sc->bar0, 4) == 0xFFFFFFFFU) {
		device_printf(dev, "BAR0 reads all ones - the endpoint is not decoding. "
		    "The NPU is probably held in reset; release it first.\n");
		err = ENXIO;
		goto fail;
	}

	err = npuep_read_barmap(sc);
	if (err != 0)
		goto fail;

	/*
	 * Doorbells BEFORE the handshake, in that order, because the vendor
	 * host driver does the same: it runs facility_t2h_dbell_init() and
	 * facility_h2t_dbell_init() and only then sets HOST_INIT. The NPU
	 * starts looking for msi_msg as soon as it sees us, so having the
	 * vectors already in place avoids a window where it asks and we have
	 * nothing to give.
	 */
	err = npuep_setup_dbells(sc);
	if (err != 0)
		goto fail;

	err = npuep_handshake(sc);
	if (err != 0)
		goto fail;

	npuep_add_sysctls(sc);

	NPUEP_LOCK(sc);
	sc->running = 1;
	callout_reset(&sc->heartbeat, hz / NPUEP_HEARTBEAT_HZ, npuep_heartbeat,
	    sc);
	NPUEP_UNLOCK(sc);

	return (0);

fail:
	npuep_detach(dev);
	return (err);
}

static int
npuep_detach(device_t dev)
{
	struct npuep_softc *sc = device_get_softc(dev);
	int i;

	if (mtx_initialized(&sc->mtx)) {
		NPUEP_LOCK(sc);
		sc->running = 0;
		NPUEP_UNLOCK(sc);
		callout_drain(&sc->heartbeat);
	}

	/*
	 * Withdraw before tearing anything down, and in this order.
	 *
	 * Clearing HOST_INIT and HOST_ALIVE is the only way to tell the far side we are
	 * leaving. It matters because the NPU raises a doorbell by writing the MSI message
	 * itself - so once the vectors are freed and the table is reused, its writes land on
	 * whatever took their place. Stopping the heartbeat alone is not enough: that takes a
	 * scan or more for the target to notice, and the race is exactly the window in which
	 * this module is being unloaded.
	 *
	 * This is still not airtight. There is no acknowledgement to wait for, and a reset
	 * pulse before unloading is the only way to be certain the NPU has stopped.
	 */
	if (sc->handshaken && sc->bar2 != NULL && !sc->lost) {
		uint32_t hs = bar2_read(sc, sc->ctrl + 4);

		if (hs != 0xFFFFFFFFU) {
			bar2_write(sc, sc->ctrl + 4,
			    hs & ~(CTRL_FCLT_HOST_INIT | CTRL_FCLT_HOST_ALIVE));
			device_printf(dev, "withdrew HOST_INIT and HOST_ALIVE "
			    "(handshake now 0x%08x)\n", bar2_read(sc, sc->ctrl + 4));
		}
		sc->handshaken = 0;
	}

	for (i = 0; i < NPUEP_TOTAL_DBELLS; i++) {
		struct npuep_dbell *db = &sc->dbell[i];

		if (db->cookie != NULL)
			bus_teardown_intr(dev, db->irq, db->cookie);
		if (db->irq != NULL)
			bus_release_resource(dev, SYS_RES_IRQ, db->rid,
			    db->irq);
		db->cookie = NULL;
		db->irq = NULL;
	}
	if (sc->nvec > 0)
		pci_release_msi(dev);

	if (sc->bar2 != NULL)
		bus_release_resource(dev, SYS_RES_MEMORY, sc->bar2_rid,
		    sc->bar2);
	if (sc->bar0 != NULL)
		bus_release_resource(dev, SYS_RES_MEMORY, sc->bar0_rid,
		    sc->bar0);

	/* Last, and never skipped: no driver means no bus mastering. */
	if (sc->busmaster) {
		pci_disable_busmaster(dev);
		sc->busmaster = 0;
	}

	if (mtx_initialized(&sc->mtx))
		mtx_destroy(&sc->mtx);

	return (0);
}

static device_method_t npuep_methods[] = {
	DEVMETHOD(device_probe,		npuep_probe),
	DEVMETHOD(device_attach,	npuep_attach),
	DEVMETHOD(device_detach,	npuep_detach),
	DEVMETHOD_END
};

static driver_t npuep_driver = {
	"npuep",
	npuep_methods,
	sizeof(struct npuep_softc)
};

DRIVER_MODULE(npuep, pci, npuep_driver, NULL, NULL);
MODULE_DEPEND(npuep, pci, 1, 1, 1);
MODULE_VERSION(npuep, 1);
