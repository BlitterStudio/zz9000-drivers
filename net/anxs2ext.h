/*
 * AmiNetXDuo, private SANA-II buffer-management extensions.
 * SPDX-License-Identifier: MIT
 */
#ifndef AMINETXDUO_ANXS2EXT_H
#define AMINETXDUO_ANXS2EXT_H

#include <exec/types.h>

/* S2_Dummy is (TAG_USER + 0xB0000), spelled out so this header needs no
   sana2.h; the offsets sit clear of S2_Dummy's growing neighbourhood. */

/* Where would `len` payload bytes for this posted CMD_READ land?  NULL
   declines, and the device falls back to the staging copy + S2_CopyToBuff.  A
   non-NULL answer must be followed by exactly one RX_FILLED. */
#define ANXD_S2_RX_DIRECT       (0x80000000UL + 0xB0000UL + 0x4181UL)

/* `len` bytes written at the answered pointer.  `summed` says whether `sum`
   holds the running longword ones-complement sum (n68k_copy_sum_longwords
   semantics, zero-padded tail).  Both hooks run at interrupt level, before
   the CMD_READ is replied. */
#define ANXD_S2_RX_FILLED       (0x80000000UL + 0xB0000UL + 0x4182UL)

/* THE LINK HEADER, WRITTEN ONCE INSTEAD OF COPIED TWICE.
 *
 * Without it the fourteen bytes travel a long way for what they are: the
 * device lifts the two addresses out of the frame into ios2_SrcAddr and
 * ios2_DstAddr, and the opener lifts them back out of the request into the
 * packet and adds the type -- four six-byte moves and a word, per frame, for
 * bytes the device was already holding.
 *
 * Set, and the device writes the WHOLE fourteen-byte header at the fourteen
 * bytes IMMEDIATELY BEFORE the pointer RX_DIRECT answered, then fills the
 * request fields as before.  The opener may then skip synthesising it.
 *
 * ONLY MEANINGFUL WITH RX_DIRECT, and only on a cooked read: the direct path
 * refuses SANA2IOF_RAW requests already (netdev_direct.c:145), because a raw
 * destination starts at the frame and there is nothing in front of it.  An
 * opener that sets this promises those fourteen bytes are its own to write.
 *
 * ti_Data IS A POINTER TO A BOOL, NOT A FLAG AND NOT A HOOK.  A device that
 * understands the tag sets it TRUE; one that does not leaves it alone, and the
 * opener then knows to keep synthesising.  There is no other way round: the
 * tag list is one-way, RX_FILLED's signature is published and cannot grow an
 * argument, and an opener that guessed from RX_FILLED alone would hand a
 * third-party direct-path driver's frames a header of stale bytes.*/
#define ANXD_S2_RX_LINK_HDR     (0x80000000UL + 0xB0000UL + 0x4183UL)

/* MORE IN RX_FILLED's LAST ARGUMENT THAN A YES OR NO.
 *
 * `summed` was a boolean.  It is a flag byte now, and the old meaning is its
 * low bit, so a device and an opener that know only the boolean still agree:
 * a device that sets SUMMED alone is the device there was, and an opener that
 * tests `!= 0` sees the same thing it saw, because the two new bits are only
 * ever set by a device that was told it may.
 *
 * VERIFIED   the device checked this frame itself: the IPv4 header checksum
 *            and the TCP or UDP checksum, from the sum its copy already
 *            produced and the headers it had in cache.  The opener may skip
 *            its own walk and mark the packet's checksums as done.  Never set
 *            on a frame with Ethernet padding past the IP total length, an
 *            IP header with options, a fragment, or a UDP checksum of zero.
 * CONTINUES  this frame's TCP payload is the next bytes of the SAME stream as
 *            the frame the device delivered immediately before it: same
 *            addresses and ports, the previous segment's end is this one's
 *            sequence number, the same acknowledgment, window and flags
 *            (ACK, or ACK+PSH), no TCP options, both VERIFIED.  An opener may
 *            chain the two into one segment -- the receive side of what a
 *            large-receive-offload does -- and hand the stack one packet
 *            where the wire carried several.  It is a hint about the bytes,
 *            not an instruction: an opener that has already delivered the
 *            previous frame delivers this one whole, and nothing is lost.
 *
 * ANXD_S2_RX_FLAGS, in the buffer-management list, is how an opener says it
 * understands the two.  ti_Data IS A POINTER TO A UBYTE.  The opener may
 * preload the flags it wants; the device replaces them with the intersection
 * it accepted and sets only those from then on.  A zero input retains the
 * first published contract and asks for every flag the device supports, so an
 * older opener and a newer device still agree.  CONTINUES requires VERIFIED;
 * an opener may request VERIFIED alone.  Without the tag a device sets SUMMED
 * alone, whatever it could have said.*/
#define ANXD_S2_RX_FLAGS        (0x80000000UL + 0xB0000UL + 0x4184UL)

#define ANXD_S2_RXF_SUMMED      0x01
#define ANXD_S2_RXF_VERIFIED    0x02
#define ANXD_S2_RXF_CONTINUES   0x04

/* THE TRANSPORT CHECKSUM, WRITTEN BY THE CARD ON THE WAY OUT.
 * ti_Data IS A POINTER TO A UBYTE, preloaded by the opener with the
 * ANXD_S2_TXF_* bits it can prepare frames for PLUS ANXD_S2_TXF_ASKED; the
 * device replaces the byte with the intersection it will honour, ASKED
 * cleared.  A device that does not know the tag leaves the byte alone, and
 * the opener that reads ASKED back knows nothing was agreed: a device that
 * answers every other tag here and not this one would otherwise be sent
 * frames whose checksum field it never finishes.  For each bit accepted, a
 * CMD_WRITE that carries
 * ANXD_S2IOF_L4_CSUM in io_Flags promises: a cooked IPv4 datagram, not a
 * fragment, whose transport header is TCP (TXF_TCP) or UDP (TXF_UDP), and
 * whose checksum field holds the ones-complement sum of the pseudo-header,
 * folded to sixteen bits and not complemented.  The device sums the
 * transport header and payload on top of it and writes the complement into
 * the field before the frame leaves; the opener writes nothing else there.
 * The bit is in SANA-II's unused part of io_Flags (bits 1-4) and is set only
 * on a device that answered the tag, so no other driver ever sees it. */
#define ANXD_S2_TX_CSUM         (0x80000000UL + 0xB0000UL + 0x4185UL)

#define ANXD_S2_TXF_TCP         0x01
#define ANXD_S2_TXF_UDP         0x02
#define ANXD_S2_TXF_ASKED       0x80

#define ANXD_S2IOF_L4_CSUM      0x10

/* ANXD_CMD_RX_POLL: "hand over what you are holding for my reads".
 *
 * A driver that empties a deep hardware ring in one interrupt can find the
 * opener's posted reads run out part way through a burst.  Rather than drop
 * the rest, a driver that knows this command leaves those frames where they
 * are and delivers them the next time reads are posted: at its next
 * interrupt, or when the opener sends this command, which an opener does
 * once at the end of each pass over its completed reads, after it has
 * re-posted them and before it sleeps.  Quick (IOF_QUICK honoured, nothing
 * to wait for), no arguments, io_Error 0; a driver that does not know it
 * answers IOERR_NOCMD like any other unknown command, a unit whose card
 * cannot hold a frame for a late read answers S2ERR_NOT_SUPPORTED, and on
 * either the opener stops sending it.  The number is in the same private
 * range as the tags above. */
#define ANXD_CMD_RX_POLL        0x4190

/* ANXD_CMD_READ_BATCH: many CMD_READs in one call.
 *
 * ios2_Data points to an Exec List of IOSana2Req, each prepared exactly as a
 * CMD_READ about to be sent with BeginIO() (io_Command, ios2_PacketType,
 * ios2_Data, the reply port); the driver queues them all as if each had
 * been sent in list order, under one Disable(), and the list is emptied.
 * Quick, no arguments of its own, io_Error 0; IOERR_NOCMD from a driver that
 * does not know it, S2ERR_NOT_SUPPORTED from a unit whose card cannot hold
 * frames while its reads are away (the list untouched either way), and the
 * opener sends them one by one from then on -- the list, not the error, says
 * who owns the reads.  An offline unit answers each request in the list
 * S2ERR_OUTOFSERVICE the way it answers a CMD_READ.  On Emu68 a BeginIO() is
 * a trapped Disable() pair, 5.5 us; a reader re-posting a burst of reads
 * pays it once with this. */
#define ANXD_CMD_READ_BATCH     0x4191

/* ANXD_CMD_RX_CAPACITY: "how much can your hardware hold from the wire?"
 *
 * Answered in ios2_DataLength: the bytes of received frames the unit's own
 * receive memory holds, at line rate, while nobody drains it -- the ring
 * or FIFO after which the next frame is lost.  A card that can pause the
 * wire instead may answer 0, which means "no limit worth stating", and so
 * does a driver that cannot say.  Quick, no arguments, io_Error 0;
 * IOERR_NOCMD from a driver that does not know it.
 *
 * What an opener does with it: keep the TCP window it advertises on that
 * interface inside the number, so a peer on the same LAN cannot put more
 * on the wire at once than the card can take.  Measured 2026-09-16 on an
 * A3000 (25 MHz 68030) with an X-Surf 100: a 100,352-byte window against
 * a 13 KB ring was 42 overruns and 42 chip resets in ten seconds and
 * 2.8 Mbit/s. */
#define ANXD_CMD_RX_CAPACITY    0x4192

typedef UBYTE *(*AnxdS2RxDirect)(APTR ios2_data, ULONG len);
typedef VOID   (*AnxdS2RxFilled)(APTR ios2_data, ULONG len, ULONG sum,
                                 UBYTE flags);

#endif /* AMINETXDUO_ANXS2EXT_H */
