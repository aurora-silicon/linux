/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* M4 AFK ring admission with old EPIC channel/kind queue fields. */
/* Wrap using duplicate queue headers, as in Sven Peter's Asahi afk_send_epic:
 * one at the tail and one before the complete payload at offset zero. Never
 * split a payload or overwrite unread blocks. Keep one block empty so equal
 * pointers mean empty. All admission checks precede shared-memory writes;
 * published packets stay mapped even if the mailbox notification fails.
 */
static int m3_epic_ring_write(struct afkc_ring *r, u32 channel, u32 kind,
			     const u8 *data, u32 bytes, u32 *published)
{
	u32 rd, wr, total, next;
	u8 *qe;
	bool wrap;

	if (!r || !r->header || !r->data || !data || !published ||
	    r->block < 64 || r->block > 256 || (r->block & (r->block - 1)) ||
	    r->size < r->block || (r->size & (r->block - 1)) ||
	    bytes < 24 || bytes > 1024)
		return -EINVAL;
	total = ALIGN(bytes + 16, r->block);
	rd = READ_ONCE(*(u32 *)(r->header + r->block));
	wr = READ_ONCE(*(u32 *)(r->header + 2 * r->block));
	dma_rmb();
	if (rd >= r->size || wr >= r->size || (rd | wr) & (r->block - 1))
		return -EPROTO;
	wrap = total > r->size - wr;
	if (total >= r->size || (wrap && (wr < rd || total >= rd)) ||
	    (wr < rd && total >= rd - wr))
		return -ENOSPC;
	next = wrap ? total : wr + total;
	if (next == r->size)
		next = 0;
	if (next == rd)
		return -ENOSPC;
	qe = r->data + wr;
	if (wrap) {
		memset(qe, 0, 16);
		put_unaligned_le32(0x20504f49, qe);
		put_unaligned_le32(bytes, qe + 4);
	put_unaligned_le32(channel,qe+8);put_unaligned_le32(kind,qe+12);
		qe = r->data;
	}
	memset(qe, 0, total);
	put_unaligned_le32(0x20504f49, qe);
	put_unaligned_le32(bytes, qe + 4);
	put_unaligned_le32(channel,qe+8);put_unaligned_le32(kind,qe+12);
	memcpy(qe + 16, data, bytes);
	dma_wmb();
	WRITE_ONCE(*(u32 *)(r->header + 2 * r->block), next);
	dma_wmb();
	*published = next;
	return 0;
}
