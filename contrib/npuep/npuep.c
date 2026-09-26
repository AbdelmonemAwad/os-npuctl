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

#include "npuep.h"

static int	npuep_detach(device_t);
static int	npuep_shutdown(device_t);

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

/*
 * Which MSI-X vector a facility's doorbells start at. The vectors are handed out in facility
 * order, so this is just the running total of everything before it - and it has to agree with
 * the order npuep_setup_msix() walks, which is the same loop.
 */
static __inline int
npuep_first_msix(int facility)
{
	int i, n = 0;

	for (i = 0; i < facility; i++)
		n += npuep_t2h_dbells[i];
	return (n);
}

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
	bus_size_t		 mgmt_off;	/* mvmgmt facility, absolute in BAR2 */
	bus_size_t		 mgmt_size;
	bus_size_t		 giu_off;	/* giu facility, absolute in BAR0 */
	bus_size_t		 giu_size;
	bus_size_t		 nwa_off;	/* network agent facility, absolute in BAR0 */
	bus_size_t		 nwa_size;
	struct resource		*bar4;		/* the target's register window */
	int			 bar4_rid;

	bus_size_t		 rpc_off;	/* the control-message channel, in the BAR2 window */
	bus_size_t		 rpc_size;
	uint64_t		 dbells_rung;

	uint32_t		 rpc_dump_off;	/* what the reader is looking at */
	int			 rpc_dump_words;

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
npuep_read_barmap(struct npuep_softc *sc, int verbose)
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
		/*
		 * Quiet unless the caller asked, because the caller polls: a coprocessor
		 * that was just reset spends about fourteen seconds here, and an ungated
		 * printf turns that into fourteen identical lines on a 115200 console.
		 */
		if (verbose)
			device_printf(dev, "NPU barmap not configured (cookie 0x%08x) - "
			    "the NPU is not running yet\n", cookie);
		return (EAGAIN);
	}
	if (version != NPU_BARMAP_VERSION) {
		device_printf(dev, "barmap version %u, expected %u\n",
		    version, NPU_BARMAP_VERSION);
		return (EINVAL);
	}

	if (verbose)
		device_printf(dev, "barmap version %u cookie 0x%08x\n", version, cookie);

	/* struct facility_bar_map { u32 bar; u32 type; u32 offset; u32 size; } */
	for (i = 0; i < MV_FACILITY_COUNT; i++) {
		bus_size_t e = base + 8 + i * 16;
		uint32_t bar = bar2_read(sc, e);
		uint32_t type = bar2_read(sc, e + 4);
		uint32_t off = bar2_read(sc, e + 8);
		uint32_t size = bar2_read(sc, e + 12);

		/*
		 * An entry the coprocessor has not written yet.
		 *
		 * Skip it, because it is not a facility and it is not a claim about one -
		 * but every field in it is zero, and zero is MV_FACILITY_CONTROL. Read as
		 * an entry it says the control facility lives on BAR0 with no window, which
		 * is the one condition below that fails attach outright, so an unwritten
		 * entry made the driver report the control facility as misplaced.
		 *
		 * This is not a hypothetical. The table is filled in entry by entry as the
		 * coprocessor's firmware comes up, so any load that races its boot - a
		 * module reload after a reset pulse, which is the normal way to work on this
		 * driver - lands in the middle of it. The caller waits for the table to be
		 * finished; this makes a half-written one readable rather than fatal.
		 */
		if (bar == 0 && type == 0 && off == 0 && size == 0)
			continue;

		if (verbose)
			device_printf(dev, "  facility %-7s bar%u off 0x%06x size %u\n",
			    type < MV_FACILITY_COUNT ? npuep_facility_name[type] : "?",
			    bar == 0 ? 0 : 2, off, size);

		/*
		 * Remember where the management facility lives while the map is in front of
		 * us. Same rule as the control facility: bound it against the window we
		 * actually mapped, because the offset came from the coprocessor.
		 */
		/*
		 * Order matters here and the obvious spelling is wrong. `size` is unsigned and
		 * comes from the coprocessor, so `off <= WINDOW_LEN - size` wraps to a huge
		 * value the moment size exceeds the window - and then every offset passes, and
		 * npumgmt's bus_write_8 lands outside BAR2 at an address the far side chose.
		 * Bound size first, then subtract. The control facility below subtracts a
		 * constant, so it never had this problem.
		 */
		if (type == MV_FACILITY_MGMT_NETDEV && bar == 1 &&
		    size >= NPUEP_MGMT_MIN_SIZE && size <= NPU_BARMAP_WINDOW_LEN &&
		    off <= NPU_BARMAP_WINDOW_LEN - size && (off & 7) == 0) {
			sc->mgmt_off = sc->window + off;
			sc->mgmt_size = size;
		}

		/*
		 * And the giu facility, which is on BAR0 rather than BAR2 - so it is bounded
		 * against that resource's real size, not against the BAR2 window length. Same
		 * arithmetic order as above: bound the size before it is used as a subtrahend.
		 */
		if (type == MV_FACILITY_GIU && bar == 0) {
			bus_size_t bar0_len = rman_get_size(sc->bar0);

			if (size >= NPUEP_GIU_MIN_SIZE && (bus_size_t)size <= bar0_len &&
			    (bus_size_t)off <= bar0_len - size && (off & 7) == 0) {
				sc->giu_off = off;
				sc->giu_size = size;
			} else {
				device_printf(dev,
				    "giu facility [off %#x size %u] does not fit BAR0 - "
				    "ignoring it\n", off, size);
			}
		}

		/*
		 * The control-message channel, which is on BAR2 like the management facility.
		 * Captured but not spoken to: see npuep.h for what is believed to be on the
		 * other end of it, and note that nothing in this driver writes here.
		 */
		if (type == MV_FACILITY_RPC && bar == 1 &&
		    size >= NPUEP_RPC_MIN_SIZE && size <= NPU_BARMAP_WINDOW_LEN &&
		    off <= NPU_BARMAP_WINDOW_LEN - size && (off & 7) == 0) {
			sc->rpc_off = sc->window + off;
			sc->rpc_size = size;
		}

		/*
		 * And the network agent's window, also on BAR0. This one decides whether the
		 * front ports exist at all - see contrib/npuep/npunwa.c.
		 */
		if (type == MV_FACILITY_NW_AGENT && bar == 0) {
			bus_size_t bar0_len = rman_get_size(sc->bar0);

			if (size >= NPUEP_NWA_MIN_SIZE && (bus_size_t)size <= bar0_len &&
			    (bus_size_t)off <= bar0_len - size && (off & 7) == 0) {
				sc->nwa_off = off;
				sc->nwa_size = size;
			} else {
				device_printf(dev,
				    "nwa facility [off %#x size %u] does not fit BAR0 - "
				    "ignoring it\n", off, size);
			}
		}

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
		/*
		 * EAGAIN rather than EINVAL: with unwritten entries skipped above, the way
		 * to have no control facility is to be reading a table that is not finished.
		 * The caller treats that as "come back in a second", which is what it is.
		 */
		if (verbose)
			device_printf(dev, "no control facility in the barmap\n");
		return (EAGAIN);
	}
	return (0);
}

/*
 * Everything the front ports need. The control facility is not in the list because
 * npuep_read_barmap cannot return success without it.
 */
static int
npuep_barmap_complete(struct npuep_softc *sc)
{
	return (sc->mgmt_off != 0 && sc->giu_size != 0 &&
	    sc->nwa_size != 0 && sc->rpc_size != 0);
}

/*
 * Wait for the coprocessor to finish publishing the facility table.
 *
 * The table is not written atomically and the cookie is not a commit: the cookie and the
 * version are in place long before the five entries are, so a driver that reads the table
 * as soon as it validates can see three facilities and act on three facilities. That is
 * exactly what a reload does - `reload.sh` pulses the reset line and loads the module a
 * second later, and a second is nowhere near long enough for the far side's Linux to come
 * back up.
 *
 * It looks like it works at boot only by accident: `06-npuctl` pulses reset there too, but
 * a minute of other boot work happens before this module is loaded, and by then the table
 * is finished. Measured on this board: complete at 60 seconds after the pulse, so the wait
 * below is twice that and still the difference between fourteen front ports and none.
 *
 * Running out of time is not a failure. Waiting is worth doing because the common case is
 * a coprocessor that is merely slow, but a firmware that genuinely publishes fewer
 * facilities should get a driver that brings up what it can - the same way attach already
 * tolerates a management interface that will not come up.
 */
#define	NPUEP_BARMAP_WAIT	120

static int
npuep_wait_barmap(struct npuep_softc *sc)
{
	device_t dev = sc->dev;
	int err, i, said = 0;

	for (i = 0; i <= NPUEP_BARMAP_WAIT; i++) {
		err = npuep_read_barmap(sc, i == 0);
		if (err != 0 && err != EAGAIN)
			return (err);

		if (err == 0 && npuep_barmap_complete(sc)) {
			if (said)
				device_printf(dev, "the facility table was complete after "
				    "%d seconds\n", i);
			return (0);
		}

		if (!said) {
			device_printf(dev, "the coprocessor has not finished publishing its "
			    "facility table - waiting up to %d seconds for it\n",
			    NPUEP_BARMAP_WAIT);
			said = 1;
		}
		if (i < NPUEP_BARMAP_WAIT)
			pause("npubarm", hz);
	}

	/* Out of time. Read it once more, loudly, and say exactly what is missing. */
	err = npuep_read_barmap(sc, 1);
	if (err == EAGAIN) {
		device_printf(dev, "the coprocessor never published a control facility. "
		    "It is not running: power cycle it rather than resetting it.\n");
		return (ENXIO);
	}
	if (err != 0)
		return (err);

	device_printf(dev, "the facility table is still incomplete after %d seconds -%s%s%s%s "
	    "going on with what was published\n", NPUEP_BARMAP_WAIT,
	    sc->mgmt_off == 0 ? " no mgmt," : "", sc->giu_size == 0 ? " no giu," : "",
	    sc->nwa_size == 0 ? " no nwa," : "", sc->rpc_size == 0 ? " no rpc," : "");
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

/*
 * The control-message channel's state, decoded.
 *
 * Reading the window as hex told us the target had published something and nothing more. This
 * says what: which revision it speaks, whether the channel has been opened, and for every ring
 * where its descriptors are and how far each side has got.
 *
 * Still read-only. Opening the channel means writing a magic word that makes the target start
 * reading ring descriptors out of this window and DMA-ing to whatever host addresses they name -
 * so it may not be written until those rings hold real addresses. A zeroed ring would point the
 * coprocessor at host physical address zero.
 */
static int
npuep_sysctl_rpc_state(SYSCTL_HANDLER_ARGS)
{
	struct npuep_softc *sc = arg1;
	char buf[1024];
	uint32_t magic, cfg;
	int i, n = 0;

	if (sc->rpc_size < RPC_STATE_SIZE)
		return (ENXIO);

	magic = bar2_read(sc, sc->rpc_off + RPC_ST_CFG_MAGIC);
	cfg = bar2_read(sc, sc->rpc_off + RPC_ST_CFG_REVISION);

	n = snprintf(buf, sizeof(buf),
	    "magic %#010x (%s)  revision %u  active_hi_rings %u  reconfig_done %u",
	    magic, magic == RPC_STATE_CFG_MAGIC ? "open" : "not opened by this host",
	    cfg & 0xFFFF, (cfg >> 16) & 0xFF, (cfg >> 24) & 0xFF);

	for (i = 0; i <= RPC_HI_RINGS_MAX; i++) {
		bus_size_t r = sc->rpc_off +
		    (i == 0 ? RPC_ST_RING_LO : RPC_ST_RINGS + (i - 1) * RPC_RING_SIZE);
		uint64_t posted, done;
		uint32_t roff, doff, dcnt, rcfg;

		if (n > (int)sizeof(buf) - 140)
			break;

		posted = (uint64_t)bar2_read(sc, r + RPC_RING_POSTED) |
		    ((uint64_t)bar2_read(sc, r + RPC_RING_POSTED + 4) << 32);
		done = (uint64_t)bar2_read(sc, r + RPC_RING_DONE) |
		    ((uint64_t)bar2_read(sc, r + RPC_RING_DONE + 4) << 32);
		roff = bar2_read(sc, r + RPC_RING_OFFSET);
		doff = bar2_read(sc, r + RPC_RING_DESC_OFFSET);
		dcnt = bar2_read(sc, r + RPC_RING_DESC_COUNT);
		rcfg = bar2_read(sc, r + RPC_RING_CFG);

		n += snprintf(buf + n, sizeof(buf) - n,
		    "\n  %-8s posted %ju done %ju  ring +%#x  desc +%#x x%u  "
		    "num %u fclt %u dbell %u shared %u",
		    i == 0 ? "low" : "high", (uintmax_t)posted, (uintmax_t)done,
		    roff, doff, dcnt,
		    rcfg & 0xFF, (rcfg >> 8) & 0xFF, (rcfg >> 16) & 0xFF, (rcfg >> 24) & 0xFF);
	}

	return (sysctl_handle_string(oidp, buf, sizeof(buf), req));
}

const struct npuep_front_port npuep_front_ports[NPUEP_NFRONT] = {
	{ "Port1",  0x8100,  0,  1 },
	{ "Port2",  0x8200,  1,  2 },
	{ "Port3",  0x8300,  2,  3 },
	{ "Port4",  0x8400,  3,  4 },
	{ "Port5",  0x8500,  4,  5 },
	{ "Port6",  0x8600,  5,  6 },
	{ "Port7",  0x8700,  6,  7 },
	{ "Port8",  0x8800,  7,  8 },
	{ "Port9",  0x0001, 10,  9 },
	{ "Port10", 0x0003, 12, 10 },
	{ "Port11", 0x0004, 13, 11 },
	{ "Port12", 0x0002, 11, 12 },
	{ "PortF1", 0x8900,  9, 13 },
	{ "PortF2", 0x8a00,  8, 14 },
};

/*
 * Ring one of the target's doorbells.
 *
 * The write itself is trivial; everything difficult about it is knowing where. The address the
 * target publishes is an OFFSET into BAR4, so this bounds it against that resource before
 * writing - an out-of-range offset would otherwise be an MMIO write somewhere unrelated on the
 * endpoint.
 *
 * Ringing with nothing posted is harmless: the target looks at its ring, finds producer and
 * consumer equal, and goes back to sleep. That is what makes this safe to try on its own, before
 * there is anything to announce.
 */
int
npuep_ring_dbell(struct npuep_softc *sc, int n)
{
	bus_size_t at;
	uint64_t off;
	uint32_t cnt, data;

	if (sc->bar4 == NULL)
		return (ENXIO);

	cnt = bar2_read(sc, sc->window + CTRL_H2T_DBELL_CNT);
	if (n < 0 || cnt > CTRL_DBELL_MAX || (uint32_t)n >= cnt)
		return (EINVAL);

	at = sc->window + CTRL_H2T_DBELL_MSG + n * CTRL_DBELL_MSG_SIZE;
	off = (uint64_t)bar2_read(sc, at + CTRL_DBELL_ADDR) |
	    ((uint64_t)bar2_read(sc, at + CTRL_DBELL_ADDR + 4) << 32);
	data = bar2_read(sc, at + CTRL_DBELL_DATA);

	if (off == 0)
		return (ENXIO);		/* the target has not armed this one */
	if ((off & 3) != 0 || off > (uint64_t)rman_get_size(sc->bar4) - sizeof(uint32_t)) {
		device_printf(sc->dev,
		    "doorbell %d offset %#jx does not fit BAR4 - refusing to write\n",
		    n, (uintmax_t)off);
		return (ERANGE);
	}

	/*
	 * Whatever the doorbell announces must be visible to the far side before the doorbell is.
	 * The barrier is against the windows this driver writes, not against BAR4 itself.
	 */
	bus_barrier(sc->bar2, 0, rman_get_size(sc->bar2), BUS_SPACE_BARRIER_WRITE);
	bus_write_4(sc->bar4, (bus_size_t)off, data);
	sc->dbells_rung++;
	return (0);
}

static int
npuep_sysctl_ring(SYSCTL_HANDLER_ARGS)
{
	struct npuep_softc *sc = arg1;
	int which = -1, err;

	err = sysctl_handle_int(oidp, &which, 0, req);
	if (err != 0 || req->newptr == NULL)
		return (err);

	err = npuep_ring_dbell(sc, which);
	device_printf(sc->dev, "doorbell %d: %s\n", which,
	    err == 0 ? "rung" :
	    err == ENXIO ? "not armed, or BAR4 is not mapped" :
	    err == EINVAL ? "no such doorbell" : "refused");
	return (err);
}

/*
 * What the target published about its doorbells.
 *
 * Read-only, and it exists because the control-message channel cannot be driven without it: that
 * facility has one host-to-target doorbell and no doorbell back, so a host that cannot ring it
 * has no way to tell the target a command is waiting.
 */
static int
npuep_sysctl_dbells(SYSCTL_HANDLER_ARGS)
{
	struct npuep_softc *sc = arg1;
	char buf[768];
	uint32_t cnt, data[CTRL_DBELL_MAX];
	uint64_t addr[CTRL_DBELL_MAX];
	int i, n = 0;

	cnt = bar2_read(sc, sc->window + CTRL_H2T_DBELL_CNT);
	if (cnt > CTRL_DBELL_MAX)
		cnt = CTRL_DBELL_MAX;

	for (i = 0; i < (int)cnt; i++) {
		bus_size_t at = sc->window + CTRL_H2T_DBELL_MSG + i * CTRL_DBELL_MSG_SIZE;

		addr[i] = (uint64_t)bar2_read(sc, at + CTRL_DBELL_ADDR) |
		    ((uint64_t)bar2_read(sc, at + CTRL_DBELL_ADDR + 4) << 32);
		data[i] = bar2_read(sc, at + CTRL_DBELL_DATA);
	}

	n = snprintf(buf, sizeof(buf), "%u host-to-target doorbell%s", cnt, cnt == 1 ? "" : "s");

	/*
	 * The raw words as well as the reading of them. struct dbell_msg is a u64 followed by a
	 * u32, which the compiler pads to sixteen bytes rather than twelve - and a stride wrong by
	 * four turns every entry after the first into nonsense, which is exactly what the first
	 * attempt printed. Show both, so the interpretation can be checked against what is there.
	 */
	n += snprintf(buf + n, sizeof(buf) - n, "\n  raw from +0x%x:", CTRL_H2T_DBELL_MSG);
	for (i = 0; i < 24 && n < (int)sizeof(buf) - 96; i++)
		n += snprintf(buf + n, sizeof(buf) - n, "%s%08x",
		    (i % 8) == 0 ? "\n   " : " ",
		    bar2_read(sc, sc->window + CTRL_H2T_DBELL_MSG + i * 4));

	for (i = 0; i < (int)cnt && n < (int)sizeof(buf) - 64; i++)
		n += snprintf(buf + n, sizeof(buf) - n, "\n  %d: write %#010x to %#018jx",
		    i, data[i], (uintmax_t)addr[i]);

	return (sysctl_handle_string(oidp, buf, sizeof(buf), req));
}

/*
 * Read the control-message window, in words, from wherever you ask.
 *
 * Read only, and deliberately so. This is the one facility whose protocol we do not know, and the
 * host module that speaks it is not in the vendor's source drop - so the only honest thing to do
 * with it for now is look. Every other facility gave up its shape this way: the management window
 * had a cookie, the network agent's had a cookie and a version gate, and both were recognisable
 * from a hex dump long before a single byte was written back.
 *
 *	sysctl dev.npuep.0.rpc="0 40"     sixty-four words from the start of the window
 *	sysctl -n dev.npuep.0.rpc
 *
 * Both numbers are hex.
 */
#define	NPUEP_RPC_DUMP_WORDS	256

static int
npuep_sysctl_rpc(SYSCTL_HANDLER_ARGS)
{
	struct npuep_softc *sc = arg1;
	uint32_t v[NPUEP_RPC_DUMP_WORDS];
	char in[64], *p, *end, *out;
	u_long off, words;
	int err, i, n = 0;

	if (sc->rpc_size == 0)
		return (ENXIO);

	if (req->newptr != NULL) {
		in[0] = '\0';
		err = sysctl_handle_string(oidp, in, sizeof(in), req);
		if (err != 0)
			return (err);

		p = in;
		off = strtoul(p, &end, 16);
		if (end == p)
			return (EINVAL);
		p = end;
		while (*p == ' ' || *p == '\t' || *p == ',')
			p++;
		words = strtoul(p, &end, 16);
		if (end == p || words == 0)
			words = 16;
		if (words > NPUEP_RPC_DUMP_WORDS)
			words = NPUEP_RPC_DUMP_WORDS;
		if ((off & 3) != 0 || off + words * 4 > (u_long)sc->rpc_size)
			return (EINVAL);

		sc->rpc_dump_off = (uint32_t)off;
		sc->rpc_dump_words = (int)words;
		return (0);
	}

	if (sc->rpc_dump_words == 0)
		return (sysctl_handle_string(oidp, "", 1, req));

	words = sc->rpc_dump_words;
	off = sc->rpc_dump_off;
	for (i = 0; i < (int)words; i++)
		v[i] = bar2_read(sc, sc->rpc_off + off + i * 4);

#define	NPUEP_RPC_OUT	(NPUEP_RPC_DUMP_WORDS * 10 + 128)
	out = malloc(NPUEP_RPC_OUT, M_DEVBUF, M_WAITOK);
	n = snprintf(out, NPUEP_RPC_OUT, "rpc window +0x%lx, %lu words:", off, words);
	for (i = 0; i < (int)words && n < NPUEP_RPC_OUT - 16; i++)
		n += snprintf(out + n, NPUEP_RPC_OUT - n, "%s%08x",
		    (i % 8) == 0 ? "\n  " : " ", v[i]);

	err = sysctl_handle_string(oidp, out, NPUEP_RPC_OUT, req);
	free(out, M_DEVBUF);
	return (err);
#undef NPUEP_RPC_OUT
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

	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "doorbells_rung", CTLFLAG_RD, &sc->dbells_rung, 0,
	    "host-to-target doorbells this driver has rung");
	SYSCTL_ADD_PROC(ctx, child, OID_AUTO, "ring",
	    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, sc, 0, npuep_sysctl_ring, "I",
	    "write a doorbell number to ring it; harmless with nothing posted");
	SYSCTL_ADD_PROC(ctx, child, OID_AUTO, "doorbells",
	    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE, sc, 0, npuep_sysctl_dbells, "A",
	    "the host-to-target doorbells the coprocessor published: where to write, and what");

	if (sc->rpc_size != 0) {
		SYSCTL_ADD_PROC(ctx, child, OID_AUTO, "rpc_state",
		    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE, sc, 0,
		    npuep_sysctl_rpc_state, "A",
		    "the control-message channel's state block, decoded");
		SYSCTL_ADD_PROC(ctx, child, OID_AUTO, "rpc",
		    CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_MPSAFE, sc, 0, npuep_sysctl_rpc, "A",
		    "write \"offset words\" in hex, read back that part of the "
		    "control-message window");
	}

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
	 * Is the endpoint actually there, before anything reads its memory?
	 *
	 * This costs one config-space read and it prevents a hard hang. A read of a BAR belonging
	 * to an endpoint whose link is down does not fail and does not time out - it never
	 * completes, and the host stops. No panic, no console output, nothing: the machine simply
	 * ceases, and only a power cycle recovers it. That was measured here, at the cost of one,
	 * after a reset pulse and a forty-five second wait that turned out not to be enough.
	 *
	 * Config space is the safe place to ask. A configuration read to a device that is not
	 * answering is completed by the root complex as an unsupported request and comes back as
	 * all-ones, so it returns rather than hanging - which is exactly the property the memory
	 * path does not have.
	 *
	 * Refusing here is also correct rather than merely cautious. Every later check in this
	 * driver - the barmap cookie, the facility cookie, DEV_READY - reads memory, so each of
	 * them is downstream of the hazard this one removes.
	 */
	if (pci_read_config(dev, PCIR_VENDOR, 2) == 0xFFFF) {
		device_printf(dev,
		    "the endpoint is not answering config space - it is held in reset or its link "
		    "is down. Refusing to touch its memory: a read of a BAR in that state hangs "
		    "the host with no panic and no log.\n");
		mtx_destroy(&sc->mtx);
		return (ENXIO);
	}

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

	/*
	 * BAR4 carries the target's registers, and the only thing in it this driver needs is the
	 * doorbells. Every facility implemented so far polls, so it has never been mapped before.
	 * Its absence is not fatal - everything that works today keeps working without it - so a
	 * failure here is reported and carried past rather than refusing the device.
	 */
	sc->bar4_rid = PCIR_BAR(NPUEP_DBELL_BAR);
	sc->bar4 = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &sc->bar4_rid, RF_ACTIVE);
	if (sc->bar4 == NULL)
		device_printf(dev, "cannot map BAR4 - the doorbells will not be reachable\n");
	else
		device_printf(dev, "BAR4 mapped, %ju bytes - the doorbells are reachable\n",
		    (uintmax_t)rman_get_size(sc->bar4));

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

	err = npuep_wait_barmap(sc);
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

	/*
	 * The management interface, once the handshake is done and not before: it publishes host
	 * memory addresses to the coprocessor, and doing that before the control facility agrees
	 * we exist is the ordering that caused a panic earlier in this project.
	 *
	 * Its failure is not this driver's failure. The endpoint, the doorbells and the handshake
	 * are useful on their own, and a machine that keeps them is easier to work on than one
	 * that detaches everything because a netdev would not come up.
	 */
	if (sc->mgmt_off != 0) {
		struct npuep_facility fac;

		fac.dev = dev;
		fac.res = sc->bar2;
		fac.off = sc->mgmt_off;
		fac.size = sc->mgmt_size;
		fac.parent_tag = bus_get_dma_tag(dev);
		fac.first_msix = npuep_first_msix(MV_FACILITY_MGMT_NETDEV);
		fac.nmsix = npuep_t2h_dbells[MV_FACILITY_MGMT_NETDEV];

		if (npumgmt_attach(&fac) != 0)
			device_printf(dev, "management interface did not attach\n");
		/*
		 * Then the command channel behind the fourteen front ports. Like the management
		 * interface, its failure is not this driver's failure: the endpoint, the
		 * doorbells, the handshake and mvmgmt0 are all useful without it.
		 */
		if (sc->giu_off != 0 || sc->giu_size != 0) {
			struct npuep_facility gfac;

			gfac.dev = dev;
			gfac.res = sc->bar0;
			gfac.off = sc->giu_off;
			gfac.size = sc->giu_size;
			gfac.parent_tag = bus_get_dma_tag(dev);
			gfac.first_msix = npuep_first_msix(MV_FACILITY_GIU);
			gfac.nmsix = npuep_t2h_dbells[MV_FACILITY_GIU];

			if (npugiu_attach(&gfac) != 0)
				device_printf(dev, "giu command channel did not attach\n");
		} else {
			device_printf(dev, "no giu facility in the barmap\n");
		}

		/*
		 * And the network agent, last, because it is the only thing here that changes
		 * the state of the outside world: it brings the front ports up. Everything
		 * above it is this machine talking to a coprocessor; this puts light on a panel
		 * and link on a wire.
		 */
		if (sc->nwa_off != 0 || sc->nwa_size != 0) {
			struct npuep_facility nfac;

			nfac.dev = dev;
			nfac.res = sc->bar0;
			nfac.off = sc->nwa_off;
			nfac.size = sc->nwa_size;
			nfac.parent_tag = bus_get_dma_tag(dev);
			nfac.first_msix = npuep_first_msix(MV_FACILITY_NW_AGENT);
			nfac.nmsix = npuep_t2h_dbells[MV_FACILITY_NW_AGENT];

			if (npunwa_attach(&nfac) != 0)
				device_printf(dev, "network agent did not attach\n");
		} else {
			device_printf(dev, "no nwa facility in the barmap\n");
		}

	/*
	 * And the control-message channel, which is the one facility that talks back through a
	 * doorbell rather than being polled - so it is handed the register window's owner and the
	 * number of the doorbell the target armed for it.
	 */
	if (sc->rpc_off != 0 && sc->rpc_size != 0) {
		struct npuep_facility rfac;

		memset(&rfac, 0, sizeof(rfac));
		rfac.dev = dev;
		rfac.res = sc->bar2;
		rfac.off = sc->rpc_off;
		rfac.size = sc->rpc_size;
		rfac.parent_tag = bus_get_dma_tag(dev);
		rfac.parent = sc;
		rfac.dbell = 0;		/* every facility before this one declares none */

		if (npurpc_attach(&rfac) != 0)
			device_printf(dev, "the control-message channel did not attach\n");
	}
	} else {
		device_printf(dev, "no mvmgmt facility in the barmap\n");
	}

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

	/* Withdraw from the far side before anything underneath it is torn down, newest first. */
	npurpc_detach();
	npunwa_detach();
	npugiu_detach();
	npumgmt_detach();

	if (mtx_initialized(&sc->mtx)) {
		NPUEP_LOCK(sc);
		sc->running = 0;
		NPUEP_UNLOCK(sc);
		callout_drain(&sc->heartbeat);
	}

	/*
	 * Withdraw before tearing anything down, and in this order.
	 *
	 * Clearing HOST_INIT and HOST_ALIVE is the only way to tell the far side we are leaving.
	 * It matters because the NPU raises a doorbell by writing the MSI message itself - so once
	 * the vectors are freed and the table is reused, its writes land on whatever took their
	 * place. Stopping the heartbeat alone is not enough: that takes a scan or more for the
	 * target to notice, and the race is exactly the window in which this module is being
	 * unloaded.
	 *
	 * This is still not airtight. There is no acknowledgement to wait for, and a reset pulse
	 * before unloading is the only way to be certain the NPU has stopped.
	 *
	 * Everything above this point WAITS - on the coprocessor, and on taskqueue threads. That is
	 * fine here and it is why npuep_shutdown does none of it: see the comment on that function.
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

	if (sc->bar4 != NULL)
		bus_release_resource(dev, SYS_RES_MEMORY, sc->bar4_rid,
		    sc->bar4);
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

/*
 * The machine is going down, and FreeBSD will not call detach on the way.
 *
 * Without this method a reboot leaves the endpoint bus mastering, with its MSI-X vectors armed and
 * the ring addresses this kernel published still live, so it goes on writing received frames and
 * doorbell messages into physical memory the next kernel is about to hand to something else.
 * docs/porting-notes.md records what that looks like from the other end: a fault in an unrelated
 * subsystem, some minutes later, with no device errors logged in between.
 *
 * It is not a hypothetical on this appliance. OPNsense's own early syshook 05-upgrade reboots the
 * machine from inside the boot sequence whenever it finds a pending firmware set, with no warning
 * and no opportunity to unload anything.
 *
 * NOTHING HERE MAY BLOCK, and that cost a power cycle to learn. The first version of this method
 * simply called the same teardown as detach - four facility withdrawals and a callout_drain - and
 * the machine stopped dead. The log reads `reboot: rebooted by root`, then syslog-ng shutting
 * down, then nothing whatsoever for thirty minutes until the power was pulled: no ---<<BOOT>>---,
 * and kern.boottime afterwards is the power cycle rather than a reboot. It never reached the reset.
 *
 * Those withdrawals wait, on a coprocessor to acknowledge and on taskqueue threads to drain. That
 * is correct in kldunload, where the system is running underneath them. Device shutdown methods
 * run late in kern_reboot, after the filesystems are flushed, and waiting on anything there is a
 * request to be hung.
 *
 * So this does only what actually stops the endpoint writing into host memory, in register writes
 * that return:
 *
 *   - clear HOST_INIT and HOST_ALIVE, so the far side is told;
 *   - clear the bus master bit, so it is stopped whether or not it was listening. Every DMA write
 *     and every MSI-X message is a memory write from the endpoint, so this covers the interrupts
 *     too - and it is enforced by the root complex rather than by the coprocessor's cooperation,
 *     which is the whole reason it is the one step that matters.
 *
 * callout_stop rather than callout_drain: stop does not wait for a callout already running, and
 * the heartbeat's entire job is one register write that is harmless at this point.
 */
static int
npuep_shutdown(device_t dev)
{
	struct npuep_softc *sc = device_get_softc(dev);

	if (mtx_initialized(&sc->mtx)) {
		NPUEP_LOCK(sc);
		sc->running = 0;
		callout_stop(&sc->heartbeat);
		NPUEP_UNLOCK(sc);
	}

	if (sc->handshaken && sc->bar2 != NULL && !sc->lost) {
		uint32_t hs = bar2_read(sc, sc->ctrl + 4);

		if (hs != 0xFFFFFFFFU)
			bar2_write(sc, sc->ctrl + 4,
			    hs & ~(CTRL_FCLT_HOST_INIT | CTRL_FCLT_HOST_ALIVE));
		sc->handshaken = 0;
	}

	if (sc->busmaster) {
		pci_disable_busmaster(dev);
		sc->busmaster = 0;
	}

	/* On a serial console this is the line that says the method ran and returned. */
	device_printf(dev, "shutdown: bus mastering disabled\n");
	return (0);
}

static device_method_t npuep_methods[] = {
	DEVMETHOD(device_probe,		npuep_probe),
	DEVMETHOD(device_attach,	npuep_attach),
	DEVMETHOD(device_detach,	npuep_detach),
	DEVMETHOD(device_shutdown,	npuep_shutdown),
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
