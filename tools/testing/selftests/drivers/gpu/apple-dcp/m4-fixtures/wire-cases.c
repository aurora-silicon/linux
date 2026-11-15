/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Synthetic inputs for the frozen, working M4 firmware request encoder. */
static void emit_case(FILE *out, unsigned variant)
{
	unsigned char bytes[0x1bd8];
	struct dcp_m4_frame frame, cursor;
	struct dcp_m4_upper upper;
	bool primary = true, brightness = false;
	unsigned nits = 0, keep = 0;
	const struct dcp_m4_frame *present;
	const struct dcp_m4_upper *overlay = NULL;

	frame = (struct dcp_m4_frame){
		.dva = 0x10012340000ULL, .backing_size = 0x4000000,
		.width = DCP_M4_PANEL_WIDTH, .height = DCP_M4_PANEL_HEIGHT,
		.stride = (DCP_M4_PANEL_WIDTH * 4 + 63) & ~63U,
		.format = DCP_M4_XRGB8888,
	};
	cursor = (struct dcp_m4_frame){
		.dva = 0x10056780000ULL, .backing_size = 0x4000,
		.width = 64, .height = 64, .stride = 256,
		.format = DCP_M4_ARGB8888,
	};
	upper = (struct dcp_m4_upper){
		.frame = &cursor, .index = 3, .x = 31, .y = 47,
		.id = 0x1234, .flags = 0x9876,
	};
	present = &frame;
	switch (variant) {
	case 0: break;
	case 1: frame.format = DCP_M4_ARGB8888; break;
	case 2: present = NULL; break;
	case 3: brightness = true; nits = 0; break;
	case 4: brightness = true; nits = 120; break;
	case 5: brightness = true; nits = 500; break;
	case 6: overlay = &upper; keep = 8; break;
	case 7: overlay = &upper; primary = false; present = NULL; break;
	case 8:
		overlay = &upper; primary = false; present = NULL;
		upper.frame = NULL;
		break;
	case 9: present = NULL; keep = 8; break;
	default: assert(!"unknown case");
	}
	memset(bytes, 0xa5, sizeof(bytes));
	assert(!dcp_m4_swap_encode_full(bytes, DCP_M4_SWAP_SIZE, 0x10203040,
		primary, present, overlay, keep, brightness, nits, dcp_m4_frame_check));
	assert(fwrite(bytes, DCP_M4_SWAP_SIZE, 1, out) == 1);
}

int main(int argc, char **argv)
{
	FILE *out;
	assert(argc == 2);
	out = fopen(argv[1], "wb");
	assert(out);
	for (unsigned variant = 0; variant < 10; variant++)
		emit_case(out, variant);
	assert(!fclose(out));
	return 0;
}
