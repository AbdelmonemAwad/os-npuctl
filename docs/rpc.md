# The control channel, and the tables that make receive work

Transmit needs nothing from the coprocessor: the host writes a port tag in front of a frame and
the frame leaves that port. Receive is the other way round — the coprocessor's fastpath has to
decide which host interface an arriving frame belongs to, and nothing tells it until this channel
does. The facility is `rpc`, at BAR2 offset `0x2000`, one megabyte, with the endpoint's single
host-to-target doorbell.

Unlike every other facility here, almost none of this was inferred. Sophos ships `usfp_rh.ko` —
their own target-side handler, not Marvell's sample — built with `-g3`, so the compiler recorded
every structure, field name, offset and `#define`. `readelf --debug-dump=info` reads them straight
back out. Where a claim below comes from the disassembly of that module rather than from its debug
information, it says so; where the vendor's own boot log settles something, it is quoted.

## What the channel is

A DMA ring, not a mailbox. The host writes a command into its **own** memory, puts a sixteen-byte
descriptor carrying that physical address into a ring inside the BAR, advances a producer index
and rings the doorbell. The target fetches the command by DMA, runs it, writes the answer back by
DMA and advances a consumer index. There is no doorbell in the other direction, so the host polls.

```c
struct rpc_state {                  /* at the start of the window */
    u32 cfg_magic;                  /* +0   the host writes 0xD7D3AB00 to open   */
    u16 cfg_revision;               /* +4                                        */
    u8  active_hi_rings;            /* +6                                        */
    u8  reconfig_done;              /* +7                                        */
    u64 zero_pad[8];                /* +8                                        */
    struct rpc_ring ring_lo;        /* +72  = 0x48                               */
    struct rpc_ring rings[];        /* +104 = 0x68                               */
};

struct rpc_ring {                   /* 32 bytes */
    u64 posted;  u64 done;
    u32 ring_offset;  u32 desc_offset;  u32 desc_count;
    union ring_hw_cfg r_cfg;        /* u8 ring_num, f_index, dbell, shared */
};

struct rpc_cmd_bar_desc {           /* 16 bytes, in the WINDOW at desc_offset */
    u64 dma_buff_addr;              /* a HOST physical address */
    u16 payload_len;  u16 flags;  u32 reserved;
};
```

`posted` and `done` are running counts, not flags. The target takes the descriptor at
`done & (desc_count - 1)` and then increments `done`, so a command's slot is fixed by its sequence
number and `desc_count` must be a power of two.

## Opening it, in two steps

The first eight bytes are **one field** to the target: `irq_handler` compares the whole quadword
against its cached copy to decide whether the configuration changed. So it is written as one
64-bit store, and twice — zero first, then the real value:

```
rpc_handler: updating ring configuration: 0
rpc_handler: updating ring configuration: 1015fd7d3ab00
rpc_handler: Allocated DMA dev #0 to hi ring #1 on doorbell 3.0
```

That is the **vendor's own host** in `npu-dmesg.txt`. The quadword decodes as magic `0xd7d3ab00`,
revision `0x015f`, **one active high ring**, `reconfig_done` clear. The zero first tears down
whatever the target was holding, so the second is taken from a known state.

`reconfig_done` is the handshake: the target clears it while it takes a configuration and sets it
when it has. Waiting for it to be non-zero *without having cleared it* measures nothing — the
state the channel starts in satisfies that immediately.

## The high ring, and why declaring none livelocks the coprocessor

This is the expensive one.

The `rpc` facility on this board has **exactly one** host-to-target doorbell
(`npu-facility.txt`: `3) rpc : type 3 dbell [h2t=1 t2h=0]`), and the target reads that as meaning
high ring zero shares it. On every doorbell that is not a configuration change, `irq_handler`
schedules that ring's tasklet — **whatever the host declared**.

Declare no high rings and `refresh_cfg` skips all high-ring setup, so `rh_ctx[0]` stays as
`kzalloc` left it: facility 0, doorbell 0. Facility 0 is `ctrl`, which has no host-to-target
doorbells at all, so `mv_dbell_enable` fails with `EINVAL` — and the failure path reschedules the
tasklet **unconditionally**. It re-arms itself forever, printing two un-ratelimited errors per
pass to a 115200 console, on the one core `isolcpus=1-3` leaves for housekeeping, which is also
the only core that can run the low ring's work item.

The symptom is that the channel answers exactly one command and then stops, and that an hour later
the whole BAR window reads `0xffffffff`. It looks like a crash. It is a livelock, and it is
recoverable only by a cold power cycle.

So this driver declares high ring zero, sharing this facility's doorbell, with its indices equal
and its descriptor area empty. The target's ring walker returns immediately when `posted` equals
`done`, so the ring carries nothing — it exists so that the tasklet has a doorbell it is allowed
to enable.

## Two fields that were wrong for a long time

Both came from reading the names rather than the code.

**`RPC_DESC_POST_FLAG` does not mean "this descriptor is posted."** It means *post only, do not
answer*. `process_ring` branches on bit 0 of `flags` and calls the routine that writes a response
only when it is **clear**. With it set the target runs the command and advances `done` without
ever writing anything back — which is why the answer buffer still held what the driver had put in
it, and why `rc` read back as the driver's own `resp_buff_sz`.

**`payload_len` is the payload alone.** The target adds the eight-byte command header itself when
it sizes the fetch, so including it made every fetch eight bytes too long.

## Filling in the tables

Two commands per front port, and nothing else — there is no enable bit, no start command, no
queue index anywhere in either of them.

```
RPC_CMD_LIF_ADD_UPDATE (3), 18 bytes:
    u32 index = (iface_id << 12) | vlan_id
    u8  my_mac[6]
    u16 mtu                 not range checked by the target
    u16 flags               bits 0-1 fwd_mode, bit2 admin_disabled, bit3 offload_disabled
    u16 pad
    u16 update_mask         exactly 0x00FF to create

RPC_CMD_PPORT_UPDATE (5), 4 bytes:
    u8  iface_id
    u8  rsvd
    u16 pport_tag           LITTLE endian here
```

The tag is **little endian in this payload and big endian in the datapath frame header**. Same
number, written the other way round, and nothing warns.

`offload_disabled` is the field that matters. The fastpath's validation chain ends
`entry.offload_disabled == 1 -> HOST`, so setting it hands every frame to this end rather than
letting the coprocessor route — which is the whole point of running the firewall on the host.
`fwd_mode = 1` (L2) is chosen for the same reason and one more: the destination-MAC check only
runs in L3 mode, so in L2 the address above never gates anything.

Silent killers, all of them DROP rather than punt: `fwd_mode == 0`, `admin_disabled == 1`, and a
frame longer than `mtu`.

Creating is **not idempotent** — `update_mask 0x00FF` on an entry that is still valid is refused
with `rc 1` — so the driver reads before it writes.

## Why programming retries, and what it is racing

The coprocessor's own userspace fastpath starts **in response to this host's handshake**, and as
part of its startup it zeroes the entire logical-interface table — about thirteen seconds after
the handshake on this board. The driver opens this channel a fraction of a second after that same
handshake, so its first pass writes into a table that is about to be cleared.

Every command is accepted and answered `rc 0`, and nothing survives. The driver printed
`14 of 14 interfaces made` over an empty table for an hour before this was understood.

The port bindings *do* survive, because the fastpath maps those two tables and clears neither.
That asymmetry is what made it look like a driver bug.

So programming runs on its own thread — not a callout, because every command waits for an answer
and a callout may not sleep — and it believes only the read-back:

```
[57] rpc: channel open - revision 351, 1 high ring sharing doorbell 0
[59] rpc: all 14 front ports have an interface and a binding, read back and confirmed
[61] rpc: the coprocessor's fastpath cleared the interface table during its own startup
[61] rpc: all 14 front ports have an interface and a binding, read back and confirmed
```

Verify with `RPC_CMD_LO_LIF_READ` (37), whose request is
`{u32 s_index, u16 num_entries, u16 flags, u32 e_index}` and whose entries are twelve bytes:
`my_mac[6]`, `u16 mtu`, `u16 flags`, `u16 reserved`. `s_index` is a **LIF index**, not an
interface number — `(iface_id << 12) | vlan`.

## Sending one by hand

The channel defines forty-five commands and this driver uses three. The rest have to be learned,
and the only way to learn a payload is to send one and read the answer:

```sh
sysctl dev.npuep.0.rpc_channel.command="25 00 00 00 00 01 00 00 00 00 00 00 00"
sysctl -n dev.npuep.0.rpc_channel.command
```

The first number is the command, the rest are payload bytes. It does **not** refuse commands that
write — it cannot, because the commands worth learning are the ones that fill in these tables — so
a mistyped command number is a real command and the fourteen front ports are downstream of it. It
logs what it is about to send before sending it.
