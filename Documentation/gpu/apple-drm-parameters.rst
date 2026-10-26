Apple display module parameters
===============================

Use ``appledrm.show_notch=1`` to include the notch rows in the panel mode.
This common preference also applies to the M3 internal display firmware;
it replaces the platform-specific ``appledrm.t6030_show_notch`` alias.
When the boot framebuffer should cover those rows too, keep the useful
``chosen.asahi,show-notch=1`` m1n1 display preference alongside it.

The old ``appledrm.dcpext_probe`` parameter had no consumer and has been
removed. External display support follows the device tree and the driver's
compatible and firmware admission checks.
