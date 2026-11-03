# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Synthetic firmware colour data through the production parser and EDID builder."""
import copy
import struct
import desktop
import rate


def panel(root, output):
    fixture = rate.parser_fixture(root)
    unit = desktop.panel_identity(root).split('int main(int argc, char **argv)')[0]
    # Standard colour-space coordinates, never a captured panel calibration.
    p3 = {'Chromaticity': {'Red': {'X': 44564, 'Y': 20972},
                          'Green': {'X': 17367, 'Y': 45220},
                          'Blue': {'X': 9830, 'Y': 3932}},
          'DefaultWhitePoint': {'X': 20493, 'Y': 21561, 'Gamma': 144179},
          'ProductAttributes': {'ManufacturerID': 'TST', 'ProductID': 42,
                                'ProductName': 'Synthetic', 'SerialNumber': 123}}
    srgb = copy.deepcopy(p3)
    srgb['Chromaticity']['Red'] = {'X': 41943, 'Y': 21627}
    srgb['Chromaticity']['Green'] = {'X': 19661, 'Y': 39322}
    absent = {'ProductAttributes': p3['ProductAttributes']}
    white_only = copy.deepcopy(p3); del white_only['Chromaticity']
    chroma_only = copy.deepcopy(p3); del chroma_only['DefaultWhitePoint']
    zero = copy.deepcopy(p3); zero['Chromaticity']['Red']['X'] = 0
    gamma = copy.deepcopy(p3); del gamma['DefaultWhitePoint']['Gamma']
    bad_gamma = copy.deepcopy(p3); bad_gamma['DefaultWhitePoint']['Gamma'] = 65535
    wrong_type = copy.deepcopy(p3); wrong_type['Chromaticity'] = 'P3'
    # The wire encoder supports an ordered multimap to exercise duplicate keys.
    pairs = fixture['ObjectPairs']
    duplicate = pairs(list(p3.items()) + [('Chromaticity', p3['Chromaticity'])])
    duplicate_white = pairs(list(p3.items()) + [('DefaultWhitePoint', p3['DefaultWhitePoint'])])
    duplicate_bad = pairs(list(zero.items()) + [('Chromaticity', p3['Chromaticity'])])
    inputs = [p3, srgb, absent, white_only, chroma_only, zero, gamma, bad_gamma,
              wrong_type, duplicate, duplicate_white, duplicate_bad]
    for i, value in enumerate(inputs):
        raw = bytearray(struct.pack('<I', 0xd3)); fixture['wire'](value, raw)
        unit += 'static const u8 color_%u[]={%s};\n' % (i, ','.join(map(str, raw)))
    unit += r'''
static struct dcp_display_metadata decode(const u8 *raw, size_t size)
{
    struct dcp_parse_ctx ctx;
    struct dcp_display_metadata metadata;
    assert(!parse(raw,size,&ctx) && !parse_panel_metadata(&ctx,&metadata));
    assert(!strcmp(metadata.manufacturer,"TST") && metadata.product==42);
    return metadata;
}
static const struct dcp_display_mode timing={.mode={
    .clock=148500,.hdisplay=1920,.hsync_start=2008,.hsync_end=2052,.htotal=2200,
    .vdisplay=1080,.vsync_start=1084,.vsync_end=1089,.vtotal=1125,
    .flags=DRM_MODE_FLAG_PHSYNC|DRM_MODE_FLAG_PVSYNC,.type=DRM_MODE_TYPE_PREFERRED}};
static void check(const struct dcp_display_metadata *metadata, bool color, const char *path)
{
    const struct drm_edid *edid=dcp_panel_edid(metadata,&timing,1,300,170);
    assert(!IS_ERR(edid) && drm_edid_valid(edid));
    const u8 *raw=(const void *)&edid->raw;
    if(color) {
        u32 expected[8]={metadata->chromaticity[0][0],metadata->chromaticity[0][1],
            metadata->chromaticity[1][0],metadata->chromaticity[1][1],
            metadata->chromaticity[2][0],metadata->chromaticity[2][1],
            metadata->white[0],metadata->white[1]};
        for(unsigned i=0;i<8;i++) {
            u8 low=raw[i<4?25:26];
            u32 coordinate=(u32)raw[27+i]<<2 | ((low>>(6-2*(i%4)))&3);
            assert(coordinate==(expected[i]+32)/64);
        }
        assert(raw[23]==(metadata->gamma?120:255));
    } else {
        assert(raw[23]==255);
        for(unsigned i=27;i<33;i++)assert(!raw[i]);
        assert(!raw[25] && !(raw[26]&0xf0));
    }
    if(path) {FILE *file=fopen(path,"wb");assert(file);assert(fwrite(raw,128,1,file)==1);fclose(file);}
    drm_edid_free(edid);
}
int main(int argc, char **argv)
{
    assert(argc==3);
    struct dcp_display_metadata p3=decode(color_0,sizeof(color_0));
    struct dcp_display_metadata srgb=decode(color_1,sizeof(color_1));
    assert(p3.gamma==144179 && p3.chromaticity[0][0]==44564 && p3.white[0]==20493);
    assert(srgb.chromaticity[0][0]==41943);
    check(&p3,true,argv[1]);check(&srgb,true,argv[2]);
'''
    for i in range(2, len(inputs)):
        if i == 6:
            unit += f'{{struct dcp_display_metadata m=decode(color_{i},sizeof(color_{i}));assert(!m.gamma && m.white[0] && m.chromaticity[0][0]);check(&m,true,NULL);}}\n'
            continue
        unit += f'{{struct dcp_display_metadata m=decode(color_{i},sizeof(color_{i}));assert(!m.gamma && !m.white[0] && !m.chromaticity[0][0]);check(&m,false,NULL);}}\n'
    unit += r'''
    struct dcp_display_metadata invalid=p3;
    memcpy(invalid.chromaticity[1],invalid.chromaticity[0],sizeof(invalid.chromaticity[0]));
    check(&invalid,false,NULL);
    invalid=p3;invalid.white[0]=1;invalid.white[1]=1;check(&invalid,false,NULL);
    invalid=p3;invalid.chromaticity[0][0]=65535;check(&invalid,false,NULL);
    /* A ProductAttributes-only setter must preserve the established colour. */
    struct apple_dcp d={.fw_compat=5,.has_panel=true,.connector=&d};
    __typeof__(*d.desktop) state={0};d.desktop=&state;notifications=0;
    dcp_panel_metadata_replace(&d,&p3);assert(notifications==1);
    struct dcp_display_metadata partial=decode(color_2,sizeof(color_2));
    dcp_panel_metadata_replace(&d,&partial);
    assert(notifications==1 && !memcmp(&state.panel_metadata,&p3,sizeof(p3)));
    /* Newly reported primaries with unknown gamma must not inherit an old gamma. */
    struct dcp_display_metadata unknown_gamma=decode(color_6,sizeof(color_6));
    dcp_panel_metadata_replace(&d,&unknown_gamma);
    assert(notifications==2 && !state.panel_metadata.gamma && state.panel_metadata.white[0]);
    dcp_panel_metadata_replace(&d,&partial);
    assert(notifications==2 && !state.panel_metadata.gamma && state.panel_metadata.white[0]);
    /* A replacement panel must not inherit another serial's colour data. */
    partial.serial++;
    dcp_panel_metadata_replace(&d,&partial);
    assert(notifications==3 && !state.panel_metadata.gamma);
    return 0;
}
'''
    return unit, ((str(output/'panel-display-p3.edid'), str(output/'panel-srgb.edid')),)
