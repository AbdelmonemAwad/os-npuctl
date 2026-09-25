PLUGIN_NAME=		npuctl
PLUGIN_VERSION=		0.1
PLUGIN_COMMENT=		Bring the Marvell NPU out of reset on Sophos XGS appliances
PLUGIN_MAINTAINER=	eg2@live.com

# No PLUGIN_DEPENDS. Everything the installed half needs is in the base system: python3 for the
# bridge tool and hidraw(4) for it to talk through. Nothing is installed on the owner's behalf.
#
# THE KERNEL MODULE IS NOT PACKAGED, and that is a decision rather than an omission.
#
# contrib/npuep is C against FreeBSD kernel headers. It cannot be built on a machine without
# kernel sources, it cannot be built at package time on a build host whose kernel differs from
# the target's, and - see docs/porting-notes.md - it cannot usefully be preloaded from
# loader.conf either, because the BAR restore this hardware needs only happens on the kldload
# path. So it ships as source, is built on the appliance, and is loaded by hand.
#
# What the package does install is the reset hook, which is the half that is safe unattended:
# it touches a USB bridge, exits 0 on hardware it does not recognise, and cannot affect the
# host if it fails.
#
# What it DOES do, as of the boot hook pair, is install a module that has already been built on the
# appliance into /boot/modules and load it at boot. Building stays manual; remembering to load it
# does not, because a firewall that comes up without its front ports because somebody forgot a
# kldload is a firewall that is down.

.include "../../Mk/plugins.mk"
