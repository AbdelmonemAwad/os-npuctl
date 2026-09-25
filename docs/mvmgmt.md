# mvmgmt0, the management interface

A virtual Ethernet link between the host and the coprocessor. No wire: two rings in **host**
memory, which the coprocessor reaches through a linear outbound window.

Implemented in `contrib/npuep/npumgmt.c`. The link reaches `PCINET_LINK_ESTABLISHED` - the
coprocessor's own reply, which it sends only once it has accepted the ring addresses it was
handed - and `mvmgmt0` appears as an ordinary Ethernet interface.

This page is the specification that implementation was written against, extracted from
Marvell's GPL `pcinet` source and checked line by line. Where the source and its own
documentation disagree, the code wins and it says so.

## The three things that overturn the obvious guess

Each of these is the opposite of what a reasonable implementer would assume, and each was
confirmed by reading the source rather than by reasoning about it.

**The status bits are not an ownership protocol.** `Q_ENTRY_STATUS_HOST_OWN` (`0x80000000`) and
`Q_ENTRY_STATUS_FREE_SKB` (`0x01000000`) appear in exactly four places in the whole driver -
`pcinet.c:261`, `262`, `446`, `447` - and every one is an assignment. **Nothing reads them.**
Ownership is carried entirely by the two ring indices. An implementation that waits on
`HOST_OWN` before touching an entry waits forever.

**There are no doorbells.** The driver allocates one for this facility and then forces
`dbell_nr = 0` on both paths - `pcinet.c:1140` and `1152` - which sets `polling = 1` and
`rx_notify = 0`. Both sides poll a 10 ms timer and neither ever rings anything. It reads like a
debug hack left in the published source. `pcinet_tx_done` is consequently unreachable code, and
would corrupt the peer's index if it were revived as written.

This was the obvious thing to doubt - a vendor could patch those two lines for a release build
without touching what it publishes - so it was checked against the binary that actually ships on
the appliance. It is there:

```
  16d8:  call   mv_get_num_dbell
  16dd:  test   eax,eax                      only the error code is examined
  16df:  jne    <error>
  16e5:  mov    BYTE PTR [rbx+0xa89],0x1     polling   = 1, an immediate

  16fa:  mov    DWORD PTR [rsp+0x4],0x0      dbell_nr  = 0, plainly visible
  1702:  call   mv_get_num_dbell
  1720:  mov    BYTE PTR [rbx+0xa8a],0x0     rx_notify = 0, an immediate
```

The count the call returns is written to a stack slot and never read again; both flags are
stored as constants, which is exactly what the source compiles to once the override is folded.
The shipped driver and the published source agree, so a port should not implement a receive
interrupt for this interface at all.

`mv_request_dbell_irq` *is* still called, so a handler does get registered. It simply never
fires, because the far side has been told not to notify.

**Addresses must be below 2^36.** The coprocessor maps host memory through a single window of
64 GiB starting at bus address zero, and `facility_host.c:837` constrains the host side to match
with `dma_set_mask_and_coherent(DMA_BIT_MASK(36))`. On FreeBSD that is `lowaddr = 0xFFFFFFFFF`
in every `bus_dma_tag_create` - not 32-bit, and emphatically not `BUS_SPACE_MAXADDR`. An address
above the ceiling maps to nothing the coprocessor can reach, and what it writes instead is
undefined.

## The shared structure

40 bytes, at the start of the mvmgmt facility's 4 KB window. The host writes it; the target
reads it.

```
+0x00  u64  rx_q_phys      bus address of the host's RX control block
+0x08  u64  tx_q_phys      bus address of the host's TX control block
+0x10  u32  link_status    enum, 4 bytes
+0x14  u8   link_change    bool, then 3 bytes of padding
+0x18  u32  status
+0x1c  u32  reserved       present only to align the next field
+0x20  u64  remote_mac
```

The field names are **host-relative**. `pcinet.c:998` assigns the host's own RX ring straight
into `rx_q_phys`; the crossover happens on the target, which reads the host's `tx_q_phys` as its
own receive ring. So the host publishes its rings without swapping anything.

Do not locate this window from the constants in `barmap.h`. Read the map the coprocessor
publishes at runtime and take the facility's offset from there - the same rule as everywhere
else in this protocol.

The two address writes must each be a **single 8-byte access**. Split into two 32-bit writes,
the target can latch a torn address.

## The rings

Two per direction, three allocations each.

```
struct pci_net_q            48 bytes, DMA-coherent - its address is what gets published
  +0x00  ptr  queue         the host's own virtual pointer; the target ignores it
  +0x10  int  q_size        1024
  +0x14  int  q_last        1023
  +0x18  int  push_idx
  +0x1c  int  pop_idx
  +0x20  u32  sanity_val    0x01234567, written by the host and never checked by anyone
  +0x28  u64  q_phys_addr   bus address of the entry array  <- this is what the target follows

struct pci_net_q_entry      32 bytes, 1024 of them = 32 KB per direction
  +0x00  u32  status        write-only, see above
  +0x04  u32  size
  +0x08  u64  pbuf_phys     bus address of this entry's buffer
  +0x10  ptr  pbuf_virt     host-private
  +0x18  ptr  skb           host-private
```

Plus one **2048-byte buffer per entry**, allocated once at link-up and never per packet. That is
2 MB per direction, a little over 4 MB in total, pinned for the life of the link.

For each ring exactly one side writes `push_idx` and the other writes `pop_idx`, so nothing
needs a read-modify-write across the bus - only release ordering before publishing an index, and
a host-local lock to serialise multiple transmitters. The two indices are adjacent 32-bit fields
in the same 8-byte word and are written by opposite sides, so **only 32-bit accesses are legal**.

### And therefore: never subscript with an index you read back

That rule binds both sides, and a host can only keep its own half of it. The far side is an
AArch64 core, where merging two adjacent 32-bit stores into a single 64-bit one is something the
compiler does as a matter of routine - so a peer that writes only its own field in C can still
put a doubleword on the bus, and it lands on the host's field as well.

The consequence is not a lost count. A host that reads its own index back out of that word and
then uses it to subscript the entry array or the buffer table turns the peer's store into a wild
pointer, and would fault inside the packet copy with a backtrace pointing nowhere near the cause.

That is reasoning from the layout, not an observation. This hazard has **not** been seen to fire
on this hardware. It was written up here after a page-fault panic that it was wrongly blamed for -
the dump showed a different cause entirely, recorded in
[porting-notes.md](porting-notes.md#if_init-is-not-optional). The hazard is real and the code
guards against it, but nothing here should be read as evidence that the coprocessor does merge
those stores.

So the host keeps its own index in its own softc, writes it out to the shared word and never
reads it back; and it range-checks the peer's index against the ring size on every use, treating
an out-of-range value as a dead link rather than clamping it and carrying on.

`q_full` is `(push + 1 == pop) || (push == q_last && pop == 0)`, so one slot is always left
empty.

## The alignment shift that is not one

The vendor adds `NET_IP_ALIGN` to both the virtual and the physical address of every buffer. On
x86-64 Linux `NET_IP_ALIGN` is **zero** - the architecture defines the shift away because
unaligned access is cheap there - so on this host the arithmetic is a no-op.

Adding a real two-byte shift on FreeBSD, by analogy with `ETHER_ALIGN`, would move every
published buffer two bytes from where the vendor's host puts it. The target does not know a
shift happened; it writes to the address it is handed.

## Bring-up, and three bugs in the vendor's version of it

The host sequence: get the `mvmgmt` facility handle, map its window, wait for the target's
readiness pattern, register the interface. The rings are allocated **not** at attach and **not**
at interface-up, but about half a second later inside a 2 Hz delayed worker.

Then, and the order matters: publish `rx_q_phys` and `tx_q_phys`, and only afterwards set
`link_status` to `PCINET_LINK_HOST_UP`. That write is the target's signal that the addresses are
valid.

```
PCINET_LINK_IS_DOWN      0x00
PCINET_NETIF_OPEN        0x80    host, on interface up
PCINET_NETIF_STOP        0x81    either side, on interface down
PCINET_LINK_HOST_UP      0x82    host, once the addresses are published
PCINET_LINK_ESTABLISHED  0x83    target, in reply
```

Three defects in the reference implementation that a port should not copy:

- **`pcinet_init()` never writes `PCINET_LINK_IS_DOWN`.** The state machine starts from whatever
  was left in BAR2, which after a host reboot is whatever the previous host left behind.
- **`pcinet_link_worker` ignores the return of `pcinet_init_port`** and advertises `HOST_UP`
  even when allocation failed - leaving stale or zero addresses in the shared structure for the
  target to write into.
- **`pcinet_stop` frees the DMA memory before telling the target to stop**, then spins
  `while (link_status != PCINET_LINK_IS_DOWN) udelay(10)` with no timeout. If the far side's
  worker is not running that is an unbounded spin, in FreeBSD's case inside the ioctl path.

Teardown is the highest-risk code in this driver, and for a reason that is worth stating plainly:
if the host goes away without completing the `NETIF_STOP` to `LINK_IS_DOWN` exchange, **the
target keeps writing into the buffers it was handed, forever.** They must be withdrawn before
they are freed, and a reset of the coprocessor is the only thing that makes that certain.

## Still open

- Whether a FreeBSD host with an active IOMMU produces bus addresses under 2^36 when the busdma
  tag asks for them. The target adds the published address to a linear window, so whatever the
  host publishes has to be exactly what appears on the bus.
- Nothing in this source ever reads `remote_mac` or the two cookie bytes in the MAC union.
