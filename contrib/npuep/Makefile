# Built on the appliance, against kernel sources unpacked into /usr/src:
#
#	make SYSDIR=/usr/src/sys
#
# Not built by the installer and not packaged. See docs/porting-notes.md - a headers-only
# subset of the source tree is not enough, and preloading the result from loader.conf would
# not work on this hardware even if it were.
#
# npumgmt.c is deliberately absent from SRCS. It holds the memory layer for the management
# interface, verified against the vendor source, while the ring protocol is still being
# settled. Building it in before that would produce a module that allocates four megabytes,
# hands the addresses to a coprocessor, and then races it.

KMOD	= npuep
SRCS	= npuep.c device_if.h bus_if.h pci_if.h

.include <bsd.kmod.mk>
