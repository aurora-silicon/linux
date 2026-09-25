.. SPDX-License-Identifier: GPL-2.0-or-later

=================================
Apple J700 C1FE multitouch report
=================================

The trackpad of the Apple J700 (MacBook Neo) is driven by Apple's C1FE
controller. Its MTP firmware delivers touch data as HID report 0x75 over
the DockChannel HID transport, in a layout that differs from the one the
earlier Apple Silicon laptops use. hid-magicmouse selects this layout when
the transport's ``multi-touch`` device tree node is compatible with
``apple,j700-multitouch``.

Report layout
=============

Offsets count from the report ID byte. Multi-byte fields are
little-endian. A report is a 32-byte header, N contact records of 30 bytes
each and an 8-byte trailer, so its total length is 32 + 30 * N + 8.

===================  ==================================================
Offset / size        Contents
===================  ==================================================
0 / 1                Report ID, 0x75
2 / 1                Header length, 32
3 / 1                4; meaning unknown
16 / 4               Contact section length, 30 * N
20 / 2               Trailer length, 8
22 / 1               Contact count N, at most 16
23 / 1               Bit 0: the button is pressed
32 + 30 * i / 30     Contact record i, for i < N
32 + 30 * N / 8      Trailer; not decoded
===================  ==================================================

Every report is checked against this shape before anything is decoded. A
report that does not match produces no events at all, neither touches nor
releases. The firmware also sends reports 0x60 and 0x52 when it starts;
they carry no touch data.

Contact records
===============

A contact record has the layout of ``struct tp_finger`` in hid-magicmouse:
signed 16-bit X at offset 4 and Y at offset 6 of the record. Y is negated,
as for the other MTP trackpads, and the axis ranges come from the same
dimensions query (feature report 0xd9). The touch area, tool area,
orientation and pressure fields of the record are not known to carry
meaningful values on this controller, which has no force sensor; they are
not decoded, and the input device does not advertise them. The touch area
field is zero on some genuine contacts, so unlike the earlier layout a
record is never dropped because of it: every one of the N declared
contacts is reported.

Contacts are matched to slots by position, as for the other MTP trackpads.
Each valid report is a complete frame, so a report with N = 0 releases
every contact. The button is reported independently of the contact count.

The limit of 16 contacts is the driver's slot capacity, not a measured
property of the hardware. The header checks describe the reports captured
from one J700; firmware that sends other header values is rejected until
its layout is understood.
