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

	/*
	 * The MSI-X vectors npuep allocated for THIS facility. The coprocessor is told a vector
	 * id per queue and signals us by writing the message itself, so a consumer must hand it
	 * a number npuep actually owns - inventing one points the device at a table entry
	 * belonging to something else.
	 */
	int		 first_msix;	/* index of this facility's first vector */
	int		 nmsix;		/* how many it has */
};

/*
 * The smallest giu window that can hold struct agnic_config_mem. Same rule as the mvmgmt
 * minimum: the size came from the coprocessor, so it is checked where the map is read.
 */
#define	NPUEP_GIU_MIN_SIZE	0x400

/* The smallest nwa window that can hold the mailbox header, the request and its reply. */
#define	NPUEP_NWA_MIN_SIZE	0x100

/*
 * The fourth facility, and the last one this driver does not speak.
 *
 * The vendor's host reaches it through usfp_firewall.ko, which its module dependencies show
 * sitting on mv_armada_drv - the facility layer - alongside the three modules whose facilities we
 * already implement. Its own boot message is `usfp_firewall_cmsg_init: real_dev: ... mv-pcimux0`,
 * so it is a control-message channel attached to the trunk interface. On the evidence, this is
 * what tells the coprocessor's fastpath where to send a frame, which is the one thing still
 * standing between a configured datapath and a frame arriving on it.
 *
 * A megabyte, one host-to-target doorbell, one DMA engine. Nothing is written to it here: this
 * only captures the window so it can be read.
 */
#define	NPUEP_RPC_MIN_SIZE	0x100

int	npumgmt_attach(struct npuep_facility *fac);
void	npumgmt_detach(void);

int	npugiu_attach(struct npuep_facility *fac);
void	npugiu_detach(void);

int	npunwa_attach(struct npuep_facility *fac);
void	npunwa_detach(void);

#endif /* _NPUEP_H_ */
