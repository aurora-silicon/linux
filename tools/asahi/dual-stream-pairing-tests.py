#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Replay both streams of a dual-stream Thunderbolt dock through the pairing pass.

On an ordered dual-stream machine each Type-C port has a primary and a
secondary connector; the secondary, a dock's second stream, is fixed to one
Type-C-only pipeline (dcp_fabric_port_crtc_mask()). The pairing pass
(dcp_fabric_plan()) and the tunnel candidate (dcp_fabric_tunnel_candidate())
only hand a stream a pipeline its connector is offered.

The production bodies from dcp-fabric-core.c are compiled against the core
header and replayed on three layouts:

- every candidate Type-C-only, as the two dcpexts of the M4 MacBook Air and
  14-inch MacBook Pro on firmware 27.2, where they are pipelines of the DRM device:
  dpin0 and dpin1 of a dock on either port, in either order, before and
  after a compositor owns the display, each get a pipeline of their own;
- a hybrid in front of one Type-C-only pipeline (M2 Pro/Max laptops);
- a hybrid in front of two Type-C-only pipelines.

Each mutation must fail it; 'lowest-typec-only' is the rule before the fix,
which gave the secondary connector the primary's pipeline whenever no hybrid
came first.
"""
import argparse
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CORE = (ROOT / 'drivers/gpu/drm/apple/dcp-fabric-core.c').read_text()
HEADER = (ROOT / 'drivers/gpu/drm/apple/dcp-fabric-core.h').read_text()
NAMES = ['dcp_fabric_available', 'dcp_fabric_score', 'dcp_fabric_fits',
         'dcp_fabric_port_route', 'dcp_fabric_port_tunneled', 'dcp_fabric_plan_pipeline',
         'dcp_fabric_plan', 'dcp_fabric_tunnel_keeps_direct', 'dcp_fabric_tunnel_candidate',
         'dcp_fabric_port_crtc_mask']
MUTATIONS = {
    # The rule before the fix: the lowest Type-C-only pipeline, whatever comes first.
    'lowest-typec-only': ('spare = typec_only & ~(candidates & -candidates);', 'spare = 0;'),
    'secondary-offered-all': ('if (!secondary || !ordered_dual)\n\t\treturn candidates;',
                              'return candidates;'),
    'secondary-highest': ('return spare & -spare;', 'return spare ? 1U << (31 - __builtin_clz(spare)) : 0;'),
    'lone-dropped': ('if (!spare)\n\t\tspare = typec_only;', 'if (!spare)\n\t\treturn 0;'),
    'candidate-ignores-mask': ('port->candidate_crtcs[dcp_fabric_dpin_slot(dpin)],',
                               '~0U,'),
}


def function(text, name):
    match = re.search(r'^(?:\w[^\n;{}]*?\b)?' + re.escape(name) + r'\([^;{]*?\)\n\{', text, re.M | re.S)
    assert match, name
    start = match.start()
    # A return type on a line of its own ("struct x *\nname(").
    if not re.match(r'(?:static\s+)?(?:bool|void|u32|u8|int|unsigned int|enum \w+|struct \w+ \*)\s+\w',
                    text[start:]):
        prev = text.rindex('\n', 0, start - 1) + 1
        start = prev
    pos = text.index('{', match.end() - 1) + 1
    depth = 1
    while depth:
        depth += (text[pos] == '{') - (text[pos] == '}')
        pos += 1
    return text[start:pos] + '\n'


PRE = r'''
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(n) (1U << (n))
'''

CASES = r'''
#define NPIPE 4
#define NPORT 2

struct layout {
	const char *name;
	unsigned int pipes;		/* external pipelines, CRTC 1.. */
	bool hybrid[NPIPE];		/* has a fixed (HDMI) output: not Type-C only */
};

struct fabric {
	struct dcp_fabric_pipeline pipe[NPIPE];
	struct dcp_fabric_route route[NPORT][NPIPE];
	struct dcp_fabric_port port[NPORT];
	struct dcp_fabric_plan plan[NPORT];
	struct dcp_fabric_policy policy;
	unsigned int pipes;
};

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); \
	assert(c); } } while (0)

/* apple_probe_typec_ports(): every candidate pipeline, and the Type-C-only ones */
static void setup(struct fabric *f, const struct layout *l)
{
	u32 candidates = 0, typec_only = 0;
	unsigned int i, p;

	memset(f, 0, sizeof(*f));
	f->pipes = l->pipes;
	f->policy.dual_stream = true;
	for (i = 0; i < l->pipes; i++) {
		struct dcp_fabric_pipeline *pipe = &f->pipe[i];

		/* CRTC 0 is the internal panel's, with no Type-C route. */
		pipe->crtc_index = i + 1;
		pipe->bound = true;
		pipe->has_fixed = l->hybrid[i];
		pipe->services_ready = true;
		pipe->presence = DCP_FABRIC_ABSENT;
		pipe->next = i + 1 < l->pipes ? &f->pipe[i + 1] : NULL;
		candidates |= BIT(pipe->crtc_index);
		if (!l->hybrid[i])
			typec_only |= BIT(pipe->crtc_index);
	}
	for (p = 0; p < NPORT; p++) {
		struct dcp_fabric_port *port = &f->port[p];

		port->key = p + 1;
		port->next = p + 1 < NPORT ? &f->port[p + 1] : NULL;
		port->plan = &f->plan[p];
		port->routes = &f->route[p][0];
		for (i = 0; i < l->pipes; i++) {
			f->route[p][i].pipeline = &f->pipe[i];
			f->route[p][i].next = i + 1 < l->pipes ? &f->route[p][i + 1] : NULL;
		}
		port->candidate_crtcs[0] = dcp_fabric_port_crtc_mask(false, true, candidates,
								     typec_only);
		port->candidate_crtcs[1] = dcp_fabric_port_crtc_mask(true, true, candidates,
								     typec_only);
	}
}

/* apple_dcp_tb_dp_tunnel(): plan (if ordered), pick, claim */
static struct dcp_fabric_route *tunnel(struct fabric *f, unsigned int p, unsigned int dpin,
				       bool ordered)
{
	struct dcp_fabric_port *port = &f->port[p];
	struct dcp_fabric_route *planned = NULL, *best;
	int error = 1;

	if (ordered) {
		dcp_fabric_plan(f->pipe, f->port, port, dpin);
		planned = port->plan->target[dpin & 1];
	}
	best = dcp_fabric_tunnel_candidate(port, &f->policy, planned, ordered, dpin, true, &error);
	if (!best) {
		CHECK(error != 0);
		return NULL;
	}
	CHECK(error == 0);
	CHECK(port->candidate_crtcs[dpin & 1] & BIT(best->pipeline->crtc_index));
	best->tunnel = true;
	best->dpin = dpin;
	best->pipeline->owned = true;
	best->pipeline->tunnel_held = true;
	port->owner[dpin & 1] = best;
	return best;
}

static void release(struct fabric *f, unsigned int p, unsigned int dpin)
{
	struct dcp_fabric_route *r = f->port[p].owner[dpin & 1];

	r->tunnel = false;
	r->pipeline->owned = r->pipeline->tunnel_held = false;
	f->port[p].owner[dpin & 1] = NULL;
}

/* A dock's two streams on @p: each on a pipeline of its own, in either order. */
static void dock(const struct layout *l, unsigned int p, bool ordered, bool reverse,
		 unsigned int want0, unsigned int want1)
{
	struct fabric f;
	struct dcp_fabric_route *s0, *s1;

	setup(&f, l);
	if (!reverse) {
		s0 = tunnel(&f, p, 0, ordered);
		s1 = tunnel(&f, p, 1, ordered);
	} else {
		s1 = tunnel(&f, p, 1, ordered);
		s0 = tunnel(&f, p, 0, ordered);
	}
	CHECK(s0 && s1);
	CHECK(s0->pipeline != s1->pipeline);
	CHECK(s0->pipeline->crtc_index == want0);
	CHECK(s1->pipeline->crtc_index == want1);
	/* Replug of the second stream finds its pipeline again. */
	release(&f, p, 1);
	s1 = tunnel(&f, p, 1, ordered);
	CHECK(s1 && s1->pipeline->crtc_index == want1);
	printf("ok %s: port %u %s%s: dpin0 CRTC %u, dpin1 CRTC %u\n", l->name, p,
	       ordered ? "ordered" : "compositor", reverse ? " (dpin1 first)" : "",
	       want0, want1);
}

int main(void)
{
	/* The dcpexts of the M4 MacBook Air and 14-inch MacBook Pro on firmware 27.2. */
	static const struct layout typec_only = { "two Type-C-only pipelines", 2, { false, false } };
	/* M2 Pro laptops: dcpext0 drives HDMI too, dcpext1 Type-C only. */
	static const struct layout hybrid = { "hybrid + Type-C-only", 2, { true, false } };
	static const struct layout hybrid2 = { "hybrid + two Type-C-only", 3, { true, false, false } };
	static const struct layout lone = { "one Type-C-only pipeline", 1, { false } };
	struct fabric f;
	unsigned int p;

	setup(&f, &typec_only);
	CHECK(f.port[0].candidate_crtcs[0] == (BIT(1) | BIT(2)));
	CHECK(f.port[0].candidate_crtcs[1] == BIT(2));
	for (p = 0; p < NPORT; p++) {
		dock(&typec_only, p, true, false, 1, 2);
		dock(&typec_only, p, false, false, 1, 2);
		dock(&typec_only, p, false, true, 1, 2);
	}

	/* Unchanged where a hybrid comes first. */
	setup(&f, &hybrid);
	CHECK(f.port[0].candidate_crtcs[1] == BIT(2));
	dock(&hybrid, 0, true, false, 1, 2);
	dock(&hybrid, 0, false, false, 1, 2);
	setup(&f, &hybrid2);
	CHECK(f.port[0].candidate_crtcs[0] == (BIT(1) | BIT(2) | BIT(3)));
	CHECK(f.port[0].candidate_crtcs[1] == BIT(2));
	dock(&hybrid2, 0, true, false, 1, 2);

	/* A lone Type-C-only pipeline stays the secondary's; it takes no second stream. */
	setup(&f, &lone);
	CHECK(f.port[0].candidate_crtcs[1] == BIT(1));
	CHECK(tunnel(&f, 0, 0, false));
	CHECK(!tunnel(&f, 0, 1, false));

	/* Without the ordered policy every connector is offered every pipeline. */
	CHECK(dcp_fabric_port_crtc_mask(true, false, BIT(1) | BIT(2), BIT(1) | BIT(2)) ==
	      (BIT(1) | BIT(2)));
	CHECK(dcp_fabric_port_crtc_mask(true, true, BIT(1), 0) == 0);

	puts("PASS both dock streams get pipelines of their own on every ordered layout");
	return 0;
}
'''


def build(tmp, name, core):
    header = re.sub(r'^#include.*\n', '', HEADER, flags=re.M)
    unit = PRE + header + ''.join(function(core, n) for n in NAMES) + CASES
    c = Path(tmp) / (name + '.c')
    binary = Path(tmp) / name
    c.write_text(unit)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-sign-compare',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(c), '-o', str(binary)], check=True)
    return subprocess.run([str(binary)], capture_output=True, text=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='dual-stream-pairing-') as temporary:
        tmp = args.out or Path(temporary)
        tmp.mkdir(parents=True, exist_ok=True)
        for name, (old, new) in MUTATIONS.items():
            assert CORE.count(old) == 1, 'stale mutation: ' + name
            run = build(tmp, name, CORE.replace(old, new))
            assert run.returncode, 'mutation survived: ' + name
            assert 'FAIL line' in run.stderr, 'mutation broke the harness: ' + name
            print('PASS negative control:', name)
        run = build(tmp, 'production', CORE)
        sys.stdout.write(run.stdout)
        sys.stderr.write(run.stderr)
        raise SystemExit(run.returncode)


if __name__ == '__main__':
    main()
