# Bringing the NPU out of reset

The coprocessor's reset line is reachable from the host, it needs no vendor code, and FreeBSD
already attaches the chip it hangs off. This document is how.

## The bridge

An **MCP2210 USB-to-SPI bridge**, VID `0x04D8` PID `0x00DE`, on the appliance's internal USB.
FreeBSD sees it without any help:

```
ugen0.2: <MCP2210 USB to SPI Master Microchip Technology, Inc.> at usbus0
usbhid0: <... MCP2210 USB to SPI Master ...> on usbus0
  hidbus0: <HID bus> on usbhid0
```

`hidbus` attaches but no child driver claims it, so there is no device node until:

```sh
kldload hidraw        # or hidraw_load="YES" in /boot/loader.conf.local
```

which gives `/dev/hidraw0`. FreeBSD's hidraw takes the same bare 64-byte reports Linux does -
no leading report-ID byte.

Three of the bridge's nine GPIO pins are wired to the coprocessor: **GP0, GP3 and GP5**, mask
`0x0029`. The vendor tool drives all three together on every operation, so which individual pin
is reset and which is a SPI mux cannot be separated by reading the code - only their combined
states are ever used.

## The protocol

64-byte reports, written and read on the hidraw node.

| command | what it does | notable fields |
|---------|--------------|----------------|
| `0x20` | get chip settings | reply `[4..12]` pin designations, `[13..14]` output value LE16, `[15..16]` direction LE16 (**1 = input**), `[17]` other |
| `0x21` | set chip settings | same fields, in the request |
| `0x30` | set live GPIO values | `[4]` low byte, `[5]` high byte. `0xFB` back means another master owns the bus |
| `0x32` | read live GPIO values | `[4..5]` |
| `0x50` | read EEPROM | `[1]` address in, `[2]` address and `[3]` data out |
| `0x61` sub `0x20` | read the NVRAM power-up chip settings | same layout as `0x20` |

An unimplemented command answers with status `0xF9`.

### The transport will lie to you

Three behaviours, all measured, all of which produce data that looks correct:

- **The device only rewrites the bytes a reply needs** and leaves the rest of its 64-byte buffer
  holding the previous reply. Read a field a command does not document and you get stale data
  sitting in exactly the right place.
- **The HID queue desynchronises.** One stale report and every later read returns the previous
  command's answer; one missing report and the next read blocks until it times out - which is
  indistinguishable from "this chip does not implement that command".
- **The first command sent after opening the device is dropped more often than not.**

A correct implementation therefore drains the queue before every command, checks that `rx[0]`
echoes the byte it sent, retries, and never reads without a timeout. `mcp2210.py` does all four.

## The per-board table

The assembly number lives in the bridge's own EEPROM, bytes 0 and 1, as `(byte0 << 8) | byte1`.
It indexes a table, and **the polarity is not the same across boards** - which is why this is a
table and not a constant.

| assembly | model | hold (host owns the bus) | release |
|----------|-------|--------------------------|---------|
| AMDA0200 | XGS desktop gen 1 | `0xFFD7` | `0xFFFF` |
| **AMDA0201** | **XGS 126 / 136** | **`0xFFFF`** | **`0xFFD7`** |
| AMDA0202-0205 | 1U / 2U | `0xFFDF` | `0xFFF7` |
| AMDA0208 | XGS 116 | `0xFFFF` | `0xFFD7` |
| AMDA0224 | XGS 138 | `0xFFFF` | `0xFFD7` |

Only AMDA0201 has been exercised on hardware.

Reading the assembly number from the bridge is also a **second, independent board
identification**, on the host side, that works while the coprocessor is in reset. The SMBIOS
alternative is less convenient: `smbios.planar.product` is the generic string `XGS` across the
whole range and `smbios.planar.version` is not the model either - the assembly number is buried
inside `smbios.planar.serial`, which also carries the unit's serial.

## It must be a pulse

This is the part that costs an evening if it is assumed rather than tested.

On a cold boot, writing the release values alone does nothing. The pins read back exactly
correct and the coprocessor stays dead - zero receive interrupts on its console, forever.
Driving the pins to the hold state and then to the release state starts it every time.

The sequence, which is what the vendor's own `-t` option does:

```
  gpio_write(hold value,    mask 0x0029)
  brief pause
  gpio_write(release value, mask 0x0029)
```

and `gpio_write` itself is, for every pin in the mask: make it a GPIO, make it an output, put
the requested bit in the value, write the chip settings, then write the live values.

The write goes to the bridge's **volatile** settings, so a power cycle restores the factory
state and the coprocessor is held again. That is why the hook runs on every boot rather than
once.

**Do not write the release state into the bridge's NVRAM to make it stick.** That is where the
assembly number lives, and a bad write there is not recoverable from the host side.

## What you should see

Released, the coprocessor boots its own system and its console - host `uart2` at `0x3E8`, IRQ 3 -
carries the whole thing:

```
BootROM - 2.03
Starting CP-0 IOROM 1.07
Booting from SPI NOR flash 0
  Comphy lanes 0-3 as PCIe
  SVC: DEV ID: CN913x
mv_ddr: 10.22.03, SNPS DDR 1D and 2D training passed
NOTICE:  BL1: v2.2(release) (Marvell-10.22.03)
U-Boot 2019.10-10.22.03
  Net: eth0: mvpp2-0, eth1: mvpp2-2, ...
Linux 4.14.207-10.22.03
  Machine model: Sophos XGS126/136
  root=/dev/mmcblk0p3   isolcpus=1-3 nohz_full=1-3 rcu_nocbs=1-3
  armada-ep: pcinet: mvmgmt0: netdev registered
Waiting for host driver handshake to complete
```

That last line is stage 2's problem.

### Reading that console without breaking it

`uart2` is not probed by default. In `/boot/loader.conf.local`:

```
hint.uart.2.at="isa"
hint.uart.2.port="0x3E8"
hint.uart.2.flags="0x0"
hint.uart.2.irq="3"
```

And then **turn the echo off before reading it**:

```sh
stty -f /dev/cuau2.init 115200 cs8 -parenb -cstopb clocal -crtscts -ixon -ixoff \
        -hupcl raw -echo -echoe -echok -echonl -isig -icanon -iexten -opost
```

Without `-echo raw`, the host's tty layer echoes every byte the coprocessor transmits straight
back into its receiver. During the three-second autoboot countdown that stops the boot, and
U-Boot then reads the coprocessor's own boot messages as commands:

```
Autoboot in 3 seconds - press SPACE to stop...
Marvell>> in RAM -o stop..
syntax error
```

The result is a coprocessor sitting at a bootloader prompt attempting network boot, for reasons
that look like a hardware fault and are not.

Finally: **never open the coprocessor's console with a blocking read from a session running on
the host's own console**. Doing that hangs the machine and costs a power cycle. Read it in the
background with a hard timeout.
