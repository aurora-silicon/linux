// SPDX-License-Identifier: GPL-2.0-only OR MIT

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static inline void put_unaligned_le32(uint32_t v, void *p) {
    unsigned char *b = p;
    for (unsigned i = 0; i < 4; i++) b[i] = (unsigned char)(v >> (8 * i));
}
#include "gpu-m4-pstate.h"
int main(void) {
    struct gpu_m4_thermal t;
    char line[512];
    gpu_m4_thermal_init(&t);
    while (fgets(line, sizeof(line), stdin)) {
        char op;
        unsigned long long now, query;
        long mc;
        int error;
        unsigned governor, power, configured, cap, user;
        if (sscanf(line, "%c %llu %ld %d %u %u %u %u %u %llu", &op, &now,
                   &mc, &error, &governor, &power, &configured, &cap, &user, &query) != 10)
            return 2;
        bool due_before = gpu_m4_thermal_due(&t, now);
        u32 ceiling = 0;
        const char *reason = "none";
        switch (op) {
        case 'R': gpu_m4_thermal_init(&t); break;
        case 'S': gpu_m4_thermal_sample(&t, now, error, mc, 0); break;
        case 'O': apple_pmp_cooling_observe(&t.policy, now, governor); break;
        // The C input contract says its optional power provider is preclamped.
        case 'C': gpu_m4_thermal_ceiling(&t, governor, power < 8 ? power : 8, &ceiling, &reason); break;
        case 'D': case 'L': case 'H': break;
        default: return 3;
        }
        const struct apple_pmp_cooling *s = &t.policy;
        printf("%llu %llu %llu %llu %llu %ld %ld %ld %u %u %u %u %u %s %u %u %u %s %u %u\n",
               s->last_ns, s->good_ns, s->anchor_ns, s->release_ns, s->dt_ns,
               s->anchor_mc, s->previous_mc, s->projected_mc, s->good_reads,
               s->prediction, s->level, (unsigned)s->hard, (unsigned)s->anchor_valid,
               gpu_m4_thermal_reason(&t), (unsigned)due_before,
               (unsigned)gpu_m4_thermal_due(&t, query), ceiling, reason,
               gpu_m4_pstate_limit(configured, cap, user), apple_pmp_heat_level(mc));
    }
    return ferror(stdin) || ferror(stdout);
}
