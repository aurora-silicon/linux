#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the production T8110 TLB command path against a modelled command engine.

The engine stays busy for as long as each case says after a command is
written, and the model checks that a command is only ever written to an idle
engine with dart->lock held. Time is simulated: udelay() advances it, and
another CPU can take the lock and write a command whenever the code under test
does not hold it.

With --dtc, it also builds the J700 device tree and runs the production
apple_dart_display_scanout() over it: only the DARTs the display subsystem
scans out through get the longer wait, not the DCP firmware DARTs and not
the Wi-Fi's PCIe DART.
"""
from pathlib import Path
import argparse
import re
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
FUNCTIONS = ('apple_dart_display_scanout', 'apple_dart_t8110_tlb_settled',
             'apple_dart_t8110_tlb_wait', 'apple_dart_t8110_hw_tlb_command_range')
# Older revisions have the command function only, or no scan-out predicate.
OPTIONAL = {'apple_dart_display_scanout', 'apple_dart_t8110_tlb_settled',
            'apple_dart_t8110_tlb_wait'}
DEFINES = ('DART_T8110_TLB_CMD', 'DART_T8110_TLB_CMD_BUSY', 'DART_T8110_TLB_CMD_OP',
           'DART_T8110_TLB_CMD_OP_FLUSH_SID', 'DART_T8110_TLB_CMD_OP_FLUSH_UNLOCK',
           'DART_T8110_TLB_CMD_V2', 'DART_T8110_TLB_CMD_RANGE', 'DART_T8110_TLB_CMD_STT',
           'DART_T8110_TLB_START', 'DART_T8110_TLB_END', 'DART_T8110_FLUSH_BUSY_TIMEOUT',
           'DART_T8110_FLUSH_STALL_TIMEOUT', 'DART_T8110_FLUSH_STALL_POLL',
           'DART_T8110_TLB_CMD_STREAM', 'DART_T8110_CTC_FLUSH')
OPTIONAL_DEFINES = {'DART_T8110_FLUSH_STALL_TIMEOUT', 'DART_T8110_FLUSH_STALL_POLL'}
CASES = ('fast', 'stall-after', 'stall-before', 'stuck', 'not-handoff', 'not-scanout',
         'not-range', 'ctc-flush', 'sleep-listed', 'superseded', 'gated', 'two-streams')
J700 = 'arch/arm64/boot/dts/apple/t8140-j700.dts'
# What the J700 display subsystem scans out through: disp0 and dispext0.
SCANOUT = {'/soc/iommu@412300000', '/soc/iommu@416300000'}

PRELUDE = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64;
typedef int64_t s64;
typedef uint32_t u32;
#define BIT(n) (1UL << (n))
#define GENMASK(h, l) (((~0UL) << (l)) & (~0UL >> (63 - (h))))
#define FIELD_PREP(m, v) (((u32)(v) << __builtin_ctzl(m)) & (m))
#define READ_ONCE(x) (*(volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, v) (*(volatile __typeof__(x) *)&(x) = (v))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define ilog2(x) (63 - __builtin_clzll(x))
#define barrier() __asm__ __volatile__("" ::: "memory")
#define dma_wmb() barrier()
#define NSEC_PER_USEC 1000LL
#define DART_MAX_STREAMS 64
#define BITS_PER_LONG 64
#define for_each_set_bit(b, map, n) \
	for ((b) = 0; (b) < (n); (b)++) if ((map)[(b) / BITS_PER_LONG] & (1UL << ((b) % BITS_PER_LONG)))

struct apple_dart {
	int lock;
	int num_streams;
	unsigned int version;
	unsigned long pgsize;
	bool commands_gated, fw_handoff, display_scanout, ctc_flush;
	struct { unsigned int count; } sleep_restore;
	unsigned long tlb_commands;
	void *dev;
};
struct apple_dart_stream_map { struct apple_dart *dart; unsigned long sidmap[1]; };

/* Simulated time in microseconds, and the command engine. */
static unsigned long now;
static unsigned long busy_until;
static unsigned long durations[8];
static int ndurations, commands, lock_drops, errors_logged, infos_logged;
static u32 written[8], starts[8], ends[8], last_command, start_reg, end_reg;
static unsigned long info_us;
static struct apple_dart *model;

/* Another CPU: at @other_at it takes the lock (only if free) and acts. */
static unsigned long other_at;
static void (*other)(void);

static bool busy(void) { return now < busy_until; }

static u32 apple_dart_readl(struct apple_dart *dart, u32 offset)
{
	assert(dart == model);
	if (offset == DART_T8110_TLB_CMD)
		return last_command | (busy() ? DART_T8110_TLB_CMD_BUSY : 0);
	assert(!"unexpected register read");
	return 0;
}

static void apple_dart_writel(struct apple_dart *dart, u32 value, u32 offset)
{
	assert(dart == model && dart->lock == 1);
	/* Nothing may touch the engine while a command runs. */
	assert(!busy());
	if (offset == DART_T8110_TLB_START) {
		start_reg = value;
	} else if (offset == DART_T8110_TLB_END) {
		end_reg = value;
	} else if (offset == DART_T8110_TLB_CMD) {
		assert(commands < 8);
		written[commands] = value;
		starts[commands] = start_reg;
		ends[commands] = end_reg;
		busy_until = now + (commands < ndurations ? durations[commands] : 0);
		commands++;
		last_command = value;
	} else if (offset == DART_T8110_CTC_FLUSH) {
		assert(!"CTC flush on a DART without one");
	} else {
		assert(!"unexpected register write");
	}
}

static void udelay(unsigned long us)
{
	now += us;
	if (other && now >= other_at && !model->lock) {
		void (*act)(void) = other;

		other = NULL;
		act();
	}
}

#define spin_lock_irqsave(l, f) do { assert(*(l) == 0); *(l) = 1; (f) = 0; } while (0)
#define spin_unlock_irqrestore(l, f) \
	do { assert(*(l) == 1); *(l) = 0; lock_drops++; (void)(f); } while (0)
#define dev_err_ratelimited(dev, fmt, ...) (errors_logged++)
#define dev_info_ratelimited(dev, fmt, command, n, map, us) (infos_logged++, info_us = (us))

/*
 * The device tree: dt_nodes, in tree order, each with its compatible list
 * (NUL-separated), whether it is available and its iommus. Every reference
 * handed out must be put again.
 */
struct device_node {
	const char *path, *compatible;
	bool available;
	int niommus;
	struct { int provider, count; u32 cells[4]; } iommus[16];
};
struct of_phandle_args { struct device_node *np; int args_count; u32 args[16]; };
static struct device_node *dt_nodes;
static int dt_count, dt_refs;

static bool dt_compatible(const struct device_node *np, const char *compat)
{
	for (const char *c = np->compatible; c && *c; c += strlen(c) + 1)
		if (!strcmp(c, compat))
			return true;
	return false;
}

static void of_node_put(struct device_node *np)
{
	if (np) {
		assert(dt_refs > 0);
		dt_refs--;
	}
}

static struct device_node *of_find_compatible_node(struct device_node *from, const char *type,
						   const char *compat)
{
	int i = from ? (int)(from - dt_nodes) + 1 : 0;

	assert(!type);
	of_node_put(from);
	for (; i < dt_count; i++)
		if (dt_compatible(&dt_nodes[i], compat)) {
			dt_refs++;
			return &dt_nodes[i];
		}
	return NULL;
}

static bool of_device_is_available(const struct device_node *np)
{
	return np->available;
}

static int of_parse_phandle_with_args(const struct device_node *np, const char *list,
				      const char *cells, int index, struct of_phandle_args *out)
{
	assert(!strcmp(list, "iommus") && !strcmp(cells, "#iommu-cells"));
	if (index < 0 || index >= np->niommus)
		return -ENOENT;
	out->np = &dt_nodes[np->iommus[index].provider];
	out->args_count = np->iommus[index].count;
	memcpy(out->args, np->iommus[index].cells, sizeof(np->iommus[index].cells));
	dt_refs++;
	return 0;
}

static void dt_node_cleanup(struct device_node **np)
{
	of_node_put(*np);
}

#define for_each_compatible_node_scoped(dn, type, compatible) \
	for (struct device_node *dn __attribute__((cleanup(dt_node_cleanup))) = \
	     of_find_compatible_node(NULL, type, compatible); dn; \
	     dn = of_find_compatible_node(dn, type, compatible))

#define read_poll_timeout_atomic(op, val, cond, delay_us, timeout_us, delay_before_read, args...) \
({ \
	long __left = (timeout_us); \
	int __ret; \
	for (;;) { \
		bool __expired = __left < 0; \
		(val) = op(args); \
		if (cond) { __ret = 0; break; } \
		if (__expired) { __ret = -ETIMEDOUT; break; } \
		udelay(delay_us); \
		__left -= (delay_us); \
	} \
	__ret; \
})
'''

MAIN = r'''
/* The old revisions have no stall wait: the expectations of this one. */
#ifndef DART_T8110_FLUSH_STALL_TIMEOUT
#define DART_T8110_FLUSH_STALL_TIMEOUT 250000
#endif
#ifndef DART_T8110_FLUSH_STALL_POLL
#define DART_T8110_FLUSH_STALL_POLL 10
#endif

static struct apple_dart dart;
static struct apple_dart_stream_map stream;

static void reset(bool handoff)
{
	memset(&dart, 0, sizeof(dart));
	dart.num_streams = 32;
	dart.version = 0x202;
	dart.pgsize = 0x4000;
	dart.fw_handoff = handoff;
	dart.display_scanout = true;
	model = &dart;
	memset(&stream, 0, sizeof(stream));
	stream.dart = &dart;
	stream.sidmap[0] = 1;
	now = busy_until = 0;
	ndurations = commands = lock_drops = errors_logged = infos_logged = 0;
	info_us = 0;
	last_command = start_reg = end_reg = 0;
	other = NULL;
}

static int range(void)
{
	/* The DVA window of the J700 display streams. */
	return apple_dart_t8110_hw_tlb_command_range(&stream, DART_T8110_TLB_CMD_OP_FLUSH_SID,
						     true, 0x10000004000ULL, 0x10000007fffULL);
}

static void check_range_command(int i, u32 sid)
{
	u32 want = FIELD_PREP(DART_T8110_TLB_CMD_OP, DART_T8110_TLB_CMD_OP_FLUSH_SID) |
		   FIELD_PREP(DART_T8110_TLB_CMD_STREAM, sid) | DART_T8110_TLB_CMD_V2 |
		   DART_T8110_TLB_CMD_RANGE | DART_T8110_TLB_CMD_STT;

	assert(written[i] == want);
	assert(starts[i] == (u32)((0x10000004000ULL >> 14) << 2));
	assert(ends[i] == (u32)((0x10000007fffULL >> 14) << 2));
}

/* Another CPU's whole command, the way the driver writes one. */
static unsigned long other_duration;
static void other_command(void)
{
	dart.lock = 1;
	assert(!busy());
	apple_dart_writel(&dart, FIELD_PREP(DART_T8110_TLB_CMD_STREAM, 16) |
			  FIELD_PREP(DART_T8110_TLB_CMD_OP, DART_T8110_TLB_CMD_OP_FLUSH_SID),
			  DART_T8110_TLB_CMD);
	busy_until = now + other_duration;
	dart.tlb_commands++;
	dart.lock = 0;
}

static void other_gates(void)
{
	dart.lock = 1;
	dart.commands_gated = true;
	dart.lock = 0;
}

int main(int argc, char **argv)
{
	const char *c = argc > 1 ? argv[1] : "";
	int ret;

	reset(true);
	if (!strcmp(c, "fast")) {
		/* An engine that answers at once: one command, the lock never let go mid-way. */
		assert(!range());
		assert(commands == 1 && lock_drops == 1 && !errors_logged && !infos_logged);
		check_range_command(0, 0);
	} else if (!strcmp(c, "stall-after")) {
		/* The J700 display: busy for 40 ms after the command, then done. */
		durations[0] = 40000;
		ndurations = 1;
		ret = range();
		printf("stall-after: %d after %lu us\n", ret, now);
		assert(ret == 0);
		assert(commands == 1 && !errors_logged && infos_logged == 1);
		assert(info_us >= 38000 && info_us <= 41000);
		assert(now >= 40000 && now < 40000 + 2 * DART_T8110_FLUSH_STALL_POLL + 10);
		check_range_command(0, 0);
	} else if (!strcmp(c, "stall-before")) {
		/* An earlier command still runs: write ours only once it is done. */
		busy_until = 30000;
		ret = range();
		assert(ret == 0);
		assert(commands == 1 && !errors_logged && infos_logged == 1);
		assert(now >= 30000);
		check_range_command(0, 0);
	} else if (!strcmp(c, "stuck")) {
		/* A command that never finishes still fails, after the longer wait. */
		durations[0] = ~0UL / 2;
		ndurations = 1;
		ret = range();
		assert(ret == -ETIMEDOUT);
		assert(commands == 1 && errors_logged == 1 && !infos_logged);
		assert(now >= DART_T8110_FLUSH_STALL_TIMEOUT);
		assert(now <= DART_T8110_FLUSH_STALL_TIMEOUT + 2 * DART_T8110_FLUSH_BUSY_TIMEOUT + 100);
		assert(!dart.lock);
	} else if (!strcmp(c, "not-handoff")) {
		/* Other DARTs keep failing after the short wait, lock held throughout. */
		reset(false);
		durations[0] = 40000;
		ndurations = 1;
		assert(range() == -ETIMEDOUT);
		assert(commands == 1 && errors_logged == 1 && lock_drops == 1);
		assert(now <= DART_T8110_FLUSH_BUSY_TIMEOUT + 10);
	} else if (!strcmp(c, "not-scanout")) {
		/*
		 * A handed-off DART no display subsystem scans out through, such as
		 * the DCP's own: the same stall fails after the short wait, lock held.
		 */
		dart.display_scanout = false;
		durations[0] = 40000;
		ndurations = 1;
		assert(range() == -ETIMEDOUT);
		assert(commands == 1 && errors_logged == 1 && !infos_logged && lock_drops == 1);
		assert(now <= DART_T8110_FLUSH_BUSY_TIMEOUT + 10);
	} else if (!strcmp(c, "scanout-dt")) {
#ifdef HAVE_DISPLAY_SCANOUT
		/* The production predicate over the J700 tree: every DART node. */
		int darts = 0;

		dt_nodes = dt_table;
		dt_count = sizeof(dt_table) / sizeof(dt_table[0]);
		for (int i = 0; i < dt_count; i++) {
			if (!dt_compatible(&dt_nodes[i], "apple,t8110-dart"))
				continue;
			darts++;
			if (apple_dart_display_scanout(&dt_nodes[i]))
				printf("scanout %s\n", dt_nodes[i].path);
			assert(!dt_refs);
		}
		printf("darts %d\n", darts);
		/* A display subsystem that is disabled scans out through none. */
		for (int i = 0; i < dt_count; i++)
			if (dt_compatible(&dt_nodes[i], "apple,display-subsystem"))
				dt_nodes[i].available = false;
		for (int i = 0; i < dt_count; i++)
			assert(!dt_compatible(&dt_nodes[i], "apple,t8110-dart") ||
			       !apple_dart_display_scanout(&dt_nodes[i]));
		assert(!dt_refs);
#else
		fprintf(stderr, "no apple_dart_display_scanout()\n");
		return 2;
#endif
	} else if (!strcmp(c, "not-range")) {
		/* The fault handler's unlock is no range invalidate: short wait. */
		durations[0] = 40000;
		ndurations = 1;
		ret = apple_dart_t8110_hw_tlb_command_range(&stream,
				DART_T8110_TLB_CMD_OP_FLUSH_UNLOCK, false, 0, 0);
		assert(ret == -ETIMEDOUT && lock_drops == 1);
		assert(now <= DART_T8110_FLUSH_BUSY_TIMEOUT + 10);
	} else if (!strcmp(c, "ctc-flush")) {
		/* A CTC flush needs the engine idle under the lock: short wait. */
		dart.ctc_flush = true;
		durations[0] = 40000;
		ndurations = 1;
		assert(range() == -ETIMEDOUT && lock_drops == 1);
		assert(now <= DART_T8110_FLUSH_BUSY_TIMEOUT + 10);
	} else if (!strcmp(c, "sleep-listed")) {
		/* A sleep prepare writes its list under the lock: short wait. */
		dart.sleep_restore.count = 1;
		durations[0] = 40000;
		ndurations = 1;
		assert(range() == -ETIMEDOUT && lock_drops == 1);
		assert(now <= DART_T8110_FLUSH_BUSY_TIMEOUT + 10);
	} else if (!strcmp(c, "superseded")) {
		/*
		 * Ours finishes at 30 ms; another CPU then writes one that stays busy.
		 * Ours is done: that one could only be written to an idle engine.
		 */
		durations[0] = 30000;
		ndurations = 1;
		other_duration = ~0UL / 2;
		other_at = 30000;
		other = other_command;
		ret = range();
		assert(ret == 0 && !errors_logged);
		assert(commands == 2);
		assert(now < 31000);
	} else if (!strcmp(c, "gated")) {
		/* Gated while waiting to write: nothing is written. */
		busy_until = 30000;
		other_at = 10000;
		other = other_gates;
		ret = range();
		assert(ret == -EHOSTDOWN && commands == 0 && !errors_logged);
		assert(!dart.lock);
	} else if (!strcmp(c, "two-streams")) {
		/* Each stream of the map, in order, each after the last finished. */
		stream.sidmap[0] = BIT(0) | BIT(16);
		durations[0] = 20000;
		durations[1] = 5000;
		ndurations = 2;
		assert(!range());
		assert(commands == 2 && !errors_logged && infos_logged == 1);
		check_range_command(0, 0);
		check_range_command(1, 16);
		assert(info_us >= 19000 && info_us <= 21000);
	} else {
		fprintf(stderr, "unknown case %s\n", c);
		return 2;
	}
	assert(!dart.lock);
	printf("pass %s\n", c);
	return 0;
}
'''


def function(text, name):
    # A definition: 'static', its type (maybe on a line of its own), the name.
    match = re.search(r'^static[^;{}()]*?\b' + re.escape(name) + r'\([^;{}]*\)\s*\{', text, re.M)
    if not match:
        return None
    start = text.index('{', match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end]


class Tree:
    """A flattened tree's nodes in depth-first order: paths, properties, parents."""

    def __init__(self, blob):
        magic, _, off_struct, off_strings = struct.unpack_from('>4I', blob)
        assert magic == 0xd00dfeed, 'not a flattened device tree'
        self.paths, self.props, self.parents = [], [], []
        stack, pos = [], off_struct
        while True:
            token, = struct.unpack_from('>I', blob, pos)
            pos += 4
            if token == 1:  # FDT_BEGIN_NODE
                end = blob.index(b'\0', pos)
                name = blob[pos:end].decode()
                pos = (end + 4) & ~3
                parent = stack[-1] if stack else None
                self.paths.append('/' if parent is None else
                                  self.paths[parent].rstrip('/') + '/' + name)
                self.parents.append(parent)
                self.props.append({})
                stack.append(len(self.paths) - 1)
            elif token == 2:  # FDT_END_NODE
                stack.pop()
            elif token == 3:  # FDT_PROP
                length, nameoff = struct.unpack_from('>II', blob, pos)
                pos += 8
                start = off_strings + nameoff
                name = blob[start:blob.index(b'\0', start)].decode()
                self.props[stack[-1]][name] = blob[pos:pos + length]
                pos = (pos + length + 3) & ~3
            elif token == 9:  # FDT_END
                assert not stack, 'unterminated node'
                return
            else:
                assert token == 4, f'bad structure token {token:#x}'  # FDT_NOP

    def words(self, node, name):
        value = self.props[node].get(name, b'')
        assert len(value) % 4 == 0, f'{self.paths[node]}: malformed {name}'
        return struct.unpack('>' + 'I' * (len(value) // 4), value)

    def compatible(self, node):
        return [c.decode() for c in self.props[node].get('compatible', b'').split(b'\0') if c]

    def phandle(self, value):
        return next(n for n in range(len(self.paths)) if self.words(n, 'phandle') == (value,))

    def iommus(self, node):
        refs, pos, result = self.words(node, 'iommus'), 0, []
        while pos < len(refs):
            provider = self.phandle(refs[pos])
            count, = self.words(provider, '#iommu-cells')
            result.append((provider, refs[pos + 1:pos + 1 + count]))
            pos += 1 + count
        assert pos == len(refs), f'{self.paths[node]}: truncated iommus'
        return result


def build_j700(dtc, tmp):
    """Preprocess and compile the J700 tree, as scripts/Makefile.dtbs does."""
    text = subprocess.check_output(['cpp', '-nostdinc', '-undef', '-D__DTS__',
                                    '-I', str(ROOT / 'scripts/dtc/include-prefixes'),
                                    '-x', 'assembler-with-cpp', str(ROOT / J700)])
    dtb = Path(tmp) / 't8140-j700.dtb'
    subprocess.run([dtc, '-q', '-I', 'dts', '-O', 'dtb', '-o', str(dtb)], input=text, check=True)
    return Tree(dtb.read_bytes())


def dt_table(tree):
    """The tree as the C model's dt_table."""
    def string(value):
        assert re.fullmatch(r'[-\w,.@/]*', value), value
        return '"' + value + '"'

    rows = []
    for node, path in enumerate(tree.paths):
        status = tree.props[node].get('status', b'okay\0')
        compat = tree.compatible(node)
        iommus = tree.iommus(node)
        assert len(iommus) <= 16 and all(len(cells) <= 4 for _, cells in iommus)
        rows.append('\t{ %s, %s, %d, %d, { %s } },' % (
            string(path), ' '.join(string(c)[:-1] + '\\0"' for c in compat) or 'NULL',
            status in (b'okay\0', b'ok\0'), len(iommus),
            ', '.join('{ %d, %d, { %s } }' % (p, len(c), ', '.join(map(str, c)) or '0')
                      for p, c in iommus) or '{ 0 }'))
    return ('\n#define HAVE_DISPLAY_SCANOUT\nstatic struct device_node dt_table[] = {\n' +
            '\n'.join(rows) + '\n};\n')


def check_scanout(tree, output):
    """The DARTs the predicate picked: the display's, not the DCPs' or the Wi-Fi's."""
    picked = set(re.findall(r'^scanout (\S+)$', output, re.M))
    darts = int(re.search(r'^darts (\d+)$', output, re.M).group(1))
    assert darts >= 20, f'only {darts} DART nodes in {J700}'
    assert picked == SCANOUT, f'scan-out DARTs {sorted(picked)}, expected {sorted(SCANOUT)}'
    nodes = range(len(tree.paths))
    dcps = {tree.paths[p] for n in nodes if 'apple,dcp' in tree.compatible(n)
            for p, _ in tree.iommus(n)}
    wifi = next(n for n in nodes if 'pci14c3,7932' in tree.compatible(n))
    host = wifi
    while 'iommu-map' not in tree.props[host]:
        host = tree.parents[host]
    radio = tree.paths[tree.phandle(tree.words(host, 'iommu-map')[1])]
    assert len(dcps) == 2 and radio == '/soc/iommu@390000000', (dcps, radio)
    assert not picked & (dcps | {radio})
    print(f'scanout-dt: {darts} DARTs; scan-out {", ".join(sorted(picked))}; '
          f'not the DCP firmware DARTs {", ".join(sorted(dcps))} or the Wi-Fi DART {radio}')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, help='an apple-dart.c to test (default: this tree)')
    parser.add_argument('--ref', help='test drivers/iommu/apple-dart.c of this revision instead')
    parser.add_argument('--case', action='append')
    parser.add_argument('--mutation', choices=('every-handoff',),
                        help='every-handoff: wait on every handed-off DART, not just the scan-out ones')
    parser.add_argument('--dtc', help="this kernel's built scripts/dtc/dtc: also check the J700 tree")
    args = parser.parse_args()
    if args.ref:
        text = subprocess.check_output(['git', '-C', str(ROOT), 'show',
                                        args.ref + ':drivers/iommu/apple-dart.c'], text=True)
    else:
        text = (args.source or ROOT / 'drivers/iommu/apple-dart.c').read_text()
    if args.mutation == 'every-handoff':
        cut = ' !dart->display_scanout ||'
        if text.count(cut) != 1:
            # Not a kill: the harness counts a traceback as a broken run.
            raise RuntimeError('every-handoff: no display_scanout check to remove')
        text = text.replace(cut, '')

    defines = []
    for name in DEFINES:
        match = re.search(r'^#define ' + re.escape(name) + r'\b.*$', text, re.M)
        if match:
            defines.append(match.group(0))
        elif name not in OPTIONAL_DEFINES:
            print(f'FAIL line 0: {name} is missing')
            sys.exit(1)
    bodies = []
    for name in FUNCTIONS:
        body = function(text, name)
        if body is None and name not in OPTIONAL:
            print(f'FAIL line 0: {name} is missing')
            sys.exit(1)
        if body:
            bodies.append(body)
    # This tree's predicate is checked against its device tree; an old revision's isn't.
    scanout = 'apple_dart_display_scanout(' in '\n'.join(bodies) and not args.ref
    if scanout and not args.dtc:
        print('FAIL line 0: apple_dart_display_scanout() needs --dtc to be checked')
        sys.exit(1)

    failed = False
    with tempfile.TemporaryDirectory(prefix='dart-tlb-stall-') as tmp:
        tree = build_j700(args.dtc, tmp) if scanout else None
        code = ('\n'.join(defines) + PRELUDE + '\n'.join(bodies) +
                (dt_table(tree) if tree else '') + MAIN)
        source = Path(tmp) / 'production.c'
        binary = Path(tmp) / 'production'
        source.write_text(code)
        subprocess.run(['cc', '-std=gnu11', '-O1', '-Wall', '-Werror', '-Wno-unused-function',
                        '-Wno-unused-variable', str(source), '-o', str(binary)], check=True)
        for case in args.case or CASES + (('scanout-dt',) if tree else ()):
            run = subprocess.run([str(binary), case], capture_output=True, text=True)
            sys.stdout.write(run.stdout)
            sys.stdout.write(run.stderr)
            if run.returncode:
                print(f'case {case}: exit {run.returncode}')
                failed = True
            elif case == 'scanout-dt':
                try:
                    check_scanout(tree, run.stdout)
                except AssertionError as error:
                    print(f'FAIL line 0: scanout-dt: {error}')
                    failed = True
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
