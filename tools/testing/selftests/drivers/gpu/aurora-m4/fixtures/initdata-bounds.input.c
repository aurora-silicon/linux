// SPDX-License-Identifier: GPL-2.0-only OR MIT
static void result(int ret) { putchar(ret ? 1 : 0); }
static void root_bounds(void) {
    u64 va[GPU_M4_OBJECT_COUNT]; u8 root[0xc8];
    for (unsigned id=0;id<GPU_M4_OBJECT_COUNT;id++) for(unsigned v=0;v<6;v++) {
        for(unsigned i=0;i<GPU_M4_OBJECT_COUNT;i++) va[i]=GPU_M4_HOST_START+0x100000+i*0x100000ULL;
        switch(v) {
        case 0: va[id]=GPU_M4_HOST_START-8;break;
        case 1: va[id]=GPU_M4_HOST_END;break;
        case 2: va[id]++;break;
        case 3: va[id]=va[(id+1)%GPU_M4_OBJECT_COUNT];break;
        case 4: va[id]=GPU_M4_HOST_END-8;break;
        case 5: va[id]=GPU_M4_HOST_END-gpu_m4_object_size[id];break;
        }
        result(gpu_m4_encode_root(root,va));
    }
}
static void channels(void) {
    const u64 s=GPU_M4_HOST_START+0x100000,r=GPU_M4_HOST_START+0x200000;
    const u64 states[]={s,s+1,r,r+0x1000,GPU_M4_HOST_START-8,GPU_M4_HOST_END,s,s,s};
    const u64 rings[]={r,r,r,r,r,r,GPU_M4_HOST_START-8,GPU_M4_HOST_END,r+1};
    for(unsigned c=0;c<2;c++)for(unsigned v=0;v<sizeof(states)/sizeof(states[0]);v++) {
        u8 record[0x20]={0};int ret=gpu_m4_encode_channel(record,states[v],rings[v],c);
        result(ret);if(ret)memset(record,0,sizeof(record));fwrite(record,1,sizeof(record),stdout);
    }
}
static void loaders(void) {
    for(unsigned slot=0;slot<GPU_M4_LOADER_COUNT+1;slot++)for(unsigned bank=0;bank<3;bank++) {
        u64 pa=0;u32 size=0,off=0;int ret=gpu_m4_loader_span(slot,bank,&pa,&size,&off);
        result(ret);u8 rec[16]={0};if(!ret){put_unaligned_le64(pa,rec);put_unaligned_le32(size,rec+8);put_unaligned_le32(off,rec+12);}fwrite(rec,1,sizeof(rec),stdout);
    }
    for(unsigned slot=0;slot<GPU_M4_LOADER_COUNT+1;slot++)for(unsigned v=0;v<5;v++) {
        u64 va=0;u8 rec[0x28]={0};
        if(slot<GPU_M4_LOADER_COUNT&&gpu_m4_loader[slot].length)va=GPU_M4_HOST_START+0x1000000+slot*0x100000ULL+(gpu_m4_loader[slot].pa&0x3fff);
        switch(v){case 1:va++;break;case 2:va=GPU_M4_HOST_START-1;break;case 3:va=GPU_M4_HOST_END;break;case 4:va=0;break;}
        int ret=gpu_m4_loader_encode(slot,va,rec);result(ret);if(ret)memset(rec,0,sizeof(rec));fwrite(rec,1,sizeof(rec),stdout);
    }
}
static void init_bounds(void) {
    struct gpu_m4_initdata_input in={0};
    for(unsigned i=0;i<GPU_M4_OBJECT_COUNT;i++){in.object[i]=calloc(1,gpu_m4_object_size[i]);assert(in.object[i]);in.va[i]=GPU_M4_HOST_START+0x100000+i*0x100000ULL;}
    for(unsigned i=0;i<12;i++){in.channel_state[i]=GPU_M4_HOST_START+0x4000000+i*0x10000ULL;in.channel_ring[i]=in.channel_state[i]+0x4000;}
    for(unsigned i=0;i<GPU_M4_LOADER_COUNT;i++)in.loader_va[i]=gpu_m4_loader[i].length?GPU_M4_HOST_START+0x5000000+i*0x100000ULL+(gpu_m4_loader[i].pa&0x3fff):0;
    in.scratch=GPU_M4_HOST_START+0xa000000;in.lower_uma=GPU_M4_VA_BOOTSTRAP_START+0x200000;in.lower_parameter=GPU_M4_VA_BOOTSTRAP_START+0x240000;in.progress_recovery=1;
    for(unsigned ceiling=0;ceiling<11;ceiling++)result(gpu_m4_encode_initdata(&in,ceiling));
    for(unsigned id=0;id<GPU_M4_OBJECT_COUNT;id++){void *p=in.object[id];in.object[id]=NULL;result(gpu_m4_encode_initdata(&in,9));in.object[id]=p;}
    for(unsigned v=0;v<8;v++) {
        struct gpu_m4_initdata_input bad=in;
        switch(v){case 0:bad.scratch=GPU_M4_HOST_END;break;case 1:bad.scratch++;break;case 2:bad.lower_uma=GPU_M4_VA_BOOTSTRAP_START-0x4000;break;case 3:bad.lower_parameter++;break;case 4:bad.lower_parameter=bad.lower_uma;break;case 5:bad.lower_parameter=bad.lower_uma+0x4000;break;case 6:bad.channel_state[11]=bad.channel_ring[11];break;case 7:bad.loader_va[41]++;break;}
        result(gpu_m4_encode_initdata(&bad,9));
    }
    for(unsigned i=0;i<GPU_M4_OBJECT_COUNT;i++)free(in.object[i]);
}
int main(void){root_bounds();channels();loaders();init_bounds();assert(!ferror(stdout));return 0;}
