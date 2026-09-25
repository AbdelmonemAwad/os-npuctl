# GIU, the datapath that owns the fourteen front ports

Where [mvmgmt0](mvmgmt.md) is two rings and a shared struct, this is a network interface
controller: a command channel, traffic classes, descriptor rings, buffer pools, checksum offload,
VLAN filtering and statistics. The vendor calls it **AGNIC**, and its facility is `giu`, at BAR0
offset 0 with four of the five target-to-host doorbells the endpoint allocates.

Not implemented. This is the specification the implementation will be written against, taken from
Marvell's GPL `giu_nic` source in the Sophos GPL drop. Every claim below is from that source,
with the file and line where it is worth checking; where the code and its own comments disagree,
the code wins and this page says so.

## The split is the opposite of pcinet's

pcinet puts both rings, both index pairs and all the buffers in host memory, and the coprocessor
reaches them through its outbound window. AGNIC does not.

| | lives in | who publishes it |
|---|---|---|
| `agnic_config_mem` | **device BAR0**, at the facility window | the device, at boot |
| every ring's producer and consumer index | **device BAR0**, at `dev_use_size` | the device reserves it |
| descriptor rings | host memory | the host, per queue, by command |
| packet buffers | host memory | the host, into a buffer pool |

So the indices are MMIO reads and writes, not loads and stores, and a host that treats them as
memory will be wrong in a way that only shows under load.

This is worth stating carefully because the source misleads twice. The macros are named
`AGNIC_RING_PROD_INDX_LOCAL_PHYS` and `AGNIC_RING_CONS_INDX_LOCAL_PHYS` (`giu_nic.h:226`), which
reads like a host physical address. It is not one. `giu_nic.c:1846` settles it, and the comment
two lines down says so outright:

```c
	adapter->ring_indices_arr =
		(void *)((u64)adapter->nic_cfg_base + adapter->nic_cfg_base->dev_use_size);
	/* in case the indices are allocated on the BAR, we would like to send only the offset
	 * from the bar base address
	 */
	adapter->ring_indices_arr_phys = adapter->nic_cfg_base->dev_use_size;
```

The array is `AGNIC_MAX_QUEUES` u32 slots inside BAR0, bounded against `AGNIC_CONFIG_BAR_SIZE`,
initialised to `0xFFFFFFFF` meaning "slot free", and each ring claims two of them. The value put
into `q_prod_offs` and `q_cons_offs` is the **BAR-relative offset of the slot**, which is exactly
what the field names in `giu_nic_hw.h` say. Only the macro name lies.

## The configuration structure

At the start of the `giu` facility window. 1024 bytes, `#pragma pack(1)`, `giu_nic_hw.h:36`.

```
+0x00  u32  status              DEV_READY 0x1 | HOST_MGMT_READY 0x2 | DEV_MGMT_READY 0x4
+0x04  u8   mac_addr[6]         the device's own address - and unlike pcinet, it is real
+0x0a  u8   res1[6]
+0x10  struct agnic_q_hw_info cmd_q      the host-to-device command ring
+0x28  struct agnic_q_hw_info notif_q    the device-to-host notification ring
+0x40  u64  bar0_vf_start_off
+0x48  u64  bar2_vf_start_off
+0x50  u8   res2[8]
+0x58  u32  dev_use_size        where the ring index array starts, BAR-relative
+0x5c  u32  msi_x_tbl_offset    MSI-X table offset within BAR0
+0x60  u8   res3[928]
```

```
struct agnic_q_hw_info          24 bytes
  +0x00  u64  q_addr            bus address of the ring in HOST memory
  +0x08  u32  q_prod_offs       BAR-relative offset of the producer slot
  +0x0c  u32  q_cons_offs       BAR-relative offset of the consumer slot
  +0x10  u32  len               number of elements
  +0x14  u32  res
```

**`mac_addr` at +0x04 is populated**, which is the opposite of pcinet's `remote_mac` - measured
zero on this hardware and documented as dead in [mvmgmt.md](mvmgmt.md). A host should read this
one and use it.

## The command channel

Two rings of 64-byte descriptors, `giu_nic_hw.h:582`:

```
struct agnic_cmd_desc            64 bytes
  +0x00  u16  cmd_idx           echoed into the response; 0xFFFF means "this is a notification"
  +0x02  u16  app_code          AC_HOST_AGNIC_NETDEV = 1, AC_PF_MANAGER = 2
  +0x04  u8   cmd_code          enum agnic_cmd_codes
  +0x05  u8   client_id
  +0x06  u8   client_type       CDT_PF = 1, CDT_VF = 2, CDT_CUSTOM = 3
  +0x07  u8   flags             ext-desc count :5 | no-resp :1 | buf-pos :2
  +0x08  u8   data[56]          serialised parameters or response
```

56 bytes of inline parameters, and a chaining scheme for anything larger: `flags` carries a count
of extension descriptors and a position code (`SINGLE`, `FIRST_MID`, `LAST`, `EXT_BUF`). A
response is matched to its command by `cmd_idx`, so commands can be outstanding concurrently.

Thirty commands are defined. The ones bring-up needs are `CC_PF_INIT`, `CC_PF_INGRESS_TC_ADD`,
`CC_PF_INGRESS_DATA_Q_ADD`, `CC_PF_EGRESS_TC_ADD`, `CC_PF_EGRESS_DATA_Q_ADD`, `CC_PF_INIT_DONE`
and `CC_PF_ENABLE`. The rest are operational: MTU, MAC address, promiscuous mode, VLAN add and
remove, pause frames, loopback, rate limiting, statistics and a capability query.

Two notifications come the other way: `NC_PF_LINK_CHANGE` and `NC_PF_KEEP_ALIVE`.

## Bring-up order

From `giu_nic.c:2957` onward, and the order is the contract:

1. `CC_PF_INIT` - how many ingress and egress traffic classes, MTU and MRU overrides, and the
   egress scheduler (`ES_STRICT_SCHED` or `ES_WRR_SCHED`).
2. `CC_PF_INGRESS_TC_ADD` - per class: queue count, packet offset, hash type
   (`NONE`, `2_TUPLE`, `5_TUPLE`).
3. `CC_PF_INGRESS_DATA_Q_ADD` - per queue, and this one carries **two** rings: the receive ring
   and its buffer pool, each with its own address and index offsets, plus the buffer size and the
   MSI-X vector id.
4. `CC_PF_EGRESS_TC_ADD` - per class: queue count and queues per DMA engine.
5. `CC_PF_EGRESS_DATA_Q_ADD` - per queue: address, index offsets, length, WRR weight, class,
   MSI-X id.
6. `CC_PF_INIT_DONE`.
7. `CC_PF_ENABLE`, on interface up (`giu_nic.c:1006`), separately from init.

## The descriptors

All three are `#pragma pack(1)`. Transmit and receive are 32 bytes; a buffer-pool entry is 16.

```
struct agnic_tx_desc             32 bytes
  +0x00  u32  flags             l3_offset:7 | ip_hdr_len:5 | csum disables | l3/l4 type | SG mode
  +0x04  u8   pkt_offset
  +0x05  u8   res4:6 | vlan_info:2      NONE / SINGLE / DOUBLE
  +0x06  u16  byte_cnt
  +0x08  u16  res5
  +0x0a  u16  num_seg_ent
  +0x0c  u32  res6
  +0x10  u64  buffer_addr
  +0x18  u64  cookie            host-private, returned on completion

struct agnic_rx_desc             32 bytes
  +0x00  u32  flags             l3_offset | ip_hdr_len | l4 status | pool_idx | l3/l4 info
  +0x04  u8   pkt_offset
  +0x05  u8   info              vlan_info:2 | l2_info:2 | l3_info:2
  +0x06  u16  byte_cnt
  +0x08  u16  port_num          see the open question below
  +0x0a  u16  num_sg_ent
  +0x0c  u32  timestamp_hashkey
  +0x10  u64  buffer_addr
  +0x18  u64  cookie

struct agnic_bpool_desc          16 bytes
  +0x00  u64  buff_addr_phys
  +0x08  u64  buff_cookie
```

Checksum offload is real in both directions: the transmit descriptor carries explicit
*disable* bits (`GEN_L4_CSUM_NOT`, `GEN_IPV4_CSUM_DIS`), so the default is to offload, and the
receive descriptor reports `DESC_CHECK_OK`, `DESC_ERR_CHECKSUM_ERR` or
`DESC_ERR_CHECKSUM_UNKNOWN`.

## Three defects the vendor fixed after publishing

Sophos ships patches against this source. Three of them are real bugs a port must not reproduce,
and they are worth more than their size suggests because each one is a mistake the original
authors made and did not catch.

**Transmit completion read the wrong index** - `0010-giu_tx_stuck_skb_fix_v2.patch`.
`agnic_tx_done_handle_ring` decided how much was left to reclaim from the **consumer** index when
it should have used the **producer**:

```c
-	cons_idx = readl(ring->consumer_p);
-	rem = AGNIC_RING_NUM_OCCUPIED(cons_idx, ring->tx_next_to_reclaim, ring->count);
+	prod_idx = readl(ring->producer_p);
+	rem = AGNIC_RING_NUM_OCCUPIED(prod_idx, ring->tx_next_to_reclaim, ring->count);
```

The patch is named for the symptom: transmit stops.

**Receive descriptors had no way to say "untouched"** - `0011-giu_nic_cookie_init.patch`. Every
receive descriptor's `cookie` is now stamped with `AGNIC_COOKIE_DRIVER_WATERMARK` when the ring is
allocated, and the receive path rejects `cookie == 0`. Without it the host cannot distinguish a
descriptor the device filled from one nobody ever wrote - the same problem, in a different place,
as trusting an index that came back from shared memory.

**Status returned uninitialised** - `0027-pcie_ep_armada-giu_nic-SQ-fixes.patch`. `ret` was
returned unset on paths through `agnic_bpool_refill_rx_buffs` and `agnic_cap_config`.

And one that was never fixed, only commented, at `giu_nic.h:85`:

```c
/*
 * TODO: WA Changed AGNIC_TX_DONE_THRESHOLD from 64 to 8
 * due to a tx stops transmitting after 12 packets
 * Must be investigated and be fixed
 */
#define AGNIC_TX_DONE_THRESHOLD		64
```

The comment describes a workaround that is not in the value beneath it - either it was reverted
and the note left behind, or it was never applied. Twelve packets is suspiciously close to the
transmit stall the patch above fixes, so these may be the same bug seen twice. Either way, a port
should not copy this threshold without knowing which.

## The sizes, since they are not in the wire header

From `giu_nic.h:64` onward: the config BAR is `AGNIC_CONFIG_BAR_ID = 0`, `AGNIC_CONFIG_BAR_SIZE =
64 KB`. Up to `AGNIC_MAX_TC = 8` traffic classes, 128 receive queues, 128 transmit queues, 128
buffer pools and 2 management queues - so `AGNIC_MAX_QUEUES` is 386, and the index array is 1544
bytes of BAR0 at `dev_use_size`. The default transmit ring is `AGNIC_DEFAULT_TXD = 2048`
descriptors. `AGNIC_COOKIE_DRIVER_WATERMARK` is `0xdeaddead`.

## The open question, and it is the whole point

**How the fourteen front ports are told apart.**

The receive descriptor has `port_num` at +0x08, and this host driver never reads it - there is no
reference to the field anywhere in `giu_nic.c`. The driver registers exactly one netdev and
treats the link as a single trunk, which matches the reference kit's behaviour and its MTU-1500,
roughly-936-Mbps ceiling.

The harvested port map shows the other half: `Port1`-`Port12` plus `PortF1`/`PortF2`, all children
of `mv-pcimux0` on the coprocessor, **each with a VLAN 4095 subinterface**. VLAN 4095 is reserved
in 802.1Q and cannot itself distinguish fourteen ports, so either the port identity is in
`port_num` and the tag is a marker, or the coprocessor is configured to map each port to a
different tag and 4095 is a default this capture happened to show.

This cannot be settled from the host driver, because the host driver does not participate. It has
to come from the coprocessor side - `mv_nwa_host` and the NPU's own configuration - and that is
the next thing to read. Until it is settled, an implementation can carry traffic on one trunk but
cannot present fourteen interfaces, and presenting fourteen is the point of the exercise.

## Also still open

- Whether the four `giu` doorbells are live, or dead the way the mvmgmt one is. pcinet forced
  `dbell_nr = 0` on both paths and polled instead; nothing yet shows whether AGNIC does the same,
  and it matters because `CC_PF_INGRESS_DATA_Q_ADD` carries an `msix_id` per queue, which suggests
  it does not.
- Whether the coprocessor's side agrees with the host's idea of how many queues exist. The host
  reserves for the maximum; what the device actually accepts at `CC_PF_INIT` is untested.
- Whether scatter-gather is worth implementing. `CC_GET_CAPABILITIES` reports a `CAPABILITIES_SG`
  flag and the descriptors have three SG modes, but a first implementation should ask for none of
  it.
