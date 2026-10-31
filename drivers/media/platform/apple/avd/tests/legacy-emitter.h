/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#undef push
#undef pusha
static void test_legacy_push(struct avd_ctx *ctx,u32 word)
{
	struct avd_segment *s=&ctx->job.segments[ctx->job.num];
	assert(s->num<AVD_MAX_INST);
	s->instructions[s->num++]=word;
}
static void test_legacy_address(struct avd_ctx *ctx,dma_addr_t addr)
{
	if(ctx->dev->variant->quirks&AVD_QUIRK_LSR)test_legacy_push(ctx,addr>>8);
	else {test_legacy_push(ctx,lower_32_bits(addr));test_legacy_push(ctx,upper_32_bits(addr));}
}
static void test_legacy_comp(struct avd_ctx *ctx,dma_addr_t addr,u32 offsets[4])
{
	for(int i=0;i<4;i++) {
		if(ctx->dev->variant->quirks&AVD_QUIRK_LSR)test_legacy_push(ctx,(addr+offsets[i])>>7);
		else test_legacy_address(ctx,addr+offsets[i]);
	}
}
#define push(inst,name) test_legacy_push(ctx,(inst))
#define pusha(addr,name,i) test_legacy_address(ctx,(addr))
