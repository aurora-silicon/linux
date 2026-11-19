// SPDX-License-Identifier: GPL-2.0-only OR MIT
int main(int argc,char **argv) {
    assert(argc==2);FILE *out=fopen(argv[1],"wb");assert(out);
    for(unsigned pipe=0;pipe<12;pipe++)for(unsigned h=0;h<3;h++)for(unsigned e=0;e<5;e++)for(unsigned n=0;n<2;n++)for(unsigned t=0;t<3;t++) {
        struct gpu_m4_exec_pipe_record r={.timestamp=(u64[]){0,0x12345678abcdef00ULL,~0ULL}[t],.queue_info=GPU_M4_EXEC_FW_START+0x100000+pipe*0x4000,.queue_producer=(u16[]){0,1,255}[h],.pipe=pipe,.scheduler_slot=(u8[]){0,15,112,113,127}[e],.new_queue=n};
        u8 bytes[24];u64 kick;
        assert(!gpu_m4_exec_pipe_record(bytes,&r));assert(!gpu_m4_exec_pipe_kick(pipe,&kick));
        assert(fwrite(bytes,1,sizeof(bytes),out)==sizeof(bytes));
        u8 k[8];put_unaligned_le64(kick,k);assert(fwrite(k,1,8,out)==8);
    }
    for(unsigned v=0;v<4;v++) {
        u64 f=GPU_M4_EXEC_FW_START+(u64)v*0x100000000ULL;
        u8 state[GPU_M4_EXEC_RING_STATE_SIZE],info[GPU_M4_EXEC_QUEUE_SIZE],context[GPU_M4_EXEC_CONTEXT_SIZE],notifiers[GPU_M4_EXEC_NOTIFY_SIZE];
        assert(!gpu_m4_exec_queue(state,info,f+0x100000,f+0x200000,f+0x300000,f+0x400000,f+0x500000));
        gpu_m4_exec_context(context);assert(!gpu_m4_exec_notifiers(notifiers,f+0x600000,1));
        assert(fwrite(state,1,sizeof(state),out)==sizeof(state));assert(fwrite(info,1,sizeof(info),out)==sizeof(info));
        assert(fwrite(context,1,sizeof(context),out)==sizeof(context));assert(fwrite(notifiers,1,sizeof(notifiers),out)==sizeof(notifiers));
    }
    assert(!fclose(out));return 0;
}
