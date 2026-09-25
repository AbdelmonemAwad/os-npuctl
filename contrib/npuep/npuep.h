/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * What npuep.c hands to the pieces built on top of it.
 *
 * The facility layer finds where each facility lives by reading the map the coprocessor
 * publishes, so a consumer must never work its offset out from a constant. It is given one.
 */

#ifndef _NPUEP_H_
#define _NPUEP_H_

/*
 * One facility's window, already located and range-checked by npuep.c.
 *
 * `res` is the BAR resource the window is inside and `off` is the absolute offset within it, so
 * a consumer reads and writes with bus_space_*_4(rman_get_bustag(res), rman_get_bushandle(res),
 * off + field). Nothing here is a pointer into the window, deliberately: this is MMIO on the
 * far side of a PCIe link, and a plain C dereference of it is how the vendor driver does it and
 * is not how FreeBSD should.
 */
/*
 * The smallest mvmgmt window that can hold struct pci_net_shared_cfg. A facility smaller than
 * this cannot be what it claims to be, and the size came from the coprocessor, so it is checked
 * where the map is read rather than trusted downstream.
 */
#define	NPUEP_MGMT_MIN_SIZE	0x28

struct npuep_facility {
	device_t	 dev;
	struct resource	*res;		/* the BAR this facility's window is in */
	bus_size_t	 off;		/* absolute offset of the window within it */
	bus_size_t	 size;		/* how much of it is ours */
	bus_dma_tag_t	 parent_tag;	/* parent for any DMA the consumer allocates */
};

int	npumgmt_attach(struct npuep_facility *fac);
void	npumgmt_detach(void);

#endif /* _NPUEP_H_ */
