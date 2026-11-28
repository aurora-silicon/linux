#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the driver's completion poll against an in-memory producer."""
from pathlib import Path
import subprocess
import tempfile

DRIVER = Path(__file__).resolve().parents[2] / "drivers/bluetooth/hci_bcm4377.c"

FIXTURE = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint16_t u16;
typedef uint16_t __le16;

struct ring_state {
	__le16 completion_ring_head[1], completion_ring_tail[1];
};
struct bcm4377_data {
	struct ring_state *ring_state;
};
struct bcm4377_completion_ring {
	u16 ring_id, n_entries;
	bool enabled;
};

#define READ_ONCE(x) (x)
#define dma_rmb() ((void)0)
#define le16_to_cpu(x) (x)
#define cpu_to_le16(x) (x)
#define dev_dbg(...) ((void)0)
#define dev_warn_ratelimited(...) ((void)0)

static u16 slots[4];
static unsigned int handled;
static bool advance_head;

static void bcm4377_handle_completion(struct bcm4377_data *data,
				      struct bcm4377_completion_ring *ring, u16 tail)
{
	assert(handled < ring->n_entries);
	slots[handled++] = tail;
	if (advance_head)
		data->ring_state->completion_ring_head[ring->ring_id] = 2;
}
"""

CASES = r"""
int main(void)
{
	struct ring_state state = {};
	struct bcm4377_data data = { .ring_state = &state };
	struct bcm4377_completion_ring ring = {
		.n_entries = sizeof(slots) / sizeof(slots[0]), .enabled = true,
	};
	u16 invalid[] = { ring.n_entries, UINT16_MAX };
	u16 last = ring.n_entries - 1;

	state.completion_ring_tail[0] = last;
	for (unsigned int i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		state.completion_ring_head[0] = invalid[i];
		bcm4377_poll_completion_ring(&data, &ring);
		assert(handled == 0 && state.completion_ring_tail[0] == last);
	}

	state.completion_ring_head[0] = last;
	bcm4377_poll_completion_ring(&data, &ring);
	assert(handled == 0 && state.completion_ring_tail[0] == last);

	/* Wrap around. A completion added mid-poll belongs to the next poll. */
	state.completion_ring_head[0] = 1;
	advance_head = true;
	bcm4377_poll_completion_ring(&data, &ring);
	assert(handled == 2 && slots[0] == last && slots[1] == 0);
	assert(state.completion_ring_tail[0] == 1);
	bcm4377_poll_completion_ring(&data, &ring);
	assert(handled == 3 && slots[2] == 1 && state.completion_ring_tail[0] == 2);
}
"""


def main():
    source = DRIVER.read_text()
    start = source.index("static void bcm4377_poll_completion_ring(")
    end = source.index("\n}", start) + 2
    with tempfile.TemporaryDirectory() as directory:
        binary = Path(directory) / "completion-test"
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                        "-x", "c", "-", "-o", binary], check=True,
                       input=FIXTURE + source[start:end] + "\n" + CASES, text=True)
        subprocess.run([binary], check=True, timeout=5)


if __name__ == "__main__":
    main()
