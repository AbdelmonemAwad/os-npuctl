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
 * front ports. It is one facility, two rings and one doorbell each way - the smallest thing
 * that moves a packet, so the smallest thing that can prove the model. And it is the link every
 * Sophos diagnostic tool on the appliance talks over: xgs-cpld, xgs-ports, xgs-sff and the rest
 * are all ssh wrappers onto fe80::...%mvmgmt0. Bringing it up turns the vendor's own toolbox on.
 *
 * WHAT IS DANGEROUS ABOUT IT
 *
 * This is the first time we hand the NPU addresses in host RAM and invite it to write there.
 * Everything up to now was the host reading and writing the endpoint's BARs, which can only
 * damage the endpoint. From here a wrong physical address, a wrong offset in the shared
 * structure, or an ownership bit read backwards means a coprocessor writing into memory that
 * belongs to something else - which is how this project already earned one general protection
 * fault. See docs/porting-notes.md. Nothing here is guessed: every offset and constant is
 * from the vendor's GPL pcinet source.
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

#include <machine/bus.h>

/*
 * NOT IN THE BUILD YET, and deliberately so. This file holds the memory layer - the part that
 * was verified directly against the vendor source - while the ring protocol (who owns an
 * entry, who moves which index, where the barriers go) is still being pinned down. Adding it
 * to SRCS before that is settled would produce a module that allocates four megabytes, hands
 * the addresses to a coprocessor, and then races it. Add it to the Makefile when the TX and RX
 * paths below are written and reviewed.
 */

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
 * Linux uses NET_IP_ALIGN, which is 2 on x86: every buffer is shifted by two bytes so that the
 * IP header lands 4-byte aligned after the 14-byte Ethernet header. Both pbuf_virt AND
 * pbuf_phys are shifted, so the target sees the shifted address. FreeBSD calls the same
 * quantity ETHER_ALIGN. It must be replicated - the target does not know it happened, it just
 * uses the address it is given, and dropping it would silently misalign every received packet.
 */
#define	PCINET_IP_ALIGN		2

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

	err = bus_dma_tag_create(parent, 8, 0, BUS_SPACE_MAXADDR, BUS_SPACE_MAXADDR,
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

	err = bus_dma_tag_create(parent, 8, 0, BUS_SPACE_MAXADDR, BUS_SPACE_MAXADDR,
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

	err = bus_dma_tag_create(parent, 8, 0, BUS_SPACE_MAXADDR, BUS_SPACE_MAXADDR,
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
