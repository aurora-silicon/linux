/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#include <assert.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
struct list_head { struct list_head *next, *prev; };
#define container_of(p,t,m) ((t *)((char *)(p) - offsetof(t,m)))
#define INIT_LIST_HEAD(h) ((h)->next = (h)->prev = (h))
static bool list_empty(struct list_head *h) { return h->next == h; }
static void list_add_tail(struct list_head *n, struct list_head *h)
{ n->prev=h->prev; n->next=h; h->prev->next=n; h->prev=n; }
static void list_del_init(struct list_head *n)
{ n->prev->next=n->next; n->next->prev=n->prev; INIT_LIST_HEAD(n); }
static void list_splice_tail_init(struct list_head *s, struct list_head *d)
{
 if (list_empty(s)) return;
 s->next->prev=d->prev; d->prev->next=s->next; s->prev->next=d; d->prev=s->prev;
 INIT_LIST_HEAD(s);
}
#define list_for_each_entry_safe(p,n,h,m) \
 for (p=container_of((h)->next,__typeof__(*p),m), n=container_of(p->m.next,__typeof__(*p),m); \
      &p->m != (h); p=n,n=container_of(n->m.next,__typeof__(*n),m))
enum vb2_buffer_state { VB2_BUF_STATE_DONE, VB2_BUF_STATE_ERROR };
struct request { unsigned int complete; };
struct vb2_v4l2_buffer {
 struct { struct { struct request *req; } req_obj; } vb2_buf;
 bool is_held, done; enum vb2_buffer_state state;
};
struct v4l2_m2m_buffer { struct vb2_v4l2_buffer vb; struct list_head list; };
struct avd_frame { unsigned int idx; struct vb2_v4l2_buffer *src, *dst; void *codec_priv; };
struct avd_ctx;
struct avd_codec_frame_ops { void (*retire)(struct avd_ctx *,struct avd_frame *,enum vb2_buffer_state); };
struct legacy_ops { void (*done)(struct avd_ctx *,struct vb2_v4l2_buffer *,struct vb2_v4l2_buffer *,enum vb2_buffer_state); };
struct desc { const struct legacy_ops *ops; };
struct avd_dev { struct list_head frame_sources[16]; };
struct avd_ctx {
 struct avd_dev *dev; struct list_head slice_sources; struct avd_frame partial;
 struct desc *coded_fmt_desc; int ctrl_hdl;
 struct { void *m2m_ctx; } fh; const struct avd_codec_frame_ops *ops;
};
static unsigned int removed, retired, legacy_retired;
static const struct avd_codec_frame_ops *avd_codec_frame_ops(struct avd_ctx *ctx) { return ctx->ops; }
static void v4l2_ctrl_request_complete(struct request *req, int *hdl) { (void)hdl; req->complete++; }
static void v4l2_m2m_buf_done(struct vb2_v4l2_buffer *buf, enum vb2_buffer_state state)
{ assert(!buf->done); buf->done=true; buf->state=state; }
static void v4l2_m2m_buf_copy_metadata(struct vb2_v4l2_buffer *src, struct vb2_v4l2_buffer *dst)
{ (void)src; (void)dst; }
static void v4l2_m2m_src_buf_remove(void *m2m) { (void)m2m; removed++; }
/* PRODUCTION_HELPERS */
static void retire(struct avd_ctx *ctx, struct avd_frame *f, enum vb2_buffer_state state)
{ (void)ctx; assert(f->codec_priv && state==VB2_BUF_STATE_ERROR); retired++; }
static void legacy_done(struct avd_ctx *ctx, struct vb2_v4l2_buffer *src,
 struct vb2_v4l2_buffer *dst, enum vb2_buffer_state state)
{ (void)ctx; assert(src && dst && state==VB2_BUF_STATE_ERROR); legacy_retired++; }
int main(void)
{
 struct avd_dev dev; struct avd_codec_frame_ops ops={ .retire=retire };
 struct legacy_ops old={ .done=legacy_done }; struct desc desc={ .ops=&old };
 struct avd_ctx ctx={ .dev=&dev, .ops=&ops, .coded_fmt_desc=&desc };
 struct v4l2_m2m_buffer src[16][3]={}; struct request req[16][3]={};
 struct vb2_v4l2_buffer dst[16]={};
 INIT_LIST_HEAD(&ctx.slice_sources);
 for (int n=0;n<16;n++) {
  INIT_LIST_HEAD(&dev.frame_sources[n]);
  for (int i=0;i<3;i++) {
   struct avd_frame f={ .idx=n, .src=&src[n][i].vb, .dst=&dst[n], .codec_priv=&dst[n] };
   INIT_LIST_HEAD(&src[n][i].list); src[n][i].vb.vb2_buf.req_obj.req=&req[n][i];
   avd_source_hold(&ctx,&f);
   assert(!src[n][i].vb.done && !req[n][i].complete && !dst[n].is_held);
   assert(!f.codec_priv && ctx.partial.codec_priv==&dst[n]);
  }
  struct avd_frame f={ .idx=n };
  avd_sources_attach(&ctx,&f);
  assert(list_empty(&ctx.slice_sources) && !ctx.partial.codec_priv);
 }
 assert(removed==48);
 for (int n=15;n>=0;n--) {
  avd_sources_done(&ctx,&dev.frame_sources[n],n&1 ? VB2_BUF_STATE_DONE : VB2_BUF_STATE_ERROR);
  for (int i=0;i<3;i++) assert(src[n][i].vb.done && req[n][i].complete==1);
  assert(list_empty(&dev.frame_sources[n]));
 }
 for (int legacy=0;legacy<2;legacy++) {
  ctx.ops=legacy ? NULL : &ops;
  for (int i=0;i<3;i++) {
   struct avd_frame f={ .src=&src[0][i].vb, .dst=&dst[0], .codec_priv=&dst[0] };
   src[0][i].vb.done=false; req[0][i].complete=0;
   avd_source_hold(&ctx,&f);
  }
  avd_partial_abort(&ctx);
  assert(list_empty(&ctx.slice_sources) && !ctx.partial.codec_priv);
  for (int i=0;i<3;i++) assert(src[0][i].vb.done && src[0][i].vb.state==VB2_BUF_STATE_ERROR);
 }
 assert(retired==1 && legacy_retired==1);
 puts("PASS: production source ownership helpers retain 48 OUTPUTs across 16 pictures; final completion and native/legacy partial abort release once");
}
