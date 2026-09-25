# Built on the appliance, against kernel sources unpacked into /usr/src:
#
#	make SYSDIR=/usr/src/sys
#
# Not built by the installer and not packaged. See docs/porting-notes.md - a headers-only
# subset of the source tree is not enough, and preloading the result from loader.conf would
# not work on this hardware even if it were.
#
KMOD	= npuep
SRCS	= npuep.c npumgmt.c npugiu.c npunwa.c npurpc.c device_if.h bus_if.h pci_if.h opt_inet.h

.include <bsd.kmod.mk>
