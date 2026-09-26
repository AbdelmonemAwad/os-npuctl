#!/bin/sh
#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Run by pkg(8) as root after the files are gone.
#
# hidraw_load is left in loader.conf.local: harmless, possibly not ours, and removing somebody
# else's line is worse than leaving one behind.
exit 0
