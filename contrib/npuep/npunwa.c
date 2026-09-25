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

struct npunwa_port {
	uint32_t	id;
	int		link;		/* last carrier we read, -1 if never read */
	int		media;		/* 3 fibre, 0 copper, -1 unknown */
	int		up;		/* we commanded it up */
};

struct npunwa_softc {
	struct npuep_facility	 fac;
	struct mtx		 mtx;
	struct callout		 ready;
	int			 tries;
	int			 running;
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

	for (i = 0; i < ticks; i++) {
		if (nwa_rd(sc, off) == want)
			return (0);
		if (nwa_rd(sc, off) == 0xFFFFFFFFU)
			return (ENXIO);		/* the endpoint stopped decoding */
		pause("npunwa", hz / 100);
	}
	return (ETIMEDOUT);
}

/*
 * One transaction. The order is the protocol, so it is written out plainly rather than
 * decomposed: wait for idle, length, body, signal, wait for the reply, read it, acknowledge.
 *
 * `reply` may be NULL. Its first two words are the marker and the status and are checked here, so
 * a caller receives only payload.
 */
static int
npunwa_command(struct npunwa_softc *sc, uint32_t op, uint32_t sub, uint32_t port,
    uint32_t payload, uint32_t *reply, int nreply)
{
	bus_size_t rb;
	uint32_t marker, status;
	int err, i;

	mtx_assert(&sc->mtx, MA_OWNED);

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

	for (i = 0; i < NWA_REQ_SIZE; i += 4)
		nwa_wr(sc, sc->body + i, 0);
	nwa_wr(sc, sc->body + NWA_RQ_OP, op);
	nwa_wr(sc, sc->body + NWA_RQ_SUB, sub);
	nwa_wr(sc, sc->body + NWA_RQ_PORT, port);
	nwa_wr(sc, sc->body + NWA_RQ_PAYLOAD, payload);

	nwa_wr(sc, NWA_REQ_LEN, NWA_REQ_SIZE);
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
	rb = sc->body + NWA_REQ_SIZE;
	marker = nwa_rd(sc, rb + NWA_RP_MARKER);
	status = nwa_rd(sc, rb + NWA_RP_STATUS);

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
		p->link = p->media = -1;

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

	device_printf(sc->fac.dev, "nwa: %d of %d ports up, %d with carrier\n",
	    up, NWA_LAST_PORT - NWA_FIRST_PORT + 1, linked);

	for (n = NWA_FIRST_PORT; n <= NWA_LAST_PORT; n++) {
		struct npunwa_port *p = &sc->port[n];

		if (p->link > 0)
			device_printf(sc->fac.dev, "nwa:   port %d (0x%04x, %s) has carrier\n",
			    n, p->id, p->media == 3 ? "fibre" : "copper");
	}
}

/*
 * Poll for the far side, then do the whole of the once-only setup and stop.
 *
 * Everything here reads a value the coprocessor published, so it all belongs on this side of the
 * wait rather than at attach: the cookie, the version gate, and the maximum request length.
 */
static void
npunwa_ready_tick(void *arg)
{
	struct npunwa_softc *sc = arg;
	uint32_t cookie, body, maxreq;

	mtx_assert(&sc->mtx, MA_OWNED);
	if (!sc->running)
		return;

	cookie = nwa_rd(sc, NWA_COOKIE);
	if (cookie != NWA_COOKIE_VALUE) {
		if (++sc->tries >= NPUNWA_READY_TRIES) {
			device_printf(sc->fac.dev,
			    "nwa: the network agent never appeared - cookie still 0x%08x after "
			    "%d seconds. The front ports stay down.\n",
			    cookie, NPUNWA_READY_TRIES / 2);
			sc->running = 0;
			return;
		}
		if (sc->tries == 1)
			device_printf(sc->fac.dev,
			    "nwa: waiting for the network agent to publish its window\n");
		callout_reset(&sc->ready, NPUNWA_READY_RETRY, npunwa_ready_tick, sc);
		return;
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
		sc->running = 0;
		return;
	}
	sc->body = body;

	maxreq = nwa_rd(sc, NWA_MAX_REQ);
	if (maxreq < NWA_REQ_SIZE || maxreq > sc->fac.size) {
		device_printf(sc->fac.dev,
		    "nwa: maximum request %u does not fit a %ju byte window - refusing\n",
		    maxreq, (uintmax_t)sc->fac.size);
		sc->running = 0;
		return;
	}
	sc->max_req = maxreq;

	device_printf(sc->fac.dev,
	    "nwa: mailbox ready after %d ms, body at +0x%x, requests up to %u bytes\n",
	    sc->tries * (1000 / 2), sc->body, sc->max_req);

	npunwa_bring_up(sc);
	/* Once only. Nothing reschedules from here. */
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
	callout_init_mtx(&sc->ready, &sc->mtx, 0);

	npunwa_sc = sc;

	mtx_lock(&sc->mtx);
	sc->running = 1;
	callout_reset(&sc->ready, NPUNWA_READY_RETRY, npunwa_ready_tick, sc);
	mtx_unlock(&sc->mtx);

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
	sc->running = 0;
	mtx_unlock(&sc->mtx);
	callout_drain(&sc->ready);

	mtx_lock(&sc->mtx);
	if (sc->body != 0 && nwa_rd(sc, NWA_COOKIE) == NWA_COOKIE_VALUE) {
		for (n = NWA_FIRST_PORT; n <= NWA_LAST_PORT; n++)
			if (sc->port[n].up)
				(void)npunwa_port_set_state(sc, n, 0);
	}
	mtx_unlock(&sc->mtx);

	device_printf(sc->fac.dev, "nwa: %ju commands, %ju refused, %ju timed out\n",
	    (uintmax_t)sc->commands, (uintmax_t)sc->failures, (uintmax_t)sc->timeouts);

	mtx_destroy(&sc->mtx);
	free(sc, M_DEVBUF);
	npunwa_sc = NULL;
}
