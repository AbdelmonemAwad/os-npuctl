/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The control-message channel.
 *
 * This is the fourth of the coprocessor's five facilities and the last one this driver learned to
 * speak. It matters because of an asymmetry in the datapath: on transmit the host names the
 * destination port itself, in the two-byte tag at the front of every frame, and that works - 189
 * frames left front port 8 and nowhere else, steered by nothing but the tag. On receive the
 * coprocessor's fastpath has to decide which host interface a frame belongs to, and nothing has
 * ever told it. The table it consults is filled over this channel.
 *
 * Unlike every other facility here, none of the layout below was inferred. Sophos ships
 * usfp_rh.ko - their own target-side handler, not Marvell's sample - built with -g3, so the
 * compiler recorded every structure, field name and offset, and every #define. See npuep.h for
 * the offsets and docs/rpc.md for how they were read out.
 *
 * What the channel is: a DMA ring, not a mailbox. The host writes a command into its OWN memory,
 * puts a sixteen-byte descriptor carrying that physical address into a ring inside the BAR,
 * advances a producer index and rings a doorbell. The target fetches the command by DMA, runs it,
 * writes the answer back by DMA and advances a consumer index. There is no doorbell in the other
 * direction, so the host polls.
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/endian.h>

#include <machine/bus.h>
#include <sys/rman.h>
#include <machine/resource.h>

#include "npuep.h"

/*
 * Where this driver puts things inside the window. The megabyte is ours to lay out - the target
 * reads the descriptor area at whatever offset the ring says - so these are chosen, not measured.
 * Well clear of the 232-byte state block, and page-aligned because it costs nothing.
 */
#define	NPURPC_DESC_OFF		0x1000
#define	NPURPC_DESC_COUNT	32

/* How long to wait for the target to accept the configuration, in hundredths of a second. */
#define	NPURPC_OPEN_WAIT	500

/* And for one command to be answered. */
#define	NPURPC_CMD_WAIT		300

struct npurpc_softc {
	struct npuep_facility	 fac;
	struct mtx		 mtx;

	/*
	 * One command buffer in host memory. The descriptors in the window point at it, so it has
	 * to satisfy the same 36-bit addressing ceiling as every other DMA on this device - see
	 * docs/mvmgmt.md for where that number came from.
	 */
	bus_dma_tag_t		 cmd_tag;
	bus_dmamap_t		 cmd_map;
	void			*cmd_vaddr;
	bus_addr_t		 cmd_paddr;

	int			 opened;	/* the magic has been written and taken */
	uint64_t		 posted;	/* our own count, never read back */
	uint64_t		 commands, answers, timeouts;
};

static struct npurpc_softc *npurpc_sc;

#define	RPC_LOCK(sc)	mtx_lock(&(sc)->mtx)
#define	RPC_UNLOCK(sc)	mtx_unlock(&(sc)->mtx)

static uint32_t
rpc_rd(struct npurpc_softc *sc, bus_size_t o)
{
	return (bus_read_4(sc->fac.res, sc->fac.off + o));
}

static void
rpc_wr(struct npurpc_softc *sc, bus_size_t o, uint32_t v)
{
	bus_write_4(sc->fac.res, sc->fac.off + o, v);
}

static void
rpc_barrier(struct npurpc_softc *sc)
{
	bus_barrier(sc->fac.res, sc->fac.off, sc->fac.size,
	    BUS_SPACE_BARRIER_READ | BUS_SPACE_BARRIER_WRITE);
}

static void
npurpc_dmamap_cb(void *arg, bus_dma_segment_t *segs, int nseg, int error)
{
	if (error == 0 && nseg == 1)
		*(bus_addr_t *)arg = segs[0].ds_addr;
}

/*
 * Lay out one ring in the window.
 *
 * Only the low-priority ring is configured. The channel allows four more at high priority, one
 * per core, and the vendor uses them to keep flow updates off the slow path - but nothing here
 * has a rate that would notice, and each additional ring is another descriptor area and another
 * set of indices to get wrong.
 */
static void
npurpc_layout_ring(struct npurpc_softc *sc)
{
	bus_size_t r = RPC_ST_RING_LO;
	int i;

	mtx_assert(&sc->mtx, MA_OWNED);

	/* Clear the descriptor area before the target is told where it is. */
	for (i = 0; i < NPURPC_DESC_COUNT * 16; i += 4)
		rpc_wr(sc, NPURPC_DESC_OFF + i, 0);

	/*
	 * Both indices start at zero, and that is what makes writing the magic safe: with nothing
	 * posted the target has no descriptor to fetch, so it cannot be pointed at a host address
	 * this driver has not chosen yet.
	 */
	rpc_wr(sc, r + RPC_RING_POSTED, 0);
	rpc_wr(sc, r + RPC_RING_POSTED + 4, 0);
	rpc_wr(sc, r + RPC_RING_DONE, 0);
	rpc_wr(sc, r + RPC_RING_DONE + 4, 0);

	rpc_wr(sc, r + RPC_RING_OFFSET, NPURPC_DESC_OFF);
	rpc_wr(sc, r + RPC_RING_DESC_OFFSET, NPURPC_DESC_OFF);
	rpc_wr(sc, r + RPC_RING_DESC_COUNT, NPURPC_DESC_COUNT);

	/*
	 * ring_num 0, facility index 3 - the control-message channel's own - doorbell 0, not
	 * shared. Doorbell 0 is this facility's: the host-to-target doorbells are handed out in
	 * facility order and every facility before this one declares none, which is also why it is
	 * the only one the target has armed.
	 */
	rpc_wr(sc, r + RPC_RING_CFG, (0) | (3 << 8) | (0 << 16) | (0 << 24));

	rpc_barrier(sc);
}

/*
 * Open the channel: publish a ring, then write the magic the target is waiting for.
 *
 * The order is the whole safety argument. The ring is laid out and the descriptor area cleared
 * BEFORE the magic goes in, because the magic is what makes the target start reading descriptors
 * out of this window. Written against an unconfigured ring it would send the coprocessor to
 * whatever address happened to be lying there - host physical zero, as the window stands.
 */
static int
npurpc_open(struct npurpc_softc *sc)
{
	uint32_t cfg;
	int i;

	mtx_assert(&sc->mtx, MA_OWNED);

	npurpc_layout_ring(sc);

	rpc_wr(sc, RPC_ST_CFG_MAGIC, RPC_STATE_CFG_MAGIC);
	rpc_barrier(sc);

	(void)npuep_ring_dbell(sc->fac.parent, sc->fac.dbell);

	/*
	 * The target clears reconfig_done while it takes the new configuration and sets it again
	 * when it has. It was already set before we wrote anything, so watching for it to come
	 * back would be satisfied instantly by the state we started from - wait for the magic to
	 * be acknowledged instead, and report what the state block says either way.
	 */
	for (i = 0; i < NPURPC_OPEN_WAIT; i++) {
		cfg = rpc_rd(sc, RPC_ST_CFG_REVISION);
		if ((cfg >> 24) != 0 && rpc_rd(sc, RPC_ST_CFG_MAGIC) == RPC_STATE_CFG_MAGIC) {
			sc->opened = 1;
			device_printf(sc->fac.dev,
			    "rpc: channel open - revision %u, %u high rings active, "
			    "descriptors at +%#x x%d\n",
			    cfg & 0xFFFF, (cfg >> 16) & 0xFF, NPURPC_DESC_OFF,
			    NPURPC_DESC_COUNT);
			return (0);
		}
		pause("npurpc", hz / 100);
	}

	device_printf(sc->fac.dev,
	    "rpc: the target did not take the configuration - magic reads %#010x, state %#010x\n",
	    rpc_rd(sc, RPC_ST_CFG_MAGIC), rpc_rd(sc, RPC_ST_CFG_REVISION));
	return (ETIMEDOUT);
}

static int
npurpc_alloc_cmd(struct npurpc_softc *sc)
{
	bus_addr_t pa = 0;
	int err;

	err = bus_dma_tag_create(sc->fac.parent_tag, 64, 0, NPUEP_DMA_LOWADDR,
	    BUS_SPACE_MAXADDR, NULL, NULL, RPC_DATA_MAX_SIZE, 1, RPC_DATA_MAX_SIZE, 0,
	    NULL, NULL, &sc->cmd_tag);
	if (err != 0)
		return (err);

	err = bus_dmamem_alloc(sc->cmd_tag, &sc->cmd_vaddr,
	    BUS_DMA_WAITOK | BUS_DMA_ZERO | BUS_DMA_COHERENT, &sc->cmd_map);
	if (err != 0)
		return (err);

	err = bus_dmamap_load(sc->cmd_tag, sc->cmd_map, sc->cmd_vaddr, RPC_DATA_MAX_SIZE,
	    npurpc_dmamap_cb, &pa, BUS_DMA_NOWAIT);
	if (err != 0 || pa == 0)
		return (err != 0 ? err : ENOMEM);

	sc->cmd_paddr = pa;
	return (0);
}

static void
npurpc_free_cmd(struct npurpc_softc *sc)
{
	if (sc->cmd_vaddr != NULL) {
		bus_dmamap_unload(sc->cmd_tag, sc->cmd_map);
		bus_dmamem_free(sc->cmd_tag, sc->cmd_vaddr, sc->cmd_map);
		sc->cmd_vaddr = NULL;
	}
	if (sc->cmd_tag != NULL) {
		bus_dma_tag_destroy(sc->cmd_tag);
		sc->cmd_tag = NULL;
	}
}

/*
 * Post one command and wait for the answer.
 *
 * The command goes in host memory; only a descriptor naming it goes in the window. The order is
 * the protocol: fill the buffer, fill the descriptor, barrier, advance the producer index,
 * barrier, ring. Anything the target is meant to see must be visible before the index that says
 * it is there, and the index before the doorbell that says to look.
 *
 * On the producer and consumer indices there is one thing this driver does not yet know: whether
 * they count or merely flag. Marvell's sample handler writes done = 1 and tests posted != 0,
 * which is what a test would do; the production handler may well treat them as running counts,
 * the way the datapath's rings do. For the FIRST command the two readings agree - post 1, wait
 * for 1 - so this is written to be correct either way, and what the target actually does to done
 * is then a measurement rather than an assumption.
 */
static int
npurpc_command(struct npurpc_softc *sc, uint8_t cmd, const void *payload, int plen,
    void *resp, int rlen, int *rcout)
{
	uint8_t *b = sc->cmd_vaddr;
	bus_size_t d = NPURPC_DESC_OFF;		/* slot zero; one in flight at a time */
	uint64_t want;
	uint32_t lo, hi;
	uint16_t rc, rpl;
	int i;

	mtx_assert(&sc->mtx, MA_OWNED);

	if (!sc->opened)
		return (-ENXIO);
	if (plen < 0 || plen + RPC_CMD_PAYLOAD > RPC_DATA_MAX_SIZE)
		return (-EINVAL);

	memset(b, 0, RPC_DATA_MAX_SIZE);
	le16enc(b + RPC_CMD_RESP_BUFF_SZ, (uint16_t)(RPC_DATA_MAX_SIZE / 2));
	b[RPC_CMD_CMD] = cmd;
	if (plen > 0)
		memcpy(b + RPC_CMD_PAYLOAD, payload, plen);

	bus_dmamap_sync(sc->cmd_tag, sc->cmd_map, BUS_DMASYNC_PREWRITE | BUS_DMASYNC_PREREAD);

	/* The descriptor: where the command is, how long it is, and that it is posted. */
	rpc_wr(sc, d + RPC_BD_DMA_ADDR, (uint32_t)(sc->cmd_paddr & 0xFFFFFFFFU));
	rpc_wr(sc, d + RPC_BD_DMA_ADDR + 4, (uint32_t)(sc->cmd_paddr >> 32));
	rpc_wr(sc, d + RPC_BD_PAYLOAD_LEN,
	    (uint32_t)(RPC_CMD_PAYLOAD + plen) |
	    ((uint32_t)(RPC_DESC_POST_FLAG | RPC_DESC_NO_AGG_DMA) << 16));
	rpc_wr(sc, d + 12, 0);
	rpc_barrier(sc);

	want = sc->posted + 1;
	rpc_wr(sc, RPC_ST_RING_LO + RPC_RING_POSTED, (uint32_t)want);
	rpc_wr(sc, RPC_ST_RING_LO + RPC_RING_POSTED + 4, (uint32_t)(want >> 32));
	rpc_barrier(sc);

	sc->commands++;
	(void)npuep_ring_dbell(sc->fac.parent, sc->fac.dbell);

	for (i = 0; i < NPURPC_CMD_WAIT; i++) {
		lo = rpc_rd(sc, RPC_ST_RING_LO + RPC_RING_DONE);
		hi = rpc_rd(sc, RPC_ST_RING_LO + RPC_RING_DONE + 4);
		if (((uint64_t)lo | ((uint64_t)hi << 32)) >= want)
			break;
		pause("npurpcc", hz / 100);
	}
	if (i >= NPURPC_CMD_WAIT) {
		sc->timeouts++;
		device_printf(sc->fac.dev,
		    "rpc: command %u went unanswered - posted %ju, done %ju\n",
		    cmd, (uintmax_t)want, (uintmax_t)((uint64_t)lo | ((uint64_t)hi << 32)));
		return (-ETIMEDOUT);
	}

	sc->posted = want;
	sc->answers++;

	bus_dmamap_sync(sc->cmd_tag, sc->cmd_map, BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);

	rc = le16dec(b + RPC_RESP_RC);
	rpl = le16dec(b + RPC_RESP_PAYLOAD_LEN);
	if (rcout != NULL)
		*rcout = rc;

	if (resp != NULL && rlen > 0) {
		if (rpl > rlen)
			rpl = rlen;
		if (RPC_RESP_PAYLOAD + rpl <= RPC_DATA_MAX_SIZE)
			memcpy(resp, b + RPC_RESP_PAYLOAD, rpl);
	}
	return (rpl);		/* how much came back, so a caller cannot print what did not */
}

/*
 * Ask the target to read one entry out of its interface table.
 *
 * Chosen because it changes nothing. The first command over a channel that has never carried one
 * should not also be the first command that configures something: if it goes wrong there are two
 * explanations, and no way to tell them apart.
 */
static int
npurpc_sysctl_probe(SYSCTL_HANDLER_ARGS)
{
	struct npurpc_softc *sc = arg1;
	uint8_t rq[RPC_TBL_REQ_SIZE], out[64];
	char buf[512];
	uint64_t posted, done;
	int err, rc = 0, go = 0, i, n, rlen = 0;

	/*
	 * Zeroed before use, and not as tidiness. The first run of this printed sixteen bytes of
	 * whatever was on the stack and they looked exactly like data - two plausible kernel
	 * pointers - because the answer carried no payload, nothing was copied in, and the buffer
	 * was never initialised. Reporting uninitialised memory as though the far side had sent it
	 * is the most misleading thing an instrument can do.
	 */
	memset(out, 0, sizeof(out));

	err = sysctl_handle_int(oidp, &go, 0, req);
	if (err != 0 || req->newptr == NULL)
		return (err);

	memset(rq, 0, sizeof(rq));
	le32enc(rq + RPC_TBL_S_INDEX, 0);
	le16enc(rq + RPC_TBL_NUM_ENTRIES, 1);
	le32enc(rq + RPC_TBL_E_INDEX, 0);

	RPC_LOCK(sc);
	rlen = npurpc_command(sc, RPC_CMD_LO_LIF_READ, rq, sizeof(rq), out, sizeof(out), &rc);
	err = rlen < 0 ? rlen : 0;
	posted = (uint64_t)rpc_rd(sc, RPC_ST_RING_LO + RPC_RING_POSTED) |
	    ((uint64_t)rpc_rd(sc, RPC_ST_RING_LO + RPC_RING_POSTED + 4) << 32);
	done = (uint64_t)rpc_rd(sc, RPC_ST_RING_LO + RPC_RING_DONE) |
	    ((uint64_t)rpc_rd(sc, RPC_ST_RING_LO + RPC_RING_DONE + 4) << 32);
	RPC_UNLOCK(sc);

	n = snprintf(buf, sizeof(buf),
	    "rpc: LIF_READ -> %s, rc %#x%s; ring now posted %ju done %ju",
	    err == 0 ? "answered" : "no answer", rc,
	    (rc & RPC_RC_ERRNO) ? " (an errno, so the target refused it)" : "",
	    (uintmax_t)posted, (uintmax_t)done);
	if (err == 0) {
		n += snprintf(buf + n, sizeof(buf) - n, "; payload %d bytes", rlen);
		for (i = 0; i < rlen && i < 16 && n < (int)sizeof(buf) - 6; i++)
			n += snprintf(buf + n, sizeof(buf) - n, " %02x", out[i]);
	}
	device_printf(sc->fac.dev, "%s\n", buf);
	return (err);
}

static int
npurpc_sysctl_state(SYSCTL_HANDLER_ARGS)
{
	struct npurpc_softc *sc = arg1;
	char buf[320];
	uint64_t posted, done;
	uint32_t magic, cfg;
	bus_size_t r = RPC_ST_RING_LO;

	RPC_LOCK(sc);
	magic = rpc_rd(sc, RPC_ST_CFG_MAGIC);
	cfg = rpc_rd(sc, RPC_ST_CFG_REVISION);
	posted = (uint64_t)rpc_rd(sc, r + RPC_RING_POSTED) |
	    ((uint64_t)rpc_rd(sc, r + RPC_RING_POSTED + 4) << 32);
	done = (uint64_t)rpc_rd(sc, r + RPC_RING_DONE) |
	    ((uint64_t)rpc_rd(sc, r + RPC_RING_DONE + 4) << 32);
	RPC_UNLOCK(sc);

	snprintf(buf, sizeof(buf),
	    "%s  magic %#010x  revision %u  hi_rings %u  reconfig_done %u\n"
	    "  low ring: posted %ju done %ju (this driver has posted %ju)\n"
	    "  commands %ju answered %ju timed out %ju",
	    sc->opened ? "open" : "not open", magic, cfg & 0xFFFF, (cfg >> 16) & 0xFF,
	    (cfg >> 24) & 0xFF, (uintmax_t)posted, (uintmax_t)done, (uintmax_t)sc->posted,
	    (uintmax_t)sc->commands, (uintmax_t)sc->answers, (uintmax_t)sc->timeouts);

	return (sysctl_handle_string(oidp, buf, sizeof(buf), req));
}

static void
npurpc_add_sysctls(struct npurpc_softc *sc)
{
	struct sysctl_ctx_list *ctx = device_get_sysctl_ctx(sc->fac.dev);
	struct sysctl_oid *tree = device_get_sysctl_tree(sc->fac.dev);
	struct sysctl_oid_list *child = SYSCTL_CHILDREN(tree);
	struct sysctl_oid *node;

	node = SYSCTL_ADD_NODE(ctx, child, OID_AUTO, "rpc_channel",
	    CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, "the control-message channel");
	if (node == NULL)
		return;
	child = SYSCTL_CHILDREN(node);

	SYSCTL_ADD_INT(ctx, child, OID_AUTO, "open", CTLFLAG_RD, &sc->opened, 0,
	    "the target has taken this host's configuration");
	SYSCTL_ADD_PROC(ctx, child, OID_AUTO, "state",
	    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE, sc, 0, npurpc_sysctl_state, "A",
	    "the channel and its ring, read live");
	SYSCTL_ADD_PROC(ctx, child, OID_AUTO, "probe",
	    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, sc, 0, npurpc_sysctl_probe, "I",
	    "write anything to send one harmless read command and report what came back");
}

int
npurpc_attach(struct npuep_facility *fac)
{
	struct npurpc_softc *sc;
	uint32_t cfg;
	int err;

	if (npurpc_sc != NULL)
		return (EBUSY);
	if (fac->size < NPURPC_DESC_OFF + NPURPC_DESC_COUNT * 16) {
		device_printf(fac->dev, "rpc: window is only %ju bytes\n",
		    (uintmax_t)fac->size);
		return (ENXIO);
	}

	sc = malloc(sizeof(*sc), M_DEVBUF, M_WAITOK | M_ZERO);
	sc->fac = *fac;
	mtx_init(&sc->mtx, "npurpc", NULL, MTX_DEF);

	/*
	 * The target publishes reconfig_done before anything else happens, and it is the only
	 * evidence that something is listening on the other end of this window. Without it there
	 * is no point writing a magic word at it.
	 */
	cfg = rpc_rd(sc, RPC_ST_CFG_REVISION);
	if ((cfg >> 24) == 0) {
		device_printf(fac->dev,
		    "rpc: nothing has claimed this window (state %#010x) - leaving it alone\n",
		    cfg);
		mtx_destroy(&sc->mtx);
		free(sc, M_DEVBUF);
		return (ENXIO);
	}

	err = npurpc_alloc_cmd(sc);
	if (err != 0) {
		device_printf(fac->dev, "rpc: no DMA buffer for commands (%d)\n", err);
		npurpc_free_cmd(sc);
		mtx_destroy(&sc->mtx);
		free(sc, M_DEVBUF);
		return (err);
	}

	npurpc_sc = sc;

	RPC_LOCK(sc);
	err = npurpc_open(sc);
	RPC_UNLOCK(sc);

	npurpc_add_sysctls(sc);

	/*
	 * A channel that would not open is not a reason to fail: everything else on this device
	 * keeps working without it, and leaving the driver attached leaves the state readable,
	 * which is what anyone looking into it would want.
	 */
	return (0);
}

void
npurpc_detach(void)
{
	struct npurpc_softc *sc = npurpc_sc;

	if (sc == NULL)
		return;

	/*
	 * Withdraw before freeing. The target stops fetching descriptors when the magic is gone,
	 * and the command buffer it might otherwise DMA into is freed immediately after this.
	 */
	RPC_LOCK(sc);
	if (sc->opened) {
		rpc_wr(sc, RPC_ST_CFG_MAGIC, 0);
		rpc_barrier(sc);
		sc->opened = 0;
	}
	RPC_UNLOCK(sc);

	npurpc_free_cmd(sc);
	mtx_destroy(&sc->mtx);
	free(sc, M_DEVBUF);
	npurpc_sc = NULL;
}
