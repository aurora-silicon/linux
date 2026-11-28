.. SPDX-License-Identifier: GPL-2.0-only

Apple display module parameters
===============================

The notch rows of a built-in panel are no longer a module parameter: the
boot loader's ``chosen/asahi,show-notch`` sets the starting choice and the
``notch`` connector property changes it at run time; see
:doc:`apple-dcp-notch`.

The old ``appledrm.dcpext_probe`` parameter had no consumer and has been
removed. External display support follows the device tree and the driver's
compatible and firmware admission checks.
