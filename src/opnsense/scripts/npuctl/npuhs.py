#!/usr/local/bin/python3
"""
Read - and optionally complete - the Marvell NPU's host handshake, from FreeBSD userspace.

The NPU on this appliance boots its own Linux and then blocks, polling one 32-bit word until it
reads 0x0B, at which point it starts NetAgent, UMSD_NPU and the usfp dataplane. Bit 0 of that
word is its own; bits 1 and 3 are the host's. So the thing standing between a booted NPU and a
running dataplane is two bits.

This needs no kernel module and no Marvell code: the word lives in BAR2, and root can reach a
BAR through /dev/mem. That is the whole trick.

Every constant here is from the GPL source in SFOS_OSS-22.0.1, pcie_ep_armada/src/, not from
guesswork - barmap.h and facility_conf.h. See docs/facility-protocol.md.

    ./npuhs.py probe     read and decode everything. Writes nothing.
    ./npuhs.py up        set HOST_INIT, then hold HOST_ALIVE. SEE THE WARNING.

DO NOT USE `up` ON A MACHINE YOU CARE ABOUT.

It works - it was used to complete the handshake for the first time, and the NPU answered.
But completing the handshake tells the far side that a host driver is present and ready, and
that is not true when the only thing here is this script. Two things follow from that lie:

  - The NPU starts raising doorbells. A doorbell is an MSI-X interrupt, and the vectors were
    never allocated by the host, so those interrupts land somewhere nobody arranged.
  - Worse, the NPU has already mapped host memory through its BAR - its own log says
    "Remapped host memory @ ... size 0x1000000000" - and bus mastering is on. A real host
    driver publishes DMA-safe addresses for it to use. This script publishes none.

The appliance took a `general protection fault` panic about half an hour after this was first
run. That has not been traced to a specific write, and it might have been something else
entirely - but the mechanism above is sufficient to explain it, and nothing here is careful
enough to rule it out.

`up` therefore refuses to run without --yes-i-accept-the-risk. The right way to complete the
handshake is os-npuctl's npuep.ko, which allocates the five MSI-X vectors FIRST and only then
sets the bits, in that order, for exactly this reason.
"""
import mmap
import os
import re
import struct
import subprocess
import sys
import time

PCI_SEL = 'pci0:1:0:0'

# barmap.h
NPU_BARMAP_COOKIE = 0xD0FAC10D
NPU_BARMAP_VERSION = 5
# facility_host.c:780 computes the window the same way, and the arithmetic is worth spelling
# out because the facilities live at the TOP of BAR2, not the bottom:
#   bar_size = NPU_BARMAP_BAR2_TOTAL_SIZE                 = 0x103000
#   offset   = pci_resource_len(BAR2) - (SZ_4K + bar_size) = size - 0x104000
# The trailing 4 KB that the window does not cover is the PCI BOOTCMD page.
BARMAP_TAIL = 0x104000          # distance from the end of BAR2 back to the window start
WINDOW_LEN = 0x103000           # what the vendor driver maps - map no more than it does
BARMAP_STRUCT_OFF = 0x102000    # npu_bar_map, inside that window

# facility_conf.h
ARMADA_FACILITY_COOKIE = 0xAFACAFAC
CTRL_FCLT_TRGT_INIT = 1 << 0
CTRL_FCLT_HOST_INIT = 1 << 1
CTRL_FCLT_TRGT_H2T_DBELL = 1 << 2
CTRL_FCLT_HOST_ALIVE = 1 << 3

HANDSHAKE_BITS = [
    (CTRL_FCLT_TRGT_INIT, 'TRGT_INIT', 'NPU'),
    (CTRL_FCLT_HOST_INIT, 'HOST_INIT', 'host'),
    (CTRL_FCLT_TRGT_H2T_DBELL, 'TRGT_H2T_DBELL', 'NPU'),
    (CTRL_FCLT_HOST_ALIVE, 'HOST_ALIVE', 'host'),
]
READY = CTRL_FCLT_TRGT_INIT | CTRL_FCLT_HOST_INIT | CTRL_FCLT_HOST_ALIVE   # 0x0b

# The enum order, which is NOT the order the entries appear in the barmap.
FACILITY_NAME = {0: 'ctrl', 1: 'mvmgmt', 2: 'nwa', 3: 'rpc', 4: 'giu'}
MV_FACILITY_CONTROL = 0
SHM_BAR_NAME = {0: 'BAR0', 1: 'BAR2'}

# facility_target.c clears HOST_ALIVE on every scan, so it is a heartbeat and not a flag. If the
# host stops re-setting it the facility restarts. One second is what the vendor's worker uses.
HEARTBEAT_S = 1.0


def bars():
    """Physical base and size of each BAR, straight out of pciconf."""
    out = subprocess.run(['pciconf', '-lb', PCI_SEL], capture_output=True, text=True).stdout
    found = {}
    for m in re.finditer(r'bar\s+\[(\w+)\]\s*=\s*type Memory.*?base (0x[0-9a-f]+), size (\d+)', out):
        found[int(m.group(1), 16)] = (int(m.group(2), 16), int(m.group(3)))
    # bar[10] is BAR0, bar[18] is BAR2, bar[20] is BAR4
    if 0x18 not in found:
        raise SystemExit('BAR2 not found on %s - is the endpoint still there?\n%s' % (PCI_SEL, out))
    return found


class Window:
    """The facility window at the top of BAR2, mapped through /dev/mem."""

    def __init__(self, base, size, write=False):
        self.phys = base + size - BARMAP_TAIL
        if self.phys % mmap.PAGESIZE:
            raise SystemExit('window 0x%x is not page aligned' % self.phys)
        flags = os.O_RDWR if write else os.O_RDONLY
        self.fd = os.open('/dev/mem', flags | getattr(os, 'O_SYNC', 0))
        prot = mmap.PROT_READ | (mmap.PROT_WRITE if write else 0)
        self.m = mmap.mmap(self.fd, WINDOW_LEN, mmap.MAP_SHARED, prot, offset=self.phys)

    def u32(self, off):
        return struct.unpack_from('<I', self.m, off)[0]

    def put_u32(self, off, val):
        struct.pack_into('<I', self.m, off, val)

    def close(self):
        self.m.close()
        os.close(self.fd)


def read_barmap(w):
    """npu_bar_map: {u32 version; u32 cookie; facility_bar_map[5];}, entries 16 bytes each."""
    base = BARMAP_STRUCT_OFF
    version = w.u32(base)
    cookie = w.u32(base + 4)
    facilities = []
    for i in range(5):
        off = base + 8 + i * 16
        facilities.append({
            'bar': w.u32(off),
            'type': w.u32(off + 4),
            'offset': w.u32(off + 8),
            'size': w.u32(off + 12),
        })
    return version, cookie, facilities


def decode_handshake(v):
    parts = []
    for bit, name, owner in HANDSHAKE_BITS:
        parts.append('%s %-14s (%s)' % ('x' if v & bit else '.', name, owner))
    return parts


def find_control(facilities):
    for f in facilities:
        if f['type'] == MV_FACILITY_CONTROL:
            return f
    return None


def verify(w, require_trgt_init):
    """Read back and check all three magic numbers before anyone writes anything.

    This matters more than it looks. The window is memory the NPU's own kernel is reading and
    writing; a wrong offset does not fail, it corrupts something else in its reserved region.
    """
    version, cookie, facilities = read_barmap(w)
    print('npu_bar_map @ phys 0x%x' % (w.phys + BARMAP_STRUCT_OFF))
    print('  version  0x%08x   %s' % (version, 'ok' if version == NPU_BARMAP_VERSION else
                                      'EXPECTED 0x%x' % NPU_BARMAP_VERSION))
    print('  cookie   0x%08x   %s' % (cookie, 'ok' if cookie == NPU_BARMAP_COOKIE else
                                      'EXPECTED 0x%08x' % NPU_BARMAP_COOKIE))
    if cookie != NPU_BARMAP_COOKIE:
        print('\nThe NPU has not published its barmap. Either it is not running, or it is still')
        print('early in boot. Nothing else here is meaningful - stopping.')
        return None
    if version != NPU_BARMAP_VERSION:
        print('\nBarmap version is not 5. The layout this tool assumes does not apply. Stopping.')
        return None

    print('\n  #  facility  bar    offset      size')
    for i, f in enumerate(facilities):
        print('  %d  %-8s  %-5s  0x%06x  %8d' % (
            i, FACILITY_NAME.get(f['type'], '?%d' % f['type']),
            SHM_BAR_NAME.get(f['bar'], '?%d' % f['bar']), f['offset'], f['size']))

    ctrl = find_control(facilities)
    if ctrl is None:
        print('\nNo CONTROL facility in the map. Stopping.')
        return None
    if ctrl['bar'] != 1:
        print('\nCONTROL facility is on %s, not BAR2 - this tool only maps BAR2. Stopping.'
              % SHM_BAR_NAME.get(ctrl['bar']))
        return None

    c_off = ctrl['offset']
    c_cookie = w.u32(c_off)
    handshake = w.u32(c_off + 4)
    dbell_cnt = w.u32(c_off + 8)

    print('\nctrl_map @ phys 0x%x' % (w.phys + c_off))
    print('  cookie     0x%08x   %s' % (c_cookie, 'ok' if c_cookie == ARMADA_FACILITY_COOKIE
                                        else 'EXPECTED 0x%08x' % ARMADA_FACILITY_COOKIE))
    print('  handshake  0x%08x' % handshake)
    for line in decode_handshake(handshake):
        print('    ' + line)
    print('  h2t dbell count %d' % dbell_cnt)

    if c_cookie != ARMADA_FACILITY_COOKIE:
        print('\nControl facility cookie is wrong. Stopping before touching anything.')
        return None
    if require_trgt_init and not (handshake & CTRL_FCLT_TRGT_INIT):
        print('\nThe NPU has not set TRGT_INIT yet. It is not ready for our half. Stopping.')
        return None

    ready = (handshake & READY) == READY
    print('\n  handshake >= 0x0b: %s%s' % (ready, '' if ready else
                                           '   <- the NPU is still waiting'))
    return c_off, handshake


def main(argv):
    cmd = argv[1] if len(argv) > 1 else 'probe'
    if cmd not in ('probe', 'up'):
        print(__doc__.strip())
        return 2

    if cmd == 'up' and '--yes-i-accept-the-risk' not in argv:
        print(__doc__.strip())
        print()
        print('Refusing to run `up`. Read the warning above, then pass '
              '--yes-i-accept-the-risk if you mean it.')
        return 2

    b = bars()
    base, size = b[0x18]
    print('BAR2  phys 0x%x  size %d (%d MB)' % (base, size, size // (1024 * 1024)))

    w = Window(base, size, write=(cmd == 'up'))
    try:
        res = verify(w, require_trgt_init=(cmd == 'up'))
        if res is None:
            return 1
        c_off, handshake = res
        if cmd == 'probe':
            return 0

        # facility_host.c:916 then :996 - HOST_INIT first, then HOST_ALIVE.
        print('\nwriting HOST_INIT (bit 1)')
        w.put_u32(c_off + 4, w.u32(c_off + 4) | CTRL_FCLT_HOST_INIT)
        time.sleep(0.2)
        print('  handshake now 0x%08x' % w.u32(c_off + 4))

        print('holding HOST_ALIVE (bit 3) - the NPU clears it on every scan, so this is a')
        print('heartbeat, not a flag. Ctrl+C to stop; the NPU will then see the host go away.')
        beats = 0
        while True:
            w.put_u32(c_off + 4, w.u32(c_off + 4) | CTRL_FCLT_HOST_ALIVE)
            time.sleep(HEARTBEAT_S)
            beats += 1
            if beats % 5 == 0:
                print('  beat %-4d handshake 0x%08x' % (beats, w.u32(c_off + 4)))
    except KeyboardInterrupt:
        print('\nstopped')
    finally:
        w.close()
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
