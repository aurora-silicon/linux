.. SPDX-License-Identifier: MIT

G17P submission microbenchmark
=============================

Build on an aarch64 Linux development machine with EGL and GLES headers::

  cc -O2 -Wall -Wextra -Werror -rdynamic g17p-submit-storm.c \
     -o g17p-submit-storm -lEGL -lGLESv2 -ldl

Run with the same Mesa build and environment on the baseline and candidate
kernels, under the platform's GPU-session guard::

  EGL_PLATFORM=surfaceless ./g17p-submit-storm 10000 64 > submit.csv

The first argument is the number of small triangle draws followed by glFlush.
The second is the number of draws between glFinish calls (1 to 256). A 32x32
pbuffer avoids display pacing. Sixty-four draws warm the shader/pipeline cache
before measurement. The executable interposes libdrm's ioctl calls and records
wall time and submitting-thread CPU time for each ASAHI_SUBMIT. No output is
written inside the measured loop. The CSV includes every return value and errno;
nonzero return values invalidate a performance comparison. The program fails
if fewer submits than draws were observed, instead of assuming glFlush submits.

CPU timing includes the clock read overhead, common to both kernels. The whole
loop duration includes GPU execution and glFinish waits; use the ioctl CPU
column when assessing kernel submission cost. This is a native EGL/GLES
workload, not a simulation of DXVK or a measurement of presentation latency.
Check the renderer, ioctl count, kernel identity and GPU health alongside the
CSV, and stop at the first fault rather than retrying a hanging case.
