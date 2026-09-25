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
#define	NPURPC_HI_DESC_OFF	0x2000
#define	NPURPC_HI_DESC_COUNT	32

/* How much of a hand-written command, and of its answer, the workbench sysctl will carry. */
#define	NPURPC_RAW_MAX		64

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
	int			 stalled;	/* a command went unanswered; post no more */

	/* The last hand-written exchange, kept so a read can report it. */
	uint8_t			 raw_cmd;
	int			 raw_plen;
	int			 raw_err;
	int			 raw_rc;
	int			 raw_rlen;
	uint8_t			 raw_resp[NPURPC_RAW_MAX];
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

/*
 * The first eight bytes of the state block are one field to the target: it compares the whole
 * quadword against its cached copy to decide whether the configuration changed. Writing it in two
 * halves would let it read a magic belonging to one configuration beside a ring count belonging to
 * another, so it is written whole.
 */
static void
rpc_wr8(struct npurpc_softc *sc, bus_size_t o, uint64_t v)
{
	bus_write_8(sc->fac.res, sc->fac.off + o, v);
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
 * Lay out the rings in the window.
 *
 * Two of them, and the second is not an optimisation - it is the difference between a channel that
 * works and one that answers a single command and then takes the coprocessor down with it.
 *
 * The rpc facility on this board has exactly one host-to-target doorbell, and the target's
 * interrupt handler treats that as meaning the high-priority ring shares it: on every doorbell
 * that is not a configuration change it schedules high ring zero's tasklet, whatever the host has
 * declared. If the host declared no high rings, that ring's context was never filled in, so the
 * tasklet runs against facility zero, doorbell zero. Facility zero is ctrl, which has no
 * host-to-target doorbells at all, so enabling it fails - and the failure path reschedules the
 * tasklet unconditionally. It re-arms itself forever, printing two unratelimited errors per pass
 * to a 115200 console, on the one core left to housekeeping by isolcpus - the same core that runs
 * the low ring's work item. The low ring is never serviced again.
 *
 * That is not a hypothesis. It is what happened here four times in a row, and the vendor's own
 * host avoids it by declaring one high ring, which its boot log records as "Allocated DMA dev #0
 * to hi ring #1 on doorbell 3.0".
 *
 * So high ring zero is declared, sharing this facility's doorbell, with its indices equal and its
 * descriptor area empty. The target's ring walker returns immediately when posted equals done, so
 * the ring carries nothing; it exists so that the tasklet has a doorbell it is allowed to enable.
 *
 * Everything the target can read is cleared first, including the four high-ring slots this driver
 * would otherwise never touch. They are bound to the target's ring contexts when its own module
 * loads, not when a ring is declared, so whatever an earlier operating system left in them is live.
 */
static void
npurpc_layout_one(struct npurpc_softc *sc, bus_size_t r, bus_size_t desc, uint32_t count,
    uint32_t cfg)
{
	uint32_t i;

	for (i = 0; i < count * 16; i += 4)
		rpc_wr(sc, desc + i, 0);

	rpc_wr(sc, r + RPC_RING_POSTED, 0);
	rpc_wr(sc, r + RPC_RING_POSTED + 4, 0);
	rpc_wr(sc, r + RPC_RING_DONE, 0);
	rpc_wr(sc, r + RPC_RING_DONE + 4, 0);

	rpc_wr(sc, r + RPC_RING_OFFSET, (uint32_t)desc);
	rpc_wr(sc, r + RPC_RING_DESC_OFFSET, (uint32_t)desc);
	rpc_wr(sc, r + RPC_RING_DESC_COUNT, count);
	rpc_wr(sc, r + RPC_RING_CFG, cfg);
}

static void
npurpc_layout_ring(struct npurpc_softc *sc)
{
	bus_size_t o;

	mtx_assert(&sc->mtx, MA_OWNED);

	/* The whole state block, not just the parts this driver fills in. */
	for (o = 0; o < RPC_STATE_SIZE; o += 4)
		rpc_wr(sc, o, 0);

	/*
	 * ring_num 0, facility index 3 - the control-message channel's own - doorbell 0, not
	 * shared. Doorbell 0 is this facility's: the host-to-target doorbells are handed out in
	 * facility order and every facility before this one declares none, which is also why it is
	 * the only one the target has armed.
	 */
	npurpc_layout_one(sc, RPC_ST_RING_LO, NPURPC_DESC_OFF, NPURPC_DESC_COUNT,
	    (0) | (3 << 8) | (0 << 16) | (0 << 24));

	/* ring_num 1, same facility, same doorbell, and shared - which is what says so. */
	npurpc_layout_one(sc, RPC_ST_RINGS, NPURPC_HI_DESC_OFF, NPURPC_HI_DESC_COUNT,
	    (1) | (3 << 8) | (0 << 16) | (1 << 24));

	rpc_barrier(sc);
}

/*
 * Wait for the target to say it has taken a configuration.
 *
 * It clears reconfig_done when it starts and sets it when it has finished, so this is only
 * meaningful after writing a configuration word with that byte zero - which both writes below do.
 * The previous version of this waited for the byte to be non-zero without ever having cleared it,
 * which the state the channel starts in satisfies immediately. It measured nothing.
 */
static int
npurpc_wait_reconfig(struct npurpc_softc *sc)
{
	int i;

	for (i = 0; i < NPURPC_OPEN_WAIT; i++) {
		if ((rpc_rd(sc, RPC_ST_CFG_REVISION) >> 24) != 0)
			return (0);
		pause("npurpco", hz / 100);
	}
	return (ETIMEDOUT);
}

/*
 * Open the channel: publish the rings, then hand the target a configuration in two steps.
 *
 * Two steps because that is what the vendor's host does - its boot log shows "updating ring
 * configuration: 0" and then "updating ring configuration: 1015fd7d3ab00", which is this same
 * quadword carrying the magic, revision 0x015f, one active high ring and reconfig_done clear. The
 * zero first tears down whatever the target was holding, so the second is taken from a known state
 * rather than merged into one this driver did not create.
 *
 * The rings are laid out and both descriptor areas cleared BEFORE any of it, because the
 * configuration is what makes the target start reading descriptors out of this window. Written
 * against an unconfigured ring it would send the coprocessor to whatever address happened to be
 * lying there - host physical zero, as the window stands.
 */
static int
npurpc_open(struct npurpc_softc *sc)
{
	uint64_t cfg;
	uint32_t st;
	int err;

	mtx_assert(&sc->mtx, MA_OWNED);

	npurpc_layout_ring(sc);

	rpc_wr8(sc, RPC_ST_CFG_MAGIC, 0);
	rpc_barrier(sc);
	(void)npuep_ring_dbell(sc->fac.parent, sc->fac.dbell);
	if ((err = npurpc_wait_reconfig(sc)) != 0) {
		device_printf(sc->fac.dev,
		    "rpc: the target did not acknowledge the empty configuration - state %#010x\n",
		    rpc_rd(sc, RPC_ST_CFG_REVISION));
		return (err);
	}

	cfg = (uint64_t)RPC_STATE_CFG_MAGIC |		/* cfg_magic       */
	    ((uint64_t)RPC_CFG_REVISION << 32) |	/* cfg_revision    */
	    ((uint64_t)1 << 48);			/* active_hi_rings */
	rpc_wr8(sc, RPC_ST_CFG_MAGIC, cfg);
	rpc_barrier(sc);
	(void)npuep_ring_dbell(sc->fac.parent, sc->fac.dbell);
	if ((err = npurpc_wait_reconfig(sc)) != 0) {
		device_printf(sc->fac.dev,
		    "rpc: the target did not take the configuration - magic reads %#010x, "
		    "state %#010x\n",
		    rpc_rd(sc, RPC_ST_CFG_MAGIC), rpc_rd(sc, RPC_ST_CFG_REVISION));
		return (err);
	}

	if (rpc_rd(sc, RPC_ST_CFG_MAGIC) != RPC_STATE_CFG_MAGIC) {
		device_printf(sc->fac.dev,
		    "rpc: the target acknowledged but the magic did not stick - reads %#010x\n",
		    rpc_rd(sc, RPC_ST_CFG_MAGIC));
		return (EIO);
	}

	sc->opened = 1;
	sc->stalled = 0;
	st = rpc_rd(sc, RPC_ST_CFG_REVISION);
	device_printf(sc->fac.dev,
	    "rpc: channel open - revision %u, %u high ring sharing doorbell %d, "
	    "descriptors at +%#x x%d\n",
	    st & 0xFFFF, (st >> 16) & 0xFF, sc->fac.dbell, NPURPC_DESC_OFF, NPURPC_DESC_COUNT);
	return (0);
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
 * The indices are running counts, not flags. The target takes the descriptor at
 * done & (desc_count - 1) and then increments done, so a command's slot is fixed by its sequence
 * number and desc_count has to be a power of two. Thirty-two is.
 *
 * Two fields here were wrong for as long as this channel has existed, and both came from reading
 * the names rather than the code. payload_len is the payload alone: the target adds the eight-byte
 * command header itself when it sizes the fetch, so including it here made every fetch eight bytes
 * too long. And the post flag does not mean "this descriptor is posted" - it means "post only, do
 * not answer". With it set the target runs the command and advances done without ever calling the
 * routine that writes a response, which is exactly what we saw: a command the target completed,
 * whose answer buffer still held what this driver had put in it.
 */
static int
npurpc_command(struct npurpc_softc *sc, uint8_t cmd, const void *payload, int plen,
    void *resp, int rlen, int *rcout)
{
	uint8_t *b = sc->cmd_vaddr;
	bus_size_t d;
	uint64_t want;
	uint32_t lo, hi;
	uint16_t rc, rpl;
	int i;

	mtx_assert(&sc->mtx, MA_OWNED);

	if (!sc->opened)
		return (-ENXIO);
	/*
	 * One unanswered command is the whole of the evidence that the target has stopped
	 * servicing this ring, and posting another on top of it is how a stall became an hour of
	 * dead coprocessor. Refuse until the channel is opened again.
	 */
	if (sc->stalled)
		return (-ESHUTDOWN);
	if (plen < 0 || plen + RPC_CMD_PAYLOAD > RPC_DATA_MAX_SIZE)
		return (-EINVAL);

	memset(b, 0, RPC_DATA_MAX_SIZE);
	le16enc(b + RPC_CMD_RESP_BUFF_SZ, (uint16_t)(RPC_DATA_MAX_SIZE / 2));
	b[RPC_CMD_CMD] = cmd;
	if (plen > 0)
		memcpy(b + RPC_CMD_PAYLOAD, payload, plen);

	bus_dmamap_sync(sc->cmd_tag, sc->cmd_map, BUS_DMASYNC_PREWRITE | BUS_DMASYNC_PREREAD);

	/* The slot this command will be taken from, and the sequence number that says so. */
	want = sc->posted + 1;
	d = NPURPC_DESC_OFF + (bus_size_t)((want - 1) & (NPURPC_DESC_COUNT - 1)) * 16;

	/* The descriptor: where the command is and how long its payload is. */
	rpc_wr(sc, d + RPC_BD_DMA_ADDR, (uint32_t)(sc->cmd_paddr & 0xFFFFFFFFU));
	rpc_wr(sc, d + RPC_BD_DMA_ADDR + 4, (uint32_t)(sc->cmd_paddr >> 32));
	rpc_wr(sc, d + RPC_BD_PAYLOAD_LEN,
	    (uint32_t)plen | ((uint32_t)RPC_DESC_NO_AGG_DMA << 16));
	rpc_wr(sc, d + 12, 0);
	rpc_barrier(sc);

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
		sc->stalled = 1;
		device_printf(sc->fac.dev,
		    "rpc: command %u went unanswered - posted %ju, done %ju. No more will be sent "
		    "until the channel is reopened.\n",
		    cmd, (uintmax_t)want, (uintmax_t)((uint64_t)lo | ((uint64_t)hi << 32)));
		return (-ETIMEDOUT);
	}

	sc->posted = want;
	sc->answers++;

	bus_dmamap_sync(sc->cmd_tag, sc->cmd_map, BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);

	/*
	 * done having advanced says the target finished with the descriptor; this byte says it
	 * wrote an answer. They are not the same thing, and keeping them apart is what turned a
	 * silent wrong answer into a visible one last time.
	 */
	if (b[RPC_RESP_DESC_DONE] == 0) {
		device_printf(sc->fac.dev,
		    "rpc: command %u completed without an answer - descriptor_done clear\n", cmd);
		return (-EIO);
	}

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

/*
 * Send one command by hand, and report what came back.
 *
 * The channel defines forty-five commands and this driver knows the payload of exactly one of
 * them. The rest have to be learned, and the only way to learn a payload is to send one and read
 * the answer. The first hexadecimal number is the command, the rest are payload bytes:
 *
 *	sysctl dev.npuep.0.rpc_channel.command="25 00 00 00 00 01 00 00 00 00 00 00 00"
 *	sysctl -n dev.npuep.0.rpc_channel.command
 *
 * Unlike the network agent's probe, this does not refuse commands that write. It cannot: the
 * commands worth learning here are precisely the ones that fill in the interface table, and a
 * read-only version of this would have nothing to say. So it is sharp. A mistyped command number
 * is still a real command, and the fourteen front ports are downstream of it. It is an instrument
 * for a workbench, not a knob for a running firewall - which is also why it reports the command it
 * is about to send before sending it, so a mistake is visible in the log even if the target never
 * answers.
 */
static int
npurpc_sysctl_command(SYSCTL_HANDLER_ARGS)
{
	struct npurpc_softc *sc = arg1;
	uint8_t pl[NPURPC_RAW_MAX];
	char in[400], out[900], *q, *end;
	unsigned long v;
	int err, n = 0, i, k = 0, cmd = -1;

	if (req->newptr == NULL)
		goto report;

	in[0] = '\0';
	err = sysctl_handle_string(oidp, in, sizeof(in), req);
	if (err != 0)
		return (err);

	memset(pl, 0, sizeof(pl));
	for (q = in; *q != '\0'; ) {
		while (*q == ' ' || *q == '\t' || *q == ',')
			q++;
		if (*q == '\0')
			break;
		v = strtoul(q, &end, 16);
		if (end == q)
			return (EINVAL);
		q = end;
		if (cmd < 0) {
			if (v > 0xFF)
				return (EINVAL);
			cmd = (int)v;
			continue;
		}
		if (v > 0xFF || n >= NPURPC_RAW_MAX)
			return (EINVAL);
		pl[n++] = (uint8_t)v;
	}
	if (cmd < 0)
		return (EINVAL);

	device_printf(sc->fac.dev, "rpc: sending command %d with %d payload bytes\n", cmd, n);

	RPC_LOCK(sc);
	sc->raw_cmd = (uint8_t)cmd;
	sc->raw_plen = n;
	sc->raw_rc = 0;
	memset(sc->raw_resp, 0, sizeof(sc->raw_resp));
	sc->raw_rlen = npurpc_command(sc, (uint8_t)cmd, pl, n, sc->raw_resp,
	    (int)sizeof(sc->raw_resp), &sc->raw_rc);
	sc->raw_err = sc->raw_rlen < 0 ? -sc->raw_rlen : 0;
	if (sc->raw_rlen < 0)
		sc->raw_rlen = 0;
	RPC_UNLOCK(sc);

report:
	RPC_LOCK(sc);
	if (sc->raw_cmd == 0 && sc->raw_plen == 0 && sc->raw_rlen == 0 && sc->raw_err == 0) {
		RPC_UNLOCK(sc);
		return (sysctl_handle_string(oidp, "nothing sent yet", 17, req));
	}
	k = snprintf(out, sizeof(out), "command %u, %d payload bytes -> %s",
	    sc->raw_cmd, sc->raw_plen,
	    sc->raw_err != 0 ? "no answer" : "answered");
	if (sc->raw_err != 0)
		k += snprintf(out + k, sizeof(out) - k, " (errno %d)", sc->raw_err);
	else {
		k += snprintf(out + k, sizeof(out) - k, ", rc %#x%s, %d bytes back:",
		    sc->raw_rc, (sc->raw_rc & RPC_RC_ERRNO) ?
		    " (an errno, so the target refused it)" : "", sc->raw_rlen);
		for (i = 0; i < sc->raw_rlen && k < (int)sizeof(out) - 6; i++)
			k += snprintf(out + k, sizeof(out) - k, " %02x", sc->raw_resp[i]);
	}
	RPC_UNLOCK(sc);

	return (sysctl_handle_string(oidp, out, sizeof(out), req));
}

static int
npurpc_sysctl_state(SYSCTL_HANDLER_ARGS)
{
	struct npurpc_softc *sc = arg1;
	char buf[512];
	uint64_t posted, done, hposted, hdone;
	uint32_t magic, cfg, hcfg;
	bus_size_t r = RPC_ST_RING_LO, h = RPC_ST_RINGS;

	RPC_LOCK(sc);
	magic = rpc_rd(sc, RPC_ST_CFG_MAGIC);
	cfg = rpc_rd(sc, RPC_ST_CFG_REVISION);
	posted = (uint64_t)rpc_rd(sc, r + RPC_RING_POSTED) |
	    ((uint64_t)rpc_rd(sc, r + RPC_RING_POSTED + 4) << 32);
	done = (uint64_t)rpc_rd(sc, r + RPC_RING_DONE) |
	    ((uint64_t)rpc_rd(sc, r + RPC_RING_DONE + 4) << 32);
	hposted = (uint64_t)rpc_rd(sc, h + RPC_RING_POSTED) |
	    ((uint64_t)rpc_rd(sc, h + RPC_RING_POSTED + 4) << 32);
	hdone = (uint64_t)rpc_rd(sc, h + RPC_RING_DONE) |
	    ((uint64_t)rpc_rd(sc, h + RPC_RING_DONE + 4) << 32);
	hcfg = rpc_rd(sc, h + RPC_RING_CFG);
	RPC_UNLOCK(sc);

	snprintf(buf, sizeof(buf),
	    "%s%s  magic %#010x  revision %u  hi_rings %u  reconfig_done %u\n"
	    "  low ring:  posted %ju done %ju (this driver has posted %ju)\n"
	    "  high ring: posted %ju done %ju  num %u fclt %u dbell %u shared %u\n"
	    "  commands %ju answered %ju timed out %ju",
	    sc->opened ? "open" : "not open", sc->stalled ? ", STALLED" : "",
	    magic, cfg & 0xFFFF, (cfg >> 16) & 0xFF, (cfg >> 24) & 0xFF,
	    (uintmax_t)posted, (uintmax_t)done, (uintmax_t)sc->posted,
	    (uintmax_t)hposted, (uintmax_t)hdone,
	    hcfg & 0xFF, (hcfg >> 8) & 0xFF, (hcfg >> 16) & 0xFF, (hcfg >> 24) & 0xFF,
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
	SYSCTL_ADD_PROC(ctx, child, OID_AUTO, "command",
	    CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_MPSAFE, sc, 0, npurpc_sysctl_command, "A",
	    "send one command by hand: the command number then its payload, all hexadecimal");
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
