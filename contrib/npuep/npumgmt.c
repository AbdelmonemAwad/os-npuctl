/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * npumgmt - the host end of mvmgmt0, the management link to the Marvell NPU.
 *
 * WHAT THIS IS
 *
 * A virtual Ethernet interface between this host and the NPU, carried over PCIe. There is no
 * wire: both rings live in HOST memory, and the NPU reaches them through its inbound window
 * using physical addresses that we publish into a shared structure in BAR2.
 *
 * It is worth being clear about why this particular interface first, ahead of the fourteen
 * front ports. It is one facility and two rings - the smallest thing that moves a packet, so
 * the smallest thing that can prove the model. And it is the link every Sophos diagnostic tool
 * on the appliance talks over: their host-side xgs-* tools are mostly thin wrappers that ssh
 * across it. Bringing it up turns the vendor's own toolbox on.
 *
 * NO DOORBELLS. The shipped driver forces dbell_nr to zero on both the receive and the transmit
 * path - pcinet.c:1140 and 1152 - which sets polling on and peer notification off. Both sides
 * then poll a 10 ms timer and nothing is ever rung. That reads like a debug hack left in, and
 * it is what the published source does, so a port that waits for an interrupt here waits
 * forever. The doorbell the mvmgmt facility is allocated is real and goes unused.
 *
 * WHAT IS DANGEROUS ABOUT IT
 *
 * This is the first time we hand the NPU addresses in host RAM and invite it to write there.
 * Everything up to now was the host reading and writing the endpoint's BARs, where the worst
 * case is a confused endpoint. From here a wrong physical address or a wrong offset in the
 * shared structure is a coprocessor writing into memory that belongs to something else - which
 * is how this project already earned one general protection fault. See docs/porting-notes.md.
 *
 * The target validates NOTHING it is given: it does not range-check the addresses, does not
 * test them for zero, and never compares the sanity value against anything. Whatever is in
 * those fields when link_status goes to HOST_UP is what it will write to.
 *
 * Nothing here is guessed. Every offset and constant is from the vendor's GPL pcinet source.
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include <sys/mbuf.h>
#include <sys/sockio.h>
#include <sys/rman.h>

#include <machine/atomic.h>
#include <machine/bus.h>
#include <machine/resource.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_types.h>
#include <net/if_media.h>
#include <net/ethernet.h>
#include <net/bpf.h>

#include "npuep.h"

/*
 * pcinet.h. The host allocates both rings; PCINET_NET_BUF_SIZE is the per-packet buffer and
 * 1024 entries each way means just over 4 MB of DMA-coherent memory in total. That is a lot to
 * pin for a management interface, and it is what the vendor does - matching it exactly is worth
 * more than saving the memory, because the target was built against these numbers.
 */
#define	PCINET_NET_BUF_SIZE	2048
#define	PCINET_NET_MTU_SIZE	1500
#define	PCINET_RX_Q_SIZE	1024
#define	PCINET_TX_Q_SIZE	1024
#define	PCINET_HOST_DEV_COMM_SANITY	0x01234567U

/*
 * The vendor shifts every buffer by NET_IP_ALIGN, in both its virtual and its physical view, so
 * that an IP header lands 4-byte aligned after the 14-byte Ethernet header.
 *
 * On x86-64 Linux NET_IP_ALIGN is ZERO - unaligned access is cheap there, so the architecture
 * defines the shift away and the vendor's arithmetic is a no-op on exactly the host this runs
 * on. Adding a real two-byte shift here would move every published buffer address two bytes
 * from where the vendor's host puts it. The target does not know the shift happened; it uses
 * the address it is handed. So this is 0, deliberately, and the constant is kept rather than
 * deleted because the code has to stay readable against the source it came from.
 */
#define	PCINET_IP_ALIGN		0

/*
 * The coprocessor reaches host memory through a single outbound window that is a linear map of
 * PCIe bus addresses [0, 64 GiB). The vendor host enforces the matching ceiling with
 * dma_set_mask_and_coherent(DMA_BIT_MASK(36)) - facility_host.c:837.
 *
 * Every address published to the target must therefore be below 2^36. This is not a performance
 * hint: an address above it maps to nothing the target can reach, and what it writes instead is
 * undefined. It is the single most important constraint in this file.
 */
#define	NPUMGMT_DMA_LOWADDR	0xFFFFFFFFFULL	/* 36 bits */

/*
 * These two look like an ownership protocol and are not one.
 *
 * Across the whole vendor driver they appear in exactly four places - pcinet.c:261, 262, 446
 * and 447 - and every one of them is an assignment. Nothing anywhere reads them. Ownership is
 * carried entirely by the two ring indices; the status word is write-only telemetry.
 *
 * They are defined here because the values are real and a debugger will show them, but an
 * implementation that waits on Q_ENTRY_STATUS_HOST_OWN before touching an entry will wait
 * forever.
 */
#define	Q_ENTRY_STATUS_HOST_OWN	0x80000000U
#define	Q_ENTRY_STATUS_FREE_SKB	0x01000000U

/* Link states, from enum link_status in pcinet.h. */
#define	PCINET_LINK_IS_DOWN	0x00
#define	PCINET_NETIF_OPEN	0x80
#define	PCINET_NETIF_STOP	0x81
#define	PCINET_LINK_HOST_UP	0x82
#define	PCINET_LINK_ESTABLISHED	0x83

/*
 * struct pci_net_shared_cfg, as laid out by the Linux x86-64 compiler. This lives in the
 * mvmgmt facility window - BAR2 + 0x1000 on this board - and the target reads it there, so
 * these offsets are a wire format, not an implementation detail.
 *
 *	+0x00  u64  rx_q_phys      physical address of the host's RX pci_net_q
 *	+0x08  u64  tx_q_phys      physical address of the host's TX pci_net_q
 *	+0x10  u32  link_status    enum, 4 bytes
 *	+0x14  u8   link_change    bool, 1 byte, 3 bytes of padding follow
 *	+0x18  u32  status
 *	+0x1c  u32  reserved       explicitly present to align the next field
 *	+0x20  u64  remote_mac
 */
#define	CFG_RX_Q_PHYS		0x00
#define	CFG_TX_Q_PHYS		0x08
#define	CFG_LINK_STATUS		0x10
#define	CFG_LINK_CHANGE		0x14
#define	CFG_STATUS		0x18
#define	CFG_REMOTE_MAC		0x20
#define	PCINET_CFG_SIZE		0x28	/* the whole struct is 40 bytes */

/*
 * struct pci_net_q - one per direction, allocated DMA-coherent, and it is the address of THIS
 * struct that goes into rx_q_phys / tx_q_phys.
 *
 *	+0x00  ptr  queue         host virtual pointer to the entry array - meaningless to the
 *	                          target, which uses q_phys_addr instead
 *	+0x08  ptr  dev_queue
 *	+0x10  int  q_size        1024
 *	+0x14  int  q_last        1023  (q_size - 1)
 *	+0x18  int  push_idx
 *	+0x1c  int  pop_idx
 *	+0x20  u32  sanity_val    0x01234567
 *	+0x28  u64  q_phys_addr   physical address of the entry array   <- the target uses this
 */
#define	Q_QUEUE_PTR		0x00
#define	Q_DEV_QUEUE		0x08
#define	Q_SIZE			0x10
#define	Q_LAST			0x14
#define	Q_PUSH_IDX		0x18
#define	Q_POP_IDX		0x1c
#define	Q_SANITY		0x20
#define	Q_PHYS_ADDR		0x28
#define	Q_STRUCT_SIZE		0x30

/*
 * struct pci_net_q_entry - 32 bytes. The target reads status, size and pbuf_phys; pbuf_virt and
 * skb are host-private and it never looks at them, which is why they can hold whatever a
 * FreeBSD implementation finds convenient.
 *
 *	+0x00  u32  status        Q_ENTRY_STATUS_* bits
 *	+0x04  u32  size          bytes in the buffer
 *	+0x08  u64  pbuf_phys     physical address of the 2048-byte buffer
 *	+0x10  ptr  pbuf_virt     host-private
 *	+0x18  ptr  skb           host-private
 */
#define	QE_STATUS		0x00
#define	QE_SIZE			0x04
#define	QE_PBUF_PHYS		0x08
#define	QE_PBUF_VIRT		0x10
#define	QE_SKB			0x18
#define	QE_ENTRY_SIZE		0x20

struct npumgmt_buf {
	bus_dmamap_t	 map;
	void		*vaddr;		/* already shifted by PCINET_IP_ALIGN */
	bus_addr_t	 paddr;		/* ditto - this is what the NPU is given */
};

struct npumgmt_ring {
	/* the pci_net_q control struct, DMA-coherent, published to the target */
	bus_dma_tag_t	 ctl_tag;
	bus_dmamap_t	 ctl_map;
	void		*ctl;
	bus_addr_t	 ctl_phys;

	/* the entry array, DMA-coherent, reached by the target via q_phys_addr */
	bus_dma_tag_t	 ent_tag;
	bus_dmamap_t	 ent_map;
	void		*ent;
	bus_addr_t	 ent_phys;

	/* the per-entry packet buffers */
	bus_dma_tag_t	 buf_tag;
	struct npumgmt_buf *buf;

	int		 size;

	/*
	 * The index THIS side owns - push on the TX ring, pop on the RX ring - kept here and
	 * never read back out of the shared struct. See the note above the accessors.
	 */
	uint32_t	 head;
	int		 fault;		/* the far side published an index we cannot trust */
};

static void
npumgmt_dmamap_cb(void *arg, bus_dma_segment_t *segs, int nseg, int error)
{
	bus_addr_t *addr = arg;

	if (error != 0 || nseg != 1) {
		*addr = 0;
		return;
	}
	*addr = segs[0].ds_addr;
}

/*
 * Allocate one direction's ring: the control struct, the entry array, and one 2048-byte buffer
 * per entry.
 *
 * Every allocation is BUS_DMA_COHERENT because both sides poll these structures without any
 * explicit sync point - the vendor uses dma_alloc_coherent throughout and never calls
 * dma_sync_*, so the memory has to be uncached rather than merely synchronised. Getting that
 * wrong would work on a machine with a coherent interconnect and fail subtly elsewhere.
 *
 * The buffers are allocated once, up front, all 1024 of them - not per packet. The target is
 * handed their physical addresses at bring-up and keeps them for the life of the link, so they
 * cannot be recycled the way an ordinary FreeBSD driver would recycle mbuf clusters.
 */
static int
npumgmt_alloc_ring(device_t dev, bus_dma_tag_t parent, struct npumgmt_ring *r, int size)
{
	int i, err;

	r->size = size;
	r->head = 0;
	r->fault = 0;

	err = bus_dma_tag_create(parent, 8, 0, NPUMGMT_DMA_LOWADDR, BUS_SPACE_MAXADDR,
	    NULL, NULL, Q_STRUCT_SIZE, 1, Q_STRUCT_SIZE, 0, NULL, NULL, &r->ctl_tag);
	if (err != 0)
		return (err);
	err = bus_dmamem_alloc(r->ctl_tag, &r->ctl, BUS_DMA_WAITOK | BUS_DMA_ZERO |
	    BUS_DMA_COHERENT, &r->ctl_map);
	if (err != 0)
		return (err);
	err = bus_dmamap_load(r->ctl_tag, r->ctl_map, r->ctl, Q_STRUCT_SIZE,
	    npumgmt_dmamap_cb, &r->ctl_phys, BUS_DMA_NOWAIT);
	if (err != 0 || r->ctl_phys == 0)
		return (err != 0 ? err : ENOMEM);

	err = bus_dma_tag_create(parent, 8, 0, NPUMGMT_DMA_LOWADDR, BUS_SPACE_MAXADDR,
	    NULL, NULL, (bus_size_t)QE_ENTRY_SIZE * size, 1,
	    (bus_size_t)QE_ENTRY_SIZE * size, 0, NULL, NULL, &r->ent_tag);
	if (err != 0)
		return (err);
	err = bus_dmamem_alloc(r->ent_tag, &r->ent, BUS_DMA_WAITOK | BUS_DMA_ZERO |
	    BUS_DMA_COHERENT, &r->ent_map);
	if (err != 0)
		return (err);
	err = bus_dmamap_load(r->ent_tag, r->ent_map, r->ent,
	    (bus_size_t)QE_ENTRY_SIZE * size, npumgmt_dmamap_cb, &r->ent_phys, BUS_DMA_NOWAIT);
	if (err != 0 || r->ent_phys == 0)
		return (err != 0 ? err : ENOMEM);

	err = bus_dma_tag_create(parent, 8, 0, NPUMGMT_DMA_LOWADDR, BUS_SPACE_MAXADDR,
	    NULL, NULL, PCINET_NET_BUF_SIZE, 1, PCINET_NET_BUF_SIZE, 0, NULL, NULL,
	    &r->buf_tag);
	if (err != 0)
		return (err);

	r->buf = malloc(sizeof(*r->buf) * size, M_DEVBUF, M_WAITOK | M_ZERO);

	for (i = 0; i < size; i++) {
		struct npumgmt_buf *b = &r->buf[i];
		uint8_t *ent = (uint8_t *)r->ent + (size_t)i * QE_ENTRY_SIZE;
		bus_addr_t pa = 0;

		err = bus_dmamem_alloc(r->buf_tag, &b->vaddr, BUS_DMA_WAITOK |
		    BUS_DMA_ZERO | BUS_DMA_COHERENT, &b->map);
		if (err != 0)
			return (err);
		err = bus_dmamap_load(r->buf_tag, b->map, b->vaddr, PCINET_NET_BUF_SIZE,
		    npumgmt_dmamap_cb, &pa, BUS_DMA_NOWAIT);
		if (err != 0 || pa == 0)
			return (err != 0 ? err : ENOMEM);

		/* the two-byte IP alignment shift, applied to both views */
		b->vaddr = (uint8_t *)b->vaddr + PCINET_IP_ALIGN;
		b->paddr = pa + PCINET_IP_ALIGN;

		*(uint32_t *)(ent + QE_STATUS) = 0;
		*(uint32_t *)(ent + QE_SIZE) = 0;
		*(uint64_t *)(ent + QE_PBUF_PHYS) = b->paddr;
		*(uint64_t *)(ent + QE_PBUF_VIRT) = (uintptr_t)b->vaddr;
		*(uint64_t *)(ent + QE_SKB) = 0;
	}

	/*
	 * Fill in the control struct. queue is the host's own virtual pointer and the target
	 * ignores it; q_phys_addr is what the target actually follows to reach the entries.
	 * q_last is size - 1, which is why the ring always keeps one slot empty.
	 */
	memset(r->ctl, 0, Q_STRUCT_SIZE);
	*(uint64_t *)((uint8_t *)r->ctl + Q_QUEUE_PTR) = (uintptr_t)r->ent;
	*(uint32_t *)((uint8_t *)r->ctl + Q_SIZE) = size;
	*(uint32_t *)((uint8_t *)r->ctl + Q_LAST) = size - 1;
	*(uint32_t *)((uint8_t *)r->ctl + Q_PUSH_IDX) = 0;
	*(uint32_t *)((uint8_t *)r->ctl + Q_POP_IDX) = 0;
	*(uint32_t *)((uint8_t *)r->ctl + Q_SANITY) = PCINET_HOST_DEV_COMM_SANITY;
	*(uint64_t *)((uint8_t *)r->ctl + Q_PHYS_ADDR) = r->ent_phys;

	device_printf(dev, "mgmt ring: ctl @ %#jx, %d entries @ %#jx, %d KB of buffers\n",
	    (uintmax_t)r->ctl_phys, size, (uintmax_t)r->ent_phys,
	    (size * PCINET_NET_BUF_SIZE) / 1024);

	return (0);
}

static void
npumgmt_free_ring(struct npumgmt_ring *r)
{
	int i;

	if (r->buf != NULL) {
		for (i = 0; i < r->size; i++) {
			struct npumgmt_buf *b = &r->buf[i];

			if (b->vaddr == NULL)
				continue;
			/* undo the alignment shift before handing the address back */
			b->vaddr = (uint8_t *)b->vaddr - PCINET_IP_ALIGN;
			bus_dmamap_unload(r->buf_tag, b->map);
			bus_dmamem_free(r->buf_tag, b->vaddr, b->map);
			b->vaddr = NULL;
		}
		free(r->buf, M_DEVBUF);
		r->buf = NULL;
	}
	if (r->buf_tag != NULL)
		bus_dma_tag_destroy(r->buf_tag);

	if (r->ent != NULL) {
		bus_dmamap_unload(r->ent_tag, r->ent_map);
		bus_dmamem_free(r->ent_tag, r->ent, r->ent_map);
		r->ent = NULL;
	}
	if (r->ent_tag != NULL)
		bus_dma_tag_destroy(r->ent_tag);

	if (r->ctl != NULL) {
		bus_dmamap_unload(r->ctl_tag, r->ctl_map);
		bus_dmamem_free(r->ctl_tag, r->ctl, r->ctl_map);
		r->ctl = NULL;
	}
	if (r->ctl_tag != NULL)
		bus_dma_tag_destroy(r->ctl_tag);

	memset(r, 0, sizeof(*r));
}

/*
 * ---------------------------------------------------------------------------------------
 * The rings in motion.
 *
 * Both rings are ordinary host memory, so they are touched with plain loads and stores. The
 * shared configuration is on the far side of the PCIe link and is touched with bus_space.
 * Keeping those two apart is the whole reason the accessors below look asymmetric.
 *
 * Ownership is the two indices and nothing else - see the note on the status bits above. For
 * the TX ring the host writes push_idx and the coprocessor writes pop_idx; for the RX ring it
 * is the other way round. So neither side ever needs a read-modify-write across the bus, and
 * the indices must be touched with 32-bit accesses only: they are adjacent halves of one
 * 8-byte word written by opposite sides.
 * ---------------------------------------------------------------------------------------
 */

/*
 * We keep our own index in the ring struct and only ever write it out. We never read it back
 * and use it.
 *
 * That is not caution for its own sake. push_idx and pop_idx are adjacent 32-bit fields inside
 * one 8-byte word, and the two sides own one each. The far side is an AArch64 core, where the
 * compiler merges two adjacent 32-bit stores into a single 64-bit one as a matter of routine -
 * so a peer that writes only its own field in C can still put a doubleword on the bus, landing
 * on ours as well. Nothing on this host can prevent that.
 *
 * Reading our index back out of that word and then subscripting buf[] with it turns the peer's
 * store into a wild pointer here. It faults inside m_copydata, a long way from the cause. So
 * indexing uses r->head, the shared word is write-only for our half, and the round trip is
 * never trusted.
 */
static __inline uint32_t
ring_ld(struct npumgmt_ring *r, bus_size_t off)
{
	return (atomic_load_32((volatile uint32_t *)((uint8_t *)r->ctl + off)));
}

static __inline void
ring_st(struct npumgmt_ring *r, bus_size_t off, uint32_t v)
{
	atomic_store_32((volatile uint32_t *)((uint8_t *)r->ctl + off), v);
}

static __inline uint32_t
ring_next(struct npumgmt_ring *r, uint32_t idx)
{
	return (idx == (uint32_t)(r->size - 1) ? 0 : idx + 1);
}

/*
 * Read the index the FAR side owns, and range-check it.
 *
 * Everything the coprocessor writes is untrusted input. An out-of-range value is not a
 * transient to be clamped and worked around: it means we have lost track of what the peer
 * thinks the ring looks like. Record it, and let the caller stop the link.
 */
static __inline int
ring_remote(struct npumgmt_ring *r, bus_size_t off, uint32_t *out)
{
	uint32_t v = ring_ld(r, off);

	if (v >= (uint32_t)r->size) {
		r->fault = 1;
		return (0);
	}
	*out = v;
	return (1);
}

/* RX: we own pop, the coprocessor owns push. Empty when the two meet. */
static __inline int
ring_empty(struct npumgmt_ring *r)
{
	uint32_t push;

	if (!ring_remote(r, Q_PUSH_IDX, &push))
		return (1);
	return (push == r->head);
}

/* TX: we own push, the coprocessor owns pop. One slot is always left empty. */
static __inline int
ring_full(struct npumgmt_ring *r)
{
	uint32_t pop;

	if (!ring_remote(r, Q_POP_IDX, &pop))
		return (1);
	return (ring_next(r, r->head) == pop);
}

static __inline uint8_t *
ring_entry(struct npumgmt_ring *r, uint32_t idx)
{
	return ((uint8_t *)r->ent + (size_t)idx * QE_ENTRY_SIZE);
}

/*
 * Publish our half of the index pair. The release fence is what makes everything written into
 * the slot visible before the index that hands the slot over.
 */
static __inline void
ring_advance(struct npumgmt_ring *r, bus_size_t off, uint32_t from)
{
	atomic_thread_fence_rel();
	r->head = ring_next(r, from);
	ring_st(r, off, r->head);
}

struct npumgmt_softc {
	struct npuep_facility	 fac;
	if_t			 ifp;
	struct mtx		 mtx;
	struct callout		 poll;
	struct npumgmt_ring	 rx;		/* the coprocessor fills it, we drain it   */
	struct npumgmt_ring	 tx;		/* we fill it, the coprocessor drains it   */
	int			 running;
	int			 link;		/* last link_status we read */
	uint64_t		 rx_packets, rx_bytes, rx_dropped;
	uint64_t		 tx_packets, tx_bytes, tx_dropped;
};

static struct npumgmt_softc *npumgmt_sc;

#define	MGMT_LOCK(sc)		mtx_lock(&(sc)->mtx)
#define	MGMT_UNLOCK(sc)		mtx_unlock(&(sc)->mtx)

/* The shared configuration, which is MMIO. */
static __inline uint32_t
cfg_rd4(struct npumgmt_softc *sc, bus_size_t o)
{
	return (bus_read_4(sc->fac.res, sc->fac.off + o));
}

static __inline void
cfg_wr4(struct npumgmt_softc *sc, bus_size_t o, uint32_t v)
{
	bus_write_4(sc->fac.res, sc->fac.off + o, v);
}

/*
 * The two ring addresses must each land as ONE 8-byte transaction. Split into a pair of 32-bit
 * writes, the coprocessor can read the field between them and follow a half-updated address
 * into memory that is not ours.
 */
static __inline void
cfg_wr8(struct npumgmt_softc *sc, bus_size_t o, uint64_t v)
{
	bus_write_8(sc->fac.res, sc->fac.off + o, v);
}

/*
 * Receive. The coprocessor has advanced rx.push_idx; we drain from our own pop.
 *
 * The buffer is ours and was handed over once at bring-up, so a received frame is copied out
 * into an mbuf rather than handed up - the coprocessor keeps writing into that same buffer as
 * soon as the slot comes round again, and an mbuf that pointed at it would be rewritten under
 * the stack. The vendor does the same copy for the same reason.
 *
 * Frames are chained rather than pushed up one at a time. if_input runs the whole IP stack
 * inline under the default direct dispatch, and it must not run under this driver's lock, so
 * the alternative is dropping and retaking the lock once per packet inside a callout. Building
 * a chain here and flushing it once in the caller narrows that window from sixty-four to one.
 */
static int
npumgmt_rx(struct npumgmt_softc *sc, int budget, struct mbuf **head, struct mbuf **tail)
{
	if_t ifp = sc->ifp;
	int done = 0;

	mtx_assert(&sc->mtx, MA_OWNED);

	while (done < budget && !ring_empty(&sc->rx)) {
		uint32_t pop = sc->rx.head;
		uint8_t *ent = ring_entry(&sc->rx, pop);
		uint32_t len = *(volatile uint32_t *)(ent + QE_SIZE);
		struct mbuf *m;

		/*
		 * The length comes from the far side. Nothing validates it there, so validate it
		 * here: a bad value would otherwise be a copy out of bounds of our own buffer.
		 */
		if (len < ETHER_HDR_LEN || len > PCINET_NET_BUF_SIZE) {
			sc->rx_dropped++;
			if_inc_counter(ifp, IFCOUNTER_IERRORS, 1);
			goto next;
		}

		m = m_getcl(M_NOWAIT, MT_DATA, M_PKTHDR);
		if (m == NULL) {
			/* Leave the slot; the coprocessor is not waiting on us for it. */
			sc->rx_dropped++;
			if_inc_counter(ifp, IFCOUNTER_IQDROPS, 1);
			break;
		}
		memcpy(mtod(m, void *), sc->rx.buf[pop].vaddr, len);
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
		if_inc_counter(ifp, IFCOUNTER_IPACKETS, 1);
next:
		/*
		 * Releasing the slot is the last thing, and it must be visible only after
		 * everything read out of it. Publishing the index first would let the far side
		 * refill a buffer we are still copying from.
		 */
		ring_advance(&sc->rx, Q_POP_IDX, pop);
		done++;
	}
	return (done);
}

/*
 * Transmit one frame. Called with the lock held.
 *
 * Returns ENOBUFS when the ring is full so the caller can requeue - that is a real condition
 * here rather than a theoretical one, because the coprocessor drains at its own pace and there
 * is no completion interrupt to tell us when it has.
 */
static int
npumgmt_encap(struct npumgmt_softc *sc, struct mbuf *m)
{
	uint32_t push;
	uint8_t *ent;
	int len;

	if (ring_full(&sc->tx))
		return (ENOBUFS);

	len = m->m_pkthdr.len;
	if (len > PCINET_NET_BUF_SIZE)
		return (EMSGSIZE);

	push = sc->tx.head;
	ent = ring_entry(&sc->tx, push);

	m_copydata(m, 0, len, sc->tx.buf[push].vaddr);

	/*
	 * The vendor pads every frame to 64 bytes before queueing it. Runts are legal on a
	 * virtual link with no wire, so this is not about the minimum Ethernet frame - it is
	 * about matching what the far side was built to expect. Zero the padding rather than
	 * leaving whatever the previous packet in this slot left behind, which would leak the
	 * tail of an unrelated frame across the link.
	 */
	if (len < 64) {
		memset((uint8_t *)sc->tx.buf[push].vaddr + len, 0, 64 - len);
		len = 64;
	}

	*(volatile uint32_t *)(ent + QE_SIZE) = len;
	/* Write-only telemetry - see the note where these are defined. Kept for parity. */
	*(volatile uint32_t *)(ent + QE_STATUS) =
	    (*(volatile uint32_t *)(ent + QE_STATUS) | Q_ENTRY_STATUS_HOST_OWN) &
	    ~Q_ENTRY_STATUS_FREE_SKB;

	/*
	 * Publish the slot. ring_advance carries the release fence: everything about the entry
	 * must be visible before the index that hands it over. The vendor emits that barrier only
	 * on the coprocessor side; omitting it on the host is an asymmetry that happens to be
	 * harmless on x86 store ordering and is not worth copying.
	 */
	ring_advance(&sc->tx, Q_PUSH_IDX, push);

	sc->tx_packets++;
	sc->tx_bytes += len;
	return (0);
}

static void
npumgmt_start_locked(struct npumgmt_softc *sc)
{
	if_t ifp = sc->ifp;
	struct mbuf *m;

	if (!sc->running || sc->link != PCINET_LINK_ESTABLISHED)
		return;

	while (!if_sendq_empty(ifp)) {
		m = if_dequeue(ifp);
		if (m == NULL)
			break;
		if (npumgmt_encap(sc, m) != 0) {
			if_sendq_prepend(ifp, m);
			if_setdrvflagbits(ifp, IFF_DRV_OACTIVE, 0);
			sc->tx_dropped++;
			break;
		}
		bpf_mtap_if(ifp, m);
		m_freem(m);
		if_inc_counter(ifp, IFCOUNTER_OPACKETS, 1);
	}
}

static void
npumgmt_start(if_t ifp)
{
	struct npumgmt_softc *sc = if_getsoftc(ifp);

	MGMT_LOCK(sc);
	npumgmt_start_locked(sc);
	MGMT_UNLOCK(sc);
}

/*
 * The link state machine and the receive poll, in one 10 ms callout.
 *
 * There is no interrupt to hang either off - see the note at the top of this file - so the
 * vendor runs a 100 Hz receive timer and a 2 Hz link worker. One callout doing both at the
 * faster rate is simpler and costs nothing measurable.
 *
 * The transitions, and who writes what:
 *
 *   host  NETIF_OPEN       on interface up
 *   host  publish rx_q_phys and tx_q_phys, THEN LINK_HOST_UP
 *   trgt  LINK_ESTABLISHED in reply - at which point it is using our memory
 *   host  NETIF_STOP       on interface down
 *   trgt  LINK_IS_DOWN     in reply - and only now may the memory be freed
 */
static void
npumgmt_tick(void *arg)
{
	struct npumgmt_softc *sc = arg;
	struct mbuf *head = NULL, *tail = NULL;
	if_t ifp;
	uint32_t link;

	mtx_assert(&sc->mtx, MA_OWNED);
	if (!sc->running)
		return;

	ifp = sc->ifp;
	link = cfg_rd4(sc, CFG_LINK_STATUS);
	if (link == 0xFFFFFFFFU) {
		/* The endpoint stopped decoding. Almost certainly the NPU was reset. */
		if (sc->link != -1) {
			device_printf(sc->fac.dev, "mgmt: endpoint gone, link down\n");
			sc->link = -1;
			if_link_state_change(ifp, LINK_STATE_DOWN);
		}
		callout_reset(&sc->poll, hz / 100, npumgmt_tick, sc);
		return;
	}

	if ((int)link != sc->link) {
		device_printf(sc->fac.dev, "mgmt: link 0x%02x -> 0x%02x\n",
		    sc->link, link);
		sc->link = link;
		if_link_state_change(ifp,
		    link == PCINET_LINK_ESTABLISHED ? LINK_STATE_UP : LINK_STATE_DOWN);
		if (link == PCINET_LINK_ESTABLISHED)
			if_setdrvflagbits(ifp, IFF_DRV_RUNNING, IFF_DRV_OACTIVE);
	}

	if (link == PCINET_LINK_ESTABLISHED) {
		npumgmt_rx(sc, 64, &head, &tail);
		/*
		 * The far side may have drained the TX ring since we last looked, and nothing
		 * tells us when. Clearing OACTIVE here is the only thing that restarts a queue
		 * that filled up.
		 */
		if (!ring_full(&sc->tx)) {
			if_setdrvflagbits(ifp, 0, IFF_DRV_OACTIVE);
			npumgmt_start_locked(sc);
		}
	}

	if (sc->rx.fault != 0 || sc->tx.fault != 0) {
		/*
		 * An index the coprocessor owns was outside its own ring. We cannot tell how long
		 * it has been wrong, so nothing these rings produce can be trusted from here on.
		 *
		 * Stop polling and take the interface down, but leave the memory published and do
		 * not free it: the far side is evidently still using it, and handing it back to
		 * the kernel now is the one move that turns a confused link into a corrupted host.
		 * Reset the coprocessor to end it.
		 */
		device_printf(sc->fac.dev,
		    "mgmt: ring index out of range (rx %d, tx %d) - link stopped. The "
		    "coprocessor still holds this memory; reset it before unloading.\n",
		    sc->rx.fault, sc->tx.fault);
		cfg_wr4(sc, CFG_LINK_STATUS, PCINET_NETIF_STOP);
		if_setdrvflagbits(ifp, 0, IFF_DRV_RUNNING);
		if_link_state_change(ifp, LINK_STATE_DOWN);
		sc->link = -1;
		sc->running = 0;
	} else {
		callout_reset(&sc->poll, hz / 100, npumgmt_tick, sc);
	}

	/*
	 * Hand the frames up with the lock dropped, and only after the next tick is already
	 * armed so that nothing here can lose the timer. We return with the lock held, which is
	 * what callout_init_mtx expects.
	 */
	if (head != NULL) {
		MGMT_UNLOCK(sc);
		while (head != NULL) {
			struct mbuf *m = head;

			head = m->m_nextpkt;
			m->m_nextpkt = NULL;
			if_input(ifp, m);
		}
		MGMT_LOCK(sc);
	}
}

/*
 * Publish the rings and ask the coprocessor to come up.
 *
 * The order is the contract: both addresses, then LINK_HOST_UP. That last write is the far
 * side's signal that the addresses are valid, and it validates nothing itself - so anything
 * stale or zero still in those fields when it lands is what it will write into.
 *
 * This is also where the reference implementation gets it wrong twice, and neither is copied:
 * it never initialises the state to LINK_IS_DOWN, so the machine starts from whatever a
 * previous host left in the BAR; and it ignores the result of allocating the rings and
 * advertises HOST_UP regardless.
 */
static void
npumgmt_publish(struct npumgmt_softc *sc)
{
	mtx_assert(&sc->mtx, MA_OWNED);

	cfg_wr4(sc, CFG_LINK_STATUS, PCINET_LINK_IS_DOWN);
	cfg_wr4(sc, CFG_STATUS, 0);

	/*
	 * Both rings restart from zero. An interface that is downed and brought up again gets a
	 * peer that has forgotten where it was, so carrying our own index across would leave the
	 * two sides disagreeing about how full the ring is from the first packet.
	 */
	sc->rx.head = 0;
	sc->tx.head = 0;
	sc->rx.fault = 0;
	sc->tx.fault = 0;
	ring_st(&sc->rx, Q_PUSH_IDX, 0);
	ring_st(&sc->rx, Q_POP_IDX, 0);
	ring_st(&sc->tx, Q_PUSH_IDX, 0);
	ring_st(&sc->tx, Q_POP_IDX, 0);

	cfg_wr8(sc, CFG_RX_Q_PHYS, (uint64_t)sc->rx.ctl_phys);
	cfg_wr8(sc, CFG_TX_Q_PHYS, (uint64_t)sc->tx.ctl_phys);

	/* Make both addresses visible before the state that says they are good. */
	bus_barrier(sc->fac.res, sc->fac.off, PCINET_CFG_SIZE, BUS_SPACE_BARRIER_WRITE);

	cfg_wr4(sc, CFG_LINK_STATUS, PCINET_NETIF_OPEN);
	cfg_wr4(sc, CFG_LINK_CHANGE, 1);
	bus_barrier(sc->fac.res, sc->fac.off, PCINET_CFG_SIZE, BUS_SPACE_BARRIER_WRITE);
	cfg_wr4(sc, CFG_LINK_STATUS, PCINET_LINK_HOST_UP);

	device_printf(sc->fac.dev, "mgmt: published rx %#jx tx %#jx, link HOST_UP\n",
	    (uintmax_t)sc->rx.ctl_phys, (uintmax_t)sc->tx.ctl_phys);
}

/*
 * Withdraw. This is the highest-risk path in the driver and the reason is worth naming: if the
 * host goes away without completing NETIF_STOP -> LINK_IS_DOWN, the coprocessor keeps writing
 * into the buffers it was handed, forever, into memory that has been returned to the kernel.
 *
 * The reference implementation frees the memory BEFORE telling the far side and then spins
 * without a timeout waiting for an answer. Both halves of that are wrong. This tells the far
 * side first, waits with a bound, and reports honestly when the bound is reached - because a
 * caller that knows the far side never answered can reset it, and a caller that hangs cannot.
 */
static int
npumgmt_withdraw(struct npumgmt_softc *sc)
{
	int i;

	mtx_assert(&sc->mtx, MA_OWNED);

	if (cfg_rd4(sc, CFG_LINK_STATUS) == 0xFFFFFFFFU)
		return (0);		/* endpoint already gone; nothing is reading our memory */

	cfg_wr4(sc, CFG_LINK_STATUS, PCINET_NETIF_STOP);
	cfg_wr4(sc, CFG_LINK_CHANGE, 1);

	for (i = 0; i < 200; i++) {		/* 2 seconds */
		uint32_t s = cfg_rd4(sc, CFG_LINK_STATUS);

		if (s == PCINET_LINK_IS_DOWN || s == 0xFFFFFFFFU)
			return (0);
		MGMT_UNLOCK(sc);
		pause("npumgw", hz / 100);
		MGMT_LOCK(sc);
	}

	device_printf(sc->fac.dev,
	    "mgmt: the coprocessor did not acknowledge NETIF_STOP. It may still be writing "
	    "into the ring buffers; reset it before this memory is reused.\n");
	return (ETIMEDOUT);
}

static int
npumgmt_ioctl(if_t ifp, u_long cmd, caddr_t data)
{
	struct npumgmt_softc *sc = if_getsoftc(ifp);
	struct ifreq *ifr = (struct ifreq *)data;
	int err = 0;

	switch (cmd) {
	case SIOCSIFFLAGS:
		MGMT_LOCK(sc);
		if ((if_getflags(ifp) & IFF_UP) != 0) {
			if ((if_getdrvflags(ifp) & IFF_DRV_RUNNING) == 0)
				npumgmt_publish(sc);
		}
		MGMT_UNLOCK(sc);
		break;
	case SIOCSIFMTU:
		/*
		 * Fixed. The buffers are 2048 bytes and were handed to the coprocessor once; it
		 * has no way to learn that the host changed its mind.
		 */
		if (ifr->ifr_mtu != PCINET_NET_MTU_SIZE)
			err = EINVAL;
		break;
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		break;		/* there is no filter on a point-to-point shared-memory link */
	default:
		err = ether_ioctl(ifp, cmd, data);
		break;
	}
	return (err);
}

int
npumgmt_attach(struct npuep_facility *fac)
{
	struct npumgmt_softc *sc;
	uint8_t mac[ETHER_ADDR_LEN];
	uint64_t remote;
	int err;

	if (npumgmt_sc != NULL)
		return (EBUSY);

	if (fac->size < PCINET_CFG_SIZE) {
		device_printf(fac->dev, "mgmt: facility window is only %ju bytes\n",
		    (uintmax_t)fac->size);
		return (ENXIO);
	}

	sc = malloc(sizeof(*sc), M_DEVBUF, M_WAITOK | M_ZERO);
	sc->fac = *fac;
	sc->link = -1;
	mtx_init(&sc->mtx, "npumgmt", NULL, MTX_DEF);
	callout_init_mtx(&sc->poll, &sc->mtx, 0);

	err = npumgmt_alloc_ring(fac->dev, fac->parent_tag, &sc->rx, PCINET_RX_Q_SIZE);
	if (err != 0)
		goto fail;
	err = npumgmt_alloc_ring(fac->dev, fac->parent_tag, &sc->tx, PCINET_TX_Q_SIZE);
	if (err != 0)
		goto fail;

	/*
	 * The peer's address is offered in remote_mac. Nothing in the vendor source ever reads
	 * it, so treat it as advisory: use it when it looks like a unicast address and invent a
	 * locally-administered one otherwise, rather than coming up with no address at all.
	 */
	remote = (uint64_t)cfg_rd4(sc, CFG_REMOTE_MAC) |
	    ((uint64_t)cfg_rd4(sc, CFG_REMOTE_MAC + 4) << 32);
	memcpy(mac, (uint8_t *)&remote + 2, ETHER_ADDR_LEN);
	if ((mac[0] & 0x01) != 0 || (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) == 0) {
		mac[0] = 0x02;		/* locally administered, unicast */
		mac[1] = 0x00;
		mac[2] = 0x00;
		mac[3] = 0x00;
		mac[4] = 0x00;
		mac[5] = 0x01;
	} else {
		mac[5] ^= 1;		/* our end of the link, not the peer's */
	}

	sc->ifp = if_alloc(IFT_ETHER);
	if_setsoftc(sc->ifp, sc);
	if_initname(sc->ifp, "mvmgmt", 0);
	if_setflags(sc->ifp, IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST);
	if_setstartfn(sc->ifp, npumgmt_start);
	if_setioctlfn(sc->ifp, npumgmt_ioctl);
	if_setsendqlen(sc->ifp, PCINET_TX_Q_SIZE - 1);
	if_setsendqready(sc->ifp);
	if_setmtu(sc->ifp, PCINET_NET_MTU_SIZE);

	ether_ifattach(sc->ifp, mac);
	if_link_state_change(sc->ifp, LINK_STATE_DOWN);

	npumgmt_sc = sc;

	MGMT_LOCK(sc);
	sc->running = 1;
	callout_reset(&sc->poll, hz / 100, npumgmt_tick, sc);
	MGMT_UNLOCK(sc);

	device_printf(fac->dev, "mgmt: %s attached, %d entries each way, %d KB of buffers\n",
	    if_name(sc->ifp), PCINET_RX_Q_SIZE,
	    ((PCINET_RX_Q_SIZE + PCINET_TX_Q_SIZE) * PCINET_NET_BUF_SIZE) / 1024);
	return (0);

fail:
	npumgmt_free_ring(&sc->tx);
	npumgmt_free_ring(&sc->rx);
	if (mtx_initialized(&sc->mtx))
		mtx_destroy(&sc->mtx);
	free(sc, M_DEVBUF);
	return (err);
}

void
npumgmt_detach(void)
{
	struct npumgmt_softc *sc = npumgmt_sc;

	if (sc == NULL)
		return;

	MGMT_LOCK(sc);
	sc->running = 0;
	MGMT_UNLOCK(sc);
	callout_drain(&sc->poll);

	if (sc->ifp != NULL) {
		ether_ifdetach(sc->ifp);
		if_free(sc->ifp);
		sc->ifp = NULL;
	}

	/*
	 * Withdraw BEFORE freeing, and only free if the far side agreed to stop. Leaking four
	 * megabytes is unpleasant; handing it back to the kernel while a coprocessor is still
	 * writing into it is a corrupted machine some minutes later with nothing to point at.
	 */
	MGMT_LOCK(sc);
	if (npumgmt_withdraw(sc) == 0) {
		MGMT_UNLOCK(sc);
		npumgmt_free_ring(&sc->tx);
		npumgmt_free_ring(&sc->rx);
	} else {
		MGMT_UNLOCK(sc);
		device_printf(sc->fac.dev,
		    "mgmt: leaking the ring memory on purpose - it is safer than freeing it\n");
	}

	mtx_destroy(&sc->mtx);
	free(sc, M_DEVBUF);
	npumgmt_sc = NULL;
}
