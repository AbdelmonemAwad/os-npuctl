/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * npunwa - the NetAgent mailbox, which is what brings a front port up.
 *
 * WHY THIS IS A SEPARATE FILE FROM npugiu.c
 *
 * Because it is a separate facility with a separate protocol, and conflating the two cost this
 * project a wrong claim in its own documentation. GIU moves packets; this decides whether a port
 * exists to move them on. They share nothing but a BAR.
 *
 * WHAT IT TALKS TO
 *
 * Sophos's NetAgent, running on the coprocessor, which polls a 64 KB window and logs "Waiting for
 * an incoming message" until something arrives. Underneath it, UMSD drives the Marvell switch
 * over MDIO - and that is what lights the front-panel LEDs. The LEDs are not on the host's side
 * of the link at all, which is why they stay dark no matter how well the datapath works until a
 * port is brought up through here.
 *
 * THE TWO THINGS THAT MADE EARLIER ATTEMPTS FAIL
 *
 * The field map was read correctly off a live window - op, sub, port and payload were all in the
 * right places - and messages built from it were still ignored, twice. The reasons are worth
 * stating where someone will read them:
 *
 *   - The request length at NWA_REQ_LEN was never written, so the far side had no reason to look.
 *   - The answer was waited for on NWA_TURN, which is the HOST's own field. The target never
 *     writes it. It answers on NWA_STATUS.
 *
 * A complete message, sent into silence, watched at the wrong address. Nothing about the format
 * was wrong; two fields were simply not part of it yet. See docs/netagent.md.
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
#include <sys/taskqueue.h>

#include <machine/bus.h>
#include <machine/resource.h>
#include <sys/rman.h>

#include "npuep.h"
#include "npunwa.h"

/*
 * The far side answers a port operation in milliseconds. The vendor allows a second before it
 * will start a transaction and thirty for the answer; those are its numbers and there is no
 * reason to be braver than the people who wrote the other end.
 */
#define	NPUNWA_IDLE_WAIT	100	/* x 10 ms - waiting for the previous transaction */
#define	NPUNWA_REPLY_WAIT	3000	/* x 10 ms - waiting for the answer */

/*
 * NetAgent does not exist yet when this driver attaches, and that is not a fault in either of
 * them. The coprocessor's startup blocks on the host handshake, which npuep completes during its
 * own attach; NetAgent starts about fifteen seconds after that and only then publishes the
 * cookie. So a driver that reads the window at attach time finds zeros.
 *
 * The vendor's module handles this by re-queueing its own work and logging "waiting for facility
 * config availability", and that is the right shape: retry, do not block the load. Sixty seconds
 * is four times what the coprocessor has ever taken.
 */
#define	NPUNWA_READY_RETRY	(hz / 2)
#define	NPUNWA_READY_TRIES	120

/*
 * Link state, once the ports are up. One port per tick rather than all ten at once: each read is
 * a full mailbox transaction against a far side that is also serving its own poll loop, and
 * sweeping the panel once a second is far more resolution than a cable being plugged in needs.
 */
#define	NPUNWA_LINK_TICK	(hz / 10)

struct npunwa_port {
	uint32_t	id;
	int		link;		/* last carrier we read, -1 if never read */
	int		media;		/* 3 fibre, 0 copper, -1 unknown */
	int		up;		/* we commanded it up */
};

/* What one transaction found, handed back to whoever asked for it. */
struct npunwa_xfer_info {
	uint32_t	rb;		/* window offset the reply was read from */
	uint32_t	marker;
	uint32_t	status;
	uint32_t	replylen;	/* bytes, as the far side reported */
};

struct npunwa_softc {
	struct npuep_facility	 fac;
	struct mtx		 mtx;
	/*
	 * A taskqueue thread, not a callout, and the reason is the wait above: every step here
	 * talks to the mailbox, every mailbox transaction sleeps, and a callout handler is not a
	 * place where sleeping is allowed. Under callout_init_mtx it is worse than not allowed -
	 * a thirty-second reply wait would stall every other callout on the machine.
	 */
	struct taskqueue	*tq;
	struct timeout_task	 task;
	int			 tries;
	/*
	 * Two flags, not one, and the difference matters. running says the driver is alive and a
	 * mailbox wait is allowed to wait. stop says the periodic task must not run again and must
	 * not requeue itself. Detach needs to stop the task while still being allowed to use the
	 * mailbox itself, to put the ports back down - one flag cannot express that, and trying to
	 * make it do so wedged an unload inside taskqueue_free waiting for a thread that kept
	 * being handed new work.
	 */
	int			 running;
	int			 stop;
	int			 ready;		/* the mailbox has been found and validated */
	int			 busy;		/* a transaction is in the window */
	int			 sweep;		/* which port the link poll looks at next */

	/* the probe: the last raw request issued by hand, and what came back */
	uint32_t		 probe_req[NWA_RAW_MAX_REQ_WORDS];
	int			 probe_nreq;
	uint32_t		 probe_reply[NWA_RAW_MAX_REPLY_WORDS];
	int			 probe_len;	/* bytes, as the far side reported */
	int			 probe_err;

	struct npunwa_xfer_info	 probe_info;	/* where the probe's reply was, and what was there */

	uint32_t		 dump_off;	/* what the window reader is looking at */
	int			 dump_words;
	uint32_t		 body;		/* NWA_BODY_OFF's value, also the gate */
	uint32_t		 max_req;
	struct npunwa_port	 port[NWA_LAST_PORT + 1];
	uint64_t		 commands, failures, timeouts;
};

static struct npunwa_softc *npunwa_sc;

static __inline uint32_t
nwa_rd(struct npunwa_softc *sc, bus_size_t o)
{
	return (bus_read_4(sc->fac.res, sc->fac.off + o));
}

static __inline void
nwa_wr(struct npunwa_softc *sc, bus_size_t o, uint32_t v)
{
	bus_write_4(sc->fac.res, sc->fac.off + o, v);
}

static __inline void
nwa_barrier(struct npunwa_softc *sc)
{
	bus_barrier(sc->fac.res, sc->fac.off, NWA_REQ_SIZE + 0x60,
	    BUS_SPACE_BARRIER_READ | BUS_SPACE_BARRIER_WRITE);
}

static int
nwa_wait(struct npunwa_softc *sc, bus_size_t off, uint32_t want, int ticks)
{
	int i;

	mtx_assert(&sc->mtx, MA_OWNED);

	for (i = 0; i < ticks; i++) {
		uint32_t v = nwa_rd(sc, off);

		if (v == want)
			return (0);
		if (v == 0xFFFFFFFFU)
			return (ENXIO);		/* the endpoint stopped decoding */
		if (!sc->running)
			return (ENXIO);		/* detach is waiting; do not make it wait */

		/*
		 * msleep releases the mutex for the duration of the sleep. pause does not, and
		 * that difference is the whole of "panic: sleeping thread holds npunwa": any
		 * other thread that then blocks on this mutex walks into propagate_priority(),
		 * finds the owner asleep, and takes the machine down. It took an unload racing
		 * the link poll to show it, which is to say it took four hours of not showing.
		 */
		msleep(&sc->busy, &sc->mtx, 0, "npunwa", hz / 100);
	}
	return (ETIMEDOUT);
}

/*
 * One transaction, with the request body given as raw words.
 *
 * The order is the protocol, so it is written out plainly rather than decomposed: wait for idle,
 * length, body, signal, wait for the reply, read it, acknowledge. `reply` may be NULL; its first
 * two words are the marker and the status and are checked here, so a caller receives only
 * payload.
 *
 * Everything about the protocol lives here; npunwa_transact() below is this with the four fields
 * it knows about filled in. The raw form exists because the vendor's host sends requests this one
 * does not yet understand - a status poll with words in places our four-field view has no name
 * for - and reproducing those exactly is the only way to learn what they return.
 *
 * reqlen is written UNROUNDED at NWA_REQ_LEN, and the reply is read at body + reqlen, because
 * that is where the far side puts it. Getting this wrong reads the tail of our own request back
 * and calls it an answer.
 */
static int
npunwa_xfer(struct npunwa_softc *sc, const uint32_t *req, int nreq, uint32_t *reply, int nreply,
    struct npunwa_xfer_info *info)
{
	bus_size_t rb;
	uint32_t marker, status;
	int err, i, reqlen = nreq * 4;

	mtx_assert(&sc->mtx, MA_OWNED);

	if (nreq <= 0 || reqlen > (int)sc->max_req)
		return (EINVAL);

	err = nwa_wait(sc, NWA_STATUS, NWA_STATUS_IDLE, NPUNWA_IDLE_WAIT);
	if (err != 0) {
		/*
		 * Something is still in the window. Do NOT clear the status and barge in: the far
		 * side may be mid-reply, and a host that resets the other end's register to get
		 * its own turn is how two drivers end up writing the same slot.
		 */
		sc->timeouts++;
		return (err);
	}

	for (i = 0; i < nreq; i++)
		nwa_wr(sc, sc->body + i * 4, req[i]);

	nwa_wr(sc, NWA_REQ_LEN, (uint32_t)reqlen);
	nwa_barrier(sc);
	nwa_wr(sc, NWA_TURN, NWA_TURN_REQUEST);		/* last, and it is the signal */
	nwa_barrier(sc);

	sc->commands++;

	err = nwa_wait(sc, NWA_STATUS, NWA_STATUS_REPLY, NPUNWA_REPLY_WAIT);
	if (err != 0) {
		sc->timeouts++;
		return (err);
	}
	nwa_barrier(sc);

	/* The reply follows the request at the UNROUNDED request length. */
	rb = sc->body + reqlen;
	marker = nwa_rd(sc, rb + NWA_RP_MARKER);
	status = nwa_rd(sc, rb + NWA_RP_STATUS);

	/*
	 * Hand back where we looked and what was there, if the caller wants it. This used to be
	 * written into the softc, which meant the link poll overwrote it between a probe issuing a
	 * request and anyone reading the answer - so the probe reported another transaction's
	 * offsets as its own. Shared scratch space for per-call results is a way to measure the
	 * wrong thing and believe it.
	 */
	if (info != NULL) {
		info->rb = (uint32_t)rb;
		info->marker = marker;
		info->status = status;
		info->replylen = nwa_rd(sc, NWA_REPLY_LEN);
	}

	if (reply != NULL) {
		for (i = 0; i < nreply; i++)
			reply[i] = nwa_rd(sc, rb + NWA_RP_PAYLOAD + 4 * i);
	}

	nwa_wr(sc, NWA_TURN, NWA_TURN_ACK);
	nwa_barrier(sc);

	if (marker != NWA_RP_MARKER_VALUE) {
		device_printf(sc->fac.dev,
		    "nwa: reply marker 0x%08x, expected 0x%02x - refusing to parse it\n",
		    marker, NWA_RP_MARKER_VALUE);
		sc->failures++;
		return (EBADMSG);
	}
	if (status != NWA_RP_STATUS_OK) {
		sc->failures++;
		return (EIO);
	}
	return (0);
}

/*
 * The four-field request this driver actually uses, which is the raw one with a fixed shape.
 */
static int
npunwa_transact(struct npunwa_softc *sc, uint32_t op, uint32_t sub, uint32_t port,
    uint32_t payload, uint32_t *reply, int nreply)
{
	uint32_t req[NWA_REQ_SIZE / 4];

	memset(req, 0, sizeof(req));
	req[NWA_RQ_OP / 4] = op;
	req[NWA_RQ_SUB / 4] = sub;
	req[NWA_RQ_PORT / 4] = port;
	req[NWA_RQ_PAYLOAD / 4] = payload;

	return (npunwa_xfer(sc, req, NWA_REQ_SIZE / 4, reply, nreply, NULL));
}

/*
 * The window holds exactly one transaction, and now that the wait above releases the mutex there
 * is a gap in which a second caller could start one. There is only one caller today - the task
 * below - but a mailbox that is single-writer by luck rather than by construction is not worth
 * the next person's afternoon.
 */
static int
npunwa_command(struct npunwa_softc *sc, uint32_t op, uint32_t sub, uint32_t port,
    uint32_t payload, uint32_t *reply, int nreply)
{
	int err;

	mtx_assert(&sc->mtx, MA_OWNED);

	while (sc->busy) {
		if (!sc->running)
			return (ENXIO);
		msleep(&sc->busy, &sc->mtx, 0, "npunwaq", hz / 10);
	}

	sc->busy = 1;
	err = npunwa_transact(sc, op, sub, port, payload, reply, nreply);
	sc->busy = 0;
	wakeup(&sc->busy);

	return (err);
}

/* Set a port's administrative state. This is the one that lights the LED. */
static int
npunwa_port_set_state(struct npunwa_softc *sc, int n, int up)
{
	return (npunwa_command(sc, NWA_OP_SET, NWA_SUB_STATE, NWA_PORT_ID(n),
	    up ? 1 : 0, NULL, 0));
}

static int
npunwa_port_get(struct npunwa_softc *sc, int n, uint32_t sub, uint32_t *out)
{
	return (npunwa_command(sc, NWA_OP_GET, sub, NWA_PORT_ID(n), 0, out, 1));
}

/*
 * Bring every port we know about up, and read back what came of it. A port that refuses is
 * reported and skipped: one dead port is not a reason to leave the other nine dark.
 */
static void
npunwa_bring_up(struct npunwa_softc *sc)
{
	uint32_t v;
	int n, up = 0, linked = 0;

	mtx_assert(&sc->mtx, MA_OWNED);

	for (n = NWA_FIRST_PORT; n <= NWA_LAST_PORT; n++) {
		struct npunwa_port *p = &sc->port[n];

		p->id = NWA_PORT_ID(n);
		p->link = -1;		/* unknown, so the first sweep always reports */
		p->media = -1;

		if (npunwa_port_set_state(sc, n, 1) != 0) {
			device_printf(sc->fac.dev, "nwa: port %d refused to come up\n", n);
			continue;
		}
		p->up = 1;
		up++;

		if (npunwa_port_get(sc, n, NWA_SUB_STATE, &v) == 0) {
			p->link = (int)v;
			if (v != 0)
				linked++;
		}
		if (npunwa_port_get(sc, n, NWA_SUB_MEDIA, &v) == 0)
			p->media = (int)v;
	}

	/*
	 * Do not report carrier here. A port that has just been commanded up has not finished
	 * autonegotiating, so this reads zero on a cable that is plugged in and working - which is
	 * exactly what the first version did, and it looked like a fault. The link poll below says
	 * what is actually connected, a moment later.
	 */
	device_printf(sc->fac.dev, "nwa: %d of %d ports up\n",
	    up, NWA_LAST_PORT - NWA_FIRST_PORT + 1);
	(void)linked;
}

/*
 * One port per tick. Reports a change and nothing else, so the log says when a cable moved rather
 * than repeating itself once a second forever.
 */
static int
npunwa_link_step(struct npunwa_softc *sc)
{
	struct npunwa_port *p;
	uint32_t v;
	int n;

	mtx_assert(&sc->mtx, MA_OWNED);

	n = sc->sweep;
	if (n < NWA_FIRST_PORT || n > NWA_LAST_PORT)
		n = NWA_FIRST_PORT;
	p = &sc->port[n];

	if (p->up && npunwa_port_get(sc, n, NWA_SUB_STATE, &v) == 0) {
		int link = (v != 0);

		if (link != p->link) {
			device_printf(sc->fac.dev, "nwa: port %d (0x%04x, %s) %s\n",
			    n, p->id, p->media == 3 ? "fibre" : "copper",
			    link ? "carrier up" : "carrier down");
			p->link = link;
		}
	}

	sc->sweep = (n >= NWA_LAST_PORT) ? NWA_FIRST_PORT : n + 1;
	return (NPUNWA_LINK_TICK);
}

/*
 * A request written by hand, for finding out what this mailbox can be asked.
 *
 * The vendor's host polls the network agent with a request our four-field view has no names for:
 * operation 0x45, sub-operation 0x10, and words in body positions this driver never writes. The
 * reply is large and full of numbers that climb, which is what per-port packet counters look
 * like. There is no way to confirm that except to send the same request and read the answer, and
 * no way to send it except to be able to write an arbitrary body.
 *
 *	sysctl dev.npuep.0.nwa.probe="45 10 0 0 0 1 0 2"
 *	sysctl -n dev.npuep.0.nwa.probe
 *
 * Reading operations only. This refuses SET, because a probe that can reconfigure ports by
 * mistyping one word is not a probe, it is a hazard - the fourteen front ports are downstream of
 * it. Widening that is a deliberate act for another day.
 */
static int
npunwa_sysctl_probe(SYSCTL_HANDLER_ARGS)
{
	struct npunwa_softc *sc = arg1;
	uint32_t rq[NWA_RAW_MAX_REQ_WORDS];
	char in[160], *p, *end;
	char *out;
	int err, i, n = 0, words, shown;

	/*
	 * Out of line, not on the stack. A thousand words of hex is ten kilobytes and a kernel
	 * stack is sixteen; putting this in an automatic would work right up until it did not.
	 */
#define	NPUNWA_PROBE_OUT	(NWA_RAW_MAX_REPLY_WORDS * 10 + 128)

	/* A read reports the last exchange. */
	if (req->newptr == NULL)
		goto report;

	in[0] = '\0';
	err = sysctl_handle_string(oidp, in, sizeof(in), req);
	if (err != 0)
		return (err);

	memset(rq, 0, sizeof(rq));
	for (p = in; *p != '\0' && n < NWA_RAW_MAX_REQ_WORDS; ) {
		while (*p == ' ' || *p == '\t' || *p == ',')
			p++;
		if (*p == '\0')
			break;
		rq[n++] = (uint32_t)strtoul(p, &end, 16);
		if (end == p)
			return (EINVAL);
		p = end;
	}
	if (n == 0)
		return (EINVAL);

	if (rq[NWA_RQ_OP / 4] == NWA_OP_SET) {
		device_printf(sc->fac.dev,
		    "nwa: the probe will not issue SET - it is for reading\n");
		return (EPERM);
	}

	mtx_lock(&sc->mtx);
	if (!sc->ready || sc->body == 0) {
		mtx_unlock(&sc->mtx);
		return (ENXIO);
	}
	memcpy(sc->probe_req, rq, sizeof(rq));
	sc->probe_nreq = n;
	sc->probe_len = 0;
	memset(sc->probe_reply, 0, sizeof(sc->probe_reply));
	memset(&sc->probe_info, 0, sizeof(sc->probe_info));
	sc->probe_err = npunwa_xfer(sc, rq, n, sc->probe_reply, NWA_RAW_MAX_REPLY_WORDS,
	    &sc->probe_info);
	sc->probe_len = (int)sc->probe_info.replylen;
	mtx_unlock(&sc->mtx);

	return (0);

report:
	if (sc->probe_nreq == 0)
		return (sysctl_handle_string(oidp, "", 1, req));

	out = malloc(NPUNWA_PROBE_OUT, M_DEVBUF, M_WAITOK);

	n = snprintf(out, NPUNWA_PROBE_OUT, "sent");
	for (i = 0; i < sc->probe_nreq; i++)
		n += snprintf(out + n, NPUNWA_PROBE_OUT - n, " %x", sc->probe_req[i]);
	n += snprintf(out + n, NPUNWA_PROBE_OUT - n, "\n");

	if (sc->probe_err != 0) {
		snprintf(out + n, NPUNWA_PROBE_OUT - n,
		    "refused (%d) - looked at window +0x%x, found marker %08x status %08x, "
		    "the agent reported %u bytes", sc->probe_err, sc->probe_info.rb,
		    sc->probe_info.marker, sc->probe_info.status, sc->probe_info.replylen);
		err = sysctl_handle_string(oidp, out, NPUNWA_PROBE_OUT, req);
		free(out, M_DEVBUF);
		return (err);
	}

	/*
	 * The far side reports a reply length; trust it for how much to show, but never past what
	 * was actually read into the buffer.
	 */
	words = sc->probe_len / 4;
	if (words > NWA_RAW_MAX_REPLY_WORDS)
		words = NWA_RAW_MAX_REPLY_WORDS;
	shown = words;

	n += snprintf(out + n, NPUNWA_PROBE_OUT - n,
	    "reply %d bytes at window +0x%x (marker %08x status %08x), %d of %d words:",
	    sc->probe_len, sc->probe_info.rb, sc->probe_info.marker, sc->probe_info.status,
	    shown, sc->probe_len / 4);
	for (i = 0; i < shown && n < NPUNWA_PROBE_OUT - 16; i++)
		n += snprintf(out + n, NPUNWA_PROBE_OUT - n, "%s%08x",
		    (i % 8) == 0 ? "\n  " : " ", sc->probe_reply[i]);

	err = sysctl_handle_string(oidp, out, NPUNWA_PROBE_OUT, req);
	free(out, M_DEVBUF);
	return (err);
#undef NPUNWA_PROBE_OUT
}

/*
 * Read the window itself, in words, from wherever you say.
 *
 * The probe above reads the reply where the protocol says the reply is - at the body plus the
 * unrounded request length - and reports what the far side put in the length register. When those
 * two disagree with each other, or with what a request of a different length returned a minute
 * earlier, the only way forward is to stop reasoning about where the answer should be and go and
 * look at the window.
 *
 *	sysctl dev.npuep.0.nwa.dump="34 40"    forty words from window offset 0x34
 *
 * Both numbers are hex. Reading is harmless: this is the coprocessor's published window and the
 * host reads most of it on every transaction anyway.
 */
static int
npunwa_sysctl_dump(SYSCTL_HANDLER_ARGS)
{
	struct npunwa_softc *sc = arg1;
	char in[64], *p, *end, *out;
	uint32_t v[NWA_RAW_MAX_REPLY_WORDS];
	u_long off, words;
	int err, i, n = 0;

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
		if (words > NWA_RAW_MAX_REPLY_WORDS)
			words = NWA_RAW_MAX_REPLY_WORDS;
		if ((off & 3) != 0 || off + words * 4 > (u_long)sc->fac.size)
			return (EINVAL);

		mtx_lock(&sc->mtx);
		sc->dump_off = (uint32_t)off;
		sc->dump_words = (int)words;
		mtx_unlock(&sc->mtx);
		return (0);
	}

	if (sc->dump_words == 0)
		return (sysctl_handle_string(oidp, "", 1, req));

	mtx_lock(&sc->mtx);
	for (i = 0; i < sc->dump_words; i++)
		v[i] = nwa_rd(sc, sc->dump_off + i * 4);
	words = sc->dump_words;
	off = sc->dump_off;
	mtx_unlock(&sc->mtx);

#define	NPUNWA_DUMP_OUT	(NWA_RAW_MAX_REPLY_WORDS * 10 + 128)
	out = malloc(NPUNWA_DUMP_OUT, M_DEVBUF, M_WAITOK);
	n = snprintf(out, NPUNWA_DUMP_OUT, "window +0x%lx, %lu words:", off, words);
	for (i = 0; i < (int)words && n < NPUNWA_DUMP_OUT - 16; i++)
		n += snprintf(out + n, NPUNWA_DUMP_OUT - n, "%s%08x",
		    (i % 8) == 0 ? "\n  " : " ", v[i]);

	err = sysctl_handle_string(oidp, out, NPUNWA_DUMP_OUT, req);
	free(out, M_DEVBUF);
	return (err);
#undef NPUNWA_DUMP_OUT
}

static void
npunwa_add_sysctls(struct npunwa_softc *sc)
{
	struct sysctl_ctx_list *ctx = device_get_sysctl_ctx(sc->fac.dev);
	struct sysctl_oid *tree = device_get_sysctl_tree(sc->fac.dev);
	struct sysctl_oid_list *child = SYSCTL_CHILDREN(tree);
	struct sysctl_oid *node;

	node = SYSCTL_ADD_NODE(ctx, child, OID_AUTO, "nwa", CTLFLAG_RD | CTLFLAG_MPSAFE, NULL,
	    "the network agent, which owns the front ports");
	if (node == NULL)
		return;
	child = SYSCTL_CHILDREN(node);

	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "commands", CTLFLAG_RD, &sc->commands, 0,
	    "mailbox transactions completed");
	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "failures", CTLFLAG_RD, &sc->failures, 0,
	    "transactions the agent refused or answered malformed");
	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "timeouts", CTLFLAG_RD, &sc->timeouts, 0,
	    "transactions the agent never answered");
	SYSCTL_ADD_INT(ctx, child, OID_AUTO, "ready", CTLFLAG_RD, &sc->ready, 0,
	    "the mailbox has been found and validated");
	SYSCTL_ADD_PROC(ctx, child, OID_AUTO, "probe",
	    CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_MPSAFE, sc, 0, npunwa_sysctl_probe, "A",
	    "write a request as hex words, read back what the agent answered");
	SYSCTL_ADD_PROC(ctx, child, OID_AUTO, "dump",
	    CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_MPSAFE, sc, 0, npunwa_sysctl_dump, "A",
	    "write \"offset words\" in hex, read back that part of the window");
}

/*
 * Poll for the far side, then do the whole of the once-only setup and stop.
 *
 * Everything here reads a value the coprocessor published, so it all belongs on this side of the
 * wait rather than at attach: the cookie, the version gate, and the maximum request length.
 */
static int
npunwa_ready_step(struct npunwa_softc *sc)
{
	uint32_t cookie, body, maxreq;

	mtx_assert(&sc->mtx, MA_OWNED);

	cookie = nwa_rd(sc, NWA_COOKIE);
	if (cookie != NWA_COOKIE_VALUE) {
		if (++sc->tries >= NPUNWA_READY_TRIES) {
			device_printf(sc->fac.dev,
			    "nwa: the network agent never appeared - cookie still 0x%08x after "
			    "%d seconds. The front ports stay down.\n",
			    cookie, NPUNWA_READY_TRIES / 2);
			return (0);
		}
		if (sc->tries == 1)
			device_printf(sc->fac.dev,
			    "nwa: waiting for the network agent to publish its window\n");
		return (NPUNWA_READY_RETRY);
	}

	/*
	 * The version gate. This field is also the body offset, so a value other than the one we
	 * understand is both "a different protocol" and "the body is somewhere else" - there is
	 * nothing sensible to do with it but refuse.
	 */
	body = nwa_rd(sc, NWA_BODY_OFF);
	if (body != NWA_BODY_EXPECTED) {
		device_printf(sc->fac.dev,
		    "nwa: mailbox body offset 0x%x, this driver speaks 0x%x - refusing\n",
		    body, NWA_BODY_EXPECTED);
		return (0);
	}
	sc->body = body;

	maxreq = nwa_rd(sc, NWA_MAX_REQ);
	if (maxreq < NWA_REQ_SIZE || maxreq > sc->fac.size) {
		device_printf(sc->fac.dev,
		    "nwa: maximum request %u does not fit a %ju byte window - refusing\n",
		    maxreq, (uintmax_t)sc->fac.size);
		return (0);
	}
	sc->max_req = maxreq;

	device_printf(sc->fac.dev,
	    "nwa: mailbox ready after %d ms, body at +0x%x, requests up to %u bytes\n",
	    sc->tries * (1000 / 2), sc->body, sc->max_req);

	npunwa_add_sysctls(sc);

	npunwa_bring_up(sc);

	/* From here the same task is the link poll. */
	sc->ready = 1;
	sc->sweep = NWA_FIRST_PORT;
	return (NPUNWA_LINK_TICK);
}

/*
 * The one place either step runs from, and the only thread that touches the mailbox.
 */
static void
npunwa_task(void *arg, int pending)
{
	struct npunwa_softc *sc = arg;
	int delay, stop;

	mtx_lock(&sc->mtx);
	if (sc->stop || !sc->running) {
		mtx_unlock(&sc->mtx);
		return;
	}
	delay = sc->ready ? npunwa_link_step(sc) : npunwa_ready_step(sc);
	if (delay <= 0)
		sc->stop = 1;			/* nothing reschedules; say so once, here */
	stop = sc->stop;
	mtx_unlock(&sc->mtx);

	/*
	 * Read under the lock, acted on outside it. Requeueing while holding the mutex would be
	 * harmless; requeueing after detach has decided to stop would not, so the decision has to
	 * be the one detach published, not one re-read afterwards.
	 */
	if (!stop && delay > 0)
		taskqueue_enqueue_timeout(sc->tq, &sc->task, delay);
}

int
npunwa_attach(struct npuep_facility *fac)
{
	struct npunwa_softc *sc;

	if (npunwa_sc != NULL)
		return (EBUSY);
	if (fac->size < 0x100) {
		device_printf(fac->dev, "nwa: window is only %ju bytes\n", (uintmax_t)fac->size);
		return (ENXIO);
	}

	sc = malloc(sizeof(*sc), M_DEVBUF, M_WAITOK | M_ZERO);
	sc->fac = *fac;
	mtx_init(&sc->mtx, "npunwa", NULL, MTX_DEF);
	sc->tq = taskqueue_create("npunwa", M_WAITOK, taskqueue_thread_enqueue, &sc->tq);
	TIMEOUT_TASK_INIT(sc->tq, &sc->task, 0, npunwa_task, sc);
	taskqueue_start_threads(&sc->tq, 1, PI_NET, "npunwa");

	npunwa_sc = sc;

	mtx_lock(&sc->mtx);
	sc->running = 1;
	mtx_unlock(&sc->mtx);
	taskqueue_enqueue_timeout(sc->tq, &sc->task, NPUNWA_READY_RETRY);

	/*
	 * Nothing here can fail any more. Everything that could - the cookie, the version, the
	 * maximum request length - reads a value the far side has not published yet, so all of it
	 * lives behind the callout and reports itself there.
	 */
	return (0);
}

void
npunwa_detach(void)
{
	struct npunwa_softc *sc = npunwa_sc;
	int n;

	if (sc == NULL)
		return;

	/*
	 * Put the ports back down on the way out. Leaving them up would leave a firewall's
	 * interfaces live with nothing behind them, which is worse than dark.
	 */
	mtx_lock(&sc->mtx);
	sc->stop = 1;
	wakeup(&sc->busy);		/* whatever is mid-wait gives up now */
	mtx_unlock(&sc->mtx);

	/*
	 * Stop the task before touching the window ourselves, and keep cancelling until there is
	 * nothing left to cancel. One drain is not enough on its own: an instance that started
	 * before the flag was published can requeue after the drain returns, and then
	 * taskqueue_free waits for a thread that keeps being handed work. That is a hung unload,
	 * and it is uninterruptible.
	 */
	while (taskqueue_cancel_timeout(sc->tq, &sc->task, NULL) != 0)
		taskqueue_drain_timeout(sc->tq, &sc->task);
	taskqueue_drain_timeout(sc->tq, &sc->task);
	taskqueue_free(sc->tq);
	sc->tq = NULL;

	/* Nothing else can reach the mailbox now, and this context is allowed to sleep. */
	mtx_lock(&sc->mtx);
	if (sc->body != 0 && nwa_rd(sc, NWA_COOKIE) == NWA_COOKIE_VALUE) {
		for (n = NWA_FIRST_PORT; n <= NWA_LAST_PORT; n++)
			if (sc->port[n].up)
				(void)npunwa_port_set_state(sc, n, 0);
	}
	sc->running = 0;
	mtx_unlock(&sc->mtx);

	device_printf(sc->fac.dev, "nwa: %ju commands, %ju refused, %ju timed out\n",
	    (uintmax_t)sc->commands, (uintmax_t)sc->failures, (uintmax_t)sc->timeouts);

	mtx_destroy(&sc->mtx);
	free(sc, M_DEVBUF);
	npunwa_sc = NULL;
}
