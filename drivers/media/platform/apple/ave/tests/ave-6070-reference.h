/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Synthetic field oracle from Start revision 2 and Process P1. This checks the
 * named fields, not the still-unqualified opaque codec/reference unions.
 * All addresses come from test allocations; no captured images are included.
 */
static void ave_6070_reference_start(const struct ave_session *s, u8 *cmd,
				     size_t len)
{
	const struct ave_params *p = &s->p;
	bool hevc = p->codec == AVE_CODEC_HEVC;

	memset(cmd, 0, len);
#define R32(o, v) put_unaligned_le32((v), cmd + (o))
#define R64(o, v) put_unaligned_le64((v), cmd + (o))
	R32(0, hevc ? 5 : 4);
	R64(8, s->token + 100);
	R32(0x10, s->client);
	R32(0x18, hevc);
	R32(0x1c, 6);
	R32(0x20, 0xc8);
	R64(0x40, s->hdr.dma);
	R32(0x48, 0x12c000);
	R32(0x60, 1920);
	R32(0x64, 1088);
	R32(0x78, p->bframes ? 3 : 0);
	R32(0xff30, 4000000);
	R32(0xff34, 26);
	R32(0xff44, 30);
	R32(0xff48, p->fps_den);
	R32(0xff4c, p->fps_num);
	R32(0xff50, 4);
	R32(0xff88, 0);
	R32(0xff8c, 51);
	R32(0xff90, 0);
	R32(0xff94, 51);
	R32(0xfcd8, hevc ? 0x736 : 0x8202);
	R32(0xfcec, 30);
	R64(0x10588, 0x412e848000000000ULL); /* f64(1000000 bytes) */
	R64(0x10590, 0x3ff0000000000000ULL);
	R32(0x105a8, 1);
	for (unsigned int i = 0; i < p->refs; i++) {
		R64(0x88 + 16 * i, s->ref_a[i].dma);
		R64(0xf6b0 + 8 * i, s->ref_b[i].dma);
		R64(0x2a8 + 8 * i, s->ref_c[i].dma);
		R64(0x3b8 + 8 * i, s->ref_d[i].dma);
	}
	for (unsigned int i = 0; i < p->slots; i++) {
		R64(0x4b8 + 8 * i, s->slot[i].coded.dma);
		R32(0x558 + 4 * i, p->coded_cap);
		R64(0x5c0 + 8 * i, s->slot[i].meta.dma);
		R32(0x660 + 4 * i, s->slot[i].meta.size);
	}
	for (unsigned int set = 0; set < 4; set++) {
		R64(0xf7d0 + 8 * set,
		    set ? s->row_a_set[set - 1].dma : s->row_a.dma);
		R64(0xf7f0 + 8 * set,
		    set ? s->row_b_set[set - 1].dma : s->row_b.dma);
		R64(0xf810 + 8 * set,
		    set ? s->fixed_set[set - 1].dma : s->fixed.dma);
		for (unsigned int k = 0; k < (hevc ? 4 : 8); k++) {
			const struct ave_buf *main =
				set ? &s->tile_set[set - 1][k] : &s->tile[k];
			const struct ave_buf *aux =
				set ? &s->tile_aux_set[set - 1][k] :
				      &s->tile_aux[k];
			unsigned int e = 2 * k;

			R64(0xf830 + 8 * (set + 4 * e), main->dma);
			R32(0xfa30 + 4 * (set + 4 * e), main->size);
			R64(0xf830 + 8 * (set + 4 * (e + 1)), aux->dma);
			R32(0xfa30 + 4 * (set + 4 * (e + 1)), aux->size);
		}
	}
	if (!hevc) {
		R32(0x105b0, 4);
		R32(0x105cc, 12);
		R32(0x109d8, p->bframes ? 4 : 1);
		R32(0x109e0, 119);
		R32(0x109e4, 67);
		R32(0x10a48, 1);
		R32(0x10a4c, 0);
		R32(0x10a50, 0);
		R32(0x10a54, 0);
		R32(0x10a58, 4);
		R32(0x10c64, 1);
		cmd[0x10caf] = 0;
	} else {
		for (unsigned int i = 0; i < 2; i++)
			R64(0xfcc8 + 8 * i, s->hevc_extra[i].dma);
		R32(0x105cc, 1);
		cmd[0x105d1] = 1;
		cmd[0x105d2] = 1;
		cmd[0x105f6] = 1;
		R32(0x10600, 6);
		R32(0x246a8, 1);
		cmd[0x246ad] = 1;
		cmd[0x246ae] = 1;
		cmd[0x246d2] = 1;
		R32(0x246dc, 6);
		R32(0x24858, 1920);
		R32(0x2485c, 1088);
		R32(0x24860, 60);
		R32(0x24864, 34);
		R32(0x24868, 1);
		R32(0x2486c, 0);
		R32(0x24870, 0);
		R32(0x24874, 0);
		R32(0x24878, 4);
		R32(0x2487c, 0);
		R32(0x24880, 0);
		R32(0x248a4, 4);
		R32(0x248a8, p->bframes ? 2 : 0);
		R32(0x28490, p->bframes);
		R32(0x28498, 0);
		R32(0x2849c, 0x10000);
	}
#undef R32
#undef R64
}

static void ave_6070_reference_process(const struct ave_session *s, u8 *cmd,
				       size_t len, unsigned int slot,
				       bool force)
{
	const struct ave_params *p = &s->p;
	bool hevc = p->codec == AVE_CODEC_HEVC;
	u32 pic = hevc ? 0x55b0 : 0x9c8;

	memset(cmd, 0, len);
	memcpy(cmd + 0x40, s->process_state + 0x40, hevc ? 0x5570 : 0x984);
	if (hevc) {
		memcpy(cmd + 0x6524, s->process_state + 0x6524, 0x16c);
		memcpy(cmd + 0x6690, s->process_state + 0x6690, 0x1a8);
		for (unsigned int i = 0; i < 256; i++)
			put_unaligned_le64(s->slot[slot].aux.dma + 0x400 * i,
					   cmd + 0x5a8 + 8 * i);
	}
	put_unaligned_le32(hevc ? 8 : 7, cmd);
	put_unaligned_le64(s->token + 100, cmd + 8);
	put_unaligned_le32(s->client, cmd + 0x10);
	put_unaligned_le32(hevc, cmd + 0x18);
	put_unaligned_le32(0x15 + slot, cmd + 0x1c);
	put_unaligned_le32(0xc8, cmd + 0x20);
#define P32(o, v) put_unaligned_le32((v), cmd + pic + (o))
#define P64(o, v) put_unaligned_le64((v), cmd + pic + (o))
	P32(0, 0xf68);
	P32(0x08, 6000000); /* runtime bitrate */
	P32(0x0c, 30);
	P32(0x10, 1);
	P64(0x18, 0x412e848000000000ULL);
	P64(0x20, 0x3ff0000000000000ULL);
	P64(0x28, 0x403e000000000000ULL);
	P32(0x6ec, force);
	P64(0x8c0, 0x10012340000ULL);
	P32(0x8c8, 1920);
	P64(0x8d0, 0x10012340000ULL + 1920 * 1080);
	P32(0x8d8, 1920);
	P32(0x964, 1920);
	P32(0x968, 1080);
	P32(0xc04, slot);
	P64(0xc08, s->slot[slot].coded.dma);
	P64(0xc10, s->slot[slot].meta.dma);
	P32(0xc18, p->coded_cap);
	if (!hevc)
		P64(0xc20, s->slot[slot].aux.dma);
	P32(0xca8, s->frames);
	P32(0xcac, 5);
	P64(0xcb8, s->frames * 480000ULL);
	P64(0xcc0, 14400000);
#undef P32
#undef P64
}
