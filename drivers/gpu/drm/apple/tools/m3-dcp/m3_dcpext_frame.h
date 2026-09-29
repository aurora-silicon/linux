/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Paged static image adapted from M4 dcpext_frame_map.h. M3 SID5/FE0,
 * qualified reservations and nonposted accesses. All allocations are retained
 * until reboot, including on partial publication: the caller is already pinned.
 */
#include <linux/vmalloc.h>
#define FRAME_DVA 0x30000000ULL
#define FRAME_SLOT (FRAME_DVA >> 25)
#define FRAME_BYTES ALIGN(1920 * 1080 * 4 + 24 * LEASE_PAGE, LEASE_PAGE)
static bool iboot_image;
module_param(iboot_image,bool,0400);
static void *frame_pixels,*frame_ram;
static u64 *frame_leaf,*frame_root;
static void __iomem *frame_regs;
static dma_addr_t frame_dma[2*FRAME_BYTES/LEASE_PAGE];
static u64 encode_address(phys_addr_t address);
static int flush_sid(void);
static int frame_prepare_publish(struct device_node *dcp,int count)
{
 struct device_node *ram,*disp,*dart;
 struct resource reservation,res;
 u32 params[2],ttbr,tcr,protect,status;
 phys_addr_t pa;int ret=-EINVAL;
 if(!iboot_image && !native_startup)return 0;
 if(!pinned)return -EINVAL;
 ram=of_parse_phandle(dcp,"memory-region",count-2);
 if(!ram)return -EINVAL;
 if(!of_device_is_compatible(ram,"apple,dart-mem") || !of_property_read_bool(ram,"no-map") ||
    of_address_to_resource(ram,0,&reservation))goto ram_out;
 if(resource_size(&reservation)>0x100000 || resource_size(&reservation)<LEASE_PAGE ||
    (reservation.start|resource_size(&reservation))&(LEASE_PAGE-1))goto ram_out;
 disp=of_find_node_by_path(port?"dispext1":"dispext0");
 if(!disp)goto ram_out;
 if(of_device_is_available(disp) || of_property_read_u32_array(disp,"iommus",params,2) || params[1]){
  of_node_put(disp);goto ram_out;
 }
 of_node_put(disp);dart=of_find_node_by_phandle(params[0]);
 if(!dart)goto ram_out;
 if(of_device_is_available(dart) || !of_device_is_compatible(dart,"apple,t8110-dart") ||
    of_address_to_resource(dart,0,&res) || res.start!=(port?0x2d5304000ULL:0x2d1304000ULL) ||
    resource_size(&res)!=0x4000 || !(res.flags&IORESOURCE_MEM_NONPOSTED)){
  of_node_put(dart);goto ram_out;
 }
 of_node_put(dart);
 if(!request_mem_region(res.start,resource_size(&res),"m3-dcpext-frame")){ret=-EBUSY;goto ram_out;}
 frame_regs=ioremap_np(res.start,resource_size(&res));
 ret=-ENOMEM;if(!frame_regs)goto ram_out;
 ttbr=readl(frame_regs+0x1400);tcr=readl(frame_regs+0x1000);protect=readl(frame_regs+0x200);
 pa=(u64)((ttbr&GENMASK(29,2))>>2)<<14;
 ret=-EINVAL;
 if((tcr & ~GENMASK(11,8))!=5 || !(ttbr&1) || !(protect&1) || pa<reservation.start || pa>reservation.end ||
    reservation.end-pa+1<LEASE_PAGE || (readl(frame_regs+0x100)&BIT(31)))goto ram_out;
 frame_ram=memremap(reservation.start,resource_size(&reservation),MEMREMAP_WB);
 ret=-ENOMEM;if(!frame_ram)goto ram_out;
 frame_root=frame_ram+pa-reservation.start;
 /* Boot audit established empty FE0. Refuse inherited mappings rather than
  * borrowing a live scanout aperture. SID4 PIODMA is not modified. */
 ret=-EBUSY;
 for(unsigned int i=0;i<LEASE_PAGE/8;i++)if(READ_ONCE(frame_root[i]))goto ram_out;
 if(READ_ONCE(root[FRAME_SLOT]))goto ram_out;
 frame_leaf=(void *)get_zeroed_page(GFP_KERNEL);frame_pixels=vzalloc(2*FRAME_BYTES);
 ret=-ENOMEM;if(!frame_leaf || !frame_pixels)goto ram_out;
 if(virt_to_phys(frame_leaf)>=BIT_ULL(42)){ret=-ERANGE;goto ram_out;}
 for(unsigned int y=0;y<1080;y++)for(unsigned int x=0;x<1920;x++){
  static const u32 bars[]={0xffff0000,0xff00ff00,0xff0000ff,0xffffff00,0xffff00ff,0xff00ffff,0xffffffff,0xff202020};
  ((u32 *)frame_pixels)[y*1920+x]=x<8 || y<8 || x>=1912 || y>=1072?0xffffffff:bars[x*8/1920];
 }
 for(unsigned int i=0;i<ARRAY_SIZE(frame_dma);i++){
  frame_dma[i]=dma_map_page(dma_dev,vmalloc_to_page(frame_pixels+i*LEASE_PAGE),0,LEASE_PAGE,DMA_TO_DEVICE);
  if(dma_mapping_error(dma_dev,frame_dma[i])){ret=-ENOMEM;goto ram_out;}
  if(frame_dma[i]>=BIT_ULL(42) || frame_dma[i]&(LEASE_PAGE-1) || dma_to_phys(dma_dev,frame_dma[i])!=frame_dma[i]){
   ret=-ERANGE;goto ram_out;
  }
  frame_leaf[i]=encode_address(frame_dma[i])|PTE_RW;
 }
 ret=-EAGAIN;
 if(readl(frame_regs+0x1400)!=ttbr || readl(frame_regs+0x1000)!=tcr || readl(frame_regs+0x200)!=protect)goto ram_out;
 dma_wmb();
 if(cmpxchg64_relaxed(&root[FRAME_SLOT],0,encode_address(virt_to_phys(frame_leaf))|1) ||
    cmpxchg64_relaxed(&frame_root[FRAME_SLOT],0,encode_address(virt_to_phys(frame_leaf))|1))goto ram_out;
 ret=flush_sid();if(ret)goto ram_out;
 dma_wmb();writel(0x100,frame_regs+0x80);
 ret=readl_poll_timeout(frame_regs+0x80,status,!(status&BIT(31)),1,10000);
 if(!ret && (readl(frame_regs+0x100)&BIT(31)))ret=-EIO;
 pr_info("m3_dcpext_frame: SID5/FE0 DVA=%#llx bytes=%#lx mapping result=%d; retained until reboot\n",FRAME_DVA,(unsigned long)FRAME_BYTES,ret);
ram_out:
 of_node_put(ram);return ret;
}

/* Native D451/D201 allocations share the retained host arena with PIODMA.
 * SID4 root has its own reservation; never alias or replace inherited leaves. */
static bool pio_published;
static int pio_prepare_publish(struct device_node *dcp,int count)
{
 struct device_node *ram;
 struct resource res;
 u32 ttbr,tcr,status;
 phys_addr_t pa;
 u64 *pio_root;
 void *mapping;
 int ret=-EINVAL;
 if(!native_startup)return 0;
 if(!frame_regs || !pinned)return -EINVAL;
 ram=of_parse_phandle(dcp,"memory-region",count-1);
 if(!ram)return -EINVAL;
 if(!of_device_is_compatible(ram,"apple,dart-mem") || !of_property_read_bool(ram,"no-map") ||
    of_address_to_resource(ram,0,&res))goto out;
 ttbr=readl(frame_regs+0x1410);tcr=readl(frame_regs+0x1010);
 pa=(u64)((ttbr&GENMASK(29,2))>>2)<<14;
 pr_info("m3_dcpext_native: PIODMA TCR=%#x TTBR=%#x root=%pap reservation=%pR\n",tcr,ttbr,&pa,&res);
 /* T8110 TCR bits11:8 are the remap selector; bit7 enables remapping.
  * Boot values a05/b05/e05 all have remapping disabled. Qualify every active
  * control bit, preserve the ignored selector, and recheck the exact register
  * before publication. Definitions: drivers/iommu/apple-dart.c. */
 if((tcr!=0xa05 && (tcr & ~GENMASK(11,8))!=5) || !(ttbr&1) || pa<res.start || pa>res.end || res.end-pa+1<LEASE_PAGE ||
    resource_size(&res)>0x100000 || (res.start|resource_size(&res))&(LEASE_PAGE-1))goto out;
 mapping=memremap(res.start,resource_size(&res),MEMREMAP_WB);
 if(!mapping){ret=-ENOMEM;goto out;}
 pio_root=mapping+pa-res.start;
 ret=-EBUSY;
 if(READ_ONCE(pio_root[LEASE_SLOT]) || readl(frame_regs+0x1410)!=ttbr ||
    readl(frame_regs+0x1010)!=tcr)goto out;
 dma_wmb();
 if(cmpxchg64_relaxed(&pio_root[LEASE_SLOT],0,encode_address(virt_to_phys(leaf))|1))goto out;
 dma_wmb();writel(0x104,frame_regs+0x80);
 ret=readl_poll_timeout(frame_regs+0x80,status,!(status&BIT(31)),1,10000);
 if(!ret && (readl(frame_regs+0x100)&BIT(31)))ret=-EIO;
 if(!ret)pio_published=true;
 pr_info("m3_dcpext_native: PIODMA SID4 arena mapped ret=%d; retained until reboot\n",ret);
out:
 of_node_put(ram);return ret;
}

static void frame_sync(unsigned int slot,bool for_cpu)
{
 unsigned int first=slot*(FRAME_BYTES/LEASE_PAGE);
 for(unsigned int i=first;i<first+FRAME_BYTES/LEASE_PAGE;i++) {
  if(for_cpu)dma_sync_single_for_cpu(dma_dev,frame_dma[i],LEASE_PAGE,DMA_TO_DEVICE);
  else dma_sync_single_for_device(dma_dev,frame_dma[i],LEASE_PAGE,DMA_TO_DEVICE);
 }
}
