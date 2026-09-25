# The NetAgent mailbox, and per-port control

Bringing a front port up, setting its MTU, reading its link state - none of that goes through
[GIU](giu.md). It goes through a separate facility, `nwa`, served on the coprocessor by Sophos's
NetAgent daemon, and its message set is published nowhere: the header the vendor's own source
includes is absent from the GPL drop, and both ends are binaries.

What follows was read off a running system, with ports being toggled one at a time and the shared
window polled in a tight loop. It is enough to describe the messages. It is **not** yet enough to
send one - see the open question at the end.

## Where it lives

`nwa` is at **BAR0 + 0x4000, 64 KB**, and it has no doorbells in either direction. The
coprocessor's own facility dump is unambiguous:

```
2) nwa : type 2 dbell [h2t=0 t2h=0] dma eng 0 flags=0x0
```

Both sides poll. Which means a packet capture sees nothing at all: `tcpdump` on the management
interface across twenty seconds of live port activity caught **zero packets**. The protocol is
not IP, and it is not the AGNIC command ring either.

The window is device memory, so it can be read from the host even on a kernel that refuses
`/dev/mem` for ordinary RAM.

## The header

```
+0x00  u32  cookie        0xCAFEBABE
+0x04  u32  0x34          command mailbox length
+0x08  u32  0x7fcc        event buffer length
+0x0c  u32  0x8000
+0x10  u32  0x8000
```

The names come from the host module's own log strings - `Initialized cmd_mbox_len(%zu),
evt_buf_len(%zu)`, and a version check behind `HOST-TARGET MailBox versions are different`. The
two `0x8000` values are most likely the boundary between the two halves of the 64 KB window;
`0x8000 + 0x7fcc` is `0xFFCC`, which lands just inside it.

These words are present and correct as soon as the coprocessor's side is up. A host that finds
them zero should wait rather than write them - the module's `waiting for facility config
availability` is doing exactly that.

## The message

Only thirty-nine bytes in the whole 64 KB change during port activity, all of them in the first
0x60:

```
+0x18  u8   turn      0x01 when a request is present, 0x02 once it has been answered
+0x1c  u8   class     0x88 periodic status, 0x20 port operation
+0x34  u32  op        0x03 set, 0x04 query, 0x45 status
+0x38  u32  sub       see below
+0x3c  u32  port_id   and on the answer this field carries the port INDEX instead
+0x44  u32  payload0
+0x48  u32  payload1
+0x5c  u32  speed on a link answer, 0x3e8 = 1000 Mb/s
```

| op | sub | meaning | how it was confirmed |
|---|---|---|---|
| 0x03 | 0x00 | set administrative state | payload0 = 2 on down, 0 or 1 on up |
| 0x03 | 0x02 | set MTU | payload0 = `0x2328` = 9000, and `0x5dc` = 1500 on a port whose MTU is 1500 |
| 0x03 | 0x03 | set MAC address | payload0/1 hold the six bytes, and they matched the toggled port's own address every time |
| 0x03 | 0x4a | multicast or filtering | payload0 contained `0x3333` |
| 0x04 | 0x04 | query link | the answer carries the speed |
| 0x45 | 0x10 | periodic status | class `0x88`, roughly every 1.1 s |

The MAC is the strongest evidence in the whole exercise: every port toggled produced an
`op=03 sub=03` carrying that port's own hardware address and no other's.

## The port identifiers

**`port_id = 0x8000 + N * 0x100`** for the switch ports, confirmed one port at a time:

| toggled | port_id | answer field |
|---|---|---|
| port 1 | `0x8100` | `0x0001` |
| port 3 | `0x8300` | `0x0003` |
| port 7 | `0x8700` | `0x0007` |
| first fibre port | `0x8900` | `0x0009` |
| second fibre port | `0x8A00` | `0x000a` |

The numbering is sequential by **physical position** - not the printed label and not the internal
port index. The eight copper ports are 1 to 8, then the two fibre ports are 9 and 10.

Only ports with a cable in them are polled with `op=04 sub=04`. That is a useful signal in itself:
the coprocessor does not ask about a port with no carrier.

**Four of the fourteen are not switch ports at all.** The coprocessor's own interface list shows
four `mvpp2` interfaces carrying the addresses of four of the front ports, so those are SoC ports
reached directly rather than through the switch. That is what the table in the window has been
saying from the start - five SoC entries numbered `0` to `4`, then the `0x8x00` block - and what
NetAgent's own log says: `soc_ports=5, switches=1`. Their identifiers are most likely the small
SoC indices, not a continuation of the `0x8x00` sequence, and that has not been confirmed by
toggling one.

## What the switch is

NetAgent names it when it refuses a request: `FEC not supported on Soho ports`. It is a Marvell
Soho-family switch, driven on the coprocessor by `UMSD_NPU` over MDIO - the register trace in its
log shows the two global register banks. `UMSD_NPU` is also what drives the front-panel link
LEDs, which is why they stay dark until a port is brought up through this mailbox: the LEDs are
not on the host's side of the link at all.

## The transaction

The field map above came from watching the window change. It was correct, and messages built from
it were still ignored, because two things were missing - both of them plain in the host module's
own code.

```
    wait for  +0x20 == 0                the previous transaction has finished, 1 s
    write     +0x1c  = request length   in bytes, unrounded
    write     +0x34  = the request body
    write     +0x18  = 1                the signal, and the LAST write
    wait for  +0x20 == 1                the reply is in the window, 30 s
    read      +0x24  = reply length
    read      the reply body at 0x34 + request_length
    write     +0x18  = 2                acknowledge
```

**`+0x20` is the target's status register**, and nothing else is. Polling `+0x18` waits forever
on a field the host itself owns and the target never writes. And a request with no length at
`+0x1c` is a request the far side has no reason to look at.

There is no sequence number, no per-message magic, no checksum and no doorbell. Three stores into
the window is the entire host side of it.

Two constraints on the reply worth checking rather than assuming:

- the reply body starts at `0x34 + request_length` using the **unrounded** length, so a 32-byte
  request is answered at `+0x54`;
- its first word must be `0x14`, and the second is a status that must be zero. Anything else is a
  malformed reply and should be refused rather than parsed.

And one on the header: `+0x04` is both the version handshake and the body offset. The host waits
for it to read exactly `0x34` before it will use the window at all, and then uses that same value
as the offset it writes the body to. A different value means a mailbox this code does not
understand, which is what the module's `HOST-TARGET MailBox versions are different` is about.

`+0x08` is the maximum request length - `0x7fcc` here - and a request longer than that is refused
before the window is touched. `+0x0c` and `+0x10` describe an event buffer that the host module
reads once, logs, and then never uses again.

## Confirmed on the hardware

Ten ports, ten replies, `status = 0` on every one:

```
op=3 sub=0 port=0x8100 payload=1
ANSWERED  reply_len=8  hdr=0x00000014  status=0x00000000
```

The front-panel link LEDs came on.
