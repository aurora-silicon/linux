# SPDX-License-Identifier: MIT
import pathlib
import subprocess
import tempfile
import unittest


class BandwidthBounds(unittest.TestCase):
    def test_shared_header_boundaries(self):
        root = pathlib.Path(__file__).resolve().parents[5]
        code = r"""
#include <assert.h>
#include <limits.h>
#include "dcp-external-bw.h"
int main(void) {
 const u64 n=0xbc000;
 assert(apple_dcp_external_bw_valid(n,0x28000,0x70000));
 assert(!apple_dcp_external_bw_valid(n,-1,-1));
 assert(!apple_dcp_external_bw_valid(n,0,-1));
 assert(!apple_dcp_external_bw_valid(n,-1,0));
 assert(!apple_dcp_external_bw_valid(n,4,4));
 assert(!apple_dcp_external_bw_valid(n,2,4));
 assert(!apple_dcp_external_bw_valid(n,4,6));
 assert(!apple_dcp_external_bw_valid(n,n,4));
 assert(!apple_dcp_external_bw_valid(n,0,n));
 assert(!apple_dcp_external_bw_valid(3,0,4));
 assert(!apple_dcp_external_bw_valid(n,INT_MAX,0));
 assert(apple_dcp_external_bw_valid(n,0,4));
 assert(apple_dcp_external_bw_valid(n,n-4,0));
 assert(apple_dcp_external_bw_valid(n,0,n-4));
 assert(apple_dcp_external_clock_window(0x302800000ULL,n,0x302800000ULL,n));
 assert(!apple_dcp_external_clock_window(0x302800000ULL,n-4,0x302800000ULL,n));
 assert(!apple_dcp_external_clock_window(0x302800004ULL,n,0x302800000ULL,n));
 return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            tmp=pathlib.Path(directory);(tmp/'linux').mkdir()
            (tmp/'linux/types.h').write_text('#include <stdint.h>\n#include <stdbool.h>\ntypedef uint64_t u64; typedef uint32_t u32;\n')
            (tmp/'test.c').write_text(code)
            subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-pedantic','-I'+str(tmp),'-I'+str(root/'drivers/gpu/drm/apple'),str(tmp/'test.c'),'-o',str(tmp/'test')],check=True)
            subprocess.run([str(tmp/'test')],check=True)


if __name__=='__main__':
    unittest.main()
