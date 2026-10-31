.. SPDX-License-Identifier: GPL-2.0-only OR MIT

Apple internal-panel colour metadata
====================================

The panel identity EDID and its colour description are separate inputs. A
checksum-valid EDID with zero RGB primaries does not describe an sRGB panel;
it describes unknown primaries. A Display P3 default profile must not be
inferred merely from the Apple vendor ID or the DCP/SoC generation.

Sources and publication path
----------------------------

The existing paths in appledrm are:

* External displays: ``epic/dpavservep.c`` obtains an EDID with the existing
  DCPAV service command. ``dcp_get_modes()`` in ``iomfb.c`` publishes it.
* Desktop internal panels: the ``DisplayAttributes`` dictionary reaches
  ``dcpep_process_chunks()`` in ``iomfb_template.c`` or the existing dictionary
  setters in ``iomfb_v26_6_desktop.c``. ``parse_panel_metadata()`` in
  ``parser.c`` decodes the description; ``dcp_panel_metadata_replace()`` in
  ``dcp.c`` retains it. ``dcp_panel_edid_snapshot()`` prefers a validated raw
  panel EDID and otherwise calls ``dcp_panel_edid()`` in
  ``dcp-display-metadata.c``. ``dcp_get_modes()`` publishes that snapshot with
  ``drm_edid_connector_update()``. The actual mode catalog remains DCP's
  ``TimingElements``, rather than the EDID's identity timing summary.
* H17P internal panels currently have no supported internal EDID command
  transport and do not enter the desktop identity fallback. An empty EDID on
  that path is a distinct missing publication path, not evidence for P3.

Before the optional-colour fix, the desktop parser retained only
``ProductAttributes`` and the panel builder unconditionally emitted zero RGB
primaries, nominal D65 white and unknown gamma (0xff). The shared colour
encoder now validates and encodes native primaries and white independently
of an optional gamma. Incomplete,
duplicate or malformed optional fields leave panel identity usable. A
standalone ``ProductAttributes`` update for the same panel retains its previous
colour description. A different panel serial cannot inherit that description.
Validated raw EDIDs retain precedence and are not rewritten by this fix.
There is no model/gamut table or new platform switch in this implementation.

The firmware fields to verify are precisely:

* ``DisplayAttributes.Chromaticity.Red.X`` and ``.Y``;
* ``DisplayAttributes.Chromaticity.Green.X`` and ``.Y``;
* ``DisplayAttributes.Chromaticity.Blue.X`` and ``.Y``;
* ``DisplayAttributes.DefaultWhitePoint.X``, ``.Y`` and optional ``.Gamma``.

The existing parser interprets these as unsigned 16.16 values. EDID base-block
chromaticities round to units of 1/1024; gamma rounds to a hundredth. An absent
transfer characteristic remains unknown (0xff) even when valid primaries and
white can be published; it must not be replaced by an invented panel gamma.
The optional values are validated before any colour bytes are changed,
including nonzero/bounded coordinates, nondegenerate primary triangle and
white-point containment. These are metadata validation rules, not panel
calibration or a measurement of its optical output.

Data still required
------------------

The current ``dcp-panel-adt.c`` reader consumes panel identity and dimensions,
not colour calibration. Neither its implementation nor the panel binding
establishes an ADT native-primary property. Do not interpret arbitrary ADT
calibration blobs, display mode colourimetry IDs, or an ICC profile's
D50-adapted matrix as native panel xy coordinates.

First inspect the cached firmware ``DisplayAttributes`` dictionary for the
fields above. If they are absent, obtain verified native panel xy coordinates,
white and transfer characteristic through a documented ADT/firmware producer.
The fallback then belongs on the existing ``panel`` child of the DCP node,
with a binding in ``Documentation/devicetree/bindings/display/apple,dcp.yaml``
and a shared reader alongside ``dcp_panel_read_dimensions()`` in ``dcp.c``.
A concrete proposed representation is ``apple,native-chromaticities`` (eight
u32 16.16 cells, Rx/Ry/Gx/Gy/Bx/By/Wx/Wy) plus ``apple,native-gamma`` (one
u32 16.16 cell). These names are a proposal, not supported properties in this
patch. The bootloader can populate them from verified native data; static
board DT values require an authoritative panel specification. Firmware data
should take precedence over a DT fallback. The internal metadata store and
publication path must also be shared with H17P rather than widening the
external-display service/ownership exceptions.

There is currently no verified runtime colour payload in the investigation
that warrants populating these properties. The optional-colour fix therefore
does not assert that a particular panel now advertises P3, and it does not
manufacture a colour description when firmware omits it.

Expected gamut and userspace
---------------------------

Apple specifies the 2024 16-inch MacBook Pro as wide colour P3 [1]_, but the
MacBook Neo as sRGB [2]_. Neo must not receive a P3 default. Standard Display P3
uses native xy R (0.680, 0.320), G (0.265, 0.690), B (0.150, 0.060), white
(0.3127, 0.3290) [3]_. These standard values are useful test fixtures, not a
substitute for a particular panel's native calibration. ICC rXYZ/gXYZ/bXYZ
values expressed in a D50 connection space require the profile's chromatic
adaptation to be undone before comparison with a D65 EDID.

Mutter 51's ``meta-color-device.c`` rejects zero primaries when generating an
EDID ICC profile, and also needs a usable gamma. Its SDR native colour mode
uses EDID primaries and transfer information; its default colour mode still
uses sRGB [4]_. Thus EDID/profile detection alone does not prove wide-gamut
output is selected. The DRM ``Colorspace`` property describes output signal
encoding, not an inventory of ICC profiles; a P3 label need not appear there.

Fedora, Arch Linux ARM and NixOS need the same usable kernel metadata and
compatible Mutter/colord colour-management stack. With a valid native EDID,
a separate distro-supplied ICC is not inherently required just to identify
the gamut. A measured/complex transfer curve, missing firmware data, or an
older compositor can still require a userspace profile or compositor work.
No three-distro runtime parity or post-patch profile selection is established
by the host selftests. Preserve an existing calibrated profile until native
EDID and the effective compositor colour mode have been checked.

Read-only verification
----------------------

Run these through the hardware controller with explicit device identity and
policy approval. Reading the EDID attribute size with stat is insufficient;
read its contents::

    for edid in /sys/class/drm/card*-*/edid; do
        bytes=$(cat "$edid" | wc -c)
        printf '%s: %s bytes\n' "$edid" "$bytes"
        if test "$bytes" -gt 0; then
            edid-decode "$edid"
        fi
    done
    modetest -M apple -c
    colormgr get-devices-by-kind display
    colormgr get-profiles

Sysfs may report zero file size even for a populated EDID. Missing tools
should be reported; do not install packages or change a display mode to obtain
this evidence. Firmware dictionary
inspection reads only the cached ``DisplayAttributes`` debugfs file, after
controller approval. Do not enable new trace streams or copy raw panel data
into public fixtures.

Check nonzero decoded native primaries/white, gamma, EDID checksum, the internal
connector and its unchanged mode list, then the display device's default
profile in GNOME Colour or colormgr. A successful result records whether the
profile is generated from EDID or explicitly assigned, and the effective
Mutter colour mode. Compare profiles after accounting for ICC chromatic
adaptation. Publish only sanitized colour summaries, not raw EDIDs, serials,
firmware dictionaries or calibration records.

Host regression command (no hardware access)::

    python3 tools/testing/selftests/drivers/gpu/apple-dcp/run.py \
        --output /tmp/apple-dcp-results

The ``panel-colorimetry`` test uses synthetic P3 and sRGB primaries with a
synthetic gamma of 2.2 through the production parser and encoder. It checks packed coordinates, gamma, checksums,
missing/malformed/duplicate fields, degenerate geometry and same-panel identity
updates. Existing external EDID and panel identity tests remain relevant.

.. [1] https://support.apple.com/en-gb/121554
.. [2] https://www.apple.com/macbook-neo/specs/
.. [3] https://www.w3.org/TR/css-color-4/#predefined-display-p3
.. [4] https://gitlab.gnome.org/GNOME/mutter/-/blob/51.0/src/backends/meta-color-device.c
