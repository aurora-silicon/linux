.. SPDX-License-Identifier: GPL-2.0-only

J700 6 GHz antenna-gain projection
=================================

The clean t-3573 antenna-gain SPEC (2026-10-31), sections 1, 3 and 6,
establishes these immutable board facts. They apply only to the J700/MT7932
board admitted by the driver's existing machine, PCI function and supplier
checks. They are not generic PCI 14c3:7932 properties. No external resource,
undocumented device-tree property or module parameter is required.

Values are signed half-dB gain offsets in SISO, CDD, MIMO order::

  G1  16  16  10
  G2  14  14   8
  G3  15  16  10
  G4  15  15   9
  G5  16  16  10
  G6  15  16  10

The primary-channel/bandwidth lookup is::

  primary          20  40  80  160 MHz
  2                 1   -   -   -
  1..29 step 4      1   1   1   1
  33..61 step 4     2   2   2   2
  65..93 step 4     3   3   3   3
  97..109 step 4    4   4   4   4
  113               4   4   5   4
  117               5   4   5   4
  121..125 step 4   5   5   5   4
  129..181 step 4   5   5   5   5
  185               5   6   5   5
  189               6   6   5   5
  193..221 step 4   6   6   6   6
  225..229 step 4   6   6   -   -
  233               6   -   -   -

A finite TPE cell subtracts the mapped gain independently for each mode.
The calculation uses a wider signed type and refuses unrepresentable
results. 0x7f is absence of a constraint and survives unchanged; no missing
gain is replaced by zero. Unknown primary/bandwidth mappings refuse finite
cells. The encoder covers four bandwidths; station operation remains at most
80 MHz. Country/class, SAR and hardware ceilings remain independent bounds.

For example, channel 117 with EIRP limits (40,40,40,40) half-dBm produces
SISO and CDD (24,25,24,25), MIMO (30,31,30,31). The same vectors apply to
LPI and VLP. PSD -1 dBm/MHz converts to EIRP (24,30,36,42) half-dBm; with G1
this produces SISO/CDD (8,14,20,26), MIMO (14,20,26,32).

KUnit exercises these vectors, every boundary row, signed arithmetic,
sentinels, unsupported edge bandwidths, and legacy v2 VLP compatibility.
No 2.4/5 GHz mode matrix, OTA resource parser, calibration derivation or
firmware readback is established. Firmware acceptance and conducted/radiated
compliance need separate owner qualification. See :doc:`mt7932-neo` for
class selection, legacy compatibility, lifetime and installation.
