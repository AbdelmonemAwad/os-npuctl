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

## How the fourteen ports are told apart

**A two-byte port identifier prepended to every frame, in network order, in front of the Ethernet
header.** Not the descriptor's `port_num` field, and not a VLAN tag.

`giu_nic.c` does not demultiplex at all. It includes `if_pport.h` and hands every received frame
straight to another module, and the error path is what gives the format away - `giu_nic.c:1714`:

```c
	if (unlikely(false == (pport_do_receive(skb)))) {
		agnic_dev_warn("pport receive error port_id(0x%08x)\n",
				ntohs(*(__be16 *)skb->data));
```

`skb->data` is the start of the received frame and the driver reads a `__be16` from it and calls
it `port_id`. The `port_num` field in the receive descriptor is never read: there is no reference
to it anywhere in `giu_nic.c`.

### pport

The module on the other side of that call is `mv_pport`, and it is an ordinary rtnetlink link
type stacked on a real device - `alias: rtnl-link-pport`, exactly the shape of the VLAN driver.
Fourteen `pport` netdevs sit on the one GIU trunk. It is GPL, its source is not in the drop, but
the module is on the appliance and its interface is legible from it:

```
exported   pport_do_receive          the receive hook giu_nic calls
           register_pport_device     create one
           pport_get_port_info
           pport_dev_pport_id
           pport_link_ops            the rtnl link ops
           cust_set_ops              where the per-port hardware operations get registered
imported   skb_push                  transmit prepends
           dev_queue_xmit            and hands the tagged frame to the real device
           eth_type_trans            receive re-parses after stripping
```

`pport_%.4x` as a name format, and `PPORT_NAME_TYPE_PLUS_PORT_ID` as a naming mode, put the width
beyond doubt: sixteen bits.

### The frame format, and it is sixty-six bytes of overhead, not two

Reading `pport_dev_hard_start_xmit` and `pport_do_receive` out of the module settles both
directions, and the answer is bigger than the tag:

```
+0x00   u16   port_id, network order
+0x02   64 bytes of metadata
+0x42   the Ethernet frame
```

Transmit, taking the branch for `pport_cust == NULL` - no custom operations registered, which is
the case a port starts from:

```asm
2361:  mov    $0x40,%esi
236f:  call   skb_push              ; 64 bytes of metadata, filled with a ramp:
228e:  ...    mov %cl,(%rdx,%rax,1) ;   byte i = (i - 64) & 0xFF, so 0xC0 0xC1 .. 0xFF
22a4:  mov    $0x2,%esi
22ac:  call   skb_push              ; then two more in front of that
22b1:  movzwl 0x800(%rbp),%eax      ; the port id, 16 bits, out of the pport's private area
22c2:  rol    $0x8,%ax              ; byte-swapped, so network order
22c6:  mov    %ax,(%rdx)            ; written at the front
```

Receive, same condition, at `0x17b`:

```asm
 a1:  movzwl (%rax),%esi            ; 16 bits at skb->data
 a4:  rol    $0x8,%si               ; ntohs
 af:  call   _pport_find_dev        ; (real_dev, port_id) -> the pport, or NULL
 b4:  test   %rax,%rax; je -> false ; NULL is what makes giu_nic print its warning
 ed:  addq   $0x2,0x110(%rbx)       ; skb->data += 2      - the tag
17b:  mov    $0x40,%eax             ; no custom ops: strip the whole metadata area
138:  add    %rax,0x110(%rbx)       ; skb->data += 64     - the metadata
162:  call   eth_type_trans
```

Sixty-six bytes each way, and symmetric. The earlier version of this page said two, inferred from
`skb_push` being imported; that was right about the tag and wrong about the frame, and the
difference is the sort that produces a link which passes a ping and corrupts everything larger.

With custom operations registered - which is what Sophos's NetAgent does on the shipped system -
the callback returns how many of those 64 bytes it used, and pport pushes or pulls only the
remainder. The 64-byte area is fixed; its contents are NetAgent's business.

Two things not to misread. `movl $0xABBACAFE,0x50(%rbx)` in the transmit path writes into the
`sk_buff` structure, not into the packet - the data pointer is at `0x110` - so it is a marker on
the buffer and not a field on the wire. And the transmit descriptor's `MD_MODE` flag, bit 22, is
almost certainly what tells the device this metadata area is present; that is a hypothesis with
the name on its side, not something read out of the code.

**The risk worth naming**: on the shipped system `pport_cust` is always registered, so the
no-custom-operations path above may never execute in practice. An implementation that sends 64
bytes of `0xC0 0xC1 ...` filler is relying on a branch the vendor may never exercise. If the far
side rejects it, the metadata format has to come out of `mv_nwa_host` before the datapath can
work at all.

### And VLAN 4095 is not it

An earlier reading of the harvested port map took the VLAN 4095 subinterface present on every
port to be the discriminator. It is not: 4095 is reserved in 802.1Q, it is the same on all
fourteen, and it therefore cannot distinguish them. Whatever that subinterface is for, the port
identity is the two-byte tag.

### What is still not published

Per-port **control** - link state, speed, duplex, MTU, MAC address, promiscuous mode,
statistics - does not ride the tag. `mv_pport` exposes `cust_set_ops` so that another module can
register a `struct pport_hw_ops`, every entry of which takes a `u16 port_id`:

```c
struct pport_hw_ops {
	int (*state_set)(u16 port_id, int state);
	int (*state_get)(u16 port_id, bool *state);
	int (*cached_state_get)(u16 port_id, bool *state);
	int (*mtu_set)(u16 port_id, u32 mtu);
	...
};
```

That module is `mv_nwa_host`, Sophos's NetAgent, and it reaches the coprocessor over the AGNIC
command channel as a `CDT_CUSTOM` client. The transport is published - `giu_custom_mgmt.c` is
`agnic_register_custom()` and `agnic_send_custom_msg()`, a generic pipe with a callback. The
message set that rides it is not: `mv_gnic_custom_mgmt.h` is included by that file and is absent
from the drop, and `mv_nwa_host` is a binary.

**This splits the work cleanly.** Fourteen interfaces that carry traffic need only the trunk and
the two-byte tag, both of which are specified here. Fourteen interfaces whose link state, speed
and MTU can be read and set need the NetAgent message set, which has to be recovered from
`mv_nwa_host` the way the MCP2210 command map was recovered from `xgs-usb-spi-flash`. The first is
worth having on its own; the second is a separate piece of work.

## The channel in numbers

Both management rings are **256 entries of the 64-byte descriptor**, 16 KiB each, in host memory
(`AGNIC_CMD_Q_LEN`, `giu_nic.h:107`; `AGNIC_NOTIF_Q_LEN` is defined as the same). Their four index
words are the first four slots of the BAR0 index array - the command ring takes 0 and 1, the
notification ring 2 and 3, and the data rings take what is left in allocation order.

The command ring's cookie table is **1024 entries, deliberately not 256** (`giu_nic.c:1909`):
`cmd_idx` is a free-running counter modulo 1024 that skips 0 and `0xFFFF`, so a tag is reused only
after 1023 further commands. A response is matched **by that tag alone** - nothing else in the
descriptor is checked against the command that produced it.

**There is no interrupt on this channel at all.** The management IRQ hooks are empty stubs; a
per-CPU timer, armed on one designated CPU, drains the notification ring, and the same loop
handles responses, asynchronous notifications (`cmd_idx == 0xFFFF`) and custom in-band messages.
Every bring-up command fits in the 56 inline bytes, so the multi-descriptor path is dead code in
this tree - which is worth knowing, because it is also the path with the worst bugs.

Two waits, both longer than their comments suggest: `DEV_READY` is polled for up to **10 to 20
seconds** at probe, before a single other field of the config structure may be read; `DEV_MGMT_READY`
for **1 to 2 seconds** after the rings are published, under a comment that says "~1 second".

## Writing a port against this: what not to copy

Four independent readings of `giu_nic.c`, `giu_nic_mgmt.c` and `giu_custom_mgmt.c` produced
eighty-six of these. What follows is the part that changes how a FreeBSD driver has to be written,
rather than the part that is merely untidy.

### There is not one memory barrier in four and a half thousand lines

No `wmb`, no `dma_wmb`, no `rmb`, no `smp_*`, no `barrier()` anywhere in `giu_nic.c`. The only two
in the whole tree are in `giu_nic_mgmt.c`. Every ring hand-over is

```c
	/* ... write the whole descriptor ... */
	writel(ring->tx_prod_shadow, ring->producer_p);
```

and the only thing ordering those descriptor stores before the doorbell is the `__iowmb()` that
Linux hides inside `writel()`. **FreeBSD's `bus_space_write_4` makes no such promise.** Transcribed
literally, the device fetches descriptors the host has not finished writing, and the failure looks
like corrupted packets rather than like a missing barrier.

A port needs an explicit release barrier between the descriptor writes and the index write, in
every direction, on every ring - and an acquire barrier after reading a device-written index and
before reading the descriptor it refers to. That second one the vendor does not have either.

### MMIO is handled as ordinary memory, and on FreeBSD it cannot be

`nic_cfg_base` is a `struct agnic_config_mem *` assigned from a `void __iomem *` with the
annotation silently dropped (`giu_nic.c:4355`). So the driver does `nic_cfg->status & DEV_READY` in
a spin loop with no `readl`, `memcpy(dev_addr, nic_cfg->mac_addr, 6)` **out of** MMIO,
`cmd_q_info->q_addr = ...` **into** MMIO, and `nic_cfg_base->status |= HOST_MGMT_READY` as a
non-atomic read-modify-write over PCIe.

The index array is the same: `memset(ring_indices_arr, 0xFF, size)` over a BAR, and the driver is
inconsistent about its own rule - `readl(ring->producer_p)` at `giu_nic_mgmt.c:427` and a bare
`*ring->producer_p` at `:494`, for the same register. The bare one can be hoisted out of a loop and
spin on a stale value.

None of this translates. Every one of these is `bus_read_4` / `bus_write_4` /
`bus_space_set_region_4` in a port, and there is no way to keep the plain-C style.

### Everything the device says is trusted, and some of it is a subscript

This is the same class of defect this project has now found four times, and it is here in three
more places:

- **`if (cmd_idx > cmd_ring->cookie_count)`** should be `>=`. `cookie_list` has exactly
  `cookie_count` entries. A device that puts `cmd_idx == 1024` in a response descriptor indexes one
  element past a `kmalloc`ed array and the driver then **writes through the pointer it finds there**.
- **`agnic_tx_done_handle_ring` walks to the device-supplied consumer index with no clamp** against
  the host's own producer. A device index that leads the producer frees cookies that were never
  populated - and that is the actual mechanism behind vendor patches 0014, 0015 and 0031, which
  chase the symptom rather than the cause. Clamp to your own producer and treat anything past it as
  a device fault.
- **`dev_use_size` is a device-supplied `u32` that decides where the index array lands in BAR0.**
  The bounds check is against `AGNIC_CONFIG_BAR_SIZE` (64 KiB) while the GIU window is 16 KiB with
  the MSI-X table at +4 KiB, so a device reporting `dev_use_size >= 4096` puts 1544 bytes of index
  array on top of the MSI-X table and the check passes.

And the cookies themselves are **raw kernel virtual pointers on the wire** - `desc->cookie =
(u64)tx_buf`, `desc->buff_cookie = (u64)cookie`. Any corruption on the far side becomes an
arbitrary kernel-pointer dereference here. Use a ring index or a bounded handle table.

### The validity scheme is a heuristic, and the wire format has no ownership bit

There is no DONE or OWN bit in the descriptor (`giu_nic_hw.h:286`). Whether the device has filled a
receive descriptor is decided by **magic values** - `0xcafecafe` in `buffer_addr`, and the
`0xdeaddead` cookie watermark that patch 0011 added afterwards. The code's own comment calls it a
workaround for a "DMA reordering issue".

A fresh implementation has to poison the ring the same way and check the same values. There is
nothing else to check.

### `msix_mode=3` does not work, whatever the module parameter says

`agnic_request_msix_irqs` returns the `start_vector` it was passed rather than the next free
vector, so with both directions enabled the transmit handlers are requested on the same doorbell
indices as the receive handlers, `request_irq` without `IRQF_SHARED` fails `-EBUSY`, and open
fails. The same confusion reaches the device: `mv_get_msi_id` is called with the same
`q_vector->v_idx` for both the ingress and the egress queue-add commands, so the device is told one
MSI-X id for two directions. A port needs a distinct vector index per direction.

Also: when receive MSI-X is off, `poll_timer_rate` is **zero**, and the per-CPU timer re-arms for
the current tick forever, on every CPU present. A FreeBSD `callout` with a zero delay does the same
thing and is worse behaved.

### Lifecycle: once per module load, and the device is never told anything

`static bool first_time` inside `agnic_net_open` gates all ring, buffer-pool, interrupt and
hardware-queue setup. It is **module-scope, not per-device**, so a second instance never allocates
its rings, and an interface taken down and brought back up reuses the old BAR index slots and the
old descriptor contents - which is a plausible route into exactly the stale-index failures the
vendor's patches chase. On FreeBSD, where `if_init` is called repeatedly and by the stack itself,
translating this literally guarantees the bug.

Any failure in open sets `AGNIC_FATAL_ERROR`, which nothing clears except probe, so one failed
`ifconfig up` bricks the interface until the module is reloaded.

And teardown tells the device nothing at all. `agnic_destroy_hw_queues()` is a stub that prints
"Not implemented"; `CC_PF_CLOSE` is defined and never sent anywhere in the tree;
`HOST_MGMT_READY` is never cleared and the `q_addr` fields are never zeroed - and then
`dma_free_coherent` hands back exactly the rings the device was told to use. `CC_PF_DISABLE` is
sent with no response buffer, so the host does not even learn when the device stopped.

This is the same mistake the sibling `pcinet` driver makes, which vendor patch 0005 fixes there and
which `npumgmt.c` in this repository refuses to make. It must not be carried into the GIU driver.

### A timed-out command leaves a pointer to a dead stack frame

Every caller passes a `struct agnic_mgmt_cmd_resp` **on its own stack** as the response buffer. On
the five-second timeout path the handler returns without clearing `mgmt_buff->buf`, so the cookie
slot stays busy forever *and* a late response makes the timer softirq `memcpy` up to 56 bytes into
a stack frame that no longer exists.

Two related ones in the same file: `static u16 cmd_idx, desc_required, desc_free, desc_idx;` - only
the first is meant to be static, and the other three are a race waiting for a second CPU; and
`spin_lock_bh` protects the send path only, while the notification handler mutates the same cookie
entries from a timer on another CPU.

### Linux shapes with no FreeBSD equivalent

NAPI, `netdev_alloc_frag`/`build_skb`, `skb_shared_info` in the buffer tail, GRO, `rtnl` (which the
synchronous send path unconditionally drops and retakes, making `rtnl` a precondition of every
command that wants a response), tasklets, and `del_timer` without `_sync`. The FreeBSD shapes -
taskqueues or iflib, mbuf external storage with a free callback, `callout_reset_on`, `counter(9)`,
`tcp_lro`, per-queue mutexes - are different enough that a literal port produces nonsense.

## Bring-up, in the order the code actually requires it

1. Get the facility window; **poll `DEV_READY`** before reading any other config field.
2. Claim the index array at `dev_use_size`, after bounds-checking it yourself.
3. Allocate the command ring, then the notification ring.
4. Write both `agnic_q_hw_info` blocks, **barrier**, set `HOST_MGMT_READY`, poll `DEV_MGMT_READY`.
5. Start whatever drains the notification ring - **before** sending any command that wants a
   response, because nothing else completes one.
6. `CC_GET_CAPABILITIES`, whose answer decides the queue counts.
7. Then, and only then, register the interface.

On the first interface-up: allocate transmit rings, receive rings, buffer-pool rings; set up
interrupts (the queue-add commands report the vector id per queue, so the vectors must exist
first); then `MGMT_ECHO`, `PF_INIT`, `INGRESS_TC_ADD` per class, `INGRESS_DATA_Q_ADD` per queue,
`EGRESS_TC_ADD` per class, `EGRESS_DATA_Q_ADD` per queue, `INIT_DONE`; then fill the buffer pools;
then `CC_PF_ENABLE`.

The buffer pool is published to the device while it is still empty, and filled afterwards. Fill it
before enabling, write the consumer index, and write the producer index **last** - the one-slot gap
between them is what keeps full and empty distinguishable.

## Also still open

- ~~Whether the four `giu` doorbells are live.~~ **They are.** `mv_giu_drv` takes a module
  parameter `msix_mode` - "MSI-X Mode (Disabled=0, Rx=1, Tx=2, Rx & Tx=3)" - so unlike pcinet,
  which forces `dbell_nr = 0` and polls, this facility does real interrupts and they can be
  turned off per direction. Its other parameters are worth knowing before implementing: `num_tcs`,
  `num_qs_per_tc` ("2 and above means RSS is enable"), `rss_mode` (2-tuple or 5-tuple),
  `default_queue` for non-IP frames, `cpu_mask`, and `feature_enable` whose bit 0 is keep-alive.
  Sophos loads it with `num_qs_per_tc=4`.
- Whether the coprocessor's side agrees with the host's idea of how many queues exist. The host
  reserves for the maximum; what the device actually accepts at `CC_PF_INIT` is untested.
- Whether scatter-gather is worth implementing. `CC_GET_CAPABILITIES` reports a `CAPABILITIES_SG`
  flag and the descriptors have three SG modes, but a first implementation should ask for none of
  it.
