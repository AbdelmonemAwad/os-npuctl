# The hardware

Measured on a Sophos XGS 136. Where a fact is known to generalise across the range it says so;
where it is not, assume it does not.

## What FreeBSD sees on the host

Working with no driver work at all:

```
CPU    AMD Ryzen Embedded R1606G, 4 CPUs (1 package x 2 cores x 2 threads)
RAM    8192 MB
Disk   ahci0 -> ada0, SATA 3.x
USB    xhci0, USB 3.0
SMBus  intsmb0 -> smbus0 (AMD FCH)      the I2C path to the SFP cages and the PoE controller
HID    Microchip MCP2210 USB to SPI Master -> usbhid0 -> hidbus0
serial uart0 0x3f8 (console) and uart2 0x3e8 (the NPU's console, needs a hint)
```

Not working, and it is the whole reason this repository exists:

```
pci1: <network, ethernet> at device 0.0 (no driver attached)
```

There is also no `superio`, so the NCT6779D hardware monitor - which the BIOS reads perfectly
well - is invisible to a stock FreeBSD. That is a separate piece of work.

## The endpoint

```
pci0:1:0:0   vendor=0x11ab device=0x7080   class=network/ethernet
  bar[10]  1 MB    64-bit
  bar[18]  16 MB   64-bit
  bar[20]  16 MB   32-bit
  PCIe     x4 @ 8.0 GT/s (Gen3)
  MSI      32 messages;  MSI-X 16 messages, table in BAR0 @0x1000, PBA @0x1800
  SR-IOV   VF device ID 0x7081, 6 VFs supported
  ARI, AER, LTR, TPH Requester, Resizable BAR, L1 PM Substates
```

The link is Gen3 x4, about 32 Gbit/s. All fourteen front ports together are 14 Gbit/s at most,
so the roughly 936 Mbps single-port ceiling reported by other projects on this hardware is **not**
a PCIe bandwidth limit, and neither is an MTU of 1500. Whatever causes them is above the bus.

The SR-IOV capability reports 6 VFs where the vendor's Linux exposes 8 in sysfs. Unresolved, and
recorded because it is the sort of discrepancy that wastes a day later.

## Identifying the board from software

Two independent sources, and the second is the more useful one.

**SMBIOS** is awkward. `smbios.planar.product` is the generic string `XGS` across the whole
range and `smbios.planar.version` is not the model either. The assembly number is buried inside
`smbios.planar.serial`, in the form:

```
NEAAMDA0201-00xxxxxxxxxx
   ^^^^^^^^^^^^
   AMDA0201-0003 = Desktop XGS 136
```

So anything keying a chassis table off SMBIOS has to match on the serial, not on product or
version - and the serial also carries the unit's own identity, which is worth remembering before
putting it in a log.

**The MCP2210's EEPROM** is better for this purpose. Bytes 0 and 1 hold the assembly number as
`(byte0 << 8) | byte1`, so `0x00, 0xC9` is 201, which is AMDA0201. It is on the host side, needs
no I2C, and answers while the coprocessor is held in reset.

## The coprocessor

```
SoC       Marvell CN9131  (AP807 + two CP115)
storage   its own eMMC, 7.28 GiB, four partitions, root on p3
firmware  U-Boot 2019.10-10.22.03, Linux 4.14.207-10.22.03
model     the kernel reports "Sophos XGS126/136" - one board for both
cmdline   console=ttyS0,115200n8 root=/dev/mmcblk0p3 cpuidle.off=1
          isolcpus=1-3 nohz_full=1-3 rcu_nocbs=1-3
```

The CN9131's second CP115 is what lets this board drive fourteen ports where the nine-port model
in the same family uses a CN9130.

Three cores are isolated from the scheduler and run the dataplane; that is what `isolcpus=1-3`
is for.

The coprocessor boots from **its own storage**, not from the host's disk. Replacing the host SSD
- which is what running OPNsense on one of these means - does not touch its firmware at all,
and that is what makes the whole exercise reversible.

## The port map

Fourteen ports, all behind the coprocessor. The switch is an 88E6193X LinkStreet; the multi-gig
ports are on an AQR412C quad PHY.

```
labels 1-8   ->  switch ports 1-8      (88E6193X, CPU port is port 0)
label 9      ->  CN9131 COMPHY 2       (AQR412C address 0, 1G)
label 10     ->  CN9131 COMPHY 4       (AQR412C address 1, 1G)
label 11     ->  CN9131 COMPHY 5       (AQR412C address 2, 2.5G + PoE)
label 12     ->  CN9131 COMPHY 3       (AQR412C address 3, 2.5G + PoE)
F1, F2       ->  switch ports 9, 10    (SFP)
```

Each port takes one offset from the board's base MAC, in that order.

The device tree is **not** where this map lives, which is worth knowing before going looking for
it there. Neither the 9130 nor the 9131 device tree contains a switch node, a DSA node, an
AQR412C node or any PHY child: both MDIO controllers per CP are declared empty and handed to
userspace. The map comes from a platform text file in the coprocessor's firmware plus the
bootloader environment.

## What the vendor's own tools tell you

Useful mostly as a map of what is reachable and from where. Roughly half of the `xgs-*` tools on
the host are seven-line shell wrappers that ssh to the coprocessor over the management link -
those are useless until that link works. The rest run on the host.

Two are worth naming:

- The USB-SPI flash tool is the one this repository's reset path is derived from. It also reads
  and writes the coprocessor's boot flash from the host, which means a bricked bootloader is
  recoverable without opening the case.
- The platform-init tool is **not** a bring-up tool despite the name. It probes I2C busses 0-7 at
  address `0x57` for the baseboard VPD EEPROM and builds a key-value database that everything
  else reads. FreeBSD has `smbus0` on this board, so that database can be rebuilt natively - it
  is where the per-port MAC addresses, PHY labels and transceiver configuration come from.
