#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the real dcp_pcm_open() over plugged, unplugged and unreadable sinks.

PipeWire and PulseAudio probe a PCM once when its card appears and drop the
output for good if the open fails. dcp_pcm_open() opens an unplugged sink
with a fixed hardware description for that reason. A sink whose DCP audio
service is between a stop and its next start (the firmware announces it again
while the display link is set up again) cannot have its elements read; the
open then takes the same unplugged path instead of failing.

The fixture compiles dcp_pcm_open() from drivers/gpu/drm/apple/audio.c against
mocks of the ALSA and DMA calls. --mutation names a change to the production
function that the matrix must kill.
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
AUDIO = 'drivers/gpu/drm/apple/audio.c'

MUTATIONS = {
    # Fail the open when the sink info cannot be read (the old behaviour).
    'fail-open': ('\tif (ret < 0) {\n\t\t/*\n\t\t * The service is between',
                  '\tif (ret < 0) {\n\t\treturn ret;\n\t\t/*\n\t\t * The service is between'),
    # Open with the unplugged description but treat the stream as plugged.
    'no-flag': ('\t\tdcpaud->open_unplugged = true;\n', ''),
}

def block(text, start):
    opening = text.index('{', start)
    depth = 0
    for end in range(opening, len(text)):
        depth += text[end] == '{'
        depth -= text[end] == '}'
        if not depth:
            return text[start:end + 1]
    raise AssertionError('unbalanced production block')

def function(text, name):
    match = re.search(r'\n(static [^\n;]*\b' + name + r'\([^;{}]*\)\n)\{', text)
    assert match, name
    return block(text, match.start(1))

HARNESS = r'''
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#define dev_dbg(...) ((void)0)
#define SNDRV_PCM_INFO_MMAP 1u
#define SNDRV_PCM_INFO_MMAP_VALID 2u
#define SNDRV_PCM_INFO_INTERLEAVED 4u
#define SNDRV_PCM_INFO_RESUME 8u
#define SNDRV_PCM_HW_PARAM_FORMAT 0
#define SNDRV_PCM_HW_PARAM_CHANNELS 1
#define SNDRV_PCM_HW_PARAM_RATE 2
#define SND_DMAENGINE_PCM_DAI_FLAG_PACK 1
#define DCPAUD_PERIODS_MAX 64
#define UNPLUGGED_FORMATS 0x700u
#define PLUGGED_FORMATS 0x7u
struct device { int unused; };
struct mutex { int held; };
static void mutex_lock(struct mutex *m) { m->held++; }
static void mutex_unlock(struct mutex *m) { m->held--; }
struct snd_pcm_hardware { unsigned int info; unsigned long long formats; unsigned int channels_min, channels_max;
	unsigned int periods_min, periods_max; size_t period_bytes_min, period_bytes_max, buffer_bytes_max, fifo_size; };
struct snd_pcm_runtime { struct snd_pcm_hardware hw; };
struct snd_pcm { void *private_data; };
struct snd_pcm_substream { struct snd_pcm *pcm; struct snd_pcm_runtime *runtime; };
struct snd_dmaengine_dai_dma_data { unsigned int flags; };
struct dma_chan { int unused; };
struct dcp_audio { struct device *dev; struct mutex data_lock; bool connected, open_unplugged;
	unsigned long open_cookie, connection_cookie; struct dma_chan *chan; };
static const struct snd_pcm_hardware dcp_pcm_hw = { .formats = PLUGGED_FORMATS };
static const struct snd_pcm_hardware dcp_pcm_hw_unplugged = { .formats = UNPLUGGED_FORMATS };
static int init_error, info_error, rules, dma_opens;
static int dcpaud_init_dma(struct dcp_audio *d) { return init_error; }
static int dcpaud_read_remote_info(struct dcp_audio *d) { return info_error; }
static int dcpaud_rule_format(void *p, void *r) { return 0; }
static int dcpaud_rule_channels(void *p, void *r) { return 0; }
static int dcpaud_rule_rate(void *p, void *r) { return 0; }
static int snd_pcm_hw_rule_add(struct snd_pcm_runtime *r, unsigned int c, int v, void *f, void *p, ...)
{ rules++; return 0; }
static int snd_dmaengine_pcm_refine_runtime_hwparams(struct snd_pcm_substream *s,
	struct snd_dmaengine_dai_dma_data *d, struct snd_pcm_hardware *h, struct dma_chan *c) { return 0; }
static int snd_dmaengine_pcm_open(struct snd_pcm_substream *s, struct dma_chan *c) { dma_opens++; return 0; }
'''

CHECKS = r'''
static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d (%s: %s)\n", __LINE__, name, #c); failures++; } } while (0)

static int run(const char *name, bool connected, int ierr, int info, struct dcp_audio *d,
	       struct snd_pcm_runtime *rt)
{
	static struct snd_pcm pcm;
	struct snd_pcm_substream s = { .pcm = &pcm, .runtime = rt };

	(void)name;
	*d = (struct dcp_audio){ .connected = connected, .connection_cookie = 7 };
	*rt = (struct snd_pcm_runtime){ 0 };
	pcm.private_data = d;
	init_error = ierr; info_error = info; rules = dma_opens = 0;
	return dcp_pcm_open(&s);
}

int main(void)
{
	struct dcp_audio d;
	struct snd_pcm_runtime rt;
	const char *name;

	name = "plugged";
	CHECK(run(name, true, 0, 0, &d, &rt) == 0);
	CHECK(!d.open_unplugged && rules == 3 && dma_opens == 1);
	CHECK(rt.hw.formats == PLUGGED_FORMATS && d.open_cookie == 7 && !d.data_lock.held);

	name = "unplugged";
	CHECK(run(name, false, 0, 0, &d, &rt) == 0);
	CHECK(d.open_unplugged && !rules && dma_opens == 1 && rt.hw.formats == UNPLUGGED_FORMATS);

	name = "service-restarting";
	CHECK(run(name, true, 0, -ENODEV, &d, &rt) == 0);
	CHECK(d.open_unplugged && !rules && dma_opens == 1);
	CHECK(rt.hw.formats == UNPLUGGED_FORMATS && !d.data_lock.held);

	name = "firmware-refused";
	CHECK(run(name, true, 0, -EIO, &d, &rt) == 0);
	CHECK(d.open_unplugged && dma_opens == 1);

	name = "no-dma";
	CHECK(run(name, true, -EPROBE_DEFER_, 0, &d, &rt) == -EPROBE_DEFER_);
	CHECK(!dma_opens && !d.data_lock.held);

	if (failures)
		return 1;
	puts("PASS dcp_pcm_open: plugged, unplugged, service restarting and refused element read "
	     "open (the last two as unplugged), DMA failure still fails");
	return 0;
}
'''

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--mutation', choices=sorted(MUTATIONS))
    args = parser.parse_args()

    source = (ROOT / AUDIO).read_text()
    if args.mutation:
        old, new = MUTATIONS[args.mutation]
        assert source.count(old) == 1, args.mutation
        source = source.replace(old, new)

    code = HARNESS + '#define EPROBE_DEFER_ 517\n' + function(source, 'dcp_pcm_open') + CHECKS
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / 'check.c'
        path.write_text(code)
        subprocess.run(['clang', '-std=gnu11', '-Wall', '-Werror', '-Wno-unused-function',
                        '-Wno-unreachable-code', '-fsanitize=address,undefined', str(path),
                        '-o', str(path.with_suffix(''))], check=True)
        sys.exit(subprocess.run([str(path.with_suffix(''))]).returncode)

if __name__ == '__main__':
    main()
