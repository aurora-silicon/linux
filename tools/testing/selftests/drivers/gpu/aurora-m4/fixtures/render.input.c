// SPDX-License-Identifier: GPL-2.0-only OR MIT
static int mapped(void *cookie,u64 va,u64 size,bool write) {
    (void)cookie;(void)va;(void)size;(void)write;return 0;
}

static void emit(const char *directory,unsigned v) {
    const u32 dimensions[4][7]={{64,64,1,1,32,32,4},{1919,1079,3,2,16,32,4},{2560,1664,1,4,16,16,8},{16384,1,2,1,32,16,16}};
    const unsigned counts[4][2]={{0,1},{1,2},{16,16},{2,0}};
    u64 f=GPU_M4_EXEC_FW_START+(u64)v*0x100000000ULL;
    u64 g=GPU_M4_RENDER_RANGE1_START+(u64)v*0x100000000ULL;
    struct gpu_m4_exec_vm vm={.start=GPU_M4_EXEC_VM_START,.end=GPU_M4_EXEC_VM_END,.check=mapped};
    struct gpu_m4_render_encoded render={0};
    struct gpu_m4_render_span fw[GPU_M4_RENDER_FIRST_FW_COUNT],gpu[GPU_M4_RENDER_FIRST_GPU_COUNT];
    for(unsigned i=0;i<GPU_M4_RENDER_FIRST_FW_COUNT;i++)fw[i]=(struct gpu_m4_render_span){f+(i+1)*0x100000ULL,0x4000};
    for(unsigned i=0;i<GPU_M4_RENDER_FIRST_GPU_COUNT;i++)gpu[i]=(struct gpu_m4_render_span){g+0x2000000+i*0x1000000ULL,0x4000};
    render.owner=(struct gpu_m4_render_owner){.context=(u32[]){1,2,63,7}[v],.asid=(u32[]){1,2,63,7}[v],.parameter_id=(u32[]){0,3,31,15}[v],.generation=(u8[]){1,2,254,127}[v],.parameter_manager_fw=f+0x3000000,.parameter_descriptor_fw=f+0x3100000,.queue_usc_base=1ULL<<40};
    memcpy(&render.geometry,dimensions[v],sizeof(render.geometry));
    for(unsigned i=0;i<GPU_M4_RENDER_ENCODED_RESOURCES;i++)render.resource[i]=(struct gpu_m4_render_span){g+(i+1)*0x100000ULL,0x100000};
    render.resource[GPU_M4_RENDER_LAYER_METADATA].va=render.resource[GPU_M4_RENDER_SCENE_METADATA].va+(render.geometry.layers>1?0x200:0);
    render.scissor=(struct gpu_m4_render_span){g+0xb00000+v*4,0x100};
    render.depth_bias=(struct gpu_m4_render_span){g+0xc00000+v*4,0x100};
    render.query=(struct gpu_m4_render_span){g+0xd00000+v*16,0x100};
    render.background_resources=0x1234000010ULL+v;render.end_of_tile_resources=0x5678000020ULL+v;
    render.background_program=(1ULL<<40)+0x10000;render.end_of_tile_program=(1ULL<<40)+0x20000;
    render.usc_base=1ULL<<40;render.multisample_control=0x8000000000000000ULL+v;
    render.isp_mode=0x280|(v&1?1ULL<<16:0);
    render.merge_limit[0]=0x3f800000+v;render.merge_limit[1]=0x40000000+v;
    render.depth_clear=0x3e800000+v;render.background_value=0x100+v;
    render.ppp_control=0x201;render.uuid=0x123400+v;
    render.depth_bias_is_integer=v&2;
    render.zls_control=v?0x1234567800000000ULL+v:0;
    render.depth_stride=0x123400+v;render.stencil_stride=0x567800+v;render.zls_pixels=0x1000+v;
    render.depth=(struct gpu_m4_render_span){g+0xe00000,0x10000};
    render.stencil=(struct gpu_m4_render_span){g+0xf00000,0x10000};
    struct gpu_m4_render_packet_chronology chronology={.target_stamp=0x100+v*0x100,.parameter_id=render.owner.parameter_id,.uma_submitted=2+v*2};
    for(unsigned engine=0;engine<2;engine++) {
        chronology.stage_fw[engine]=f+0x4000000+engine*0x4000;
        chronology.engine[engine].counter=2+v*2-engine;
        render.attachment_count[engine]=counts[v][engine];
    }
    struct gpu_m4_render_packet_scratch scratch={0};
    struct gpu_m4_exec_geometry geometry;
    assert(!gpu_m4_exec_geometry(&geometry,render.geometry.width,render.geometry.height,render.geometry.layers,render.geometry.samples,render.geometry.utile_width,render.geometry.utile_height,render.geometry.sample_size));
    for(unsigned engine=0;engine<2;engine++) {
        struct gpu_m4_render_register_output *encoded=&scratch.registers[engine];
        encoded->geometry=geometry;
        unsigned count;
        if(!engine)gpu_m4_render_tiler_registers(encoded->registers,&render,&geometry,&count);
        else gpu_m4_render_fragment_registers(encoded->registers,&render,&geometry,render.background_program,render.end_of_tile_program,&count);
        encoded->count=count;encoded->bytes=count*12;encoded->zero_suffix_records=engine?2:3;
        struct gpu_m4_exec_attachment attachments[16];
        for(unsigned i=0;i<16;i++)attachments[i]=(struct gpu_m4_exec_attachment){g+0x50000000+i*0x10000ULL+v*13,(i+1)*137};
        assert(!gpu_m4_exec_attachments(encoded->attachments,&vm,attachments,counts[v][engine]));
        unsigned work_gpu=engine?GPU_M4_RENDER_FIRST_FRAGMENT_WORK_GPU:GPU_M4_RENDER_FIRST_TILER_WORK_GPU;
        unsigned work_fw=engine?GPU_M4_RENDER_FIRST_FRAGMENT_WORK_FW:GPU_M4_RENDER_FIRST_TILER_WORK_FW;
        unsigned queue_fw=engine?GPU_M4_RENDER_FIRST_FRAGMENT_QUEUE_FW:GPU_M4_RENDER_FIRST_TILER_QUEUE_FW;
        assert(!gpu_m4_render_register_metadata(scratch.register_metadata[engine],&vm,gpu[work_gpu].va+(engine?0x80:0x40),encoded,engine));
        assert(!gpu_m4_render_timestamps(&scratch.timestamps[engine],fw[work_fw].va,fw[queue_fw].va,&fw[GPU_M4_RENDER_FIRST_SCENE_FW],render.uuid,engine));
    }
    scratch.partial_background=(1ULL<<40)+0x30000;scratch.partial_end_of_tile=(1ULL<<40)+0x40000;
    struct gpu_m4_render_packet_encoded packet={.render=&render,.firmware=fw,.gpu=gpu,.tpc_firmware={f+0x3200000,0x100000},.chronology=&chronology,.partial_background_resources=0x9876000030ULL+v,.partial_end_of_tile_resources=0x5432000040ULL+v};
    struct gpu_m4_render_packet_output out={0};
    gpu_m4_render_packet_tiler(&out,&packet,&scratch);gpu_m4_render_packet_fragment(&out,&packet,&scratch);
    char path[4096];assert(snprintf(path,sizeof(path),"%s/render-%u.bin",directory,v)>0);
    FILE *file=fopen(path,"wb");assert(file);
    assert(fwrite(out.tiler,1,sizeof(out.tiler),file)==sizeof(out.tiler));
    assert(fwrite(out.fragment,1,sizeof(out.fragment),file)==sizeof(out.fragment));
    assert(fwrite(out.tiler_sequence,1,sizeof(out.tiler_sequence),file)==sizeof(out.tiler_sequence));
    assert(fwrite(out.fragment_sequence,1,GPU_M4_RENDER_FRAGMENT_SEQUENCE_SIZE,file)==GPU_M4_RENDER_FRAGMENT_SEQUENCE_SIZE);
    assert(!fclose(file));

    struct gpu_m4_render_native_owner owner={0};
    for(unsigned i=0;i<GPU_M4_RENDER_FIRST_FW_COUNT;i++)owner.firmware[i].span=fw[i];
    struct gpu_m4_render_native_input native={.owner=&owner};
    struct gpu_m4_render_native_state state={.encoded=render};
    for(unsigned i=0;i<4;i++)
        if(v==2 || (v==1&&(i==0||i==3)))state.timestamp_va[i]=0xfffffc2ffc000000ULL+0x100+v*0x100+i*8;
    for(unsigned i=0;i<8;i++)
        if(v==3 || (v==2&&(i==0||i==3||i==7)) || (v==1&&i==0)) {
            state.timestamp_va[4+i]=0xfffffc2ffc000000ULL+0x1000+v*0x100+i*8;
            state.end_copy_count++;
        }
    if(v&1)gpu_m4_render_init_buffer_emit(out.init_buffer,&render.owner,chronology.target_stamp);
    gpu_m4_render_dependency_emit(out.fragment_dependency,chronology.stage_fw[0],chronology.target_stamp,0x70,chronology.target_stamp,false);
    put_unaligned_le64(chronology.uma_submitted,out.uma_submitted_counter);
    gpu_m4_render_native_scene_timestamps(&out,&native,&state);
    assert(snprintf(path,sizeof(path),"%s/render-timestamps-%u.bin",directory,v)>0);
    file=fopen(path,"wb");assert(file);
    assert(fwrite(out.tiler,1,sizeof(out.tiler),file)==sizeof(out.tiler));
    assert(fwrite(out.fragment,1,sizeof(out.fragment),file)==sizeof(out.fragment));
    assert(fwrite(out.tiler_sequence,1,sizeof(out.tiler_sequence),file)==sizeof(out.tiler_sequence));
    unsigned sequence_size=GPU_M4_RENDER_FRAGMENT_SEQUENCE_SIZE+state.end_copy_count*0x4c;
    assert(fwrite(out.fragment_sequence,1,sequence_size,file)==sequence_size);
    assert(fwrite(out.scene,1,sizeof(out.scene),file)==sizeof(out.scene));
    assert(!fclose(file));
}

int main(int argc,char **argv) {
    assert(argc==2);for(unsigned variant=0;variant<4;variant++)emit(argv[1],variant);return 0;
}
