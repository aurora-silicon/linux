// SPDX-License-Identifier: GPL-2.0-only OR MIT
static int mapped(void *cookie,u64 va,u64 size,bool write) {
    (void)cookie;(void)va;(void)size;(void)write;return 0;
}

static void emit(const char *dir,unsigned variant) {
    u64 f=GPU_M4_EXEC_FW_START+(u64)variant*0x100000000ULL;
    u64 g=0x1000000000ULL+(u64)variant*0x100000000ULL;
    struct gpu_m4_exec_vm vm={.start=GPU_M4_EXEC_VM_START,.end=GPU_M4_EXEC_VM_END,.check=mapped};
    unsigned counts[]={0,1,16,2};
    struct gpu_m4_exec_attachment attachments[16];
    for(unsigned i=0;i<16;i++)
        attachments[i]=(struct gpu_m4_exec_attachment){g+0x50000000+i*0x10000ULL+variant*13,(i+1)*137};
    struct gpu_m4_exec_compute_input in={
        .vm=&vm,.registers={.preemption=g+0x200000,.preemption_size=0x4000,
            .cdm_base=g+0x300000+variant*4,.cdm_end=g+0x300200+variant*8,
            .usc_base=(variant&1)?(1ULL<<40):0},
        .work_fw=f+0x100000,.work_gpu=g+0x100000,.queue_fw=f+0x200000,.stats_fw=f+0x300000,
        .auxiliary_fw=f+0x400000,.auxiliary_gpu=g+0x400000,.context_store_gpu=g+0x500000,
        .counter_fw=f+0x500000,.stamps_fw=(variant&2)?f+0x500040:0,
        .notifier_fw=f+0x600400,.record_fw=(variant&1)?f+0x700000:0,
        .firmware_stamp_fw=f+0x600640,.user_stamp_fw=f+0x600648,
        .pool_state_gpu=g+0x600000,.page_list_gpu=g+0x700000,
        .event_sequence=0,.binding_generation=1,.attachments=attachments,.attachment_count=counts[variant],
    };
    u8 work[GPU_M4_EXEC_COMPUTE_SIZE];
    memset(work,0xa5,sizeof(work));
    assert(!gpu_m4_exec_first_compute(work,&in));
    char path[4096];
    assert(snprintf(path,sizeof(path),"%s/compute-%u.bin",dir,variant)>0);
    FILE *out=fopen(path,"wb");assert(out);
    assert(fwrite(work,1,sizeof(work),out)==sizeof(work));assert(!fclose(out));
    u8 state[GPU_M4_EXEC_POOL_STATE_SIZE],list[GPU_M4_EXEC_POOL_PAGE_LIST_SIZE];
    memset(state,0xa5,sizeof(state));memset(list,0xa5,sizeof(list));
    assert(!gpu_m4_exec_uma_pool(state,list,&vm,g+0x2000000));
    assert(snprintf(path,sizeof(path),"%s/compute-pool-%u.bin",dir,variant)>0);
    out=fopen(path,"wb");assert(out);
    assert(fwrite(state,1,sizeof(state),out)==sizeof(state));
    assert(fwrite(list,1,sizeof(list),out)==sizeof(list));assert(!fclose(out));
}

int main(int argc,char **argv) {
    assert(argc==2);
    for(unsigned variant=0;variant<4;variant++)emit(argv[1],variant);
    return 0;
}
