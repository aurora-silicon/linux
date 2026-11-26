#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Replay both streams of a dual-stream Thunderbolt dock through the pairing pass.

On an ordered dual-stream machine each Type-C port has a primary and a
secondary connector; the secondary, a dock's second stream, is offered every
Type-C-only pipeline whose route can carry that stream
(dcp_fabric_port_crtc_mask()). The pairing pass (dcp_fabric_plan()) and the
tunnel candidate (dcp_fabric_tunnel_candidate()) only hand a stream a
pipeline its connector is offered, and keep the second stream off the first
stream's pipeline while the first has none (dcp_fabric_second_yields()).

The production bodies from dcp-fabric-core.c are compiled against the core
header and replayed on these layouts:

- every candidate Type-C-only, as the two dcpexts of the M4 MacBook Air and
  14-inch MacBook Pro on firmware 27.2, where they are pipelines of the DRM
  device: dpin0 and dpin1 of a dock on either port, in either order, before
  and after a compositor owns the display, each get a pipeline of their own;
- the same with one pipeline busy on another port first: the dock's first
  stream takes the other pipeline, and its second takes the first one once
  the other port lets it go, in either stream order;
- a pipeline whose route cannot clock the second stream: never offered to it;
- a hybrid in front of one or two Type-C-only pipelines (M2 Pro/Max laptops).

Every assignment made is inside the connectors' masks as read at probe, which
is what a compositor that reads possible_crtcs once pairs from.

Each mutation must fail it; 'single-above-lowest' is the rule this replaces
(the busy-then-free sequence left the second stream no pipeline), and
'lowest-typec-only' the rule before it (the second stream never had one
while the first had the lowest).
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
         'dcp_fabric_port_route', 'dcp_fabric_port_tunneled', 'dcp_fabric_lowest',
         'dcp_fabric_second_yields', 'dcp_fabric_plan_pipeline',
         'dcp_fabric_plan', 'dcp_fabric_tunnel_keeps_direct', 'dcp_fabric_tunnel_candidate',
         'dcp_fabric_port_crtc_mask']
MUTATIONS = {
    # The rule this replaces: one Type-C-only pipeline above the port's lowest.
    'single-above-lowest': ('return mask & carriers ? mask & carriers : mask;',
                            'return dcp_fabric_lowest(mask & ~dcp_fabric_lowest(candidates)) ?:\n'
                            '\t       dcp_fabric_lowest(mask);'),
    # The rule before it: the lowest Type-C-only pipeline, whatever comes first.
    'lowest-typec-only': ('return mask & carriers ? mask & carriers : mask;',
                          'return dcp_fabric_lowest(mask);'),
    'secondary-offered-all': ('if (!secondary || !ordered_dual)\n\t\treturn candidates;',
                              'return candidates;'),
    'carriers-ignored': ('return mask & carriers ? mask & carriers : mask;', 'return mask;'),
    'candidate-ignores-mask': ('port->candidate_crtcs[dcp_fabric_dpin_slot(dpin)],',
                               '~0U,'),
    'candidate-no-yield': ('dcp_fabric_second_yields(port, pipeline) &&\n\t\t    score < UINT_MAX - 100)',
                           'false &&\n\t\t    score < UINT_MAX - 100)'),
    'plan-no-yield': ('if (stream && !port->plan->second_any &&', 'if (false &&'),
    'plan-yields-for-good': ('port->plan->second_any = true;\n\t\t\t\tagain = true;', ';'),
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
	bool no_dpin1[NPIPE];		/* its routes cannot clock DP IN 1 */
	u32 port_pipes[NPORT];		/* pipelines (by index) a port reaches; 0: all */
};

struct fabric {
	struct dcp_fabric_pipeline pipe[NPIPE];
	struct dcp_fabric_route route[NPORT][NPIPE];
	struct dcp_fabric_port port[NPORT];
	struct dcp_fabric_plan plan[NPORT];
	struct dcp_fabric_policy policy;
	unsigned int pipes;
	u32 probed[NPORT][2];		/* possible_crtcs as read at probe */
};

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); \
	assert(c); } } while (0)

/*
 * apple_probe_typec_ports(): every candidate pipeline, the Type-C-only ones,
 * and those whose route carries the second stream (dcp_typec_port_carries_stream()).
 */
static void setup(struct fabric *f, const struct layout *l)
{
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
	}
	for (p = 0; p < NPORT; p++) {
		struct dcp_fabric_port *port = &f->port[p];
		struct dcp_fabric_route **tail = &port->routes;
		u32 candidates = 0, typec_only = 0, carriers = 0;

		port->key = p + 1;
		port->next = p + 1 < NPORT ? &f->port[p + 1] : NULL;
		port->plan = &f->plan[p];
		for (i = 0; i < l->pipes; i++) {
			struct dcp_fabric_route *route = &f->route[p][i];

			if (l->port_pipes[p] && !(l->port_pipes[p] & BIT(i)))
				continue;
			route->pipeline = &f->pipe[i];
			route->tunnel_clock_blocked = l->no_dpin1[i] ? BIT(1) : 0;
			*tail = route;
			tail = &route->next;
			candidates |= BIT(i + 1);
			if (!l->hybrid[i])
				typec_only |= BIT(i + 1);
			if (!l->no_dpin1[i])
				carriers |= BIT(i + 1);
		}
		*tail = NULL;
		port->candidate_crtcs[0] = dcp_fabric_port_crtc_mask(false, true, candidates,
								     typec_only, carriers);
		port->candidate_crtcs[1] = dcp_fabric_port_crtc_mask(true, true, candidates,
								     typec_only, carriers);
		f->probed[p][0] = port->candidate_crtcs[0];
		f->probed[p][1] = port->candidate_crtcs[1];
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
	CHECK(!(best->tunnel_clock_blocked & BIT(dpin)));
	/* A compositor that read the masks once can pair it. */
	CHECK(f->probed[p][dpin & 1] & BIT(best->pipeline->crtc_index));
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

/*
 * Another port's display holds pipeline @busy (1-based CRTC) while the dock
 * on @p brings its first stream (or its second, @reverse); that display goes,
 * then the dock's other stream comes. Both streams light, on different
 * pipelines, each one its connector was offered at probe.
 */
static void busy_then_free(const struct layout *l, unsigned int p, unsigned int busy,
			   bool reverse)
{
	unsigned int other = !p, first = reverse, second = !reverse;
	struct dcp_fabric_route *s_first, *s_second, *held;
	struct fabric f;

	setup(&f, l);
	/* The other port's DP-alt display (an owned pipeline, no tunnel). */
	held = &f.route[other][busy - 1];
	held->pipeline->owned = true;
	f.port[other].owner[0] = held;
	s_first = tunnel(&f, p, first, false);
	CHECK(s_first && s_first->pipeline->crtc_index != busy);
	/* The other display goes. */
	held->pipeline->owned = false;
	f.port[other].owner[0] = NULL;
	s_second = tunnel(&f, p, second, false);
	CHECK(s_second && s_second->pipeline->crtc_index == busy);
	CHECK(s_first->pipeline != s_second->pipeline);
	/* The second stream replugged finds the free pipeline again. */
	release(&f, p, second);
	CHECK(tunnel(&f, p, second, false) == s_second);
	printf("ok %s: port %u, CRTC %u busy then free%s: dpin%u CRTC %u, dpin%u CRTC %u\n",
	       l->name, p, busy, reverse ? " (dpin1 first)" : "",
	       first, s_first->pipeline->crtc_index, second, busy);
}

int main(void)
{
	/* The dcpexts of the M4 MacBook Air and 14-inch MacBook Pro on firmware 27.2. */
	static const struct layout typec_only = { .name = "two Type-C-only pipelines", .pipes = 2 };
	/* M2 Pro laptops: dcpext0 drives HDMI too, dcpext1 Type-C only. */
	static const struct layout hybrid = { .name = "hybrid + Type-C-only", .pipes = 2,
					      .hybrid = { true, false } };
	static const struct layout hybrid2 = { .name = "hybrid + two Type-C-only", .pipes = 3,
					       .hybrid = { true, false, false } };
	static const struct layout lone = { .name = "one Type-C-only pipeline", .pipes = 1 };
	/* The lower pipeline's routes have no pixel clock for DP IN 1. */
	static const struct layout unclocked = { .name = "lower pipeline cannot clock dpin1", .pipes = 2,
						 .no_dpin1 = { true, false } };
	/* Port 0 reaches the upper pipeline only. */
	static const struct layout narrow = { .name = "other port on the upper pipeline only", .pipes = 2,
					      .port_pipes = { BIT(1), 0 } };
	struct fabric f;
	unsigned int p;

	setup(&f, &typec_only);
	CHECK(f.port[0].candidate_crtcs[0] == (BIT(1) | BIT(2)));
	CHECK(f.port[0].candidate_crtcs[1] == (BIT(1) | BIT(2)));
	for (p = 0; p < NPORT; p++) {
		dock(&typec_only, p, true, false, 1, 2);
		dock(&typec_only, p, true, true, 1, 2);
		dock(&typec_only, p, false, false, 1, 2);
		dock(&typec_only, p, false, true, 1, 2);
		busy_then_free(&typec_only, p, 1, false);
		busy_then_free(&typec_only, p, 1, true);
		busy_then_free(&typec_only, p, 2, false);
		busy_then_free(&typec_only, p, 2, true);
	}

	/* A pipeline the second stream cannot clock is never offered to it. */
	setup(&f, &unclocked);
	CHECK(f.port[0].candidate_crtcs[1] == BIT(2));
	dock(&unclocked, 0, true, false, 1, 2);
	dock(&unclocked, 0, false, true, 1, 2);
	setup(&f, &unclocked);
	f.pipe[0].owned = true;
	CHECK(tunnel(&f, 0, 0, false)->pipeline->crtc_index == 2);
	f.pipe[0].owned = false;
	CHECK(!tunnel(&f, 0, 1, false));
	puts("ok lower pipeline cannot clock dpin1: dpin1 refused it while dpin0 holds CRTC 2");

	/*
	 * Before a compositor: the dock's second stream comes first, and the
	 * one pipeline above the first stream's is planned for another port's
	 * display. It takes the first stream's pipeline after all.
	 */
	setup(&f, &narrow);
	f.port[0].wanted = f.port[0].hpd = true;
	{
		struct dcp_fabric_route *s1 = tunnel(&f, 1, 1, true);

		CHECK(s1 && s1->pipeline->crtc_index == 1);
	}
	puts("ok other port on the upper pipeline only: ordered dpin1 takes CRTC 1");

	/* Unchanged where a hybrid comes first. */
	setup(&f, &hybrid);
	CHECK(f.port[0].candidate_crtcs[1] == BIT(2));
	dock(&hybrid, 0, true, false, 1, 2);
	dock(&hybrid, 0, false, false, 1, 2);
	dock(&hybrid, 0, false, true, 1, 2);
	/*
	 * As before: with the hybrid busy the first stream takes the Type-C-only
	 * pipeline, and the second is not handed the hybrid once it is free,
	 * which its connector was never offered.
	 */
	setup(&f, &hybrid);
	f.pipe[0].owned = true;
	CHECK(tunnel(&f, 0, 0, false)->pipeline->crtc_index == 2);
	f.pipe[0].owned = false;
	CHECK(!tunnel(&f, 0, 1, false));
	puts("ok hybrid + Type-C-only: hybrid busy then free: dpin1 not handed the hybrid");
	setup(&f, &hybrid2);
	CHECK(f.port[0].candidate_crtcs[0] == (BIT(1) | BIT(2) | BIT(3)));
	CHECK(f.port[0].candidate_crtcs[1] == (BIT(2) | BIT(3)));
	dock(&hybrid2, 0, true, false, 1, 2);
	dock(&hybrid2, 0, false, false, 1, 2);

	/* A lone Type-C-only pipeline stays the secondary's; it takes no second stream. */
	setup(&f, &lone);
	CHECK(f.port[0].candidate_crtcs[1] == BIT(1));
	CHECK(tunnel(&f, 0, 0, false));
	CHECK(!tunnel(&f, 0, 1, false));

	/* Without the ordered policy every connector is offered every pipeline. */
	CHECK(dcp_fabric_port_crtc_mask(true, false, BIT(1) | BIT(2), BIT(1) | BIT(2), 0) ==
	      (BIT(1) | BIT(2)));
	CHECK(dcp_fabric_port_crtc_mask(true, true, BIT(1), 0, ~0U) == 0);

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
