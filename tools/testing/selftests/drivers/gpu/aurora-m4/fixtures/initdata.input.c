// SPDX-License-Identifier: GPL-2.0-only OR MIT
static void emit(const char *dir, unsigned variant) {
    struct gpu_m4_initdata_input in={0};
    const u32 ceiling[]={1,9,4};
    const u32 masks[]={0,0x12345678,0xfedcba98};
    u64 shift=(u64)variant*0x100000000ULL;
    for(unsigned i=0;i<GPU_M4_OBJECT_COUNT;i++) {
        in.object[i]=malloc(gpu_m4_object_size[i]);
        assert(in.object[i]);
        memset(in.object[i],0xa5,gpu_m4_object_size[i]);
        in.va[i]=GPU_M4_HOST_START+shift+0x100000+i*0x100000ULL;
    }
    for(unsigned i=0;i<12;i++) {
        in.channel_state[i]=GPU_M4_HOST_START+shift+0x4000000+i*0x10000ULL;
        in.channel_ring[i]=in.channel_state[i]+0x4000;
    }
    for(unsigned i=0;i<GPU_M4_LOADER_COUNT;i++)
        in.loader_va[i]=gpu_m4_loader[i].length?GPU_M4_HOST_START+shift+0x5000000+i*0x100000ULL+(gpu_m4_loader[i].pa&0x3fff):0;
    in.scratch=GPU_M4_HOST_START+shift+0xa000000;
    in.lower_uma=GPU_M4_VA_BOOTSTRAP_START+0x200000;
    in.lower_parameter=GPU_M4_VA_BOOTSTRAP_START+0x240000;
    in.ktrace_mask=masks[variant];
    in.progress_recovery=variant!=1;
    assert(!gpu_m4_encode_initdata(&in,ceiling[variant]));
    char path[4096];
    assert(snprintf(path,sizeof(path),"%s/initdata-%u.bin",dir,variant)>0);
    FILE *f=fopen(path,"wb");assert(f);
    for(unsigned i=0;i<GPU_M4_OBJECT_COUNT;i++) {
        assert(fwrite(in.object[i],1,gpu_m4_object_size[i],f)==gpu_m4_object_size[i]);
        free(in.object[i]);
    }
    assert(!fclose(f));
}

int main(int argc,char **argv) {
    assert(argc==2);
    for(unsigned variant=0;variant<3;variant++)emit(argv[1],variant);
    return 0;
}
