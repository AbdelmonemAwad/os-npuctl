/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * npugiu - the AGNIC management channel, which is the door to the fourteen front ports.
 *
 * WHAT THIS IS, AND WHAT IT IS NOT YET
 *
 * The GIU facility is a whole NIC: traffic classes, descriptor rings, buffer pools, checksum
 * offload. All of that is built on one thing first - a command channel - and this file is that
 * channel and nothing more. It publishes two rings, completes the two-stage status handshake,
 * and sends CC_PF_MGMT_ECHO. If the coprocessor echoes, the model is proved and everything above
 * it is ordinary work. If it does not, nothing above it would have worked anyway.
 *
 * That is the same order mvmgmt0 was built in, for the same reason.
 *
 * THE SPLIT, WHICH IS THE OPPOSITE OF mvmgmt0's
 *
 * mvmgmt0 puts the rings, the buffers AND both index pairs in host memory. AGNIC does not:
 *
 *	descriptors and buffers	host memory, published to the device by bus address
 *	producer/consumer indices	DEVICE BAR0, at the offset the device advertises
 *
 * So an index here is an MMIO access, not a load or a store, and a port that treats it as memory
 * is wrong in a way that only shows under load. See docs/giu.md, where the vendor's own macro
 * name says otherwise and the vendor's own comment two lines down corrects it.
 *
 * WHAT THE VENDOR'S DRIVER DOES THAT THIS DELIBERATELY DOES NOT
 *
 * Four independent readings of giu_nic.c produced eighty-six things not to copy. The ones that
 * shaped this file:
 *
 *   - There is not one memory barrier in 4478 lines. Every hand-over is "write the descriptor,
 *     then writel the index", and the only thing ordering those is the __iowmb() Linux hides
 *     inside writel(). bus_space_write_4 promises no such thing, so every publication here has
 *     an explicit barrier and every device-written index is read with one.
 *   - MMIO is touched as ordinary memory - memcpy out of it, memset over it, a read-modify-write
 *     on the status word. None of that is expressible here and none of it is attempted.
 *   - `cmd_idx > cookie_count` should be `>=`, so a device-supplied index one past the end is
 *     accepted and then written through. Everything the coprocessor hands us is bounded before
 *     it is used, and the bound is `>=`.
 *   - dev_use_size is a device-supplied u32 that decides where the index array lands, checked
 *     against a size that is not the window's. It is checked against the window here.
 *   - Cookies are raw kernel pointers on the wire. Here a cookie is a ring index.
 *   - Teardown tells the device nothing and then frees the rings it was given. This withdraws
 *     first, in the order mvmgmt0 already learned the hard way.
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
#include <sys/socket.h>
#include <sys/mbuf.h>
#include <sys/sockio.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_types.h>
#include <net/ethernet.h>
#include <net/bpf.h>

#include <machine/atomic.h>
#include <machine/bus.h>
#include <machine/resource.h>
#include <sys/rman.h>

#include "npuep.h"
#include "npugiu.h"

/*
 * Ring geometry. The vendor uses 256 descriptors for both management rings and a cookie table of
 * 1024 - deliberately decoupled, so a tag is reused only after 1023 further commands. We keep the
 * ring length because the device was built against it, and size the tag space to match the ring
 * because this channel issues commands one at a time under a lock.
 */
#define	NPUGIU_CMD_Q_LEN	256
#define	NPUGIU_NOTIF_Q_LEN	256

/* The device answers DEV_READY within ten to twenty seconds of its own boot; the vendor waits
 * that long and so do we. DEV_MGMT_READY comes back in one to two. */
#define	NPUGIU_DEV_READY_WAIT	1000	/* x 10 ms */
#define	NPUGIU_MGMT_READY_WAIT	400	/* x 10 ms */
#define	NPUGIU_CMD_WAIT		500	/* x 10 ms, for one command's answer */

#define	NPUGIU_DMA_LOWADDR	0xFFFFFFFFFULL	/* 36 bits - see docs/mvmgmt.md */

/*
 * One traffic class each way with one queue in it - what the vendor's module defaults to, and
 * the smallest configuration that can carry a packet. Sophos runs four queues per class; that is
 * a tuning decision and belongs after this works at all.
 *
 * The buffer has to hold an Ethernet frame AND the sixty-six bytes the coprocessor prepends -
 * two for the port identifier and sixty-four of metadata. See docs/giu.md.
 */
#define	NPUGIU_DATA_Q_LEN	256
#define	NPUGIU_BUF_SIZE		2048
#define	NPUGIU_MTU		1500

/*
 * The coprocessor puts sixty-six bytes in front of every frame: two of port identifier in network
 * order, then sixty-four of metadata. Both directions, and it is not optional - a host that sends
 * only the two-byte tag produces a link that passes a ping and corrupts everything larger. See
 * docs/giu.md, where this was read out of the vendor's own transmit path rather than guessed.
 */
#define	NPUGIU_TAG_LEN		2
#define	NPUGIU_META_LEN		64
#define	NPUGIU_HDR_LEN		(NPUGIU_TAG_LEN + NPUGIU_META_LEN)

/*
 * Port identifiers are 0x8000 + n * 0x100, so the port number is the low seven bits of the high
 * byte. Sixteen is more than the fourteen front ports and keeps the arithmetic a mask.
 */
#define	NPUGIU_MAX_PORTS	16
#define	NPUGIU_PORT_NUM(id)	(((id) >> 8) & 0x7F)

/*
 * Which port a frame from this interface leaves by. The real answer is one interface per port,
 * which is the next piece of work; until then this makes the datapath testable against a socket
 * that has a cable in it. Changeable at runtime through a sysctl.
 */
#define	NPUGIU_DEFAULT_PORT	0x8800

struct npugiu_buf {
	bus_dmamap_t	 map;
	void		*vaddr;
	bus_addr_t	 paddr;
};

struct npugiu_ring {
	bus_dma_tag_t	 tag;
	bus_dmamap_t	 map;
	void		*desc;		/* NPUGIU_*_Q_LEN * AGNIC_CMD_DESC_SIZE */
	bus_addr_t	 phys;
	int		 len;
	int		 descsz;

	uint32_t	 prod_slot;	/* BAR-relative offset of our producer word */
	uint32_t	 cons_slot;	/* and of our consumer word */
	int		 prod_idx;	/* which slot number we claimed, for release */
	int		 cons_idx;

	uint32_t	 shadow;	/* the index THIS side owns, kept locally */
};

struct npugiu_softc {
	struct npuep_facility	 fac;
	struct mtx		 mtx;
	struct callout		 poll;
	int			 running;

	bus_size_t		 idx_base;	/* index array, absolute in BAR0 */
	uint32_t		 idx_count;

	struct npugiu_ring	 cmd;
	struct npugiu_ring	 notif;

	/* the datapath: one transmit ring, one receive ring, one buffer pool */
	struct npugiu_ring	 tx;
	struct npugiu_ring	 rx;
	struct npugiu_ring	 bp;

	bus_dma_tag_t		 buf_tag;
	struct npugiu_buf	*buf;		/* receive buffers, handed to the pool */
	struct npugiu_buf	*txbuf;		/* transmit buffers, one per descriptor */
	int			 nbuf;
	int			 datapath;	/* the bring-up sequence completed */

	if_t			 ifp;
	uint32_t		 out_port;	/* NPUGIU_DEFAULT_PORT unless changed */
	uint64_t		 rx_packets, rx_bytes, rx_dropped, rx_bad, rx_nobuf;
	uint64_t		 tx_packets, tx_bytes, tx_full, tx_toolong;
	uint64_t		 rx_port[NPUGIU_MAX_PORTS];

	uint16_t		 next_tag;
	uint8_t			 mac[6];		/* what the coprocessor advertises */
	uint8_t			 hostmac[6];		/* what this end actually answers to */
	uint32_t		 msix_tbl_off;

	/* the one outstanding command */
	int			 waiting;
	uint16_t		 wait_tag;
	int			 answered;
	/*
	 * An answer can span descriptors. flags carries a buffer position - SINGLE, FIRST_MID or
	 * LAST - and a run is only complete on SINGLE or LAST. Four descriptors is 224 bytes,
	 * comfortably more than the largest response the header defines.
	 */
	uint8_t			 answer[4 * AGNIC_MGMT_DESC_DATA_LEN];
	int			 answer_len;

	uint64_t		 commands, answers, notifications, drops;
	uint64_t		 keepalives, late, multipart, overruns;
};

static struct npugiu_softc *npugiu_sc;

#define	GIU_LOCK(sc)	mtx_lock(&(sc)->mtx)
#define	GIU_UNLOCK(sc)	mtx_unlock(&(sc)->mtx)

/*
 * ---------------------------------------------------------------------------------------
 * The facility window, which is MMIO, and the index array inside it, which is also MMIO.
 * ---------------------------------------------------------------------------------------
 */
static __inline uint32_t
cfg_rd(struct npugiu_softc *sc, bus_size_t o)
{
	return (bus_read_4(sc->fac.res, sc->fac.off + o));
}

static __inline void
cfg_wr(struct npugiu_softc *sc, bus_size_t o, uint32_t v)
{
	bus_write_4(sc->fac.res, sc->fac.off + o, v);
}

static __inline uint32_t
idx_rd(struct npugiu_softc *sc, uint32_t slot_off)
{
	return (bus_read_4(sc->fac.res, sc->fac.off + slot_off));
}

static __inline void
idx_wr(struct npugiu_softc *sc, uint32_t slot_off, uint32_t v)
{
	bus_write_4(sc->fac.res, sc->fac.off + slot_off, v);
}

/*
 * Publish an index we own. The barrier is the whole point: the descriptor stores that precede it
 * are in host memory and the index store is MMIO, and nothing in FreeBSD orders the two for us.
 */
static __inline void
idx_publish(struct npugiu_softc *sc, uint32_t slot_off, uint32_t v)
{
	atomic_thread_fence_rel();
	bus_barrier(sc->fac.res, sc->fac.off, AGNIC_CFG_SIZE, BUS_SPACE_BARRIER_WRITE);
	bus_write_4(sc->fac.res, sc->fac.off + slot_off, v);
}

/*
 * Read an index the DEVICE owns, and bound it. An out-of-range value is not a transient to clamp:
 * it means we have lost track of what the far side thinks the ring looks like.
 */
static __inline int
idx_remote(struct npugiu_softc *sc, uint32_t slot_off, int len, uint32_t *out)
{
	uint32_t v = bus_read_4(sc->fac.res, sc->fac.off + slot_off);

	bus_barrier(sc->fac.res, sc->fac.off, AGNIC_CFG_SIZE, BUS_SPACE_BARRIER_READ);
	atomic_thread_fence_acq();
	if (v >= (uint32_t)len)
		return (0);
	*out = v;
	return (1);
}

static __inline uint8_t *
desc_at(struct npugiu_ring *r, uint32_t i)
{
	return ((uint8_t *)r->desc + (size_t)i * (size_t)r->descsz);
}

static __inline uint32_t
ring_next(struct npugiu_ring *r, uint32_t i)
{
	return (i == (uint32_t)(r->len - 1) ? 0 : i + 1);
}

/*
 * ---------------------------------------------------------------------------------------
 * Index slots. The array lives in the device's BAR at the offset the device advertises, one
 * u32 per claimed index, 0xFFFFFFFF meaning free. The HOST initialises it - the device does
 * not - so it is marked free here before anything is claimed.
 * ---------------------------------------------------------------------------------------
 */
static void
npugiu_init_slots(struct npugiu_softc *sc)
{
	uint32_t i;

	for (i = 0; i < sc->idx_count; i++)
		idx_wr(sc, sc->idx_base - sc->fac.off + i * 4, AGNIC_RING_INDEX_SLOT_FREE);
}

static int
npugiu_claim_slot(struct npugiu_softc *sc, uint32_t *slot_off)
{
	uint32_t i, rel;

	for (i = 0; i < sc->idx_count; i++) {
		rel = (uint32_t)(sc->idx_base - sc->fac.off) + i * 4;
		if (idx_rd(sc, rel) == AGNIC_RING_INDEX_SLOT_FREE) {
			idx_wr(sc, rel, 0);
			*slot_off = rel;
			return ((int)i);
		}
	}
	return (-1);
}

static void
npugiu_release_slot(struct npugiu_softc *sc, int which)
{
	if (which < 0)
		return;
	idx_wr(sc, (uint32_t)(sc->idx_base - sc->fac.off) + (uint32_t)which * 4,
	    AGNIC_RING_INDEX_SLOT_FREE);
}

/*
 * ---------------------------------------------------------------------------------------
 * Rings. Descriptors in host memory, DMA-coherent, under the 36-bit ceiling.
 * ---------------------------------------------------------------------------------------
 */
static void
npugiu_dmamap_cb(void *arg, bus_dma_segment_t *segs, int nseg, int error)
{
	bus_addr_t *addr = arg;

	*addr = (error != 0 || nseg != 1) ? 0 : segs[0].ds_addr;
}

static int
npugiu_alloc_ring(struct npugiu_softc *sc, struct npugiu_ring *r, int len, int descsz,
    const char *what)
{
	bus_size_t bytes = (bus_size_t)len * descsz;
	int err;

	r->len = len;
	r->descsz = descsz;
	r->shadow = 0;
	r->prod_idx = r->cons_idx = -1;

	err = bus_dma_tag_create(sc->fac.parent_tag, 64, 0, NPUGIU_DMA_LOWADDR,
	    BUS_SPACE_MAXADDR, NULL, NULL, bytes, 1, bytes, 0, NULL, NULL, &r->tag);
	if (err != 0)
		return (err);
	err = bus_dmamem_alloc(r->tag, &r->desc,
	    BUS_DMA_WAITOK | BUS_DMA_ZERO | BUS_DMA_COHERENT, &r->map);
	if (err != 0)
		return (err);
	err = bus_dmamap_load(r->tag, r->map, r->desc, bytes, npugiu_dmamap_cb,
	    &r->phys, BUS_DMA_NOWAIT);
	if (err != 0 || r->phys == 0)
		return (err != 0 ? err : ENOMEM);

	r->prod_idx = npugiu_claim_slot(sc, &r->prod_slot);
	r->cons_idx = npugiu_claim_slot(sc, &r->cons_slot);
	if (r->prod_idx < 0 || r->cons_idx < 0) {
		device_printf(sc->fac.dev, "giu: no free index slot for the %s ring\n", what);
		return (ENOSPC);
	}

	device_printf(sc->fac.dev,
	    "giu: %s ring %d x %d B @ %#jx, indices at BAR+%#x and BAR+%#x\n",
	    what, len, descsz, (uintmax_t)r->phys,
	    (unsigned)(sc->fac.off + r->prod_slot), (unsigned)(sc->fac.off + r->cons_slot));
	return (0);
}

static void
npugiu_free_ring(struct npugiu_softc *sc, struct npugiu_ring *r)
{
	npugiu_release_slot(sc, r->prod_idx);
	npugiu_release_slot(sc, r->cons_idx);
	r->prod_idx = r->cons_idx = -1;

	if (r->desc != NULL) {
		bus_dmamap_unload(r->tag, r->map);
		bus_dmamem_free(r->tag, r->desc, r->map);
		r->desc = NULL;
	}
	if (r->tag != NULL) {
		bus_dma_tag_destroy(r->tag);
		r->tag = NULL;
	}
}

/* Write one struct agnic_q_hw_info into the configuration window. */
static void
npugiu_publish_ring(struct npugiu_softc *sc, bus_size_t at, struct npugiu_ring *r)
{
	cfg_wr(sc, at + AGNIC_QI_ADDR, (uint32_t)(r->phys & 0xFFFFFFFFU));
	cfg_wr(sc, at + AGNIC_QI_ADDR + 4, (uint32_t)(r->phys >> 32));
	cfg_wr(sc, at + AGNIC_QI_PROD_OFFS, r->prod_slot);
	cfg_wr(sc, at + AGNIC_QI_CONS_OFFS, r->cons_slot);
	cfg_wr(sc, at + AGNIC_QI_LEN, (uint32_t)r->len);
	cfg_wr(sc, at + AGNIC_QI_RES, 0);
}

/*
 * ---------------------------------------------------------------------------------------
 * Commands.
 * ---------------------------------------------------------------------------------------
 */
static uint16_t
npugiu_next_tag(struct npugiu_softc *sc)
{
	do {
		sc->next_tag++;
	} while (sc->next_tag == AGNIC_CMD_ID_ILLEGAL ||
	    sc->next_tag == AGNIC_CMD_ID_NOTIFICATION);
	return (sc->next_tag);
}

/*
 * Post one command. Called with the lock held. The descriptor is zeroed first, deliberately: the
 * vendor never clears it, so a command with no parameters ships whatever the previous occupant of
 * that ring slot left in all fifty-six payload bytes.
 */
static int
npugiu_post(struct npugiu_softc *sc, uint8_t code, const void *params, size_t plen, int want_resp)
{
	struct npugiu_ring *r = &sc->cmd;
	uint32_t cons, push;
	uint8_t *d;

	mtx_assert(&sc->mtx, MA_OWNED);

	if (plen > AGNIC_MGMT_DESC_DATA_LEN)
		return (EMSGSIZE);
	if (!idx_remote(sc, r->cons_slot, r->len, &cons)) {
		device_printf(sc->fac.dev, "giu: command ring consumer out of range\n");
		return (EIO);
	}
	push = r->shadow;
	if (ring_next(r, push) == cons)
		return (EBUSY);			/* full; one slot is always left empty */

	d = desc_at(r, push);
	memset(d, 0, AGNIC_CMD_DESC_SIZE);

	sc->wait_tag = npugiu_next_tag(sc);
	*(uint16_t *)(d + AGNIC_CMD_IDX) = sc->wait_tag;
	*(uint16_t *)(d + AGNIC_CMD_APP_CODE) = AGNIC_AC_PF_MANAGER;
	d[AGNIC_CMD_CODE] = code;
	d[AGNIC_CMD_CLIENT_ID] = 0;
	d[AGNIC_CMD_CLIENT_TYPE] = AGNIC_CDT_PF;
	d[AGNIC_CMD_FLAGS] = (uint8_t)((AGNIC_BUF_POS_SINGLE << AGNIC_CMD_F_BUF_POS_SHIFT) |
	    ((want_resp ? 0 : 1) << AGNIC_CMD_F_NO_RESP_SHIFT));
	if (params != NULL && plen != 0)
		memcpy(d + AGNIC_CMD_DATA, params, plen);

	r->shadow = ring_next(r, push);
	idx_publish(sc, r->prod_slot, r->shadow);

	sc->commands++;
	sc->waiting = want_resp;
	sc->answered = 0;
	sc->answer_len = 0;
	return (0);
}

/*
 * Drain the notification ring. Everything the device puts here - command answers, asynchronous
 * notifications, custom messages - arrives on this one ring and is told apart by cmd_idx.
 *
 * Note what is NOT here: an interrupt. The vendor's management IRQ hooks are empty stubs and a
 * timer does this work; the facility dump on the coprocessor confirms the channel is polled.
 */
static void
npugiu_drain(struct npugiu_softc *sc)
{
	struct npugiu_ring *r = &sc->notif;
	uint32_t prod;
	int guard = r->len;

	mtx_assert(&sc->mtx, MA_OWNED);

	if (!idx_remote(sc, r->prod_slot, r->len, &prod)) {
		device_printf(sc->fac.dev, "giu: notification producer out of range\n");
		sc->running = 0;
		return;
	}

	while (r->shadow != prod && guard-- > 0) {
		uint8_t *d = desc_at(r, r->shadow);
		uint16_t tag = *(uint16_t *)(d + AGNIC_CMD_IDX);
		uint8_t code = d[AGNIC_CMD_CODE];

		if (tag == AGNIC_CMD_ID_NOTIFICATION) {
			sc->notifications++;
			switch (code) {
			case AGNIC_NC_PF_KEEP_ALIVE:
				sc->keepalives++;
				break;
			case AGNIC_NC_PF_LINK_CHANGE:
				device_printf(sc->fac.dev, "giu: link change reported\n");
				break;
			default:
				device_printf(sc->fac.dev,
				    "giu: unknown notification code 0x%02x\n", code);
				break;
			}
		} else if (!sc->waiting && tag == sc->wait_tag) {
			/*
			 * An answer to a command we have already given up on. Not an error and not
			 * somebody else's tag - it is ours, arriving late. The first version of
			 * this counted it as unmatched, which made a healthy channel look faulty.
			 */
			sc->late++;
		} else if (sc->waiting && tag == sc->wait_tag) {
			int pos = (d[AGNIC_CMD_FLAGS] >> AGNIC_CMD_F_BUF_POS_SHIFT) &
			    AGNIC_CMD_F_BUF_POS_MASK;

			if (sc->answer_len + AGNIC_MGMT_DESC_DATA_LEN <= (int)sizeof(sc->answer)) {
				memcpy(sc->answer + sc->answer_len, d + AGNIC_CMD_DATA,
				    AGNIC_MGMT_DESC_DATA_LEN);
				sc->answer_len += AGNIC_MGMT_DESC_DATA_LEN;
			} else {
				sc->overruns++;
			}

			/*
			 * Only SINGLE and LAST end a run. Treating every descriptor as a complete
			 * answer is what made a two-part response look like one answer plus one
			 * late duplicate - the first version of this counted exactly one spurious
			 * late arrival per command, which is what led here.
			 */
			if (pos == AGNIC_BUF_POS_SINGLE || pos == AGNIC_BUF_POS_LAST) {
				sc->answered = 1;
				sc->waiting = 0;
				sc->answers++;
				if (pos == AGNIC_BUF_POS_LAST)
					sc->multipart++;
				wakeup(&sc->answered);
			}
		} else {
			/* A tag we are not waiting on. Consume it; never index anything with it. */
			sc->drops++;
		}

		/*
		 * Advance past the descriptor only after everything has been read out of it.
		 * The moment the consumer index moves, the device owns the slot again.
		 */
		r->shadow = ring_next(r, r->shadow);
		idx_publish(sc, r->cons_slot, r->shadow);
	}
}

/*
 * ---------------------------------------------------------------------------------------
 * Packets.
 *
 * Every frame on this link carries sixty-six bytes in front of its Ethernet header, so a buffer
 * holds the frame plus that and every copy in either direction has to account for it.
 * ---------------------------------------------------------------------------------------
 */

/*
 * Transmit one frame. Called with the lock held.
 *
 * The descriptor is filled completely, then a release barrier, then the index - and that ordering
 * is the only thing between this and the device fetching a descriptor the host has not finished
 * writing. The vendor's driver has no barrier here at all; it relies on the one Linux hides
 * inside writel(), which is not a guarantee this code can borrow.
 */
static int
npugiu_encap(struct npugiu_softc *sc, struct mbuf *m)
{
	struct npugiu_ring *r = &sc->tx;
	uint32_t cons, push;
	uint8_t *d, *b;
	int len = m->m_pkthdr.len;

	mtx_assert(&sc->mtx, MA_OWNED);

	if (len <= 0 || len + NPUGIU_HDR_LEN > NPUGIU_BUF_SIZE) {
		sc->tx_toolong++;
		return (EMSGSIZE);
	}
	if (!idx_remote(sc, r->cons_slot, r->len, &cons))
		return (EIO);

	push = r->shadow;
	if (ring_next(r, push) == cons) {
		sc->tx_full++;
		return (ENOBUFS);
	}

	b = sc->txbuf[push].vaddr;
	/* the port identifier, network order, in front of everything */
	b[0] = (uint8_t)((sc->out_port >> 8) & 0xFF);
	b[1] = (uint8_t)(sc->out_port & 0xFF);
	/*
	 * The metadata area. The vendor fills it with a descending ramp where it has nothing to
	 * put; zeros are the honest equivalent and do not pretend to carry information.
	 */
	memset(b + NPUGIU_TAG_LEN, 0, NPUGIU_META_LEN);
	m_copydata(m, 0, len, (caddr_t)(b + NPUGIU_HDR_LEN));

	d = desc_at(r, push);
	memset(d, 0, AGNIC_TXD_SIZE);
	le32enc(d + AGNIC_TXD_FLAGS, AGNIC_TXD_F_MD_MODE | AGNIC_TXD_F_GEN_L4_CSUM_NOT |
	    AGNIC_TXD_F_GEN_IPV4_CSUM_DIS);
	d[AGNIC_TXD_PKT_OFFSET] = 0;
	le16enc(d + AGNIC_TXD_BYTE_CNT, (uint16_t)(len + NPUGIU_HDR_LEN));
	le64enc(d + AGNIC_TXD_BUFFER_ADDR, (uint64_t)sc->txbuf[push].paddr);
	le64enc(d + AGNIC_TXD_COOKIE, (uint64_t)push);	/* an index, never a pointer */

	r->shadow = ring_next(r, push);
	idx_publish(sc, r->prod_slot, r->shadow);

	sc->tx_packets++;
	sc->tx_bytes += len;
	return (0);
}

static void
npugiu_start_locked(struct npugiu_softc *sc)
{
	if_t ifp = sc->ifp;
	struct mbuf *m;

	mtx_assert(&sc->mtx, MA_OWNED);
	if (!sc->datapath || ifp == NULL)
		return;
	if ((if_getdrvflags(ifp) & IFF_DRV_RUNNING) == 0)
		return;

	while (!if_sendq_empty(ifp)) {
		m = if_dequeue(ifp);
		if (m == NULL)
			break;
		if (npugiu_encap(sc, m) != 0) {
			if_sendq_prepend(ifp, m);
			if_setdrvflagbits(ifp, IFF_DRV_OACTIVE, 0);
			break;
		}
		bpf_mtap_if(ifp, m);
		m_freem(m);
		if_inc_counter(ifp, IFCOUNTER_OPACKETS, 1);
	}
}

static void
npugiu_start(if_t ifp)
{
	struct npugiu_softc *sc = if_getsoftc(ifp);

	GIU_LOCK(sc);
	npugiu_start_locked(sc);
	GIU_UNLOCK(sc);
}

/*
 * Give a buffer back to the pool. The pool is how the device gets memory to receive into, so a
 * buffer that is not returned is a receive slot lost for the life of the link.
 */
static void
npugiu_bpool_return(struct npugiu_softc *sc, uint32_t bufidx)
{
	struct npugiu_ring *r = &sc->bp;
	uint32_t cons, push;
	uint8_t *d;

	mtx_assert(&sc->mtx, MA_OWNED);

	if (bufidx >= (uint32_t)sc->nbuf)
		return;
	if (!idx_remote(sc, r->cons_slot, r->len, &cons))
		return;
	push = r->shadow;
	if (ring_next(r, push) == cons)
		return;		/* the pool is full; nothing is lost, the buffer stays ours */

	d = desc_at(r, push);
	le64enc(d + AGNIC_BPD_BUFF_ADDR_PHYS, (uint64_t)sc->buf[bufidx].paddr);
	le64enc(d + AGNIC_BPD_BUFF_COOKIE, (uint64_t)bufidx);

	r->shadow = ring_next(r, push);
	idx_publish(sc, r->prod_slot, r->shadow);
}

/*
 * Receive. Builds a chain for the caller to push up with the lock dropped.
 *
 * There is no ownership bit anywhere in this wire format, so whether a descriptor has been filled
 * is decided by a cookie watermark the vendor had to add after the fact. That is the only check
 * there is, which is why the ring is stamped with it before it is ever published.
 */
static int
npugiu_rx(struct npugiu_softc *sc, int budget, struct mbuf **head, struct mbuf **tail)
{
	struct npugiu_ring *r = &sc->rx;
	if_t ifp = sc->ifp;
	uint32_t prod;
	int done = 0;

	mtx_assert(&sc->mtx, MA_OWNED);

	if (!idx_remote(sc, r->prod_slot, r->len, &prod))
		return (0);

	while (done < budget && r->shadow != prod) {
		uint8_t *d = desc_at(r, r->shadow);
		uint64_t cookie;
		uint16_t total;
		uint32_t bufidx;
		struct mbuf *m;
		const uint8_t *src;
		uint16_t port;
		int len, n;

		/*
		 * Acquire against the producer index just read: the descriptor contents must not
		 * be loaded before the index that says they are there.
		 */
		atomic_thread_fence_acq();
		cookie = le64dec(d + AGNIC_RXD_COOKIE);
		total = le16dec(d + AGNIC_RXD_BYTE_CNT);
		bufidx = (uint32_t)cookie;

		/*
		 * A descriptor the device has not really filled still holds the watermark this
		 * driver stamped into it. Either that or a cookie outside the buffer array means
		 * the slot is not ours to read.
		 */
		if (cookie == AGNIC_COOKIE_DRIVER_WATERMARK || bufidx >= (uint32_t)sc->nbuf) {
			sc->rx_bad++;
			goto next;
		}
		if (total < NPUGIU_HDR_LEN + ETHER_HDR_LEN || total > NPUGIU_BUF_SIZE) {
			sc->rx_bad++;
			npugiu_bpool_return(sc, bufidx);
			goto next;
		}

		src = sc->buf[bufidx].vaddr;
		port = ((uint16_t)src[0] << 8) | src[1];
		len = total - NPUGIU_HDR_LEN;

		m = m_getcl(M_NOWAIT, MT_DATA, M_PKTHDR);
		if (m == NULL) {
			sc->rx_nobuf++;
			npugiu_bpool_return(sc, bufidx);
			break;
		}
		memcpy(mtod(m, void *), src + NPUGIU_HDR_LEN, len);
		m->m_pkthdr.len = m->m_len = len;
		m->m_pkthdr.rcvif = ifp;
		m->m_nextpkt = NULL;

		if (*head == NULL)
			*head = m;
		else
			(*tail)->m_nextpkt = m;
		*tail = m;

		sc->rx_packets++;
		sc->rx_bytes += len;
		n = NPUGIU_PORT_NUM(port);
		if (n < NPUGIU_MAX_PORTS)
			sc->rx_port[n]++;
		if (ifp != NULL)
			if_inc_counter(ifp, IFCOUNTER_IPACKETS, 1);

		/* Stamp the slot back before releasing it, so the next round can tell. */
		le64enc(d + AGNIC_RXD_COOKIE, AGNIC_COOKIE_DRIVER_WATERMARK);
		npugiu_bpool_return(sc, bufidx);
next:
		atomic_thread_fence_rel();
		r->shadow = ring_next(r, r->shadow);
		idx_publish(sc, r->cons_slot, r->shadow);
		done++;
	}
	return (done);
}

static void
npugiu_init_locked(struct npugiu_softc *sc)
{
	mtx_assert(&sc->mtx, MA_OWNED);

	if (!sc->datapath || sc->ifp == NULL)
		return;
	if ((if_getdrvflags(sc->ifp) & IFF_DRV_RUNNING) != 0)
		return;

	if_setdrvflagbits(sc->ifp, IFF_DRV_RUNNING, IFF_DRV_OACTIVE);
	if_link_state_change(sc->ifp, LINK_STATE_UP);
}

/*
 * if_init, and it is not optional.
 *
 * The stack calls this through a bare function pointer and does not check it first:
 * in6_update_ifa() goes straight through it when an address is added to an interface that is up
 * but not running. An ifnet that never had one set is a jump to address zero, and this project
 * has already taken a machine down that way once.
 */
static void
npugiu_ifinit(void *xsc)
{
	struct npugiu_softc *sc = xsc;

	GIU_LOCK(sc);
	npugiu_init_locked(sc);
	GIU_UNLOCK(sc);
}

static int
npugiu_ioctl(if_t ifp, u_long cmd, caddr_t data)
{
	struct npugiu_softc *sc = if_getsoftc(ifp);
	struct ifreq *ifr = (struct ifreq *)data;
	int err = 0;

	switch (cmd) {
	case SIOCSIFFLAGS:
		GIU_LOCK(sc);
		if ((if_getflags(ifp) & IFF_UP) != 0)
			npugiu_init_locked(sc);
		GIU_UNLOCK(sc);
		break;
	case SIOCSIFMTU:
		/* The buffers were sized once, for this MTU plus the sixty-six byte header. */
		if (ifr->ifr_mtu != NPUGIU_MTU)
			err = EINVAL;
		break;
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		/* Filtering is the switch's, not ours; nothing to program here. */
		break;
	default:
		err = ether_ioctl(ifp, cmd, data);
		break;
	}
	return (err);
}

static int
npugiu_attach_ifnet(struct npugiu_softc *sc)
{

	sc->ifp = if_alloc(IFT_ETHER);
	if_setsoftc(sc->ifp, sc);
	if_initname(sc->ifp, "npugiu", 0);
	if_setflags(sc->ifp, IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST);
	if_setinitfn(sc->ifp, npugiu_ifinit);
	if_setstartfn(sc->ifp, npugiu_start);
	if_setioctlfn(sc->ifp, npugiu_ioctl);
	if_setsendqlen(sc->ifp, NPUGIU_DATA_Q_LEN - 1);
	if_setsendqready(sc->ifp);
	if_setmtu(sc->ifp, NPUGIU_MTU);

	ether_ifattach(sc->ifp, sc->hostmac);
	return (0);
}

static void
npugiu_tick(void *arg)
{
	struct npugiu_softc *sc = arg;
	struct mbuf *head = NULL, *tail = NULL;
	if_t ifp;

	mtx_assert(&sc->mtx, MA_OWNED);
	if (!sc->running)
		return;

	npugiu_drain(sc);

	if (sc->datapath) {
		(void)npugiu_rx(sc, 64, &head, &tail);
		if (sc->ifp != NULL) {
			if_setdrvflagbits(sc->ifp, 0, IFF_DRV_OACTIVE);
			npugiu_start_locked(sc);
		}
	}

	if (sc->running)
		callout_reset(&sc->poll, hz / 100, npugiu_tick, sc);

	/*
	 * Hand the frames up with the lock dropped, and only after the next tick is armed so that
	 * nothing below can lose the timer. Calling into the stack while holding a driver mutex is
	 * how drivers deadlock. We return with the lock held, which is what a callout started by
	 * callout_init_mtx requires.
	 */
	if (head != NULL) {
		ifp = sc->ifp;
		GIU_UNLOCK(sc);
		while (head != NULL) {
			struct mbuf *m = head;

			head = m->m_nextpkt;
			m->m_nextpkt = NULL;
			if (ifp != NULL)
				if_input(ifp, m);
			else
				m_freem(m);
		}
		GIU_LOCK(sc);
	}
}

/* Post a command and wait for its answer. Called with the lock held. */
static int
npugiu_command(struct npugiu_softc *sc, uint8_t code, const void *params, size_t plen)
{
	int err, i;

	err = npugiu_post(sc, code, params, plen, 1);
	if (err != 0)
		return (err);

	for (i = 0; i < NPUGIU_CMD_WAIT; i++) {
		if (sc->answered)
			return (0);
		if (!sc->running)
			return (ENXIO);
		msleep(&sc->answered, &sc->mtx, 0, "npugiu", hz / 100);
	}
	sc->waiting = 0;
	return (ETIMEDOUT);
}

/*
 * ---------------------------------------------------------------------------------------
 * Bring-up.
 * ---------------------------------------------------------------------------------------
 */

/*
 * ---------------------------------------------------------------------------------------
 * The datapath: buffers, and the bring-up sequence that hands it all to the coprocessor.
 * ---------------------------------------------------------------------------------------
 */
static void
npugiu_free_buffers(struct npugiu_softc *sc)
{
	int i;

	if (sc->buf != NULL) {
		for (i = 0; i < sc->nbuf; i++) {
			if (sc->buf[i].vaddr == NULL)
				continue;
			bus_dmamap_unload(sc->buf_tag, sc->buf[i].map);
			bus_dmamem_free(sc->buf_tag, sc->buf[i].vaddr, sc->buf[i].map);
			sc->buf[i].vaddr = NULL;
		}
		free(sc->buf, M_DEVBUF);
		sc->buf = NULL;
	}
	if (sc->txbuf != NULL) {
		for (i = 0; i < sc->nbuf; i++) {
			if (sc->txbuf[i].vaddr == NULL)
				continue;
			bus_dmamap_unload(sc->buf_tag, sc->txbuf[i].map);
			bus_dmamem_free(sc->buf_tag, sc->txbuf[i].vaddr, sc->txbuf[i].map);
			sc->txbuf[i].vaddr = NULL;
		}
		free(sc->txbuf, M_DEVBUF);
		sc->txbuf = NULL;
	}
	if (sc->buf_tag != NULL) {
		bus_dma_tag_destroy(sc->buf_tag);
		sc->buf_tag = NULL;
	}
	sc->nbuf = 0;
}

static int
npugiu_alloc_buffers(struct npugiu_softc *sc, int n)
{
	int i, err;

	err = bus_dma_tag_create(sc->fac.parent_tag, 64, 0, NPUGIU_DMA_LOWADDR,
	    BUS_SPACE_MAXADDR, NULL, NULL, NPUGIU_BUF_SIZE, 1, NPUGIU_BUF_SIZE, 0,
	    NULL, NULL, &sc->buf_tag);
	if (err != 0)
		return (err);

	sc->buf = malloc(sizeof(*sc->buf) * n, M_DEVBUF, M_WAITOK | M_ZERO);
	sc->nbuf = n;

	for (i = 0; i < n; i++) {
		bus_addr_t pa = 0;

		err = bus_dmamem_alloc(sc->buf_tag, &sc->buf[i].vaddr,
		    BUS_DMA_WAITOK | BUS_DMA_ZERO | BUS_DMA_COHERENT, &sc->buf[i].map);
		if (err != 0)
			return (err);
		err = bus_dmamap_load(sc->buf_tag, sc->buf[i].map, sc->buf[i].vaddr,
		    NPUGIU_BUF_SIZE, npugiu_dmamap_cb, &pa, BUS_DMA_NOWAIT);
		if (err != 0 || pa == 0)
			return (err != 0 ? err : ENOMEM);
		sc->buf[i].paddr = pa;
	}
	return (0);
}

static int
npugiu_alloc_txbuffers(struct npugiu_softc *sc, int n)
{
	int i, err;

	sc->txbuf = malloc(sizeof(*sc->txbuf) * n, M_DEVBUF, M_WAITOK | M_ZERO);

	for (i = 0; i < n; i++) {
		bus_addr_t pa = 0;

		err = bus_dmamem_alloc(sc->buf_tag, &sc->txbuf[i].vaddr,
		    BUS_DMA_WAITOK | BUS_DMA_ZERO | BUS_DMA_COHERENT, &sc->txbuf[i].map);
		if (err != 0)
			return (err);
		err = bus_dmamap_load(sc->buf_tag, sc->txbuf[i].map, sc->txbuf[i].vaddr,
		    NPUGIU_BUF_SIZE, npugiu_dmamap_cb, &pa, BUS_DMA_NOWAIT);
		if (err != 0 || pa == 0)
			return (err != 0 ? err : ENOMEM);
		sc->txbuf[i].paddr = pa;
	}
	return (0);
}

/*
 * Fill the buffer pool, and publish the producer index LAST.
 *
 * The vendor hands the pool's address to the device while it is still empty and fills it
 * afterwards; that works only because the device has not been enabled yet. Filling before the
 * enable removes the window entirely. The one slot left between producer and consumer is what
 * keeps full and empty distinguishable - the same rule as every other ring here.
 */
static void
npugiu_fill_bpool(struct npugiu_softc *sc)
{
	struct npugiu_ring *r = &sc->bp;
	int i, fill = r->len - 1;

	mtx_assert(&sc->mtx, MA_OWNED);

	if (fill > sc->nbuf)
		fill = sc->nbuf;

	for (i = 0; i < fill; i++) {
		uint8_t *d = desc_at(r, (uint32_t)i);

		le64enc(d + AGNIC_BPD_BUFF_ADDR_PHYS, (uint64_t)sc->buf[i].paddr);
		/*
		 * The cookie is ours and comes back untouched. The vendor puts a kernel virtual
		 * pointer here, so any corruption on the far side becomes an arbitrary
		 * dereference. An index cannot do that.
		 */
		le64enc(d + AGNIC_BPD_BUFF_COOKIE, (uint64_t)i);
	}

	idx_wr(sc, r->cons_slot, 0);
	r->shadow = (uint32_t)fill;
	idx_publish(sc, r->prod_slot, r->shadow);

	device_printf(sc->fac.dev, "giu: buffer pool filled with %d of %d x %d B\n",
	    fill, r->len, NPUGIU_BUF_SIZE);
}

/*
 * The bring-up sequence, in the order the code requires rather than the order the header
 * suggests. Every step waits for its answer, because a queue the device did not accept is a
 * queue it will not read - and the vendor ignores these returns.
 */
static int
npugiu_bringup(struct npugiu_softc *sc)
{
	uint8_t p[AGNIC_MGMT_DESC_DATA_LEN];
	int err;

	mtx_assert(&sc->mtx, MA_OWNED);

#define	STEP(code, len, what)						\
	do {								\
		err = npugiu_command(sc, (code), p, (len));		\
		if (err != 0) {						\
			device_printf(sc->fac.dev,			\
			    "giu: %s failed (%d)\n", (what), err);	\
			return (err);					\
		}							\
	} while (0)

/* The same, for a step whose failure is worth reporting but not worth refusing the link over. */
#define	SOFT_STEP(code, len, what)					\
	do {								\
		int serr = npugiu_command(sc, (code), p, (len));	\
		if (serr != 0)						\
			device_printf(sc->fac.dev,			\
			    "giu: %s failed (%d) - carrying on\n",	\
			    (what), serr);				\
	} while (0)

	/* 1. how many traffic classes, and how the egress scheduler behaves */
	memset(p, 0, sizeof(p));
	le32enc(p + AGNIC_P_INIT_NUM_EGRESS_TC, 1);
	le32enc(p + AGNIC_P_INIT_NUM_INGRESS_TC, 1);
	le16enc(p + AGNIC_P_INIT_MTU_OVERRIDE, NPUGIU_MTU);
	le16enc(p + AGNIC_P_INIT_MRU_OVERRIDE, NPUGIU_MTU);
	p[AGNIC_P_INIT_EGRESS_SCHED] = AGNIC_ES_STRICT_SCHED;
	STEP(AGNIC_CC_PF_INIT, 0x10, "PF_INIT");

	/* 2. the ingress class */
	memset(p, 0, sizeof(p));
	le32enc(p + AGNIC_P_ITC_TC, 0);
	le32enc(p + AGNIC_P_ITC_NUM_QUEUES, 1);
	le32enc(p + AGNIC_P_ITC_PKT_OFFSET, 0);
	p[AGNIC_P_ITC_HASH_TYPE] = AGNIC_ING_HASH_NONE;
	STEP(AGNIC_CC_PF_INGRESS_TC_ADD, 0x10, "INGRESS_TC_ADD");

	/* 3. the receive queue, which carries its buffer pool with it */
	memset(p, 0, sizeof(p));
	le64enc(p + AGNIC_P_IQ_PHYS_ADDR, (uint64_t)sc->rx.phys);
	le32enc(p + AGNIC_P_IQ_PROD_OFFS, sc->rx.prod_slot);
	le32enc(p + AGNIC_P_IQ_CONS_OFFS, sc->rx.cons_slot);
	le64enc(p + AGNIC_P_IQ_BPOOL_PHYS_ADDR, (uint64_t)sc->bp.phys);
	le32enc(p + AGNIC_P_IQ_BPOOL_PROD_OFFS, sc->bp.prod_slot);
	le32enc(p + AGNIC_P_IQ_BPOOL_CONS_OFFS, sc->bp.cons_slot);
	le32enc(p + AGNIC_P_IQ_LEN, (uint32_t)sc->rx.len);
	le32enc(p + AGNIC_P_IQ_MSIX_ID, (uint32_t)sc->fac.first_msix);
	le32enc(p + AGNIC_P_IQ_TC, 0);
	le32enc(p + AGNIC_P_IQ_BUF_SIZE, NPUGIU_BUF_SIZE);
	STEP(AGNIC_CC_PF_INGRESS_DATA_Q_ADD, 0x30, "INGRESS_DATA_Q_ADD");

	/* 4. the egress class */
	memset(p, 0, sizeof(p));
	le32enc(p + AGNIC_P_ETC_TC, 0);
	le32enc(p + AGNIC_P_ETC_NUM_QUEUES, 1);
	le32enc(p + AGNIC_P_ETC_NUM_Q_PER_DMA, 1);
	STEP(AGNIC_CC_PF_EGRESS_TC_ADD, 0x0c, "EGRESS_TC_ADD");

	/* 5. the transmit queue */
	memset(p, 0, sizeof(p));
	le64enc(p + AGNIC_P_EQ_PHYS_ADDR, (uint64_t)sc->tx.phys);
	le32enc(p + AGNIC_P_EQ_PROD_OFFS, sc->tx.prod_slot);
	le32enc(p + AGNIC_P_EQ_CONS_OFFS, sc->tx.cons_slot);
	le32enc(p + AGNIC_P_EQ_LEN, (uint32_t)sc->tx.len);
	le32enc(p + AGNIC_P_EQ_WRR_WEIGHT, 1);
	le32enc(p + AGNIC_P_EQ_TC, 0);
	le32enc(p + AGNIC_P_EQ_MSIX_ID, (uint32_t)(sc->fac.first_msix + 1));
	STEP(AGNIC_CC_PF_EGRESS_DATA_Q_ADD, 0x20, "EGRESS_DATA_Q_ADD");

	/* 6. and that is the configuration */
	memset(p, 0, sizeof(p));
	STEP(AGNIC_CC_PF_INIT_DONE, 0, "INIT_DONE");

	/* 7. buffers before the enable, never after */
	npugiu_fill_bpool(sc);

	memset(p, 0, sizeof(p));
	/*
	 * 7. the receive filter.
	 *
	 * Without this the link transmits and never receives, which is not obvious from either
	 * end: every command is answered, the rings are consumed, and nothing arrives. None of
	 * these three is allowed to abort the bring-up - a datapath that transmits is worth
	 * keeping and worth being told about, and the failure is named where it happens.
	 */
	memset(p, 0, sizeof(p));
	memcpy(p + AGNIC_P_MAC_ADDR, sc->hostmac, AGNIC_P_MAC_ADDR_LEN);
	SOFT_STEP(AGNIC_CC_PF_MAC_ADDR, AGNIC_P_MAC_ADDR_LEN, "PF_MAC_ADDR");

	/*
	 * Promiscuous, and not as a debugging convenience. This end is a trunk: frames for all
	 * fourteen front ports arrive on it, addressed to whatever the hosts behind those ports
	 * are. There is no single address that would let the right ones through.
	 */
	memset(p, 0, sizeof(p));
	p[AGNIC_P_PROMISC] = AGNIC_PROMISC_ENABLE;
	SOFT_STEP(AGNIC_CC_PF_PROMISC, AGNIC_P_PROMISC_LEN, "PF_PROMISC");

	memset(p, 0, sizeof(p));
	p[AGNIC_P_PROMISC] = AGNIC_PROMISC_ENABLE;
	SOFT_STEP(AGNIC_CC_PF_MC_PROMISC, AGNIC_P_PROMISC_LEN, "PF_MC_PROMISC");

	STEP(AGNIC_CC_PF_ENABLE, 0, "PF_ENABLE");

#undef SOFT_STEP
#undef STEP
	sc->datapath = 1;
	return (0);
}

static int
npugiu_wait_status(struct npugiu_softc *sc, uint32_t bit, int ticks, const char *what)
{
	uint32_t st;
	int i;

	for (i = 0; i < ticks; i++) {
		st = cfg_rd(sc, AGNIC_CFG_STATUS);
		if (st == 0xFFFFFFFFU) {
			device_printf(sc->fac.dev, "giu: endpoint not decoding\n");
			return (ENXIO);
		}
		if ((st & bit) != 0)
			return (0);
		pause("npugiu", hz / 100);
	}
	device_printf(sc->fac.dev, "giu: timed out waiting for %s (status 0x%08x)\n",
	    what, cfg_rd(sc, AGNIC_CFG_STATUS));
	return (ETIMEDOUT);
}

/*
 * The counters. This is the whole verification surface for the datapath: what went out, what came
 * in, which physical port it came in on, and every way a frame was refused.
 */
static void
npugiu_add_sysctls(struct npugiu_softc *sc)
{
	struct sysctl_ctx_list *ctx = device_get_sysctl_ctx(sc->fac.dev);
	struct sysctl_oid *tree = device_get_sysctl_tree(sc->fac.dev);
	struct sysctl_oid_list *child = SYSCTL_CHILDREN(tree);
	struct sysctl_oid *node;
	int i;

	node = SYSCTL_ADD_NODE(ctx, child, OID_AUTO, "giu", CTLFLAG_RD | CTLFLAG_MPSAFE, NULL,
	    "the packet coprocessor's datapath");
	if (node == NULL)
		return;
	child = SYSCTL_CHILDREN(node);

	SYSCTL_ADD_U32(ctx, child, OID_AUTO, "out_port", CTLFLAG_RW, &sc->out_port, 0,
	    "port identifier written in front of every transmitted frame");

	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "rx_packets", CTLFLAG_RD, &sc->rx_packets, 0,
	    "frames received");
	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "rx_bytes", CTLFLAG_RD, &sc->rx_bytes, 0,
	    "bytes received, not counting the sixty-six byte header");
	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "rx_bad", CTLFLAG_RD, &sc->rx_bad, 0,
	    "descriptors refused: unstamped, or a length that cannot be a frame");
	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "rx_nobuf", CTLFLAG_RD, &sc->rx_nobuf, 0,
	    "frames dropped because no mbuf cluster was available");
	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "rx_dropped", CTLFLAG_RD, &sc->rx_dropped, 0,
	    "frames dropped after being read out of the pool");

	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "tx_packets", CTLFLAG_RD, &sc->tx_packets, 0,
	    "frames transmitted");
	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "tx_bytes", CTLFLAG_RD, &sc->tx_bytes, 0,
	    "bytes transmitted, not counting the sixty-six byte header");
	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "tx_full", CTLFLAG_RD, &sc->tx_full, 0,
	    "transmits deferred because the ring was full");
	SYSCTL_ADD_U64(ctx, child, OID_AUTO, "tx_toolong", CTLFLAG_RD, &sc->tx_toolong, 0,
	    "frames refused because they did not fit a buffer");

	node = SYSCTL_ADD_NODE(ctx, child, OID_AUTO, "rx_port", CTLFLAG_RD | CTLFLAG_MPSAFE, NULL,
	    "frames received, counted by the port identifier they arrived with");
	if (node == NULL)
		return;
	child = SYSCTL_CHILDREN(node);
	for (i = 0; i < NPUGIU_MAX_PORTS; i++) {
		char name[8];

		snprintf(name, sizeof(name), "%d", i);
		SYSCTL_ADD_U64(ctx, child, OID_AUTO, name, CTLFLAG_RD, &sc->rx_port[i], 0,
		    "frames received on this port");
	}
}

int
npugiu_attach(struct npuep_facility *fac)
{
	struct npugiu_softc *sc;
	uint32_t duse, need, lo, hi;
	int err;

	if (npugiu_sc != NULL)
		return (EBUSY);
	if (fac->size < AGNIC_CFG_SIZE) {
		device_printf(fac->dev, "giu: window is only %ju bytes\n", (uintmax_t)fac->size);
		return (ENXIO);
	}

	sc = malloc(sizeof(*sc), M_DEVBUF, M_WAITOK | M_ZERO);
	sc->fac = *fac;
	mtx_init(&sc->mtx, "npugiu", NULL, MTX_DEF);
	callout_init_mtx(&sc->poll, &sc->mtx, 0);

	/* Nothing else in the window may be read until the device says it is ready. */
	err = npugiu_wait_status(sc, AGNIC_CFG_STATUS_DEV_READY, NPUGIU_DEV_READY_WAIT,
	    "DEV_READY");
	if (err != 0)
		goto fail;

	lo = cfg_rd(sc, AGNIC_CFG_MAC_ADDR);
	hi = cfg_rd(sc, AGNIC_CFG_MAC_ADDR + 4);
	sc->mac[0] = lo & 0xFF; sc->mac[1] = (lo >> 8) & 0xFF;
	sc->mac[2] = (lo >> 16) & 0xFF; sc->mac[3] = (lo >> 24) & 0xFF;
	sc->mac[4] = hi & 0xFF; sc->mac[5] = (hi >> 8) & 0xFF;
	sc->msix_tbl_off = cfg_rd(sc, AGNIC_CFG_MSI_X_TBL_OFFSET);

	/*
	 * dev_use_size is a device-supplied u32 that decides where the index array lands. The
	 * vendor bounds it against a constant that is not this window's size, so a device
	 * reporting a large value puts the array on top of the MSI-X table and the check passes.
	 * Bound it against the window we were actually given, and against itself.
	 */
	duse = cfg_rd(sc, AGNIC_CFG_DEV_USE_SIZE);
	sc->idx_count = AGNIC_MAX_QUEUES;
	need = sc->idx_count * 4;
	if (duse < AGNIC_CFG_SIZE || duse > fac->size || need > fac->size - duse ||
	    (duse & 3) != 0) {
		device_printf(fac->dev,
		    "giu: dev_use_size %#x will not hold %u index words inside a %ju byte "
		    "window - refusing\n", duse, sc->idx_count, (uintmax_t)fac->size);
		err = ERANGE;
		goto fail;
	}
	sc->idx_base = sc->fac.off + duse;

	device_printf(fac->dev,
	    "giu: device ready, mac %02x:%02x:%02x:%02x:%02x:%02x, "
	    "indices at BAR+%#x, msix table at BAR+%#x\n",
	    sc->mac[0], sc->mac[1], sc->mac[2], sc->mac[3], sc->mac[4], sc->mac[5],
	    (unsigned)sc->idx_base, sc->msix_tbl_off);

	npugiu_init_slots(sc);

	err = npugiu_alloc_ring(sc, &sc->cmd, NPUGIU_CMD_Q_LEN, AGNIC_CMD_DESC_SIZE, "command");
	if (err != 0)
		goto fail;
	err = npugiu_alloc_ring(sc, &sc->notif, NPUGIU_NOTIF_Q_LEN, AGNIC_CMD_DESC_SIZE, "notification");
	if (err != 0)
		goto fail;

	/*
	 * Publish both rings, then the barrier, then the status bit that says they are valid.
	 * That last write is the device's signal; anything still stale in those fields when it
	 * lands is what it will follow.
	 */
	npugiu_publish_ring(sc, AGNIC_CFG_CMD_Q, &sc->cmd);
	npugiu_publish_ring(sc, AGNIC_CFG_NOTIF_Q, &sc->notif);
	atomic_thread_fence_rel();
	bus_barrier(sc->fac.res, sc->fac.off, AGNIC_CFG_SIZE, BUS_SPACE_BARRIER_WRITE);

	cfg_wr(sc, AGNIC_CFG_STATUS,
	    cfg_rd(sc, AGNIC_CFG_STATUS) | AGNIC_CFG_STATUS_HOST_MGMT_READY);

	err = npugiu_wait_status(sc, AGNIC_CFG_STATUS_DEV_MGMT_READY, NPUGIU_MGMT_READY_WAIT,
	    "DEV_MGMT_READY");
	if (err != 0)
		goto fail;

	npugiu_sc = sc;

	GIU_LOCK(sc);
	sc->running = 1;
	callout_reset(&sc->poll, hz / 100, npugiu_tick, sc);

	/*
	 * The smallest thing that proves the channel: an echo. It must come after the poller is
	 * running, because the poller is the only thing that completes a command - there is no
	 * interrupt on this channel at all.
	 */
	err = npugiu_command(sc, AGNIC_CC_PF_MGMT_ECHO, NULL, 0);
	GIU_UNLOCK(sc);

	if (err != 0) {
		device_printf(fac->dev, "giu: MGMT_ECHO did not come back (%d)\n", err);
		npugiu_detach();
		return (err);
	}

	device_printf(fac->dev, "giu: MGMT_ECHO answered - the command channel is up\n");

	/*
	 * With the channel proved, hand over the datapath. Its failure leaves the command channel
	 * running, which is worth keeping: it is what a later attempt would need anyway, and a
	 * machine that keeps it is easier to work on than one that tears everything down.
	 */
	GIU_LOCK(sc);
	err = npugiu_alloc_ring(sc, &sc->tx, NPUGIU_DATA_Q_LEN, AGNIC_TXD_SIZE, "transmit");
	if (err == 0)
		err = npugiu_alloc_ring(sc, &sc->rx, NPUGIU_DATA_Q_LEN, AGNIC_RXD_SIZE, "receive");
	if (err == 0)
		err = npugiu_alloc_ring(sc, &sc->bp, NPUGIU_DATA_Q_LEN, AGNIC_BPD_SIZE,
		    "buffer pool");
	if (err == 0)
		err = npugiu_alloc_buffers(sc, NPUGIU_DATA_Q_LEN);
	if (err == 0)
		err = npugiu_alloc_txbuffers(sc, NPUGIU_DATA_Q_LEN);
	if (err == 0) {
		int i;

		/*
		 * The address this end answers to. The coprocessor advertises the trunk's own,
		 * and taking it unchanged would put two interfaces with one address on the same
		 * link, so the host end uses a locally administered variant of it. It is settled
		 * here rather than at ether_ifattach() because the bring-up has to tell the
		 * coprocessor the same address the interface will carry.
		 */
		memcpy(sc->hostmac, sc->mac, sizeof(sc->hostmac));
		sc->hostmac[0] = (uint8_t)((sc->hostmac[0] | 0x02) & ~0x01);

		/*
		 * Stamp the receive ring before it is published. There is no ownership bit in
		 * this wire format, so this watermark is the only way to tell a descriptor the
		 * device has filled from one nobody has touched yet. The vendor added it after
		 * the fact; here it goes in before the ring is ever handed over.
		 */
		for (i = 0; i < sc->rx.len; i++)
			le64enc(desc_at(&sc->rx, (uint32_t)i) + AGNIC_RXD_COOKIE,
			    AGNIC_COOKIE_DRIVER_WATERMARK);
		sc->out_port = NPUGIU_DEFAULT_PORT;
		err = npugiu_bringup(sc);
	}
	if (err == 0)
		err = npugiu_attach_ifnet(sc);
	GIU_UNLOCK(sc);

	if (err != 0)
		device_printf(fac->dev,
		    "giu: the datapath did not come up (%d) - the command channel is still "
		    "running\n", err);
	else {
		npugiu_add_sysctls(sc);
		device_printf(fac->dev,
		    "giu: datapath enabled - %s, %d descriptors each way, frames tagged for "
		    "port 0x%04x\n", sc->ifp != NULL ? if_name(sc->ifp) : "no interface",
		    NPUGIU_DATA_Q_LEN, sc->out_port);
	}

	return (0);

fail:
	npugiu_free_buffers(sc);
	npugiu_free_ring(sc, &sc->bp);
	npugiu_free_ring(sc, &sc->rx);
	npugiu_free_ring(sc, &sc->tx);
	npugiu_free_ring(sc, &sc->notif);
	npugiu_free_ring(sc, &sc->cmd);
	callout_drain(&sc->poll);
	mtx_destroy(&sc->mtx);
	free(sc, M_DEVBUF);
	return (err);
}

void
npugiu_detach(void)
{
	struct npugiu_softc *sc = npugiu_sc;
	uint32_t st;

	if (sc == NULL)
		return;

	GIU_LOCK(sc);
	sc->running = 0;
	sc->datapath = 0;
	wakeup(&sc->answered);
	GIU_UNLOCK(sc);
	callout_drain(&sc->poll);

	/*
	 * Detach the interface before anything it points at is freed. Nothing can be queued to it
	 * afterwards, which is what makes the rest of this safe.
	 */
	if (sc->ifp != NULL) {
		ether_ifdetach(sc->ifp);
		if_free(sc->ifp);
		sc->ifp = NULL;
	}

	/*
	 * Tell the device before taking the memory back, which the vendor's driver never does -
	 * it frees the rings it handed over and leaves HOST_MGMT_READY set. Clear the bit, zero
	 * the addresses, and only then free. If the endpoint has stopped decoding we cannot say
	 * any of that, so we keep the memory rather than hand a live DMA target to the allocator.
	 */
	st = cfg_rd(sc, AGNIC_CFG_STATUS);
	if (st != 0xFFFFFFFFU) {
		cfg_wr(sc, AGNIC_CFG_STATUS, st & ~AGNIC_CFG_STATUS_HOST_MGMT_READY);
		bus_barrier(sc->fac.res, sc->fac.off, AGNIC_CFG_SIZE, BUS_SPACE_BARRIER_WRITE);
		cfg_wr(sc, AGNIC_CFG_CMD_Q + AGNIC_QI_ADDR, 0);
		cfg_wr(sc, AGNIC_CFG_CMD_Q + AGNIC_QI_ADDR + 4, 0);
		cfg_wr(sc, AGNIC_CFG_CMD_Q + AGNIC_QI_LEN, 0);
		cfg_wr(sc, AGNIC_CFG_NOTIF_Q + AGNIC_QI_ADDR, 0);
		cfg_wr(sc, AGNIC_CFG_NOTIF_Q + AGNIC_QI_ADDR + 4, 0);
		cfg_wr(sc, AGNIC_CFG_NOTIF_Q + AGNIC_QI_LEN, 0);
		bus_barrier(sc->fac.res, sc->fac.off, AGNIC_CFG_SIZE, BUS_SPACE_BARRIER_WRITE);

		npugiu_free_buffers(sc);
		npugiu_free_ring(sc, &sc->bp);
		npugiu_free_ring(sc, &sc->rx);
		npugiu_free_ring(sc, &sc->tx);
		npugiu_free_ring(sc, &sc->notif);
		npugiu_free_ring(sc, &sc->cmd);
	} else {
		device_printf(sc->fac.dev,
		    "giu: endpoint gone before withdrawal - leaking the rings on purpose, "
		    "reset the coprocessor before this memory is reused\n");
	}

	device_printf(sc->fac.dev,
	    "giu: %ju commands, %ju answers (%ju multi-part, %ju late, %ju overrun), "
	    "%ju notifications (%ju keep-alive), %ju unmatched\n",
	    (uintmax_t)sc->commands, (uintmax_t)sc->answers, (uintmax_t)sc->multipart,
	    (uintmax_t)sc->late, (uintmax_t)sc->overruns,
	    (uintmax_t)sc->notifications, (uintmax_t)sc->keepalives, (uintmax_t)sc->drops);

	mtx_destroy(&sc->mtx);
	free(sc, M_DEVBUF);
	npugiu_sc = NULL;
}
