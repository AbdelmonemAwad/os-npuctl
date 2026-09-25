/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * What npuep.c hands to the pieces built on top of it.
 *
 * The facility layer finds where each facility lives by reading the map the coprocessor
 * publishes, so a consumer must never work its offset out from a constant. It is given one.
 */

#ifndef _NPUEP_H_
#define _NPUEP_H_

/*
 * One facility's window, already located and range-checked by npuep.c.
 *
 * `res` is the BAR resource the window is inside and `off` is the absolute offset within it, so
 * a consumer reads and writes with bus_space_*_4(rman_get_bustag(res), rman_get_bushandle(res),
 * off + field). Nothing here is a pointer into the window, deliberately: this is MMIO on the
 * far side of a PCIe link, and a plain C dereference of it is how the vendor driver does it and
 * is not how FreeBSD should.
 */
/*
 * The smallest mvmgmt window that can hold struct pci_net_shared_cfg. A facility smaller than
 * this cannot be what it claims to be, and the size came from the coprocessor, so it is checked
 * where the map is read rather than trusted downstream.
 */
#define	NPUEP_MGMT_MIN_SIZE	0x28

struct npuep_facility {
	device_t	 dev;
	struct resource	*res;		/* the BAR this facility's window is in */
	bus_size_t	 off;		/* absolute offset of the window within it */
	bus_size_t	 size;		/* how much of it is ours */
	bus_dma_tag_t	 parent_tag;	/* parent for any DMA the consumer allocates */

	/*
	 * The MSI-X vectors npuep allocated for THIS facility. The coprocessor is told a vector
	 * id per queue and signals us by writing the message itself, so a consumer must hand it
	 * a number npuep actually owns - inventing one points the device at a table entry
	 * belonging to something else.
	 */
	int		 first_msix;	/* index of this facility's first vector */
	int		 nmsix;		/* how many it has */
};

/*
 * The smallest giu window that can hold struct agnic_config_mem. Same rule as the mvmgmt
 * minimum: the size came from the coprocessor, so it is checked where the map is read.
 */
#define	NPUEP_GIU_MIN_SIZE	0x400

/* The smallest nwa window that can hold the mailbox header, the request and its reply. */
#define	NPUEP_NWA_MIN_SIZE	0x100

/*
 * The fourth facility, and the last one this driver does not speak.
 *
 * The vendor's host reaches it through usfp_firewall.ko, which its module dependencies show
 * sitting on mv_armada_drv - the facility layer - alongside the three modules whose facilities we
 * already implement. Its own boot message is `usfp_firewall_cmsg_init: real_dev: ... mv-pcimux0`,
 * so it is a control-message channel attached to the trunk interface. On the evidence, this is
 * what tells the coprocessor's fastpath where to send a frame, which is the one thing still
 * standing between a configured datapath and a frame arriving on it.
 *
 * A megabyte, one host-to-target doorbell, one DMA engine. Nothing is written to it here: this
 * only captures the window so it can be read.
 */
#define	NPUEP_RPC_MIN_SIZE	0x100

/*
 * The control facility's map, from the vendor's facility_conf.h. We already read its cookie and
 * handshake; what was never read is the rest, and the rest is how a host interrupts the target.
 *
 *	struct ctrl_map {
 *		u32 cookie;
 *		u32 handshake;
 *		u32 h2t_dbell_cnt;
 *		struct dbell_msg h2t_dbell_msg[];   // u64 address, u32 data - twelve bytes each
 *	};
 *
 * To ring doorbell n the host writes that entry's `data` to that entry's `address`. Every other
 * facility we have implemented polls, so this has never been needed - but the control-message
 * channel has one host-to-target doorbell and no way back, so it is needed there.
 */
#define	CTRL_H2T_DBELL_CNT	0x08	/* u32 */
/*
 * And the array starts at 0x10, not 0x0c. Three u32 fields come before it, but its first member
 * is a u64, so the compiler pads to the next eight-byte boundary. Reading it at 0x0c produces an
 * address of 0x0028004000000000 and a data word of zero, which is what this did until the raw
 * words were printed beside the interpretation.
 */
#define	CTRL_H2T_DBELL_MSG	0x10
#define	CTRL_DBELL_MSG_SIZE	16	/* a u64 then a u32, padded - not twelve */
#define	  CTRL_DBELL_ADDR	0x00	/* u64 */
#define	  CTRL_DBELL_DATA	0x08	/* u32 */
#define	CTRL_DBELL_MAX		16	/* more than the five this board has */

/*
 * And the address in each entry is an OFFSET, not somewhere to write directly. The vendor's host
 * adds its target-register BAR to it:
 *
 *	dbell_msg->address = map_dbell->address + trgt_reg_addr;	  facility_host.c
 *	trgt_reg_addr = (u64)fclts_cnf->trg_reg_bar.vaddr;
 *
 * and its own sysfs calls that BAR "bar4 target reg". So ringing a doorbell is an MMIO write into
 * BAR4 - a register on the endpoint - and NOT a write to a host physical address. The difference
 * matters more than most: this board publishes offset 0x280040, and 0x280040 as a host physical
 * address is low system memory. Nothing was written there until this was settled.
 */
#define	NPUEP_DBELL_BAR		4

struct npuep_softc;
int	npuep_ring_dbell(struct npuep_softc *sc, int n);

/*
 * ---------------------------------------------------------------------------------------
 * The control-message channel's state block, at the start of its window.
 *
 * These are not inferred. Sophos ships usfp_rh.ko built with -g3, so the compiler recorded every
 * field name and offset, and `readelf --debug-dump=info` reads them straight back out. The
 * constants come from --debug-dump=macro on the same file.
 *
 *	struct rpc_state {			struct rpc_ring {
 *		u32 cfg_magic;			    u64 posted;
 *		u16 cfg_revision;		    u64 done;
 *		u8  active_hi_rings;		    u32 ring_offset;
 *		u8  reconfig_done;		    u32 desc_offset;
 *		u64 zero_pad[8];		    u32 desc_count;
 *		struct rpc_ring ring_lo;	    union ring_hw_cfg r_cfg;
 *		struct rpc_ring rings[];	};
 *	};
 *
 * The three fields the target actually touches on each pass are confirmed twice over: the
 * disassembly of Marvell's sample handler reads its ring at +0x00 and +0x08 and takes a window
 * offset from +0x14, which is exactly posted, done and desc_offset.
 * ---------------------------------------------------------------------------------------
 */
#define	RPC_ST_CFG_MAGIC	0x00	/* u32 - the HOST writes this to open the channel */
#define	  RPC_STATE_CFG_MAGIC	0xD7D3AB00U
#define	RPC_ST_CFG_REVISION	0x04	/* u16 - the target publishes 1 */
#define	RPC_ST_ACTIVE_HI_RINGS	0x06	/* u8  */
#define	RPC_ST_RECONFIG_DONE	0x07	/* u8  */
#define	RPC_ST_RING_LO		0x48	/* the low-priority ring */
#define	RPC_ST_RINGS		0x68	/* and the high-priority ones */
#define	RPC_STATE_SIZE		232

#define	RPC_RING_POSTED		0x00	/* u64 */
#define	RPC_RING_DONE		0x08	/* u64 */
#define	RPC_RING_OFFSET		0x10	/* u32 */
#define	RPC_RING_DESC_OFFSET	0x14	/* u32 */
#define	RPC_RING_DESC_COUNT	0x18	/* u32 */
#define	RPC_RING_CFG		0x1c	/* u8 ring_num, u8 f_index, u8 dbell, u8 shared */
#define	RPC_RING_SIZE		32

#define	RPC_LO_RINGS_MAX	1
#define	RPC_HI_RINGS_MAX	4

/* A command, in host memory, pointed at by a descriptor in the window. */
#define	RPC_CMD_RESP_BUFF_SZ	0x00	/* u16 */
#define	RPC_CMD_CMD		0x03	/* u8 - an index into a 255-entry handler table */
#define	RPC_CMD_PAYLOAD		0x08
/* The descriptor itself, sixteen bytes, in the window. */
#define	RPC_BD_DMA_ADDR		0x00	/* u64 - a HOST physical address */
#define	RPC_BD_PAYLOAD_LEN	0x08	/* u16 */
#define	RPC_BD_FLAGS		0x0a	/* u16 */
#define	  RPC_DESC_POST_FLAG	1
#define	  RPC_DESC_NO_AGG_DMA	2

/* The commands worth naming, of the forty-five the channel defines. */
#define	RPC_CMD_LIF_ADD_UPDATE	3	/* make a logical interface */
#define	RPC_CMD_LIF_DELETE	4
#define	RPC_CMD_PPORT_UPDATE	5	/* bind a port tag to one: u8 iface, u8 rsvd, u16 tag */

int	npumgmt_attach(struct npuep_facility *fac);
void	npumgmt_detach(void);

int	npugiu_attach(struct npuep_facility *fac);
void	npugiu_detach(void);

int	npunwa_attach(struct npuep_facility *fac);
void	npunwa_detach(void);

#endif /* _NPUEP_H_ */
