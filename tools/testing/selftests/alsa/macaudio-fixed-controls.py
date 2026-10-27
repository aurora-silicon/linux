#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""No-device regression for fixed macaudio controls and direct ALSA writes.

Run: python3 tools/testing/selftests/alsa/macaudio-fixed-controls.py
Requires a C compiler. Extracts the actual setup, pattern matcher, enum setter,
enum put callback, activation and write functions from the source under test.
Shims model serialized controls and in-memory registers, not devices or ioctls.
Input-validation info/range checking is shimmed for valid enum values. Both
CONFIG_SND_CTL_INPUT_VALIDATION settings exercise the real permission check.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def function(source, name):
    match = re.search(r"^(?:static (?:inline )?)?(?:int|bool) " + name +
                      r"\(.*?^\}", source, re.M | re.S)
    if not match:
        raise ValueError("missing source function: " + name)
    return match.group() + "\n"


PREFIX = r'''
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define SNDRV_CTL_ELEM_ACCESS_WRITE 2
#define SNDRV_CTL_ELEM_ACCESS_INACTIVE 256
#define SNDRV_CTL_EVENT_MASK_INFO 2
#define SNDRV_CTL_EVENT_MASK_VALUE 1
#define SNDRV_CTL_ELEM_TYPE_ENUMERATED 3
#define IS_ENABLED(x) VALIDATE
#define unlikely(x) (x)
#define dev_err(...) ((void)0)
#define dev_dbg(...) ((void)0)
struct snd_ctl_file { int unused; };
struct snd_ctl_elem_id { char name[64]; unsigned int index; };
struct snd_ctl_elem_value {
 struct snd_ctl_elem_id id;
 union { struct { unsigned int item[2]; } enumerated; } value;
};
struct snd_ctl_elem_info {
 struct snd_ctl_elem_id id; unsigned int type, count;
 union { struct { unsigned int items, item; char name[64]; } enumerated; } value;
};
struct snd_kcontrol_volatile { unsigned int access; struct snd_ctl_file *owner; };
struct snd_soc_component { unsigned int reg, writes; };
struct soc_enum { unsigned int items, shift_l, shift_r, mask, reg; };
struct snd_kcontrol {
 struct snd_ctl_elem_id id; struct snd_kcontrol_volatile vd[2]; unsigned int count;
 int (*put)(struct snd_kcontrol *, struct snd_ctl_elem_value *);
 int (*info)(struct snd_kcontrol *, struct snd_ctl_elem_info *);
 unsigned long private_value; struct snd_soc_component *component;
 struct snd_kcontrol *next; const char * const *texts;
};
struct snd_card { int controls_rwsem; struct snd_kcontrol *controls; };
enum { AMP_NONE, AMP_TAS5770, AMP_SN012776, AMP_SSM3515, AMP_MAX98360A };
struct macaudio_platform_cfg { int amp, amp_gain; bool stereo; };
struct macaudio_snd_data { bool has_speakers; struct macaudio_platform_cfg *cfg; };
struct snd_soc_card { struct snd_card *snd_card; struct macaudio_snd_data *data; };
static int please_blow_up_my_speakers;
#define snd_soc_card_get_drvdata(card) ((card)->data)
#define down_write(x) ((void)0)
#define up_write(x) ((void)0)
#define downgrade_write(x) ((void)0)
#define up_read(x) ((void)0)
#define list_for_each_entry(pos, head, member) \
 for (pos = *(head); pos; pos = pos->next)
static struct snd_kcontrol *snd_ctl_find_id(struct snd_card *c, struct snd_ctl_elem_id *id)
{
 struct snd_kcontrol *k;
 for (k = c->controls; k; k = k->next)
  if (!strcmp(k->id.name, id->name) && id->index < k->count) return k;
 return NULL;
}
static unsigned int snd_ctl_get_ioff(struct snd_kcontrol *k, struct snd_ctl_elem_id *id) { return id->index; }
static void snd_ctl_build_ioff(struct snd_ctl_elem_id *id, struct snd_kcontrol *k, unsigned int i) {}
static void snd_ctl_notify_one(struct snd_card *c, int e, struct snd_kcontrol *k, unsigned int i) {}
static int __snd_ctl_elem_info(struct snd_card *c, struct snd_kcontrol *k, struct snd_ctl_elem_info *i, void *p) { return 0; }
static int sanity_check_input_values(struct snd_card *c, struct snd_ctl_elem_value *v, struct snd_ctl_elem_info *i, bool b) { return v->value.enumerated.item[0] < 7 ? 0 : -EINVAL; }
static struct snd_soc_component *snd_kcontrol_chip(struct snd_kcontrol *k) { return k->component; }
static unsigned int snd_soc_enum_item_to_val(struct soc_enum *e, unsigned int item) { return item; }
static int snd_soc_component_update_bits(struct snd_soc_component *c, unsigned int r, unsigned int m, unsigned int v)
{ unsigned int old = c->reg; c->writes++; c->reg = (old & ~m) | (v & m); return old != c->reg; }
/* Amp gain is volume-limited separately, not a fixed enum. */
static int snd_soc_limit_volume(struct snd_soc_card *c, const char *n, int v) { return 1; }
static int enum_info(struct snd_kcontrol *k, struct snd_ctl_elem_info *i)
{
 struct soc_enum *e = (void *)k->private_value;
 i->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED; i->count = 1;
 i->value.enumerated.items = e->items;
 if (i->value.enumerated.item >= e->items) i->value.enumerated.item = e->items - 1;
 snprintf(i->value.enumerated.name, sizeof(i->value.enumerated.name), "%s", k->texts[i->value.enumerated.item]);
 return 0;
}
'''

MAIN = r'''
static int failures, attempts;
static void check(bool ok, const char *what, const char *name)
{
 if (!ok) { failures++; fprintf(stderr, "FAIL validation=%d param=%d %s: %s\n", VALIDATE, please_blow_up_my_speakers, name, what); }
}
static void scenario(int amp, const char *prefix, bool tweeter, bool stereo)
{
 const char * const texts[] = { "Disabled", "Left", "2 Hz", "800 Hz", "Retry", "8.4 V Span", "Other" };
 const char * const suffix[] = { "ASI1 Sel", "HPF Corner Frequency", "OCE Handling", "DAC Analog Gain Select" };
 struct soc_enum e = { .items = 7, .mask = 7 };
 struct snd_kcontrol controls[9] = {0};
 struct snd_soc_component regs[9] = {0};
 struct macaudio_platform_cfg cfg = { .amp = amp, .amp_gain = 10, .stereo = stereo };
 struct macaudio_snd_data ma = { .has_speakers = true, .cfg = &cfg };
 struct snd_card card = { .controls = controls };
 struct snd_soc_card soc = { .snd_card = &card, .data = &ma };
 struct snd_ctl_file ordinary = {0};
 unsigned int i, element;
 for (i = 0; i < 9; i++) {
  const char *side = i < 4 ? "Left" : "Right";
  const char *kind = tweeter ? "Tweeter" : "Woofer";
  /* Broad 1W prefixes are used with CS42L83, which has no jack HPF. */
  if (i == 8) snprintf(controls[i].id.name, sizeof(controls[i].id.name), "%s",
                       !strcmp(prefix, "* ") ? "Jack Playback Volume" : "Jack HPF Corner Frequency");
  else if (!strcmp(prefix, "")) snprintf(controls[i].id.name, sizeof(controls[i].id.name), "%s", suffix[i % 4]);
  else if (!strcmp(prefix, "* ")) snprintf(controls[i].id.name, sizeof(controls[i].id.name), "%s %s", side, suffix[i % 4]);
  else snprintf(controls[i].id.name, sizeof(controls[i].id.name), "%s %s %s", side, kind, suffix[i % 4]);
  controls[i].count = 2; controls[i].vd[0].access = controls[i].vd[1].access = 3;
  controls[i].put = snd_soc_put_enum_double; controls[i].info = enum_info;
  controls[i].private_value = (unsigned long)&e; controls[i].component = &regs[i];
  controls[i].texts = texts; controls[i].next = i < 8 ? &controls[i + 1] : NULL;
  regs[i].reg = 6;
 }
 /* Unprefixed mono controls must have distinct names. */
 if (!strcmp(prefix, "")) { controls[3].next = &controls[8]; }
 check(macaudio_set_speaker(&soc, prefix, tweeter) == 0, "setup", prefix);
 for (i = 0; i < 9; i++) {
  if (!strcmp(prefix, "") && i >= 4 && i < 8) continue;
  unsigned int which = i % 4, expected = 6;
  bool fixed = false;
  if (i != 8) {
   if ((amp == AMP_TAS5770 || amp == AMP_SN012776) && stereo && which == 0) { fixed = true; expected = 1; }
   if (amp == AMP_SN012776 && which == 1) { fixed = true; expected = tweeter ? 3 : 2; }
   if (amp == AMP_SN012776 && which == 2) { fixed = true; expected = 4; }
   if (amp == AMP_SSM3515 && which == 3) { fixed = true; expected = 5; }
  }
  check(regs[i].reg == expected, "configured enum", controls[i].id.name);
  for (element = 0; element < controls[i].count; element++) {
   struct snd_ctl_elem_value v = { .id = controls[i].id };
   v.id.index = element;
   unsigned int before = regs[i].writes;
   int rc = snd_ctl_elem_write(&card, &ordinary, &v);
   attempts++;
   check(fixed ? rc == -EPERM : rc == 0, fixed ? "direct write must return -EPERM" : "unrelated control remains writable", controls[i].id.name);
   if (fixed) check(regs[i].writes == before && regs[i].reg == expected, "blocked callback preserves transform", controls[i].id.name);
  }
 }
}
int main(void)
{
 int values[] = {0, 1, 2, 3, -1}; unsigned int p;
 for (p = 0; p < sizeof(values) / sizeof(values[0]); p++) {
  please_blow_up_my_speakers = values[p];
  scenario(AMP_SN012776, "* Tweeter ", true, true);
  scenario(AMP_SN012776, "* Woofer ", false, true);
  scenario(AMP_SN012776, "* ", false, true);
  scenario(AMP_SN012776, "", false, false);
  scenario(AMP_TAS5770, "* ", false, true);
  /* SSM3515 is refused at admission; keep its dormant fixed setup locked too. */
  scenario(AMP_SSM3515, "* ", false, true);
  scenario(AMP_MAX98360A, "* ", false, true);
 }
 printf("validation=%d attempts=%d failures=%d\n", VALIDATE, attempts, failures);
 return failures ? 1 : 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[4])
    args = parser.parse_args()
    core = (args.source / "sound/core/control.c").read_text()
    ops = (args.source / "sound/soc/soc-ops.c").read_text()
    ma = (args.source / "sound/soc/apple/macaudio.c").read_text()
    parts = [PREFIX, function(ops, "snd_soc_control_matches"),
             function(core, "snd_ctl_activate_id"),
             function(ops, "snd_soc_put_enum_double"),
             core[core.index("static inline int snd_ctl_put("):core.index("\n#endif", core.index("static inline int snd_ctl_put("))],
             function(core, "snd_ctl_elem_write"),
             function(ops, "snd_soc_deactivate_kctl"),
             function(ops, "soc_set_enum_kctl"), function(ops, "snd_soc_set_enum_kctl")]
    if "static int macaudio_set_fixed_enum(" in ma:
        parts.append(function(ma, "macaudio_set_fixed_enum"))
    parts += [ma[ma.index("#define CHECK(call,"):ma.index("static int macaudio_set_speaker(")],
              function(ma, "macaudio_set_speaker"), MAIN]
    with tempfile.TemporaryDirectory(prefix="macaudio-fixed-") as temp:
        source, binary = Path(temp) / "test.c", Path(temp) / "test"
        source.write_text("\n".join(parts))
        failures = 0
        for validation in (0, 1):
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2",
                            "-DVALIDATE=" + str(validation), str(source),
                            "-o", str(binary)], check=True)
            failures += subprocess.run([str(binary)]).returncode != 0
        if failures:
            raise SystemExit(1)


if __name__ == "__main__":
    main()
