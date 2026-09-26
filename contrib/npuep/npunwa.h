/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The NetAgent mailbox, as it lies in the nwa facility window.
 *
 * Offsets rather than a structure, for the same reason as npugiu.h: this is MMIO across a PCIe
 * link, where a C struct invites the compiler to merge, split or elide accesses that have to
 * happen exactly once and in exactly the order written - and here the order is the protocol.
 *
 * See docs/netagent.md for how each of these was established and for the transaction they take
 * part in. Nothing below was guessed: the field map came from watching a live window while ports
 * were toggled one at a time, and the transaction from the host module's own code.
 */

#ifndef _NPUNWA_H_
#define _NPUNWA_H_

/*
 * ---------------------------------------------------------------------------------------
 * The header. Published by the coprocessor; the host reads it and writes none of it.
 * ---------------------------------------------------------------------------------------
 */
#define	NWA_COOKIE		0x00
#define	  NWA_COOKIE_VALUE	0xCAFEBABEU
/*
 * Both the version gate and the body offset, which is unusual enough to be worth saying twice.
 * The host waits for this to read exactly NWA_BODY_EXPECTED before it will touch the window, and
 * then writes the request body at that offset. A different value means a mailbox this code does
 * not understand - which is what the vendor's "HOST-TARGET MailBox versions are different" is.
 */
#define	NWA_BODY_OFF		0x04
#define	  NWA_BODY_EXPECTED	0x34
#define	NWA_MAX_REQ		0x08	/* longest request the far side will accept */
#define	NWA_EVT_OFF		0x0c	/* an event buffer the vendor's host never uses */
#define	NWA_EVT_LEN		0x10

/*
 * ---------------------------------------------------------------------------------------
 * The transaction. Three host writes and one register to watch.
 * ---------------------------------------------------------------------------------------
 */
#define	NWA_TURN		0x18	/* host writes 1 to send, 2 to acknowledge */
#define	  NWA_TURN_REQUEST	1
#define	  NWA_TURN_ACK		2
#define	NWA_REQ_LEN		0x1c	/* request length in bytes, UNROUNDED */
/*
 * The target's status register, and the only thing worth polling. NWA_TURN is the host's own
 * field and the target never writes it - waiting on it waits forever, which is exactly what two
 * attempts did before the host module was read.
 */
#define	NWA_STATUS		0x20
#define	  NWA_STATUS_IDLE	0	/* the previous transaction has finished */
#define	  NWA_STATUS_REPLY	1	/* a reply is in the window */
#define	NWA_REPLY_LEN		0x24

/*
 * ---------------------------------------------------------------------------------------
 * The request body, at NWA_BODY_EXPECTED. Get and set requests are 32 bytes.
 * ---------------------------------------------------------------------------------------
 */
#define	NWA_REQ_SIZE		0x20
#define	NWA_RQ_OP		0x00
#define	NWA_RQ_SUB		0x04
#define	NWA_RQ_PORT		0x08
#define	NWA_RQ_PAYLOAD		0x10

/*
 * The reply body sits immediately after the request, at NWA_BODY_EXPECTED + the UNROUNDED request
 * length - so a 32-byte request is answered at +0x54. Its first word is a fixed marker and its
 * second a status; anything else is a malformed reply and is worth refusing rather than parsing.
 */
#define	NWA_RP_MARKER		0x00
#define	  NWA_RP_MARKER_VALUE	0x14
#define	NWA_RP_STATUS		0x04
#define	  NWA_RP_STATUS_OK	0
#define	NWA_RP_PAYLOAD		0x08

/*
 * The longest raw request and reply the probe below will carry, in 32-bit words. The window is
 * tens of kilobytes and the far side advertises a maximum request of about 32 KB, so neither of
 * these is a protocol limit - they are what fits in a sysctl string without becoming unreadable.
 */
/*
 * The port-statistics request is 34 words: an opcode, a count, and sixteen eight-byte entries.
 * Sixty-four leaves room for a longer list without leaving room for a typo to matter.
 */
#define	NWA_RAW_MAX_REQ_WORDS	64
/*
 * The status reply is 4232 bytes - measured, by sending the request the vendor's host sends and
 * reading what came back - so anything smaller truncates it. 1088 words is that with room.
 */
#define	NWA_RAW_MAX_REPLY_WORDS	1088

/* operations */
#define	NWA_OP_DISCOVER		0x01
#define	NWA_OP_SET		0x03
#define	NWA_OP_GET		0x04
#define	NWA_OP_STATUS		0x45

/*
 * Sub-operations, mapped by issuing the same query to every port and keeping the ones whose
 * answer DIFFERS between them. A value identical everywhere is a capability; a value that tracks
 * which cable is plugged in is state. That distinction is what separated 0x00 from 0x04.
 */
#define	NWA_SUB_STATE		0x00	/* set: admin up/down.  get: carrier, 0 or 1 */
#define	NWA_SUB_MTU		0x02
#define	NWA_SUB_MAC		0x03
#define	NWA_SUB_SPEED		0x04	/* capability, 1000 on every port including dark ones */
#define	NWA_SUB_MEDIA		0x0a	/* 3 on a fibre port, 0 on copper */
#define	NWA_SUB_PROMISC		0x45	/* set: 1 opens the port's TCAM catch-all, 0 closes it */
#define	NWA_SUB_ALLMULTI	0x46
#define	NWA_SUB_MCAST		0x4a

/*
 * ---------------------------------------------------------------------------------------
 * The ports.
 *
 * Numbered by PHYSICAL POSITION, which is not the label the vendor's Linux gives them: under
 * SFOS the two fibre netdevs appear in the opposite order to their sockets. Eight copper ports,
 * then the two fibre sockets in the order they sit on the panel.
 *
 * Four more front ports are reached as SoC interfaces rather than through the switch and do not
 * carry these identifiers. They are not handled here yet.
 * ---------------------------------------------------------------------------------------
 */
#define	NWA_PORT_ID(n)		(0x8000U + (uint32_t)(n) * 0x100U)
#define	NWA_FIRST_PORT		1
#define	NWA_LAST_PORT		10	/* 1-8 copper, 9 and 10 the fibre sockets */

#endif /* _NPUNWA_H_ */
