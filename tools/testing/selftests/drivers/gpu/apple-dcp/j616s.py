# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Panel identity with absent ADT panel nodes and notch-hidden timings."""
import re
from common import block
import desktop


def identity(root, output):
    base = root / 'drivers/gpu/drm/apple'
    unit = desktop.panel_identity(root).split('int main(int argc, char **argv)')[0]
    unit = unit.replace('int fw_compat;bool has_panel;',
                        'int fw_compat;unsigned notch_height;bool has_panel;')
    unit += r'''
typedef int32_t s32;
#define ALIGN(x,n) (((x)+(n)-1)&~((n)-1))
#define SZ_16M (16U<<20)
static u32 get_unaligned_le32(const void *p) {u32 n;memcpy(&n,p,4);return n;}
'''
    unit += block((base / 'dcp-panel-adt.h').read_text(), 'struct dcp_panel_adt {') + ';\n'
    unit += '\n'.join(line for line in (base / 'dcp-panel-adt.c').read_text().splitlines()
                      if not line.startswith('#include'))
    unit += (base / 'dcp-panel-test.h').read_text()
    source = (base / 'dcp.c').read_text()
    # Also exercise the pre-fix property read, so the regression fails on r5g.
    if 'void dcp_panel_read_dimensions(' in source:
        reader = block(source, 'void dcp_panel_read_dimensions(')
    else:
        start = source.index('\t\tof_property_read_u32(panel_np, "width-mm"')
        end = source.index('\n\n', start)
        reader = ('void dcp_panel_read_dimensions(struct apple_dcp *dcp, '
                  'struct device_node *panel_np) {\n'
                  'const char height_prop[2][16] = {"adj-height-mm", "height-mm"};\n'
                  + source[start:end] + '\n}')
    dts = (root / 'arch/arm64/boot/dts/apple/t6040-j616s.dts').read_text()
    values = []
    for name in ('width-mm', 'height-mm', 'adj-height-mm'):
        match = re.search(r'^\s*' + name + r'\s*=\s*<(\d+)>;', dts, re.M)
        values.append(int(match[1]) if match else 0)
    unit += r'''
struct device_node { u32 width, height, adjusted; };
static int of_property_read_u32(struct device_node *np, const char *name, u32 *out)
{
    u32 value = !strcmp(name,"width-mm") ? np->width :
        !strcmp(name,"height-mm") ? np->height : np->adjusted;
    if (!value) return -EINVAL;
    *out=value;
    return 0;
}
''' + reader + '\n' + r'''
int main(int argc, char **argv)
{
    assert(argc==3);
    struct apple_dcp d={.fw_compat=5,.has_panel=true};
    __typeof__(*d.panel_identity) state={0};d.panel_identity=&state;
    struct device_node np={DTS_VALUES};
    d.notch_height=74;
    if (!strcmp(argv[1],"dimensions")) {
        dcp_panel_read_dimensions(&d,&np);
        assert(d.panel.width_mm==345 && d.panel.height_mm==216);
        /* A missing optional adjusted height must retain the full height. */
        np.adjusted=0;d.panel.height_mm=0;
        dcp_panel_read_dimensions(&d,&np);
        assert(d.panel.width_mm==345 && d.panel.height_mm==223);
        d.notch_height=0;np.adjusted=216;
        dcp_panel_read_dimensions(&d,&np);
        assert(d.panel.height_mm==223);
        return 0;
    }
    struct dcp_panel_adt panel={
        .identity={.manufacturer="APP",.product=0x616,.name="Apple Panel"},
        .width_mm=345,.height_mm=216,
    }, before=panel;
    assert(!dcp_panel_adt_parse(panel_test_adt,sizeof(panel_test_adt),&panel));
    assert(!panel.found && !memcmp(&panel,&before,sizeof(panel)));
    for (unsigned n=0;n<sizeof(panel_test_adt);n++) {
        assert(dcp_panel_adt_parse(panel_test_adt,n,&panel)<0);
        assert(!memcmp(&panel,&before,sizeof(panel)));
    }
    d.panel.width_mm=panel.width_mm;d.panel.height_mm=panel.height_mm;
    state.adt=panel.identity;
    struct dcp_display_mode mode=panel_test_mode, saved=mode;
    u64 generation=99;
    const struct drm_edid *edid=dcp_panel_edid_snapshot(&d,&mode,1,&generation);
    assert(!IS_ERR(edid) && drm_edid_valid(edid));
    assert(!memcmp(&mode,&saved,sizeof(mode)) && generation==0);
    const u8 *raw=(const u8 *)&edid->raw;
    assert(!memcmp(raw+77,"Apple Panel",11));
    assert(!memcmp(raw+95,"unspecified",11));
    assert((raw[66] | (raw[68]>>4)<<8)==345);
    assert((raw[67] | (raw[68]&15)<<8)==216);
    FILE *file=fopen(argv[2],"wb");assert(file);
    assert(fwrite(raw,128,1,file)==1);fclose(file);
    drm_edid_free(edid);
    /* Invalid geometry must still be refused rather than repaired. */
    mode.mode.vsync_end=mode.mode.vtotal+1;
    assert(IS_ERR(dcp_panel_edid_snapshot(&d,&mode,1,&generation)));
    return 0;
}
'''.replace('DTS_VALUES', ','.join(map(str, values)))
    return unit, (("dimensions", str(output / 'j616s-panel.edid')),
                  ("identity", str(output / 'j616s-panel.edid')))
