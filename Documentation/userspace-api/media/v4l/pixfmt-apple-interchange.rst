.. SPDX-License-Identifier: GFDL-1.1-no-invariants-or-later
.. c:namespace:: V4L

.. _pixfmt-apple-interchange:

Apple Interchange YUV formats
=============================

Apple Interchange is a losslessly compressed, tiled representation used for
sharing images between video decoders, GPUs and display controllers. These
fourccs describe opaque storage, rather than CPU-addressable linear pixels.
The names describe the component order, subsampling and bit depth:

.. flat-table:: Apple Interchange formats
    :header-rows: 1

    * - Identifier
      - Fourcc
      - Components
      - Subsampling
      - Bits per sample
    * - ``V4L2_PIX_FMT_APPLE_AX12``
      - ``AX12``
      - NV12
      - 4:2:0
      - 8
    * - ``V4L2_PIX_FMT_APPLE_AX16``
      - ``AX16``
      - NV16
      - 4:2:2
      - 8
    * - ``V4L2_PIX_FMT_APPLE_AX24``
      - ``AX24``
      - NV24
      - 4:4:4
      - 8
    * - ``V4L2_PIX_FMT_APPLE_AX03``
      - ``AX03``
      - P030
      - 4:2:0
      - 10
    * - ``V4L2_PIX_FMT_APPLE_AX23``
      - ``AX23``
      - P230
      - 4:2:2
      - 10
    * - ``V4L2_PIX_FMT_APPLE_AX43``
      - ``AX43``
      - P430
      - 4:4:4
      - 10

Allocation and component offsets
--------------------------------

There is one memory plane. It contains Y image data, Y compression metadata,
interleaved CbCr image data, and CbCr compression metadata, in that order.
Each region starts at a 128-byte-aligned offset from the memory plane base.
Metadata follows its corresponding image data. Luma uses 32 by 32 sample
tiles and 32 bytes of metadata per tile; chroma uses 16 by 16 CbCr-pair tiles
and 8 bytes of metadata per tile. Edge tiles reserve their full capacity.
Ten-bit sample capacity is packed at ten bits per sample, independently of
any linear P010 representation.

For each component, the image capacity is the number of tile columns times
the number of tile rows times the uncompressed tile capacity. Each tile
count is rounded up from the component dimensions. Metadata capacity uses
the tile column and row counts rounded up separately to powers of two.
Each capacity is then rounded up to 128 bytes. Chroma dimensions use rounded
up division by the horizontal and vertical subsampling factors.

For example, a 1920 by 1080 AX12 image has 60 by 34 luma tiles and 60 by 34
chroma tiles. The regions begin at offsets 0 (Y), 2088960 (Y metadata),
2220032 (CbCr), and 3264512 (CbCr metadata). The image occupies 3297280 bytes.
The corresponding AX03 image occupies 4080640 bytes.

``bytesperline`` is zero for AVD Interchange capture: it has no linear row
stride. Always use the negotiated ``sizeimage`` for allocation. Drivers may
reserve additional private reference or codec scratch storage after the
image; that storage is not another exported image component. Do not derive
a chroma offset by multiplying ``bytesperline`` by height. The format-info
bits-per-pixel entries describe sample capacity only, not a complete tiled
allocation including metadata.

DMA-BUF and display import
--------------------------

Use ``DRM_FORMAT_MOD_APPLE_INTERCHANGE = fourcc_mod_code(APPLE, 3)`` when
sharing these images with a DRM consumer. Its numeric value is
``0x0c00000000000003``. The DRM modifier already exists in this tree; this
change does not allocate a new modifier value. V4L2 AX fourccs must not be
passed to DRM as DRM fourccs. The DRM importer must support both the
component format and this modifier, including its opaque single-plane
storage contract. Exporting a DMA-BUF does not establish scanout support.

AVD draft status
----------------

The open-firmware instruction backend offers AX12, AX16, AX03 and AX23
alongside its existing linear capture formats. Linear output remains the
default. The selected AX format uses the compressed reference image at the
start of the allocation and disables decompression through HDR bit 12.
Linear capture keeps decompression and reserves separate compressed
reference storage after the linear image. Format probing does not change
the active reference layout.

AX24 and AX43 have public identifiers, but AVD does not enumerate them:
4:4:4 codec support has not been implemented. The Apple frame-params backend
does not advertise AX formats until its output packing is implemented.
H.264 monochrome requests with AX capture fail with ``EINVAL`` because the
linear CPU chroma fill cannot populate a compressed CbCr plane.

Host layout and capture negotiation checks are available with::

    python3 drivers/media/platform/apple/avd/tests/interchange-test.py

Hardware proof is TODO. Allocation arithmetic and instruction selection
alone do not prove decoded pixel correctness, DMA-BUF import, DCP scanout,
compositor direct scanout, or a power saving. Validate each codec and bit
depth, reference reuse, stream restart, and linear fallback before enabling
this output in a distribution's default userspace pipeline.

The proposed package and browser integration is described in
:doc:`apple-avd-userspace`.
