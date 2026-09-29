/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* J514S 14.6 RTKit system-only handshake. Based on the M4 session adapter
 * and Asahi mailbox/RTKit transport. No application endpoint or PHY access.
 * All allocations and power references are retained until reboot, even after
 * successful protocol quiescence. That ACK alone is not proof of DMA stop.
 */
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/reboot.h>
#include <linux/debugfs.h>
#include <linux/vmalloc.h>
#include <linux/ktime.h>
#include <asm/sysreg.h>
#include <linux/unaligned.h>
#include "m3_hdmi_control.h"
/* Controller identity selects firmware/memory; connector routing is separate. */
static bool usb_c=DCPEXT_INSTANCE!=0;
module_param(usb_c,bool,0400);
/* Opt-in compatibility experiment for eventual shared-stream routing.
 * The two-lane experiment retains the USB-C payload limit.
 */
static bool hdmi_test_two_lane_hbr2;
module_param(hdmi_test_two_lane_hbr2,bool,0400);
static unsigned int link_max_lanes(void)
{return usb_c || hdmi_test_two_lane_hbr2 ? 2 : 4;}
static unsigned int link_max_rate(void)
{return usb_c || hdmi_test_two_lane_hbr2 ? 20 : 30;}
static unsigned int link_payload_kbps(void)
{return link_max_lanes() * link_max_rate() * 270000U * 8 / 10;}
static const struct m3_usbc_route_ops *usbc_ops;
#include "dcpext_session.c"
#include "dcpext_syslog.h"
static struct dcpext_syslog syslog_capture;
static struct debugfs_blob_wrapper syslog_blob;
static struct dcpext_session session;
static DECLARE_COMPLETION(desktop_started);
static DECLARE_COMPLETION(desktop_finished);
static bool desktop_stop,desktop_done;
static int desktop_result=-EINPROGRESS;
extern int m3_hdmi_bridge_hpd(void);
static int (*desktop_read_hpd)(void);
static u64 desktop_hpd_next;
static unsigned int desktop_hpd_low_samples;
static unsigned int desktop_hpd_high_samples;
static bool desktop_disconnected;
static bool desktop_hpd_dirty, desktop_hpd_rpc, desktop_hpd_value;
static u64 desktop_hpd_deadline;
static unsigned int desktop_link_stage;
/* Bounded diagnostic HPD cycle; actual GPIO is sampled again after 3 s. */
static bool desktop_retrain;
static u64 desktop_retrain_until;
module_param(desktop_retrain,bool,0600);
module_param(desktop_disconnected,bool,0400);
module_param(desktop_stop,bool,0600);
module_param(desktop_done,bool,0400);
module_param(desktop_result,int,0400);
static void desktop_notify(int ret)
{WRITE_ONCE(desktop_result,ret);complete_all(&desktop_started);}

static struct regmap *session_pmgr;
static bool cpu_no_auto;
module_param(cpu_no_auto, bool, 0400);
static bool probe_nmi,nmi_sent;
module_param(probe_nmi,bool,0400);
static bool probe_ping;
module_param(probe_ping, bool, 0400);
static bool ping_sent, ping_replied;
static void __iomem *session_cpu, *session_fifo;
static bool session_cpu_owned, session_fifo_owned;
static phys_addr_t session_base;
static struct resource session_table_ram;
static void *session_table_map, *session_table_saved;
static size_t session_used;
static struct {
 struct resource ram;
 u64 remap;
 u32 size, flags;
} session_segments[32];
static unsigned int session_nsegments;
static void session_cleanup(void)
{
 if (session_table_map) memunmap(session_table_map);
 kfree(session_table_saved);
 if (session_fifo) iounmap(session_fifo);
 if (session_cpu) iounmap(session_cpu);
 if (session_fifo_owned) release_mem_region(session_base + 0x8000, 0x4000);
 if (session_cpu_owned) release_mem_region(session_base, 0x4000);
}
static int session_prepare(struct device_node *dcp)
{
	struct device_node *node;
	struct resource res;
	const u8 *metadata;
	struct regmap *map;
	u32 power;
	int bytes, ret;

	session_base = port ? 0x2d6c00000ULL : 0x2d2c00000ULL;
	if (of_address_to_resource(dcp, 0, &res) || res.start != session_base ||
	    resource_size(&res) != 0x4000 || !(res.flags & IORESOURCE_MEM_NONPOSTED))
		return -ENODEV;
	metadata = of_get_property(dcp, "apple,j514s-dcpext-segments", &bytes);
	if (!metadata || bytes <= 0 || bytes % 32 || bytes / 32 > 32)
		return -EINVAL;
	session_nsegments = bytes / 32;
	for (unsigned int i = 0; i < session_nsegments; i++) {
		node = of_parse_phandle(dcp, "memory-region", i);
		ret = -EINVAL;
		if (node && of_device_is_available(node) &&
		    of_property_read_bool(node, "no-map") &&
		    of_device_is_compatible(node, "apple,asc-mem"))
			ret = of_address_to_resource(node, 0, &session_segments[i].ram);
		of_node_put(node);
		if (ret)
			return ret;
		session_segments[i].remap = get_unaligned_be64(metadata + i * 32 + 16);
		session_segments[i].size = get_unaligned_be32(metadata + i * 32 + 24);
		session_segments[i].flags = get_unaligned_be32(metadata + i * 32 + 28);
		if (!session_segments[i].size ||
		    session_segments[i].ram.start != get_unaligned_be64(metadata + i * 32) ||
		    resource_size(&session_segments[i].ram) != ALIGN((u64)session_segments[i].size, LEASE_PAGE))
			return -EINVAL;
	}
	/* No CPU control writes. SYS/FE checks are in the shared mapper. */
	node = of_find_node_by_path("/soc/power-management@350700000");
	if (!node)
		return -ENODEV;
	map = syscon_node_to_regmap(node);
	of_node_put(node);
	if (IS_ERR(map))
		return PTR_ERR(map);
	session_pmgr = map;
	ret = regmap_read(map, port ? 0x3f8 : 0x3e0, &power);
	if (ret)
		return ret;
	if ((power & 15) != 15 || power & (BIT(31) | BIT(12) | BIT(11) | BIT(10)))
		return -EHOSTDOWN;
	if (!request_mem_region(session_base, 0x4000, "m3-dcpext-session"))
		return -EBUSY;
	session_cpu_owned = true;
	if (!request_mem_region(session_base + 0x8000, 0x4000, "m3-dcpext-session"))
		return -EBUSY;
	session_fifo_owned = true;
	/* Mapping here does not touch the registers; fabric is checked before
	 * session_run accesses these windows.
	 */
	session_cpu = ioremap_np(session_base, 0x4000);
	session_fifo = ioremap_np(session_base + 0x8000, 0x4000);
	return session_cpu && session_fifo ? 0 : -ENOMEM;
}

static int session_tables(const struct resource *reservation)
{
	session_table_ram = *reservation;
	if (!resource_size(reservation) || resource_size(reservation) > 0x100000)
		return -EINVAL;
	session_table_map = memremap(reservation->start, resource_size(reservation), MEMREMAP_WB);
	if (!session_table_map)
		return -ENOMEM;
	session_table_saved = kmemdup(session_table_map, resource_size(reservation), GFP_KERNEL);
	return session_table_saved ? 0 : -ENOMEM;
}

static int session_translate(u64 wire, phys_addr_t *physical)
{
	u64 dva, entry, leaf_pa, pte;
	u64 *table;

	if ((wire >> 36) != 0x10)
		return -ERANGE;
	dva = wire & GENMASK_ULL(35, 0);
	entry = READ_ONCE(root[dva >> 25]);
	if (!(entry & 1))
		return -ENOENT;
	leaf_pa = (entry & PTE_ADDRESS) << 4;
	if (leaf_pa < session_table_ram.start || leaf_pa > session_table_ram.end ||
	    session_table_ram.end - leaf_pa + 1 < LEASE_PAGE)
		return -ERANGE;
	table = session_table_map + leaf_pa - session_table_ram.start;
	pte = READ_ONCE(table[(dva >> 14) & 2047]);
	if (!(pte & 1))
		return -ENOENT;
	*physical = ((pte & PTE_ADDRESS) << 4) | (dva & (LEASE_PAGE - 1));
	return 0;
}

static int session_buffer(void *cookie, u8 ep, u64 requested, u32 size, u64 *dva)
{
	phys_addr_t pa, checked;
	size_t rounded = ALIGN(size, LEASE_PAGE);
	int ret;

	if (!requested) {
		if (rounded > LEASE_SIZE - session_used)
			return -ENOSPC;
		memset(pool + session_used, 0, rounded);
		*dva = BIT_ULL(40) | (LEASE_DVA + session_used);
		session_used += rounded;
		pr_info("m3_dcpext_session: ep=%u host buffer size=%#x dva=%#llx\n", ep, size, *dva);
		return 0;
	}
	for (unsigned int i = 0; i < session_nsegments; i++) {
		/* ADT flags 0xa OS-log region is a host physical carveout, not
		 * a DART mapping. Exact base and declared size are mandatory.
		 */
		if (ep == 8 && session_segments[i].flags == 0xa &&
		    requested == session_segments[i].ram.start &&
		    requested == session_segments[i].remap && size <= session_segments[i].size) {
			*dva = requested;
			pr_info("m3_dcpext_session: ep=8 reserved physical OSLog size=%#x address=%#llx\n", size, requested);
			return 0;
		}
	}
	ret = session_translate(requested, &pa);
	if (ret)
		return ret;
	for (unsigned int i = 0; i < session_nsegments; i++) {
		struct resource *r = &session_segments[i].ram;
		if (session_segments[i].flags == 0xa || pa < r->start || pa > r->end ||
		    size - 1 > r->end - pa)
			continue;
		/* Verify every page boundary, including an unaligned first page. */
		for (u64 off = 0; off < size; ) {
			ret = session_translate(requested + off, &checked);
			if (ret || checked != pa + off)
				return -ERANGE;
			off += LEASE_PAGE - ((requested + off) & (LEASE_PAGE - 1));
		}
		ret = session_translate(requested + size - 1, &checked);
		if (ret || checked != pa + size - 1)
			return -ERANGE;
		*dva = requested;
		pr_info("m3_dcpext_session: ep=%u inherited size=%#x dva=%#llx physical=%#llx\n", ep, size, requested, (u64)pa);
		return 0;
	}
	return -ERANGE;
}

static u64 session_now(void *cookie)
{
	return ktime_get_boottime_ns() / NSEC_PER_MSEC;
}


/* Validate ALL inherited mappings against the bootloader-owned segments,
 * not just addresses requested by the firmware later. Never follow a table
 * pointer outside the reserved page-table storage. */
static int session_audit(void)
{
 for (unsigned int i = 0; i < LEASE_PAGE / 8; i++) {
  u64 entry = READ_ONCE(root[i]), pa;
  u64 *table;
  if (!(entry & 1)) continue;
  pa = (entry & PTE_ADDRESS) << 4;
  if (pa < session_table_ram.start || pa > session_table_ram.end ||
      session_table_ram.end - pa + 1 < LEASE_PAGE) return -ERANGE;
  table = session_table_map + pa - session_table_ram.start;
  for (unsigned int j = 0; j < LEASE_PAGE / 8; j++) {
   u64 pte = READ_ONCE(table[j]), va = ((u64)i << 25) | ((u64)j << 14);
   bool found = false;
   if (!(pte & 1)) continue;
   pa = (pte & PTE_ADDRESS) << 4;
   for (unsigned int n = 0; n < session_nsegments; n++) {
    u64 start = session_segments[n].remap & GENMASK_ULL(35, 0);
    if (session_segments[n].flags == 0xa) continue;
    if (va >= start && va - start < ALIGN((u64)session_segments[n].size, LEASE_PAGE) &&
        pa == session_segments[n].ram.start + va - start) found = true;
   }
   if (!found) return -ERANGE;
  }
 }
 return 0;
}
static int session_send(void *cookie, u8 ep, u64 message)
{
 u32 status;
 int ret = readl_poll_timeout(session_fifo + 0x110, status, !(status & BIT(16)), 100, 100000);
 if (ret) return ret;
 dma_wmb();
 writeq(message, session_fifo + 0x800);
 writeq(ep, session_fifo + 0x808);
 if(!session.runtime)pr_info("m3_dcpext_session: TX ep=%#x msg=%#llx\n", ep, message);
 return 0;
}
#include "dcpext_afk_channel.h"
static bool quiesce_first;
module_param(quiesce_first, bool, 0400);
static unsigned int afk_endpoint;
module_param(afk_endpoint, uint, 0400);
MODULE_PARM_DESC(afk_endpoint, "Optional announcement-only AFK endpoint, 0x20..0x2b; zero for system-only");
static struct afkc afk, aux_afk;
static unsigned int aux_endpoint;
module_param(aux_endpoint, uint, 0400);
static bool validate_connection;
module_param(validate_connection, bool, 0400);
static bool connect_port;
module_param(connect_port, bool, 0400);
static bool validation_sent, validation_reply, port0_announced;
static u32 port0_channel;

static u64 afk_deadline;
static int afk_allocate(void *cookie, u32 size, u8 **buffer, u64 *dva)
{
 size_t offset = session_used;
 int ret = session_buffer(NULL, afk_endpoint, 0, size, dva);
 if (!ret) *buffer = pool + offset;
 return ret;
}
#include "m3_dcpext_native_query.h"
static bool request_display;
module_param(request_display, bool, 0400);
static bool hpd_low_only;
module_param(hpd_low_only,bool,0400);
static bool hotplug;
module_param(hotplug, bool, 0400);
static u64 hpd_until;
static unsigned int rpc_stage, rpc_command, rpc_bytes, rpc_tag, rpc_target;
static u8 *rpc_rx, *rpc_tx;
static u64 rpc_rx_dva, rpc_tx_dva;
static bool rpc_pending;
static u16 epic_seq;
static unsigned int link_lanes;
module_param(link_lanes,uint,0400);
#include "m3_epic_tx.h"
static int old_epic_write_channel(struct afkc *c,u32 channel,u32 kind,u8 *payload,u32 bytes)
{
 u32 next;int ret;
 if(!c->running || c->stopping || c->error)return -EINVAL;
 ret=m3_epic_ring_write(&c->rings[0],channel,kind,payload,bytes,&next);
 return ret ?: afkc_send(c,0xa2,next);
}
static int old_epic_write(u32 channel,u32 kind,u8 *payload,u32 bytes)
{return old_epic_write_channel(&afk,channel,kind,payload,bytes);}
#include "m3_dcpext_iboot.h"
static int link_activate(void)
{
 int ret;int (*fn)(void);
 if(usb_c)return usbc_ops->activate(port);
 fn=symbol_get(m3_hdmi_phy_activate);if(!fn)return -ENODEV;
 ret=fn();symbol_put(m3_hdmi_phy_activate);return ret;
}
static int link_deactivate(void)
{
 int ret;int (*fn)(void);
 if(usb_c)return usbc_ops->deactivate(port);
 fn=symbol_get(m3_hdmi_phy_deactivate);if(!fn)return -ENODEV;
 ret=fn();symbol_put(m3_hdmi_phy_deactivate);return ret;
}
static int link_xbar_down(void)
{
 int ret;int (*fn)(void);
 if(usb_c)return usbc_ops->xbar_down(port);
 fn=symbol_get(m3_hdmi_xbar_deactivate);if(!fn)return -ENODEV;
 ret=fn();symbol_put(m3_hdmi_xbar_deactivate);return ret;
}
static int link_xbar_up(void)
{
 int ret;int (*fn)(void);
 if(usb_c)return usbc_ops->xbar_up(port);
 fn=symbol_get(m3_hdmi_xbar_activate);if(!fn)return -ENODEV;
 ret=fn();symbol_put(m3_hdmi_xbar_activate);return ret;
}
static int link_get_rate(void)
{
 int ret;int (*fn)(void);
 if(usb_c)return usbc_ops->get_rate(port);
 fn=symbol_get(m3_hdmi_phy_get_rate);if(!fn)return -ENODEV;
 ret=fn();symbol_put(m3_hdmi_phy_get_rate);return ret;
}
static int link_set_rate(unsigned int rate)
{
 int ret;int (*fn)(unsigned int);
 if(usb_c)return usbc_ops->set_rate(port,rate);
 fn=symbol_get(m3_hdmi_phy_set_rate);if(!fn)return -ENODEV;
 ret=fn(rate);symbol_put(m3_hdmi_phy_set_rate);return ret;
}
static int link_set_drive(const unsigned int *drive)
{
 u32 hdmi_drive[12];
 int ret;int (*fn)(const unsigned int *);
 if(usb_c)return usbc_ops->set_drive(port,drive);
 /* The HDMI PHY retains its qualified four-lane physical orientation.
  * DPTX encodes only the two negotiated lanes in the diagnostic mode.
  * Program equal pair settings, as required by the existing PHY helper;
  * this does not claim a new physical lane order or power down either pair.
  */
 if(hdmi_test_two_lane_hbr2){
  memcpy(hdmi_drive,drive,6*sizeof(*drive));
  memcpy(hdmi_drive+6,drive,6*sizeof(*drive));
  drive=hdmi_drive;
 }
 fn=symbol_get(m3_hdmi_phy_set_drive);if(!fn)return -ENODEV;
 ret=fn(drive);symbol_put(m3_hdmi_phy_set_drive);return ret;
}
static int link_hpd(void)
{return usbc_ops->hpd(port);}
static int dptx_rpc_send(u32 command)
{
 u8 *tx,wire[70]={};u64 tx_dva;u32 bytes;
 int ret;
 if (rpc_pending) return -EBUSY;
 bytes=command==12?112:(command==11||command==8)?96:80;
 /* One outstanding RPC: reuse its pinned buffers only after its reply. */
 if(!rpc_rx){ret=afk_allocate(NULL,112,&rpc_rx,&rpc_rx_dva);if(ret)return ret;}
 if(!rpc_tx){ret=afk_allocate(NULL,112,&rpc_tx,&rpc_tx_dva);if(ret)return ret;}
 tx=rpc_tx;tx_dva=rpc_tx_dva;memset(tx,0,112);memset(rpc_rx,0,112);
 put_unaligned_le32(command,tx+4);put_unaligned_le32(bytes-64,tx+8);
 put_unaligned_le32(0x69706378,tx+12);
 if(command==8){put_unaligned_le16(8,tx+2);put_unaligned_le32(desktop_hpd_rpc?desktop_hpd_value:rpc_stage==2 && !hpd_low_only,tx+80);}
 if(command==11||command==12){
  put_unaligned_le32(command==12?0x100:0,tx+64);
  rpc_target=0x8030;
  if(usb_c){
   ret=usbc_ops->target(port);if(ret<0)return ret;
   rpc_target=ret;
  }
  put_unaligned_le32(rpc_target,tx+68);
 }
 wire[0]=2;put_unaligned_le16(epic_seq++,wire+1);
 put_unaligned_le32(30,wire+16);wire[20]=4;wire[21]=0x30;
 put_unaligned_le16(0xc0,wire+22);put_unaligned_le16(rpc_tag,wire+32);
 put_unaligned_le64(rpc_rx_dva,wire+44);put_unaligned_le64(tx_dva,wire+52);
 put_unaligned_le32(bytes,wire+60);put_unaligned_le32(bytes,wire+64);
 rpc_command=command;rpc_bytes=bytes;rpc_pending=true;
 pr_info("m3_dcpext_session: DPTX command=%u tag=%u bytes=%u\n",command,rpc_tag,bytes);
 return old_epic_write(port0_channel,3,wire,sizeof(wire));
}
#include "m3_dcpext_av.h"
static u32 link_drive[8];
static int dptx_apcall(u32 channel,const u8 *payload,u32 size)
{
 u8 reply[512];u32 idx,len;u8 *data;
 int ret;
 if(size<108 || size>sizeof(reply) || get_unaligned_le32(payload+56)!=0x69706378) return -EPROTO;
 idx=get_unaligned_le32(payload+48);len=get_unaligned_le32(payload+52);
 if(len<4||len>size-108)return -EPROTO;
 pr_info("m3_dcpext_session: DPTX callback=%u size=%u\n",idx,len);
 memcpy(reply,payload,size);data=reply+108;
 put_unaligned_le32(0,data);
 switch(idx){
 case 0:
  ret=link_activate();if(ret)return ret;
  break;
 case 1:
  ret=link_xbar_down();if(ret)return ret;
  ret=link_deactivate();if(ret)return ret;
  break;
 case 2: /* 14.6 ABI: two words, voltage and preemphasis (no kind). */
  if(len!=32)return -EINVAL;
  put_unaligned_le32(3,data+16);put_unaligned_le32(3,data+20);break;
 case 3: {
  u32 drive[12]={};
  if(len!=32+8*link_max_lanes() || get_unaligned_le32(data+16)!=link_max_lanes() || link_lanes!=link_max_lanes())return -EOPNOTSUPP;
  for(unsigned int i=0;i<link_max_lanes();i++) {
   drive[3*i+1]=get_unaligned_le32(data+32+i*8);
   drive[3*i+2]=get_unaligned_le32(data+36+i*8);
   if(drive[3*i+1]>3 || drive[3*i+2]>3 || drive[3*i+1]+drive[3*i+2]>3)return -EINVAL;
  }
  ret=link_set_drive(drive);if(ret)return ret;
  memset(link_drive,0,sizeof(link_drive));
  for(unsigned int i=0;i<2*link_max_lanes();i++)link_drive[i]=get_unaligned_le32(data+32+i*4);
  pr_info("m3_dcpext_session: drive %u/%u %u/%u %u/%u %u/%u applied\n",
    link_drive[0],link_drive[1],link_drive[2],link_drive[3],link_drive[4],link_drive[5],link_drive[6],link_drive[7]);
  break;
 }
 case 4:
  if(len!=32+8*link_max_lanes() || get_unaligned_le32(payload+108)!=link_max_lanes() || link_lanes!=link_max_lanes())return -EOPNOTSUPP;
  put_unaligned_le32(link_max_lanes(),data);put_unaligned_le32(0,data+16);
  for(unsigned int i=0;i<2*link_max_lanes();i++)put_unaligned_le32(link_drive[i],data+32+i*4);
  break;
 case 5:
  if(len!=16)return -EINVAL;
  ret=link_xbar_down();if(ret)return ret;break;
 case 6:
  if(len!=16)return -EINVAL;
  ret=link_get_rate();if(ret<0)return ret;
  if(ret){ret=link_xbar_up();if(ret)return ret;}break;
 case 7:
  if(len!=32)return -EINVAL;put_unaligned_le32(link_max_rate(),data+16);break;
 case 8:
  if(len!=32)return -EINVAL;
  ret=link_get_rate();if(ret<0)return ret;
  put_unaligned_le32(ret,data+16);break;
 case 9:
  if(len!=32 || get_unaligned_le32(data+20))return -EINVAL;
  ret=link_xbar_down();if(ret)return ret;
  ret=link_set_rate(get_unaligned_le32(data+16));if(ret)return ret;break;
 case 10:case 11:
  if(len!=32)return -EINVAL;put_unaligned_le64(idx==10?link_max_lanes():link_lanes,data+16);break;
 case 12:
  if(len!=32 || get_unaligned_le64(data+16)>link_max_lanes() ||
     (get_unaligned_le64(data+16)!=link_max_lanes() && get_unaligned_le64(data+16)!=0))return -EOPNOTSUPP;
  link_lanes=get_unaligned_le32(data+16);break;
 case 15:
  if(len!=32 || get_unaligned_le64(data+16))return -EOPNOTSUPP;break;
 case 20: /* DPTX_APCALL_INACTIVE_SINK_DETECTED: notification only. */
  if(len!=16)return -EINVAL;break;
 case 21: /* DPTX_APCALL_SET_TILED_DISPLAY_HINTS: retain/report hints. */
  if(len!=144)return -EINVAL;
  pr_info("m3_dcpext_session: display hints width=%u height=%u pixel_clock=%u\n",
    get_unaligned_le32(data+56),get_unaligned_le32(data+60),get_unaligned_le32(data+64));
  break;
 case 18: /* Host owns the HDMI HPD line and sends the hotplug event. */
  if(len<32)return -EINVAL;put_unaligned_le32(hotplug,data+16);break;
 case 13:case 14:case 16:
  if(len<32)return -EINVAL;put_unaligned_le32(0,data+16);break;
 default:
  /* Never ACK an unimplemented hardware transition. */
  print_hex_dump(KERN_INFO,"m3_dcpext_unhandled: ",DUMP_PREFIX_OFFSET,16,1,data,len,true);
  return -EOPNOTSUPP;
 }
 reply[0]=2;put_unaligned_le16(epic_seq++,reply+1);reply[21]=0x20;
 reply[35]=8;put_unaligned_le32(size-44,reply+36);
 return old_epic_write(channel,8,reply,size);
}
static int afk_record(void *cookie, const u8 *payload, u32 size)
{
 u32 channel = get_unaligned_le32(afk.capture + afk.capture_size - size - 12);
 u32 kind = get_unaligned_le32(afk.capture + afk.capture_size - size - 8);
 if (size >= 76 && payload[0] == 2 && payload[20] == 4 && !payload[21] &&
     get_unaligned_le16(payload + 22) == 0x30 &&
     !memcmp(payload + 40, port?"dispext1:dcpdptx-port-epic:0":"dispext0:dcpdptx-port-epic:0", sizeof("dispext0:dcpdptx-port-epic:0")) &&
     get_unaligned_le32(payload + 72) == 0xd3) {
  port0_announced = true;
  port0_channel = channel;
 }
 if (rpc_pending && (kind==0 || kind==4) && channel==port0_channel &&
     size>=70 && payload[0]==2 && payload[20]==4 && payload[21]==0x20 &&
     get_unaligned_le16(payload+22)==0xc0 && get_unaligned_le16(payload+32)==rpc_tag) {
  pr_info("m3_dcpext_session: RPC response status=%#x bytes=%u\n",get_unaligned_le32(payload+40),get_unaligned_le32(payload+60));
  if(get_unaligned_le32(payload+40) || get_unaligned_le64(payload+44)!=rpc_rx_dva ||
     get_unaligned_le32(payload+60)!=rpc_bytes)return -EPROTO;
  dma_rmb();
  if(get_unaligned_le32(rpc_rx+12)!=0x69706378 ||
     get_unaligned_le32(rpc_rx+4)!=rpc_command)return -EPROTO;
  if((rpc_command==11||rpc_command==12) &&
     (get_unaligned_le32(rpc_rx+64)!=(rpc_command==12?0x100:0) ||
      get_unaligned_le32(rpc_rx+68)!=rpc_target))return -EPROTO;
  pr_info("m3_dcpext_session: DPTX command=%u reply accepted\n",rpc_command);
  rpc_pending=false;rpc_tag++;
  if(desktop_hpd_rpc){
   desktop_hpd_rpc=false;
   pr_info("m3_dcpext_session: runtime link stage=%u HPD=%u acknowledged\n",desktop_link_stage,desktop_hpd_value);
   desktop_link_stage=(desktop_link_stage==3 || desktop_link_stage==5)?0:desktop_link_stage+1;
   return 0;
  }
  rpc_stage++;
  if(hotplug && rpc_stage==3)hpd_until=session_now(NULL)+2000;
  validation_reply = !request_display || rpc_stage==(hotplug?5:3);
 }
 if(kind==0 && channel==port0_channel && size>=40 && payload[0]==2 &&
    payload[20]==4 && payload[21]==0x10 && get_unaligned_le16(payload+22)==0xc0)
  return dptx_apcall(channel,payload,size);
 pr_info("m3_dcpext_session: AFK record ep=%#x bytes=%u\n", afk_endpoint, size);
 print_hex_dump(KERN_INFO, "m3_dcpext_afk: ", DUMP_PREFIX_OFFSET, 16, 1,
                payload, min(size, 256U), true);
 return 0;
}
static const struct afkc_ops afk_ops = {
 .send = session_send, .allocate = afk_allocate, .record = afk_record,
};
static bool log_sent, log_replied;
static int aux_record(void *cookie, const u8 *payload, u32 size)
{
 pr_info("m3_dcpext_session: auxiliary AFK ep=%#x bytes=%u\n",aux_endpoint,size);
 if(aux_endpoint==0x28){
  const u8 *h=aux_afk.capture+aux_afk.capture_size-size-12;
  int ret=av_record(get_unaligned_le32(h),get_unaligned_le32(h+4),payload,size);if(ret)return ret;
 }
 if(iboot_modes && aux_endpoint==0x23){
  u32 channel=get_unaligned_le32(aux_afk.capture+aux_afk.capture_size-size-12);
  int ret=ib_record(channel,payload,size);if(ret)return ret;
 }
 if(aux_endpoint==0x20 && size>=72 && payload[21]==0 && !memcmp(payload+40,"system",7)){
  u8 *tx,*rx,wire[70]={};u64 txd,rxd;int ret;
  static const u8 property[]={0x14,0,0,0,'g','A','F','K','C','o','n','f','i','g','L','o','g','M','a','s','k',0,0,0,0xd3,0,0,0,0x40,0,0,0x84,0xff,0xff,0,0,0,0,0,0};
  u32 channel=get_unaligned_le32(aux_afk.capture+aux_afk.capture_size-size-12);
  if(log_sent)return -EPROTO;
  ret=afk_allocate(NULL,sizeof(property),&tx,&txd);if(ret)return ret;
  ret=afk_allocate(NULL,sizeof(property),&rx,&rxd);if(ret)return ret;
  memcpy(tx,property,sizeof(property));wire[0]=2;put_unaligned_le32(30,wire+16);
  wire[20]=4;wire[21]=0x30;put_unaligned_le16(0x43,wire+22);
  put_unaligned_le64(rxd,wire+44);put_unaligned_le64(txd,wire+52);
  put_unaligned_le32(sizeof(property),wire+60);put_unaligned_le32(sizeof(property),wire+64);
  log_sent=true;return old_epic_write_channel(&aux_afk,channel,3,wire,sizeof(wire));
 }
 if(aux_endpoint==0x20 && log_sent && size>=70 && payload[21]==0x20 && get_unaligned_le16(payload+22)==0x43){
  if(get_unaligned_le32(payload+40))return -EREMOTEIO;
  log_replied=true;pr_info("m3_dcpext_session: firmware verbose logging enabled\n");
 }
 print_hex_dump(KERN_INFO,"m3_dcpext_aux: ",DUMP_PREFIX_OFFSET,16,1,payload,min(size,256U),true);
 return 0;
}
static const struct afkc_ops aux_ops={.send=session_send,.allocate=afk_allocate,.record=aux_record};
#include "m3_dcpext_native_adapter.h"
#include "m3_dcpext_audio.h"
static int session_dispatch(u64 endpoint,u64 message)
{
 if(!session.runtime)pr_info("m3_dcpext_session: RX ep=%#llx msg=%#llx\n",endpoint,message);
 if(session.runtime){int ret=dcpext_session_runtime_message(&session);if(ret)return ret;}
 else if(++received>(native_kms_trial?8192:native_startup?2048:512))return -E2BIG;
 if(!(endpoint&0xff) && (message>>52)==4 && ping_sent && !ping_replied){ping_replied=true;return 0;}
 if(native_query && (endpoint&0xff)==0x37)return native_receive(message);
 if(audio_afk.started && (endpoint&0xff)==0x29)return audio_receive(message);
 if(aux_endpoint && (endpoint&0xff)==aux_endpoint)return afkc_receive(&aux_afk,message);
 if(afk_endpoint && (endpoint&0xff)==afk_endpoint)return afkc_receive(&afk,message);
 if ((endpoint&0xff)==2) {
  struct dcpext_buffer *b=&session.buffers[2];
  u64 offset=b->dva & ~BIT_ULL(40); int ret;
  if (b->ready && (offset<LEASE_DVA || offset-LEASE_DVA>session_used || b->size>session_used-(offset-LEASE_DVA))) return -ERANGE;
  if(session.runtime && syslog_capture.used>sizeof(syslog_capture.capture)-512)syslog_capture.used=0;
  ret=dcpext_syslog_observe(&syslog_capture,message,b->ready?pool+offset-LEASE_DVA:NULL,b->size);
  if(ret)return ret;
 }
 return dcpext_session_receive(&session,endpoint&0xff,message);
}

static struct debugfs_blob_wrapper ram_snapshot[2][32];
static void snapshot_ram(struct dentry *dir, unsigned int sample)
{
 u32 pmgr_state;
 regmap_read(session_pmgr, port?0x3f8:0x3e0, &pmgr_state);
 pr_info("m3_dcpext_session: CPU PMGR=%#x\n", pmgr_state);
 pr_info("m3_dcpext_session: ASC status=%#x timer_mask_set=%#x timer_mask_clear=%#x\n", readl(session_cpu+0x48), readl(session_cpu+0x1010), readl(session_cpu+0x1018));
 pr_info("m3_dcpext_session: RAM sample=%u host cntpct=%#llx boottime_ms=%llu\n", sample, read_sysreg(cntpct_el0), session_now(NULL));
 for(unsigned int i=0;i<session_nsegments;i++){
  void *map;char name[40];u32 size=session_segments[i].size;
  if(session_segments[i].flags==1 || size>0x1000000)continue;
  map=memremap(session_segments[i].ram.start,size,MEMREMAP_WB);
  if(!map)continue;
  ram_snapshot[sample][i].data=vmalloc(size);ram_snapshot[sample][i].size=size;
  if(ram_snapshot[sample][i].data){
   /* Reserved no-map RAM is written only by DCP. This host alias is
    * read-only: discard clean stale lines before every asynchronous sample.
    * All segment boundaries are page aligned; never discard adjacent data. */
   for (unsigned long at=(unsigned long)map; at<(unsigned long)map+size; at+=64)
    asm volatile("dc ivac, %0" :: "r"(at) : "memory");
   dsb(sy);
   memcpy(ram_snapshot[sample][i].data,map,size);
   snprintf(name,sizeof(name),"firmware-ram-%u%s",i,sample?"":"-early");
   debugfs_create_blob(name,0400,dir,&ram_snapshot[sample][i]);
   pr_info("m3_dcpext_session: snapshot %s flags=%#x pa=%#llx remap=%#llx size=%#x\n",name,session_segments[i].flags,(u64)session_segments[i].ram.start,session_segments[i].remap,size);
  }
  memunmap(map);
 }
}
static ssize_t pool_read(struct file *file,char __user *buf,size_t count,loff_t *offset)
{
 dma_rmb();return simple_read_from_buffer(buf,count,offset,pool,session_used);
}
static const struct file_operations pool_fops={.owner=THIS_MODULE,.read=pool_read,.llseek=default_llseek};
static ssize_t aux_read(struct file *file,char __user *buf,size_t count,loff_t *offset)
{
 return simple_read_from_buffer(buf,count,offset,aux_afk.capture,aux_afk.capture_size);
}
static const struct file_operations aux_fops={.owner=THIS_MODULE,.read=aux_read,.llseek=default_llseek};
static ssize_t afk_read(struct file *file, char __user *buf, size_t count, loff_t *offset)
{
 return simple_read_from_buffer(buf, count, offset, afk.capture, afk.capture_size);
}
static const struct file_operations afk_fops = {
 .owner = THIS_MODULE, .read = afk_read, .llseek = default_llseek,
};
/* Snapshot the retained RTKit log while the desktop remains active. Allocate
 * outside the route lock and copy under it; userspace reads never block RPCs.
 * Each open owns an immutable, bounded snapshot, including during wraparound.
 */
struct syslog_snapshot { size_t size; u8 data[]; };
static int syslog_live_open(struct inode *inode, struct file *file)
{
 struct syslog_snapshot *snapshot;
 snapshot=kvmalloc(struct_size(snapshot,data,sizeof(syslog_capture.capture)),GFP_KERNEL);
 if(!snapshot)return -ENOMEM;
 mutex_lock(&native_route_lock);
 snapshot->size=syslog_capture.used;
 if(snapshot->size>sizeof(syslog_capture.capture)){
  mutex_unlock(&native_route_lock);kvfree(snapshot);return -EIO;
 }
 memcpy(snapshot->data,syslog_capture.capture,snapshot->size);
 mutex_unlock(&native_route_lock);
 file->private_data=snapshot;
 return 0;
}
static ssize_t syslog_live_read(struct file *file,char __user *buf,size_t count,loff_t *offset)
{
 struct syslog_snapshot *snapshot=file->private_data;
 return simple_read_from_buffer(buf,count,offset,snapshot->data,snapshot->size);
}
static int syslog_live_release(struct inode *inode,struct file *file)
{kvfree(file->private_data);return 0;}
static const struct file_operations syslog_live_fops={
 .owner=THIS_MODULE,.open=syslog_live_open,.read=syslog_live_read,
 .release=syslog_live_release,.llseek=default_llseek,
};
static int session_run(void)
{
 static const struct dcpext_session_ops ops = {
  .now_ms = session_now, .send = session_send, .buffer = session_buffer,
 };
 u32 state;
 struct dentry *dir = NULL;
 u64 early_at;
 bool early_saved = false;
 int ret;
 if(native_desktop && !native_kms_trial)return -EINVAL;
 if(native_kms_trial && aux_endpoint!=0x28)return -EINVAL;
 if(native_kms_trial && !native_frame)return -EINVAL;
 if(native_frame && !native_startup)return -EINVAL;
 if(probe_nmi && !iboot_image)return -EINVAL;
 if(native_startup && (!native_query || !hotplug || iboot_modes))return -EINVAL;
 if(iboot_image && !iboot_modes)return -EINVAL;
 if(iboot_modes && (aux_endpoint!=0x23 || !hotplug || !cpu_no_auto))return -EINVAL;
 if(aux_endpoint && ((aux_endpoint!=0x23 && aux_endpoint!=0x24 && aux_endpoint!=0x20 && aux_endpoint!=0x28) || !hotplug))return -EINVAL;
 if (hotplug && !request_display) return -EINVAL;
 if (request_display && !connect_port) return -EINVAL;
 if (connect_port && !validate_connection) return -EINVAL;
 if (validate_connection && afk_endpoint != 0x2a) return -EINVAL;
 if (afk_endpoint && (afk_endpoint < 0x20 || afk_endpoint > 0x2b)) return -EINVAL;
 if(port && !usb_c)return -EINVAL;
 if(usb_c && hdmi_test_two_lane_hbr2)return -EINVAL;
 link_lanes=link_max_lanes();
 if(usb_c){
  const struct m3_usbc_route_ops *(*get_ops)(void)=symbol_get(m3_usbc_get_ops);
  if(!get_ops)return -ENODEV;
  /* Retain the module reference with the session, including on failure. */
  usbc_ops=get_ops();
  ret=usbc_ops->prepare(port);if(ret)return ret;
 }
 if(native_desktop){
  /* Retain the GPIO owner's module reference with this session's resources.
   * Diagnostics without a desktop do not require the bridge module. */
  desktop_read_hpd=usb_c?link_hpd:symbol_get(m3_hdmi_bridge_hpd);
  if(!desktop_read_hpd)return -ENODEV;
  ret=desktop_read_hpd();
  if(ret<0 || (!usb_c && ret!=1))return ret<0?ret:-ENOLINK;
 }
 state = readl(session_cpu + 0x44);
 /* Fresh boot only. CPU is stopped and no protocol session is inherited.
  * Standard Asahi dcp.c start: RUN only, preserve vector/reset state. */
 if (state != 0 || !(readl(session_fifo + 0x114) & BIT(17))) return -EBUSY;
 if (cpu_no_auto) {
  ret=regmap_update_bits(session_pmgr,port?0x3f8:0x3e0,BIT(28),0);
  if(ret)return ret;
  ret=regmap_read_poll_timeout(session_pmgr,port?0x3f8:0x3e0,state,
    !(state&BIT(28)) && (state&0xff)==0xff,1,1000);
  if(ret)return ret;
  pr_info("m3_dcpext_session: diagnostic CPU auto-gating disabled PMGR=%#x\n",state);
 }
 writel(BIT(4), session_cpu + 0x44);
 if (readl(session_cpu + 0x44) != BIT(4)) return -EIO;
 pr_info("m3_dcpext_session: CPU RUN set; firmware vector unchanged\n");
 session.defer_iop_quiesce = quiesce_first;
 if (afk_endpoint) dir = debugfs_create_dir(port ? "m3_dcpext_session1" : "m3_dcpext_session", NULL);
 if(dir){audio_debugfs(dir);
  debugfs_create_file("syslog-live",0400,dir,NULL,&syslog_live_fops);
  if(native_startup)debugfs_create_file("native-capture",0400,dir,NULL,&native_capture_fops);
 }
 early_at = session_now(NULL) + 2000;
 if(iboot_modes || native_startup){session.duration_ms=16000;session.message_limit=512;}
 ret = dcpext_session_begin(&session, &ops, NULL);
 mutex_lock(&native_route_lock);
 while (!ret && session.phase != DCPEXT_QUIESCED) {
  if(native_desktop && native_kms_ready && !native_kms_finished &&
     !READ_ONCE(desktop_stop) && session_now(NULL)>=desktop_hpd_next){
   int level=desktop_read_hpd();
   desktop_hpd_next=session_now(NULL)+200;
   if(level<0){ret=level;break;}
   if(READ_ONCE(desktop_retrain) && native_kms_connected && !desktop_link_stage){
    WRITE_ONCE(desktop_retrain,false);
    desktop_retrain_until=session_now(NULL)+3000;
    pr_info("m3_dcpext_session: diagnostic three-second HPD-low cycle\n");
   }
   if(session_now(NULL)<desktop_retrain_until)level=0;
   /* PS190 pulses HPD during link training: debounce low for two seconds,
    * high for 400 ms. Keep the live session throughout cable absence. */
   if(!level){
    desktop_hpd_high_samples=0;
    if(!desktop_disconnected && ++desktop_hpd_low_samples>=11){
     desktop_disconnected=true;desktop_hpd_dirty=true;
     native_kms_disconnect();
     pr_info("m3_dcpext_session: HDMI disconnected; DRM retained for reconnect\n");
    }
   }else{
    desktop_hpd_low_samples=0;
    if(desktop_disconnected && ++desktop_hpd_high_samples>=3){
     desktop_disconnected=false;desktop_hpd_dirty=true;
     pr_info("m3_dcpext_session: HDMI cable reconnected; retraining\n");
    }
   }
  }
  if(desktop_hpd_rpc && session_now(NULL)>=desktop_hpd_deadline){ret=-ETIMEDOUT;break;}
  if((desktop_hpd_dirty || desktop_link_stage) && !rpc_pending && !av_pending &&
     (!READ_ONCE(desktop_stop) || desktop_link_stage)){
   if(!desktop_link_stage){
    desktop_hpd_dirty=false;desktop_hpd_value=!desktop_disconnected;
    /* Match the native DPTX lifecycle: low/release, then connect/request/high.
     * HPD-high alone reopens AV but leaves the sink inactive after removal. */
    desktop_link_stage=desktop_hpd_value?1:4;
    m3_dcpext_native_invalidate_sink(native_client);
    av_edid_done=false;av_read_ready=false;native_frame_submitted=false;
   }
   /* USB-C discovery needs its AUX route before request/HPD, including
    * reconnect at main-link rate zero (same ordering as M4). */
   if(usb_c && desktop_link_stage==1){
    ret=link_xbar_up();
    if(ret==-EAGAIN){
     /* No crossbar write occurred: mux setup has not caught up with HPD.
      * Return to offline polling, without losing the live firmware session. */
     desktop_disconnected=true;desktop_hpd_dirty=false;desktop_link_stage=0;
     desktop_hpd_high_samples=0;ret=0;
     pr_info("m3_dcpext_session: waiting for Type-C PHY before reconnect\n");
     goto runtime_link_deferred;
    }
    if(ret)break;
   }
   desktop_hpd_rpc=true;desktop_hpd_deadline=session_now(NULL)+5000;
   ret=dptx_rpc_send(desktop_link_stage==1?11:desktop_link_stage==2?6:desktop_link_stage==5?7:8);
   if(ret)break;
  }
runtime_link_deferred:
  if(native_kms_ready && !native_kms_finished && !desktop_link_stage && (READ_ONCE(desktop_stop) || (!native_desktop && session_now(NULL)>=native_kms_until))){
   mutex_unlock(&native_route_lock);native_kms_unplug();mutex_lock(&native_route_lock);
   if(session.runtime){ret=dcpext_session_leave_runtime(&session);received=0;if(ret)break;}
  }
  u64 message, endpoint;
  if (hotplug && !early_saved && session_now(NULL) >= early_at) {
   snapshot_ram(dir, 0);
   early_saved = true;
   if (probe_ping) {
    ping_sent = true;
    ret = session_send(NULL, 0, 3ULL << 52);
    if (ret) break;
   }
  }
  if (probe_nmi && !nmi_sent && ib_pending && ib_operation==6 && session_now(NULL)>=early_at+4000) {
   /* AppleASCWrapV6::_generateNMI legacy fallback, 26A428 +0x1004/1014.
    * Intentionally request a diagnostic FIQ after the mode command stalls.
    * This trial always requires reboot; no firmware code is patched. */
   nmi_sent=true;
   pr_warn("m3_dcpext_session: diagnostic ASC NMI fallback; expect firmware crash/reboot recovery\n");
   writel(0x10,session_cpu+0x1004);writel(1,session_cpu+0x1014);
  }
  ret = dcpext_session_poll(&session);
  if (ret) break;
  if (session.phase == DCPEXT_RUNNING) {
   if(native_query && !native_phase){ret=native_query_start();if(ret)break;}
   if(aux_endpoint && !aux_afk.started){
    ret=afkc_start(&aux_afk,&aux_ops,NULL,aux_endpoint);if(ret)break;
   }
   if (afk_endpoint && !afk.started && (!aux_endpoint || (aux_afk.running && (aux_endpoint!=0x20 || log_replied)))) {
    if (!(session.endpoints[1] & BIT(afk_endpoint - 32))) { ret = -ENODEV; break; }
    ret = afkc_start(&afk, &afk_ops, NULL, afk_endpoint);
    afk_deadline = session_now(NULL) + 1500;
    if (ret) break;
   }
   if (validate_connection && port0_announced && !validation_sent) {
    validation_sent=true;
    ret = dptx_rpc_send(connect_port?11:12);
    if (ret) break;
   }
   ret=av_progress();if(ret)break;
   ret=native_progress();if(ret)break;
   if(native_kms_connected && !native_kms_finished && native_kms_generation!=m3_dcpext_native_generation(native_client)){
    pr_info("m3_dcpext_native: timing publication changed; refreshing HDMI connector\n");
    native_kms_disconnect();
   }
   ret=ib_poll();if(ret)break;
   if (request_display && validation_sent && !rpc_pending && rpc_stage>0 && rpc_stage<(hotplug?5:3) &&
       (!hotplug || rpc_stage!=3 || (session_now(NULL)>=hpd_until && (!iboot_modes || ib_done) && (!native_frame || native_frame_done)))) {
    /* Type-C negotiation can be stable while a waking HDMI adapter pulses
     * HPD low. Prepare AUX first, then announce the actual high level. */
    int level=port && desktop_read_hpd && rpc_stage==2?desktop_read_hpd():1;
    if(level<0){ret=level;break;}
    if(level)ret=dptx_rpc_send(rpc_stage==1?6:hotplug && rpc_stage<4?8:7);
    if(ret)break;
   }
   if (afk_endpoint && afk.running && !afk.stopping && session_now(NULL) >= afk_deadline && (!validate_connection || validation_reply) && (!native_query || native_phase==3)) {
    ret = quiesce_first ? dcpext_session_quiesce(&session) : afkc_stop(&afk);
    if (ret) break;
   }
   if (afk.stopped && aux_endpoint && aux_afk.running && !aux_afk.stopping && (aux_endpoint!=0x28 || !av_announced || av_closed)) {
    ret=afkc_stop(&aux_afk);if(ret)break;
   }
   if ((!afk_endpoint || afk.stopped) && (!aux_endpoint || aux_afk.stopped)) {
    pr_info("m3_dcpext_session: application endpoints stopped; requesting AP/IOP quiescence\n");
    ret = dcpext_session_quiesce(&session);
    if (ret) break;
   }
  }
  if (quiesce_first && session.phase == DCPEXT_AP_QUIESCE && session.ap_quiesced) {
   if (!afk.stopping) ret = afkc_stop(&afk);
   else if (afk.stopped) ret = dcpext_session_finish_quiesce(&session);
   if (ret) break;
  }
  if (readl(session_fifo + 0x114) & BIT(17)) {
   mutex_unlock(&native_route_lock);
   if(session.runtime)usleep_range(1000,1500);else usleep_range(100,200);
   mutex_lock(&native_route_lock);
   continue;
  }
  message = readq(session_fifo + 0x830);
  endpoint = readq(session_fifo + 0x838);
  dma_rmb();
  ret=session_dispatch(endpoint,message);
  mutex_unlock(&native_route_lock);cond_resched();mutex_lock(&native_route_lock);
 }
 mutex_unlock(&native_route_lock);
 if(native_kms_ready && !native_kms_finished)native_kms_unplug();
 if (afk_endpoint) {
  syslog_blob.data=syslog_capture.capture;syslog_blob.size=syslog_capture.used;
  debugfs_create_blob("syslog-capture",0400,dir,&syslog_blob);
  if(aux_endpoint)debugfs_create_file("aux-capture",0400,dir,NULL,&aux_fops);
  debugfs_create_file("session-pool",0400,dir,NULL,&pool_fops);
  if(hotplug)snapshot_ram(dir, 1);
  debugfs_create_file("afk-capture", 0400, dir, NULL, &afk_fops);
  pr_info("m3_dcpext_session: AFK running=%u stopped=%u error=%d captured=%u\n",
          afk.running, afk.stopped, afk.error, afk.capture_size);
 }
 pr_info("m3_dcpext_session: result=%d phase=%u messages=%u; retaining all resources until reboot\n",
         ret, session.phase, session.received);
 return ret;
}

static int session_thread(void *unused)
{
 int ret=session_run();
 desktop_notify(ret);WRITE_ONCE(desktop_done,true);complete_all(&desktop_finished);
 return ret;
}
static int desktop_reboot(struct notifier_block *nb,unsigned long event,void *data)
{
 WRITE_ONCE(desktop_stop,true);
 if(!wait_for_completion_timeout(&desktop_finished,msecs_to_jiffies(20000)))
  pr_err("m3_dcpext: shutdown timed out; all DMA retained\n");
 return NOTIFY_DONE;
}
static struct notifier_block desktop_reboot_nb={.notifier_call=desktop_reboot};
static int session_start(void)
{
 struct task_struct *task;int ret;
 if(!native_desktop)return session_run();
 ret=register_reboot_notifier(&desktop_reboot_nb);if(ret)return ret;
 task=kthread_run(session_thread,NULL,"m3-hdmi");
 if(IS_ERR(task)){unregister_reboot_notifier(&desktop_reboot_nb);return PTR_ERR(task);}
 if(!wait_for_completion_timeout(&desktop_started,msecs_to_jiffies(20000))){
  WRITE_ONCE(desktop_stop,true);return -ETIMEDOUT;
 }
 return READ_ONCE(desktop_result);
}
