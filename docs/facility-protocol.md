# The facility protocol

What the coprocessor publishes into its BARs, what it waits for, and what it offers once the
wait is over.

Every constant here is from Marvell's GPL-2.0-only `pcie_ep_armada` driver - `barmap.h`,
`facility_conf.h`, `facility_host.c` - published by Sophos in its SFOS_OSS source ISO, which is
a free download. No vendor code is reproduced in this repository. The layout was first recovered
by disassembling the shipped binary module and later confirmed against the source; the two
agreed exactly, which is a reasonable check that these are the right structures.

## The BAR map

```
BAR0 - 1 MB window, 0x14000 used
  +0x00000   GIU          16 KB      (the MSI-X table sits at +0x1000)
  +0x04000   NW AGENT     64 KB

BAR2 - 16 MB, and the facilities are at the TOP of it, not the bottom
  window = BAR2 size - 0x104000
  window +0x000000   ctrl_map        4 KB
  window +0x001000   mgmt netdev     4 KB
  window +0x002000   RPC             1 MB
  window +0x102000   npu_bar_map     4 KB
  window +0x103000   PCI BOOTCMD     4 KB
```

The trailing 4 KB the window does not cover is the bootloader command page.

On a board with a 16 MB BAR2 the window starts at BAR2 + `0xEFC000`, so `npu_bar_map` is at
BAR2 + `0xFFE000` and `ctrl_map` at BAR2 + `0xEFC000`.

## npu_bar_map

```c
struct npu_bar_map {
        uint32_t version;                    /* must be 5 */
        uint32_t cookie;                     /* 0xD0FAC10D when published */
        struct facility_bar_map map[5];      /* 16 bytes each */
};

struct facility_bar_map {
        uint32_t bar;      /* 0 = BAR0, 1 = BAR2 */
        uint32_t type;
        uint32_t offset;   /* within the facility window */
        uint32_t size;
};
```

Poll the cookie. Until the coprocessor has published it the whole area reads as ones, and
`0xFFFFFFFF` there means either the coprocessor is not running or its BARs are not decoding -
those two are worth telling apart before drawing any conclusion.

The facility types, in enum order, which is **not** the order the entries appear in the map:

| value | name | t2h doorbells |
|-------|------|---------------|
| 0 | `ctrl` | 0 |
| 1 | `mvmgmt` | **1** |
| 2 | `nwa` | 0 |
| 3 | `rpc` | 0 |
| 4 | `giu` | **4** |

`mvmgmt`'s count is 1 even though its IRQ-count macro is 0. The vendor source carries the
comment *"Workaround for MSI NMP zero MSIX ID entry issue"* next to it. Keep it: the vector
numbering is a flat index across facilities in this order, so dropping it shifts every later
vector and the coprocessor asks for one nobody allocated.

Read the map from the device rather than using these as compile-time constants. The coprocessor
is what publishes it, and an implementation that assumed would write into the middle of
something else if a later firmware moved a facility.

## The control facility and the handshake

At the `ctrl` facility's offset:

```c
struct ctrl_map {
        uint32_t cookie;        /* 0xAFACAFAC */
        uint32_t handshake;
        uint32_t h2t_dbell_cnt;
        struct { uint64_t address; uint32_t data; } h2t_dbell_msg[];
};
```

```
bit 0   TRGT_INIT         target init done       - the coprocessor sets it
bit 1   HOST_INIT         host init done         - the host sets it
bit 2   TRGT_H2T_DBELL    target doorbells ready - the coprocessor sets it
bit 3   HOST_ALIVE        host alive             - the host sets it, repeatedly
```

The coprocessor's startup script polls this word through sysfs and blocks until it reads at
least `0x0b`, which is `TRGT_INIT | HOST_INIT | HOST_ALIVE`. Bit 2 is not part of the condition.

Sequence, host side:

```
  check cookie == 0xAFACAFAC          refuse to write anything if it is not
  wait for bit 0
  allocate MSI-X vectors, install handlers        <- before, not after
  set bit 1
  set bit 3, and keep setting it
```

**Bit 3 is a heartbeat, not a flag.** The target clears it every time it looks, so a host that
sets it once and stops is a host the facility decides has gone away. This is what the vendor
datapath driver's `feature_enable Bit0=Keep-alive` parameter refers to.

## Doorbells

Five target-to-host doorbells: one for `mvmgmt`, four for `giu`, numbered as a flat index in
facility order, so `mvmgmt` is vector 0 and `giu` is 1 through 4.

The host does **not** write MSI-X messages into the shared memory. It allocates MSI-X vectors
the ordinary way; the MSI-X table lives in BAR0 at `+0x1000`, which is inside the GIU facility's
own window, and the operating system fills it as part of allocating the vectors. The
coprocessor then reads its own table to learn where to write.

Two consequences worth stating plainly:

- Mapping BAR0 as a driver resource is **required**, not a conflict with MSI-X allocation. The
  table is in it.
- The coprocessor raises a doorbell by **writing the MSI message itself**, outbound. So freeing
  the vectors does not stop it - its writes simply land on whatever takes their place. A driver
  unloading has to withdraw from the handshake first, and even that is not airtight because
  there is no acknowledgement to wait for. A reset pulse is the only certain answer.

## The GIU datapath, as far as it has been read

Not implemented here. Recorded because it is the next thing and the reading is done.

One netdev carrying all fourteen front ports, up to 16 Rx and 16 Tx queues. A control block in
the BAR whose first byte is a status word - bit 0 target ready, bit 1 host has configured the
queues, bit 2 target acknowledged - and the interface MAC published at control block + 4.

Two rings of **256 descriptors of 64 bytes**, command and notification, DMA-coherent in host
memory, with their physical addresses and index-register offsets written into eight fields at
`+0x10` through `+0x38` of the control block.

A descriptor is an 8-byte header and 56 bytes of payload; longer messages split across
descriptors with first/middle/last bits. Requests carry a 16-bit index, never 0 and never
`0xFFFF`, which the coprocessor echoes in the notification ring so a response can be matched to
its caller. `0xFFFF` is reserved for coprocessor-initiated events such as link up and down.

Sixteen management opcodes. Named here by what each calling function does - the enum labels are
not in the binary:

| code | function |
|------|----------|
| `0x01`, `0x05`, `0x09` | configure queues, in that call order |
| `0x07` | enable |
| `0x0d` | set MAC address |
| `0x0e`, `0x0f` | Rx promiscuous, Rx multicast promiscuous |
| `0x10` | change MTU |
| `0x11` | Tx port loopback |
| `0x12`, `0x13` | add and remove VLAN |
| `0x16`, `0x18` | add MAC address, add Rx multicast address |
| `0x1c`, `0x1d` | port and queue rate limit |
| `0x1e` | get hardware capabilities |

Every command uses a 48-byte request and a 105-byte response except `0x09` and `0x1e`, which
send no request body - so the whole command ABI is two structures.

A minimum viable set for link and traffic: `0x1e`, then `0x09`/`0x01`/`0x05`, then `0x0d`,
`0x10`, `0x07`.

The coprocessor's end of this is **not** vendor-proprietary. It is Marvell's open-source MUSDK
network management proxy, started from a shell script in the coprocessor's own firmware, so the
protocol is specified in readable C rather than only in one disassembly.

## The ports

The fourteen front ports arrive on one trunk, separated by **VLAN 4095** subinterfaces. A host
implementation has to demultiplex them; on Linux the vendor ships a small driver that registers
a link type for exactly this.
