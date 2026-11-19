#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Host check of which device tree makes the main DCP a PMP DISP consumer.

pmp_disp_consumer() of drivers/soc/apple/pmp-display.c is compiled against a
mocked "dcp" alias node. The DISP vote belongs to a machine with an internal
panel: an available main DCP without apple,connector-type. A main DCP that
drives a connector, one that is not available, a tree without the node and a
machine without a qualified retained PMP node are no consumers. With --mutations the
function is replaced by each constant and the test must then fail.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def definition(source, name):
    start = source.index(name)
    start = source.rfind('\n', 0, start) + 1
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


UNIT = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
struct device_node { bool available, connector; };
static struct device_node *alias;
static bool retained_pmp;
static int puts_;
static struct device_node *of_find_node_by_path(const char *path) { assert(!strcmp(path, "dcp")); return alias; }
static bool of_device_is_available(const struct device_node *n) { return n->available; }
static bool of_property_present(const struct device_node *n, const char *p)
{ assert(!strcmp(p, "apple,connector-type")); return n->connector; }
static void of_node_put(struct device_node *n) { if (n) puts_++; }
static bool apple_pmp_j713_board(void) { return retained_pmp; }
FUNCTION
int main(void)
{
	struct device_node panel = { true, false }, fixed = { true, true };
	struct device_node off = { false, false }, off_fixed = { false, true };

	retained_pmp = true;
	alias = &panel; assert(pmp_disp_consumer());		/* a machine with a panel */
	alias = &fixed; assert(!pmp_disp_consumer());		/* its main DCP drives a connector */
	alias = &off; assert(!pmp_disp_consumer());		/* main DCP not available */
	alias = &off_fixed; assert(!pmp_disp_consumer());
	alias = NULL; assert(!pmp_disp_consumer());		/* no main DCP described */
	assert(puts_ == 4);
	retained_pmp = false;
	alias = &panel; assert(!pmp_disp_consumer());		/* no qualified retained PMP node */
	puts("DISP consumer selection: 6 cases");
	return 0;
}
'''


def run(cc, text, directory, name):
    c = Path(directory) / (name + '.c')
    binary = Path(directory) / name
    c.write_text(text)
    subprocess.run([cc, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                    '-Wno-unused-parameter', str(c), '-o', str(binary)], check=True)
    return subprocess.run([str(binary)], capture_output=True, text=True).returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--mutations', action='store_true')
    args = parser.parse_args()
    source = (args.source / 'drivers/soc/apple/pmp-display.c').read_text()
    function = definition(source, 'static bool pmp_disp_consumer(void)')
    with tempfile.TemporaryDirectory(prefix='pmp-disp-') as directory:
        assert run(args.cc, UNIT.replace('FUNCTION', function), directory, 'base') == 0
        print('PASS DISP consumer selection follows the main DCP node')
        if args.mutations:
            for value in ('true', 'false'):
                constant = 'static bool pmp_disp_consumer(void) { return %s; }' % value
                assert run(args.cc, UNIT.replace('FUNCTION', constant), directory, value) != 0, value
            print('PASS 2 constant mutants fail')


if __name__ == '__main__':
    main()
