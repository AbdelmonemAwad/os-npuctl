/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The AGNIC wire format, as the coprocessor lays it out.
 *
 * Every offset, size and constant here is transcribed from Marvell's GPL giu_nic_hw.h, which
 * opens by saying the file "should be copied AS-IS, in order to make sure that data-structures on
 * both sides are 100% aligned". That is the right instinct and the wrong mechanism for a FreeBSD
 * port: copying a GPL header into a BSD-licensed tree is not something to do casually, and a
 * struct declaration would in any case be a lie here. The far side is across a PCIe link, so
 * every field is reached with bus_space, and a C struct laid over MMIO invites the compiler to
 * merge, split, reorder or elide accesses that must happen exactly once and exactly as written.
 *
 * So this is offsets, not structures. It is more tedious to write against and it is the only
 * form that cannot quietly become wrong.
 *
 * See docs/giu.md for what these mean and where each claim was checked.
 */

#ifndef _NPUGIU_H_
#define _NPUGIU_H_

/*
 * ---------------------------------------------------------------------------------------
 * struct agnic_config_mem - at the start of the giu facility window in BAR0. 1024 bytes,
 * packed.
 * ---------------------------------------------------------------------------------------
 */
#define	AGNIC_CFG_STATUS		0x00
#define	  AGNIC_CFG_STATUS_DEV_READY		0x1
#define	  AGNIC_CFG_STATUS_HOST_MGMT_READY	0x2
#define	  AGNIC_CFG_STATUS_DEV_MGMT_READY	0x4
#define	AGNIC_CFG_MAC_ADDR		0x04	/* six bytes, and unlike pcinet it is populated */
#define	AGNIC_CFG_CMD_Q			0x10	/* struct agnic_q_hw_info */
#define	AGNIC_CFG_NOTIF_Q		0x28	/* struct agnic_q_hw_info */
#define	AGNIC_CFG_BAR0_VF_START		0x40
#define	AGNIC_CFG_BAR2_VF_START		0x48
#define	AGNIC_CFG_DEV_USE_SIZE		0x58	/* where the ring index array starts, BAR-relative */
#define	AGNIC_CFG_MSI_X_TBL_OFFSET	0x5c	/* within BAR0 */
#define	AGNIC_CFG_SIZE			0x400

/*
 * struct agnic_q_hw_info, 24 bytes. q_prod_offs and q_cons_offs are BAR-relative offsets of two
 * u32 slots, NOT host addresses - see docs/giu.md, where the source's own macro name says
 * otherwise and the source's own comment corrects it.
 */
#define	AGNIC_QI_ADDR			0x00	/* u64, bus address of the ring in HOST memory */
#define	AGNIC_QI_PROD_OFFS		0x08
#define	AGNIC_QI_CONS_OFFS		0x0c
#define	AGNIC_QI_LEN			0x10
#define	AGNIC_QI_RES			0x14
#define	AGNIC_QI_SIZE			0x18

/*
 * ---------------------------------------------------------------------------------------
 * The command channel. 64-byte descriptors, 56 of them parameters.
 * ---------------------------------------------------------------------------------------
 */
#define	AGNIC_CMD_IDX			0x00	/* u16, echoed into the response */
#define	AGNIC_CMD_APP_CODE		0x02	/* u16 */
#define	AGNIC_CMD_CODE			0x04	/* u8  */
#define	AGNIC_CMD_CLIENT_ID		0x05	/* u8  */
#define	AGNIC_CMD_CLIENT_TYPE		0x06	/* u8  */
#define	AGNIC_CMD_FLAGS			0x07	/* u8  */
#define	AGNIC_CMD_DATA			0x08	/* 56 bytes */
#define	AGNIC_CMD_DESC_SIZE		0x40
#define	AGNIC_MGMT_DESC_DATA_LEN	56

#define	AGNIC_CMD_ID_ILLEGAL		0x0000
#define	AGNIC_CMD_ID_NOTIFICATION	0xFFFF	/* in cmd_idx: this is not a response */

/* enum agnic_app_codes */
#define	AGNIC_AC_HOST_NETDEV		0x1
#define	AGNIC_AC_PF_MANAGER		0x2

/* enum agnic_cmd_dest_type */
#define	AGNIC_CDT_INVALID		0
#define	AGNIC_CDT_PF			1
#define	AGNIC_CDT_VF			2
#define	AGNIC_CDT_CUSTOM		3	/* what Sophos's NetAgent rides on */

/* flags: ext-desc count in [4:0], no-response at bit 5, buffer position in [7:6] */
#define	AGNIC_CMD_F_NUM_EXT_MASK	0x1F
#define	AGNIC_CMD_F_NUM_EXT_SHIFT	0
#define	AGNIC_CMD_F_NO_RESP_SHIFT	5
#define	AGNIC_CMD_F_BUF_POS_SHIFT	6
#define	AGNIC_CMD_F_BUF_POS_MASK	0x3
#define	  AGNIC_BUF_POS_SINGLE		0
#define	  AGNIC_BUF_POS_FIRST_MID	1
#define	  AGNIC_BUF_POS_LAST		2
#define	  AGNIC_BUF_POS_EXT_BUF		3

/* enum agnic_cmd_codes - the ones bring-up and operation need */
#define	AGNIC_CC_PF_INIT		0x01
#define	AGNIC_CC_PF_INIT_DONE		0x02
#define	AGNIC_CC_PF_EGRESS_TC_ADD	0x03
#define	AGNIC_CC_PF_EGRESS_DATA_Q_ADD	0x04
#define	AGNIC_CC_PF_INGRESS_TC_ADD	0x05
#define	AGNIC_CC_PF_INGRESS_DATA_Q_ADD	0x06
#define	AGNIC_CC_PF_ENABLE		0x07
#define	AGNIC_CC_PF_DISABLE		0x08
#define	AGNIC_CC_PF_MGMT_ECHO		0x09	/* the cheapest proof the channel works */
#define	AGNIC_CC_PF_LINK_STATUS		0x0a
#define	AGNIC_CC_PF_GET_STATISTICS	0x0b
#define	AGNIC_CC_PF_CLOSE		0x0c
#define	AGNIC_CC_PF_MAC_ADDR		0x0d
#define	AGNIC_CC_PF_PROMISC		0x0e
#define	AGNIC_CC_PF_MC_PROMISC		0x0f
#define	AGNIC_CC_PF_MTU			0x10
#define	AGNIC_CC_PF_ADD_VLAN		0x12
#define	AGNIC_CC_PF_REMOVE_VLAN		0x13
#define	AGNIC_CC_PF_LINK_INFO		0x19
#define	AGNIC_CC_GET_CAPABILITIES	0x1e

/* enum agnic_notif_codes */
#define	AGNIC_NC_PF_LINK_CHANGE		0x1
#define	AGNIC_NC_PF_KEEP_ALIVE		0x2

/* response status, first byte of the response payload */
#define	AGNIC_NOTIF_STATUS_OK		0
#define	AGNIC_NOTIF_STATUS_FAIL		1

/* enum agnic_egress_sched */
#define	AGNIC_ES_STRICT_SCHED		0x1
#define	AGNIC_ES_WRR_SCHED		0x2

/* enum agnic_ingress_hash_type */
#define	AGNIC_ING_HASH_NONE		0x0
#define	AGNIC_ING_HASH_2_TUPLE		0x1
#define	AGNIC_ING_HASH_5_TUPLE		0x2

/*
 * ---------------------------------------------------------------------------------------
 * Command parameter payloads, at AGNIC_CMD_DATA. Offsets are from the start of the payload,
 * and the whole structure is packed - so a u64 can and does land unaligned.
 * ---------------------------------------------------------------------------------------
 */

/* CC_PF_INIT */
#define	AGNIC_P_INIT_NUM_EGRESS_TC	0x00	/* u32 */
#define	AGNIC_P_INIT_NUM_INGRESS_TC	0x04	/* u32 */
#define	AGNIC_P_INIT_MTU_OVERRIDE	0x08	/* u16 */
#define	AGNIC_P_INIT_MRU_OVERRIDE	0x0a	/* u16 */
#define	AGNIC_P_INIT_EGRESS_SCHED	0x0c	/* u8  */

/* CC_PF_EGRESS_TC_ADD */
#define	AGNIC_P_ETC_TC			0x00	/* u32 */
#define	AGNIC_P_ETC_NUM_QUEUES		0x04	/* u32 */
#define	AGNIC_P_ETC_NUM_Q_PER_DMA	0x08	/* u32 */

/* CC_PF_EGRESS_DATA_Q_ADD - also used for buffer-pool queues */
#define	AGNIC_P_EQ_PHYS_ADDR		0x00	/* u64 */
#define	AGNIC_P_EQ_PROD_OFFS		0x08	/* u32 */
#define	AGNIC_P_EQ_CONS_OFFS		0x0c	/* u32 */
#define	AGNIC_P_EQ_LEN			0x10	/* u32 */
#define	AGNIC_P_EQ_WRR_WEIGHT		0x14	/* u32 */
#define	AGNIC_P_EQ_TC			0x18	/* u32, irrelevant for a buffer pool */
#define	AGNIC_P_EQ_MSIX_ID		0x1c	/* u32 */

/* CC_PF_INGRESS_TC_ADD */
#define	AGNIC_P_ITC_TC			0x00	/* u32 */
#define	AGNIC_P_ITC_NUM_QUEUES		0x04	/* u32 */
#define	AGNIC_P_ITC_PKT_OFFSET		0x08	/* u32 */
#define	AGNIC_P_ITC_HASH_TYPE		0x0c	/* u8  */

/* CC_PF_INGRESS_DATA_Q_ADD - carries the receive ring AND its buffer pool */
#define	AGNIC_P_IQ_PHYS_ADDR		0x00	/* u64 */
#define	AGNIC_P_IQ_PROD_OFFS		0x08	/* u32 */
#define	AGNIC_P_IQ_CONS_OFFS		0x0c	/* u32 */
#define	AGNIC_P_IQ_BPOOL_PHYS_ADDR	0x10	/* u64 */
#define	AGNIC_P_IQ_BPOOL_PROD_OFFS	0x18	/* u32 */
#define	AGNIC_P_IQ_BPOOL_CONS_OFFS	0x1c	/* u32 */
#define	AGNIC_P_IQ_LEN			0x20	/* u32 */
#define	AGNIC_P_IQ_MSIX_ID		0x24	/* u32 */
#define	AGNIC_P_IQ_TC			0x28	/* u32 */
#define	AGNIC_P_IQ_BUF_SIZE		0x2c	/* u32 */

/* CC_PF_MTU */
#define	AGNIC_P_MTU			0x00	/* u32 */

/*
 * ---------------------------------------------------------------------------------------
 * Data descriptors. Transmit and receive are 32 bytes, a buffer-pool entry is 16.
 * ---------------------------------------------------------------------------------------
 */

/* struct agnic_tx_desc */
#define	AGNIC_TXD_FLAGS			0x00	/* u32 */
#define	AGNIC_TXD_PKT_OFFSET		0x04	/* u8  */
/*
 * vlan_info is bits 7:6 of this byte, not 1:0. The vendor declares it as `u8 res4:6;
 * u8 vlan_info:2;`, and the struct's own comment gives the order as "msb ... lsb", so the two
 * bits are the top of the byte. Bitfield allocation order is compiler-defined, which is exactly
 * why this file uses a shift instead of copying the declaration.
 */
#define	AGNIC_TXD_VLAN_INFO		0x05	/* u8 */
#define	AGNIC_TXD_VLAN_INFO_SHIFT	6
#define	AGNIC_TXD_VLAN_INFO_MASK	0x3
#define	AGNIC_TXD_BYTE_CNT		0x06	/* u16 */
#define	AGNIC_TXD_NUM_SEG_ENT		0x0a	/* u16 */
#define	AGNIC_TXD_BUFFER_ADDR		0x10	/* u64 */
#define	AGNIC_TXD_COOKIE		0x18	/* u64, host-private, returned on completion */
#define	AGNIC_TXD_SIZE			0x20

/*
 * Transmit flags. The checksum bits are DISABLES - leaving them clear asks the device to compute
 * the checksum, which is the opposite of the usual convention and easy to get backwards.
 */
#define	AGNIC_TXD_F_L3_OFFSET_SHIFT	0
#define	AGNIC_TXD_F_L3_OFFSET_MASK	0x7F
#define	AGNIC_TXD_F_IP_HDR_LEN_SHIFT	8
#define	AGNIC_TXD_F_IP_HDR_LEN_MASK	0x1F
#define	AGNIC_TXD_F_GEN_L4_CSUM_NOT	(1U << 14)
#define	AGNIC_TXD_F_GEN_IPV4_CSUM_DIS	(1U << 15)
#define	AGNIC_TXD_F_MD_MODE		(1U << 22)
#define	AGNIC_TXD_F_L4_TCP		(0U << 24)
#define	AGNIC_TXD_F_L4_UDP		(1U << 24)
#define	AGNIC_TXD_F_L4_OTHER		(2U << 24)
#define	AGNIC_TXD_F_L3_IPV4		(0U << 26)
#define	AGNIC_TXD_F_L3_IPV6		(1U << 26)
#define	AGNIC_TXD_F_L3_OTHER		(2U << 26)
#define	AGNIC_TXD_F_SG_INDIRECT		(1U << 28)
#define	AGNIC_TXD_F_SG_DIRECT		(2U << 28)
#define	AGNIC_TXD_F_SG_SINGLE_ENTRY	(3U << 28)

#define	AGNIC_TX_VLAN_TAG_NONE		0
#define	AGNIC_TX_VLAN_TAG_SINGLE	1
#define	AGNIC_TX_VLAN_TAG_DOUBLE	2

/* struct agnic_rx_desc */
#define	AGNIC_RXD_FLAGS			0x00	/* u32 */
#define	AGNIC_RXD_PKT_OFFSET		0x04	/* u8  */
#define	AGNIC_RXD_INFO			0x05	/* u8, vlan_info:2 | l2_info:2 | l3_info:2 */
#define	AGNIC_RXD_BYTE_CNT		0x06	/* u16 */
#define	AGNIC_RXD_PORT_NUM		0x08	/* u16 - the vendor's host driver never reads it */
#define	AGNIC_RXD_NUM_SG_ENT		0x0a	/* u16 */
#define	AGNIC_RXD_TIMESTAMP_HASHKEY	0x0c	/* u32 */
#define	AGNIC_RXD_BUFFER_ADDR		0x10	/* u64 */
#define	AGNIC_RXD_COOKIE		0x18	/* u64 */
#define	AGNIC_RXD_SIZE			0x20

#define	AGNIC_RXD_F_L3_OFFSET_SHIFT	0
#define	AGNIC_RXD_F_L3_OFFSET_MASK	0x7F
#define	AGNIC_RXD_F_IP_HDR_LEN_SHIFT	8
#define	AGNIC_RXD_F_IP_HDR_LEN_MASK	0x1F
#define	AGNIC_RXD_F_L4_STATUS_SHIFT	13
#define	AGNIC_RXD_F_L3_STATUS_SHIFT	23
#define	AGNIC_RXD_F_STATUS_MASK		0x3
#define	  AGNIC_DESC_CHECK_OK		0x0
#define	  AGNIC_DESC_ERR_CHECKSUM_ERR	0x1
#define	  AGNIC_DESC_ERR_CSUM_UNKNOWN	0x2
#define	AGNIC_RXD_F_MD_MODE		(1U << 22)

/*
 * The vendor stamps every receive descriptor with this when the ring is allocated, and rejects a
 * descriptor whose cookie still holds it or is zero. Without that the host cannot tell a
 * descriptor the device filled from one nobody ever wrote. It is not in the published source; it
 * arrived in patch 0011 after the fact, and it is the same lesson as never trusting an index
 * read back out of shared memory.
 */
#define	AGNIC_COOKIE_DRIVER_WATERMARK	0xdeaddeadULL

/* struct agnic_bpool_desc */
#define	AGNIC_BPD_BUFF_ADDR_PHYS	0x00	/* u64 */
#define	AGNIC_BPD_BUFF_COOKIE		0x08	/* u64 */
#define	AGNIC_BPD_SIZE			0x10

/*
 * ---------------------------------------------------------------------------------------
 * Sizes, from giu_nic.h - not part of the wire format, but the device was built against them.
 * ---------------------------------------------------------------------------------------
 */
#define	AGNIC_CONFIG_BAR_SIZE		(64 * 1024)
#define	AGNIC_MAX_TC			8
#define	AGNIC_MAX_RXQ_COUNT		128
#define	AGNIC_MAX_TXQ_COUNT		128
#define	AGNIC_BPOOLS_COUNT		128
#define	AGNIC_MAX_MNG_Q_COUNT		2
#define	AGNIC_MAX_QUEUES		(AGNIC_MAX_RXQ_COUNT + AGNIC_MAX_TXQ_COUNT + \
					 AGNIC_BPOOLS_COUNT + AGNIC_MAX_MNG_Q_COUNT)
#define	AGNIC_RING_INDEX_SLOT_FREE	0xFFFFFFFFU

/*
 * ---------------------------------------------------------------------------------------
 * The port tag.
 *
 * Two bytes in network order in front of the Ethernet header, and the only thing that tells the
 * fourteen front ports apart. Not the descriptor's port_num field, which the vendor's own host
 * driver never reads, and not a VLAN tag. See docs/giu.md for the evidence, and for the fact
 * that the receive direction is proven from the source while the transmit direction is inferred
 * and should be confirmed with a capture before it is trusted.
 * ---------------------------------------------------------------------------------------
 */
#define	NPUGIU_PORT_TAG_LEN		2
#define	NPUGIU_METADATA_LEN		64
#define	NPUGIU_HEADROOM			(NPUGIU_PORT_TAG_LEN + NPUGIU_METADATA_LEN)

#endif /* _NPUGIU_H_ */
