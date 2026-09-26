# Where the knowledge in this tree came from

This project is **BSD-2-Clause**. Every file says so, and `LICENSE` is the whole of it.

Almost nothing here could have been written without material that is licensed differently, and a
reader is entitled to know exactly what was taken and what was not. That is what this page is for.

It is not legal advice. It is a statement of practice, written so that anyone reviewing the
project can check the practice against the tree.

## The rule

> **Facts are transcribed. Expression is not copied.**

A register offset, a command number, a field width, a byte order, the value a device expects in a
particular word — these are facts about a piece of hardware. Learning them from a published source
and writing them down in a different form is how every clean driver for undocumented hardware has
ever been written.

The code that acts on those facts is written here, from nothing.

`contrib/npuep/npugiu.h` is where the rule is visible. Marvell's `giu_nic_hw.h` opens by asking
that it be copied verbatim into any host driver, which is the sensible request for a C header
shared by two ends of a link. It was not copied. That file is a list of `#define`d offsets with no
structure declarations at all, and the header explains at length why the offsets are the right
form and the structures would have been wrong here anyway — a C struct laid over MMIO invites the
compiler to merge, split, reorder or elide accesses that must happen exactly once.

The result is more tedious to write against. It is also the only form that could be published
under this licence.

## What was read

| Source | Licence | What was taken |
|---|---|---|
| Marvell's `giu_nic_hw.h`, in Sophos's GPL drop | GPL | Offsets, command numbers, flag bits, descriptor layouts |
| Marvell's UMSD switch driver, same drop | GPL | Register names, TCAM entry semantics, the switch's initial state |
| Sophos's NetAgent sources, same drop | GPL | The network-agent message and attribute numbering, request and reply shapes |
| `usfp_rh.ko`, shipped on the appliance | proprietary binary | Structure layouts and enumerator names, read from the DWARF the vendor left in it |
| The appliance's own boot logs and captures | — | Confirmation that a reading was right |

Every one of those is a description of an interface. None of it is in this tree as code.

## Quotation

Four short comments are quoted verbatim, each attributed to its source where it appears:

- `contrib/npuep/npuep.c` and `docs/facility-protocol.md` — Marvell's *"Workaround for MSI NMP
  zero MSIX ID entry issue"*, which is the only explanation anywhere for a doorbell count that
  makes no sense otherwise.
- `contrib/npuep/npugiu.h` — Marvell's request that the header be *"copied AS-IS"*, quoted
  precisely because this project declined to do that.
- `contrib/npuep/npunwa.c` — UMSD's *"On this stage, only Broadcast mode is enabled, for other
  packets TCAM HIT will be only on Drop entry, which drops the packet"*, which is the whole
  explanation for why unicast did not work for weeks.

These are single sentences, used to show the evidence for a claim rather than to reproduce a work.
Paraphrasing them would be worse for the reader: the point of quoting is that you can check the
claim against what the vendor actually wrote.

If any rights holder disagrees with that judgement, the remedy is one line each and the surrounding
explanation stands without them. Open an issue.

## What is deliberately not here

- **No vendor source.** Not a file, not a function, not a structure declaration.
- **No firmware.** The coprocessor runs the vendor's own, which stays on the appliance.
- **No binaries.** Nothing is redistributed. What the appliance ships, the appliance keeps.
- **No credentials, no real addresses, no hardware identifiers.** `tools/check-private-data.py`
  runs in CI and fails the build over any of them, including a MAC folded inside an IPv6
  link-local. It has caught real leaks, including one from the author of this page.

## Why BSD-2-Clause and not GPL

Three reasons, in order of weight.

**It matches FreeBSD.** This is a FreeBSD kernel module. The kernel it loads into is BSD-2-Clause,
and a driver under the same terms is one that could be offered upstream rather than one that
forever cannot.

**It keeps the boundary legible.** A GPL project built beside GPL sources invites exactly the
confusion this page exists to prevent — did that come from there, or was it written here? Under a
different licence the question has to be answered file by file, and it has been.

**It does not restrict the reader.** Anyone who wants to put this on an appliance and ship it can.
That was the point of writing it.
