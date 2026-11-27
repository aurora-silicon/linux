// SPDX-License-Identifier: GPL-2.0-only
#undef get_unaligned_le32
#undef get_unaligned_le64
#undef memcpy
#undef memchr
#undef strcmp
#undef strscpy

/* The channels the 6070 firmware publishes (as in the KUnit suite). */
static const struct {
	const char *name;
	u32 dir, doorbell, slots, offset;
} channels[] = {
	{ "TERMINAL", 2, 0, 512, 0x700 }, { "IO", 0, 1, 32, 0x8700 },
	{ "DEBUG", 0, 1, 8, 0x8f00 },	  { "BUF_H2T", 0, 2, 1, 0x9100 },
	{ "BUF_T2H", 1, 3, 1, 0x9140 },	  { "SHAREDMALLOC", 1, 3, 8, 0x9180 },
	{ "IO_T2H", 1, 3, 32, 0x9380 },
};

static const struct ave_fw_abi abi = { .transport = &ave_transport_6070 };
static const struct ave_fw_layout layout = { .abi = &abi };
static struct ave_dev ave;
static u8 *expect;

static void setup(unsigned int mode)
{
	fw_ave = &ave;
	fw_mode = mode;
	fw_descriptors = ARRAY_SIZE(channels);
	memset(fw_fetched, 0, sizeof(fw_fetched));
	free(ave.ipc.cpu);
	memset(&ave, 0, sizeof(ave));
	ave.layout = &layout;
	ave.ipc.size = ave_transport_6070.ipc_bytes;
	/* Exactly the window: ASan reports any write past its end. */
	ave.ipc.cpu = malloc(ave.ipc.size);
	assert(ave.ipc.cpu);
	memset(ave.ipc.cpu, 0x5a, ave.ipc.size);
	ave.ipc_fw_base = 0x100000000ULL;
	ave.channel_bytes = ave_transport_6070.hello[1];
	for (unsigned int i = 0; i < ARRAY_SIZE(channels); i++) {
		u8 *desc = (u8 *)ave.ipc.cpu + AVE_IPC_CHANNEL_OFF +
			   i * AVE_CHAN_DESC_STRIDE;

		memset(desc, 0, AVE_CHAN_DESC_STRIDE);
		memcpy(desc, channels[i].name, strlen(channels[i].name) + 1);
		put_unaligned_le32(channels[i].dir, desc + AVE_CHAN_DESC_DIR);
		put_unaligned_le32(channels[i].doorbell, desc + AVE_CHAN_DESC_IRQ);
		put_unaligned_le32(channels[i].slots, desc + AVE_CHAN_DESC_SLOTS);
		put_unaligned_le64(ave.ipc_fw_base + AVE_IPC_CHANNEL_OFF +
				   channels[i].offset, desc + AVE_CHAN_DESC_ADDR);
	}
}

static void check_ring(unsigned int id, const char *name, u32 mode,
		       u32 doorbell, u32 slots, u32 offset)
{
	const struct ave_ring *r = &ave.ring[id];

	assert(!strcmp(r->name, name));
	assert(r->mode == mode && r->doorbell == doorbell);
	assert(r->slots == slots && r->offset == AVE_IPC_CHANNEL_OFF + offset);
	assert(r->consumer == (mode ? -1 : 0) && r->producer == (mode ? 0 : -1));
}

static void check_result(void)
{
	size_t table = AVE_IPC_CHANNEL_OFF;
	size_t table_end = table + ARRAY_SIZE(channels) * AVE_CHAN_DESC_STRIDE;
	const u8 *ipc = ave.ipc.cpu;

	check_ring(AVE_RING_IO, "IO", 1, 1, 32, 0x8700);
	check_ring(AVE_RING_T2H, "IO_T2H", 0, 3, 32, 0x9380);
	check_ring(AVE_RING_SMALLOC, "SHAREDMALLOC", 0, 3, 8, 0x9180);
	check_ring(AVE_RING_LOG, "TERMINAL", 0, 0, 512, 0x700);
	/*
	 * Outside the descriptors (the firmware's to change) the window holds
	 * exactly the hand-over of the validated host-to-firmware rings.
	 */
	for (size_t b = 0; b < ave.ipc.size; b++) {
		if (b >= table && b < table_end)
			continue;
		if (ipc[b] != expect[b]) {
			fprintf(stderr, "FAIL: byte %#zx is %#x, want %#x\n", b,
				ipc[b], expect[b]);
			abort();
		}
	}
}

static void well_behaved(void)
{
	setup(0);
	expect = malloc(ave.ipc.size);
	assert(expect);
	memcpy(expect, ave.ipc.cpu, ave.ipc.size);
	for (unsigned int i = 0; i < ARRAY_SIZE(channels); i++) {
		if (channels[i].dir)
			continue;
		for (unsigned int n = 0; n < channels[i].slots; n++) {
			u8 *slot = expect + AVE_IPC_CHANNEL_OFF +
				   channels[i].offset + n * AVE_RING_SLOT_BYTES;

			put_unaligned_le64(1, slot);
			put_unaligned_le64(0, slot + 8);
			put_unaligned_le64(0, slot + 16);
		}
	}
	assert(ave_ipc_parse_channels(&ave) == 0);
	check_result();
	/* Every descriptor field the parser depends on was read. */
	for (unsigned int i = 0; i < ARRAY_SIZE(channels); i++)
		for (unsigned int b = AVE_CHAN_DESC_DIR;
		     b < AVE_CHAN_DESC_ADDR + 8; b++)
			assert(fw_fetched[i * AVE_CHAN_DESC_STRIDE + b]);
}

/* The firmware rewrites a field right after the host has read it. */
static const struct {
	const char *name, *what;
	unsigned int mode;
} cases[] = {
	{ "slots", "ring size changed after its check", FW_SLOTS },
	{ "addr", "ring address changed after its check", FW_ADDR },
	{ "name", "channel name changed after its check", FW_NAME },
	{ "once", "descriptor bytes fetched more than once", FW_ONCE },
};

int main(int argc, char **argv)
{
	well_behaved();
	for (unsigned int c = 0; c < ARRAY_SIZE(cases); c++) {
		int ret;

		if (argc > 1 && strcmp(argv[1], cases[c].name))
			continue;
		setup(cases[c].mode);
		ret = ave_ipc_parse_channels(&ave);
		if (ret) {
			fprintf(stderr, "FAIL: %s: parse returned %d\n",
				cases[c].what, ret);
			abort();
		}
		check_result();
		printf("ok: %s\n", cases[c].what);
	}
	free(ave.ipc.cpu);
	free(expect);
	puts("ok: channel descriptors read once, hand-over only to validated rings");
	return 0;
}
