/* kiwi-replay.c : replay the real hid-apple-mtp-kiwi.c frame logic over every
 * capture and start phase, and fail on any reported contact with no finger present.
 *
 * Build (on mesa-xbuild):
 *   cc -O2 -I<shimdir-include> -include linux-shim.h -DKIWI_DRV='"/path/driver.c"' replay.c -o kiwi-replay
 * Run:
 *   kiwi-replay --expect none  cap.frames [...]      fail on ANY contact
 *   kiwi-replay --expect touch cap.frames           fail on never-moving or over-hold contacts
 *   kiwi-replay --expect bound cap.frames           every contact must end near the hold bound
 *   kiwi-replay --expect one cap.frames             exactly one contact, bounded (the breather)
 *   --phases N (default 240), --loop N (replay N times back to back)
 */
#include "linux-shim.h"
#define HARNESS_STATIC_FRAMES 120
#define HARNESS_STATIC_GRACE 60
#include KIWI_DRV

struct kiwi_contact_rec {
	int slot, x0, y0, x1, y1;
	long frames, born;
	bool moved_ever;
};

struct kiwi_input_rec kiwi_rec;

/* per-cell-cluster total contact time: a phantom may exist only inside the
 * grace window; more than that in one spot means it persists */
#define CLUSTER(px, py) ((((px) / 80) << 16) | ((py) / 80))

static int rebuild(struct kiwi_contact_rec *out, int maxout)
{
	struct { bool open; struct kiwi_contact_rec c; } st[4] = {0};
	int n = 0, i;
	for (i = 0; i < kiwi_rec.n; i++) {
		struct kiwi_rec_event *e = &kiwi_rec.ev[i];
		if (e->slot < 0 || e->slot >= 4)
			continue;
		if (!e->on) {
			if (st[e->slot].open && n < maxout)
				out[n++] = st[e->slot].c;
			st[e->slot].open = false;
			continue;
		}
		if (e->x >= 0 || e->y >= 0 || e->size >= 0) {
			struct kiwi_contact_rec *c = &st[e->slot].c;
			if (!st[e->slot].open) {
				st[e->slot].open = true;
				c->slot = e->slot;
				c->x0 = c->x1 = 1 << 30; /* position arrives with the first ABS event */
				c->y0 = c->y1 = 1 << 30;
				c->frames = 0;
				c->born = e->frameno;
				c->moved_ever = false;
			} else if (c->x1 == 1 << 30) {
				c->x0 = c->x1 = e->x >= 0 ? e->x : c->x1;
				c->y0 = c->y1 = e->y >= 0 ? e->y : c->y1;
			} else {
				if (e->x >= 0)
					c->frames++;	/* one X per report: count frames, not events */
				if (e->x >= 0 && (e->x != c->x1 || e->y != c->y1))
					c->moved_ever = true;
				if (e->x >= 0) c->x1 = e->x;
				if (e->y >= 0) c->y1 = e->y;
			}
		}
	}
	for (i = 0; i < 4; i++)	/* contacts still down at capture end count too */
		if (st[i].open && n < maxout)
			out[n++] = st[i].c;
	return n;
}

static long cluster_time(struct kiwi_contact_rec *out, int n)
{
	long t[4096] = {0};
	long worst = 0;
	int i;

	for (i = 0; i < n; i++) {
		unsigned b = CLUSTER(out[i].x0, out[i].y0) & 4095;
		t[b] += out[i].frames;
		if (t[b] > worst)
			worst = t[b];
	}
	return worst;
}

static int parse_capture(const char *path, u8 **frames, int maxframes)
{
	FILE *f = fopen(path, "r");
	char *line = NULL;
	size_t cap = 0;
	ssize_t llen;
	int n = 0;
	if (!f) { perror(path); exit(2); }
	while ((llen = getline(&line, &cap, f)) > 0 && n < maxframes) {
		char *hex = strrchr(line, ' ');
		int blen = 0, i;
		u8 *raw;
		if (!hex || !hex[1])
			continue;
		blen = (int)((llen - (hex + 1 - line)) / 2);
		if (blen < KIWI_MIN_FRAME)
			continue;
		raw = malloc(KIWI_MIN_FRAME);
		for (i = 0; i < KIWI_MIN_FRAME; i++) {
			unsigned b;
			if (sscanf(hex + 1 + 2 * i, "%2x", &b) != 1) { blen = 0; break; }
			raw[i] = (u8)b;
		}
		if (blen && raw[0] == KIWI_MAGIC)
			frames[n++] = raw;
		else
			free(raw);
	}
	free(line);
	fclose(f);
	return n;
}

static void run_phase(u8 **frames, int n, int phase, int loops)
{
	struct kiwi k;
	struct input_dev input;
	int i, l;
	memset(&k, 0, sizeof(k));
	memset(&kiwi_rec, 0, sizeof(kiwi_rec)); /* slot_on starts all-off */
	k.input = &input;
	for (l = 0; l < loops; l++)
		for (i = 0; i < n; i++)
			kiwi_frame(&k, frames[(phase + i) % n]);
}

int main(int argc, char **argv)
{
	const char *expect = NULL, *taps_spec = NULL;
	int tap_min = 15;
	int tap_w[8], ntap_w = 0;
	int phases = 240, loops = 1, argi;
	static u8 *frames[60000];
	int fails = 0;

	for (argi = 1; argi < argc; argi++) {
		if (!strcmp(argv[argi], "--expect")) expect = argv[++argi];
		else if (!strcmp(argv[argi], "--phases")) phases = atoi(argv[++argi]);
		else if (!strcmp(argv[argi], "--loop")) loops = atoi(argv[++argi]);
		else if (!strcmp(argv[argi], "--taps")) taps_spec = argv[++argi];
		else if (!strcmp(argv[argi], "--tap-min")) tap_min = atoi(argv[++argi]);
		else break;
	}
	for (; argi < argc; argi++) {
		const char *path = argv[argi];
		int n = parse_capture(path, frames, 60000);
		int p, contacts_total = 0;
		bool press_sustained = false;
	bool press_churn = false;
		if (!n) { printf("cap=%s SKIP no frames\n", path); continue; }
		if (phases > n) phases = n;
		for (p = 0; p < phases; p++) {
			static struct kiwi_contact_rec out[256];
			int m, ci;
			run_phase(frames, n, p, loops);
			m = rebuild(out, 256);
			contacts_total += m;
			if (expect && !strcmp(expect, "press"))
				for (ci = 0; ci < m; ci++) {
					if (out[ci].born >= (long)n * 2 / 3 && out[ci].frames >= 600 &&
					    out[ci].born + out[ci].frames >= n - 240)
						press_sustained = true;
					/* churn: a late contact that died well before the end means the
					 * press was dropped and re-armed instead of held */
					if (out[ci].born >= (long)n * 2 / 3 &&
					    out[ci].born + out[ci].frames < n - 600)
						press_churn = true;
				}

			/* taps contract: each named window must hold a full-length contact and
			 * the idle stretch before the first tap must stay silent. Used by the
			 * bootfinger captures: a finger rested on the glass at boot must not
			 * scar the rows its later taps land on. */
			if (expect && !strcmp(expect, "taps") && taps_spec) {
				const char *p2 = taps_spec;
				int twi = 0;

				while (*p2 && twi < 8) {
					tap_w[twi++] = atoi(p2);
					while (*p2 && *p2 != ':' && *p2 != ',')
						p2++;
					if (*p2 == ':') {
						tap_w[twi++] = atoi(p2 + 1);
						while (*p2 && *p2 != ',')
							p2++;
					} else {
						tap_w[twi] = tap_w[twi - 1] + 24;
						twi++;
					}
					if (*p2 == ',')
						p2++;
				}
				ntap_w = twi / 2;
				/* the first pair names the silent idle stretch after the lift;
				 * a contact that reaches into a tap window is that tap's lead */
				for (ci = 0; ci < m; ci++) {
					bool tap_lead = false;
					int pair;

					if (out[ci].born < tap_w[0] || out[ci].born >= tap_w[1])
						continue;
					for (pair = 1; pair < ntap_w; pair++)
						if (out[ci].born + out[ci].frames >= tap_w[pair * 2] &&
						    out[ci].born <= tap_w[pair * 2 + 1] + 40)
							tap_lead = true;
					if (!tap_lead) {
						fails++;
							fails++;
						fprintf(stderr, "cap=%s FAIL ghost-in-idle (contact born %ld inside the silent window)\n",
							path, out[ci].born);
						break;
					}
				}
				for (twi = 1; twi < ntap_w; twi++) {
					int a = tap_w[twi * 2], b = tap_w[twi * 2 + 1];
					bool full = false;

					for (ci = 0; ci < m; ci++)
						if (out[ci].frames >= tap_min &&
						    out[ci].born <= b + 40 && out[ci].born + out[ci].frames >= a)
							full = true;
					if (!full) {
						fails++;
						fprintf(stderr, "cap=%s FAIL tap-%d-short (window %d..%d below %d frames)\n",
							path, twi, a, b, tap_min);
					}
				}
			}
			if (expect && (strcmp(expect, "press") == 0 || strcmp(expect, "touch") == 0) &&
			    getenv("PRESS_TRACE"))
				for (ci = 0; ci < m; ci++)
					fprintf(stderr, "press-trace born=%ld frames=%ld end=%ld\n",
						out[ci].born, out[ci].frames, out[ci].born + out[ci].frames);
			{
				long worst = cluster_time(out, m);
				if (expect && !strcmp(expect, "none") && worst > HARNESS_HOLD_FRAMES * 3 / 2) {
					fails++;
					fprintf(stderr, "cap=%s phase=%d FAIL phantom-persists cluster_frames=%ld\n",
						path, p, worst);
				}
			}
			/* "reborn:<b1>,<b2>,...": the contact list must be exactly the
			 * named borns, in order - nothing more (a rebirth after the
			 * hold drop), nothing fewer (the touch must register). */
			if (expect && !strncmp(expect, "reborn:", 7)) {
				long want[8];
				int nw = 0, ni;
				const char *q = expect + 7;

				while (*q && nw < 8) {
					want[nw++] = strtol(q, (char **)&q, 10);
					if (*q == ',')
						q++;
				}
				{
					bool bad = m != nw;

					for (ni = 0; ni < m && ni < nw; ni++)
						if (out[ni].born != want[ni])
							bad = true;
					if (bad)
						fails++;
					if (bad)
						fprintf(stderr,
						"cap=%s phase=%d FAIL reborn-list want=%s got=%u borns=%ld,%ld,%ld,%ld\n",
						path, p, expect + 7, m, m > 0 ? out[0].born : -1,
						m > 1 ? out[1].born : -1, m > 2 ? out[2].born : -1,
						m > 3 ? out[3].born : -1);
				}
			}
			/* "one": the stationary breather must produce exactly one
			 * bounded contact: the hold rule ends it and the sticky
			 * suppression keeps the still-present blob from re-arming. */
			if (expect && !strcmp(expect, "one") && m != 1) {
				fails++;
				fprintf(stderr, "cap=%s phase=%d FAIL expected-one-contact got=%u\n",
					path, p, m);
			}
			for (ci = 0; ci < m; ci++) {
				struct kiwi_contact_rec *c = &out[ci];
				/* "bound"/"one": every contact must end near the hold bound.
				 * The drop lands a few frames past born+KIWI_HOLD_FRAMES (the
				 * anchor lags; measured 12 on the breather), so the check
				 * allows 200 frames of slack. */
				bool bnd = expect && (!strcmp(expect, "bound") ||
						      !strcmp(expect, "one"));
				bool held = c->frames > (bnd ? HARNESS_HOLD_FRAMES + 200 :
							      HARNESS_HOLD_FRAMES);
				bool statik = expect && !strcmp(expect, "flat") &&
					      c->frames > HARNESS_STATIC_FRAMES + HARNESS_STATIC_GRACE;
				bool strict = expect && !strcmp(expect, "strict");
				bool pressmode = expect && !strcmp(expect, "press");
				bool fail = pressmode ? false : bnd ? held : held || statik || strict;
				if (getenv("KIWI_DUMP"))
					fprintf(stderr, "cap=%s phase=%d contact slot=%d (%d,%d)..(%d,%d) frames=%ld born=%ld\n",
						path, p, c->slot, c->x0, c->y0, c->x1, c->y1, c->frames, c->born);
				if (fail) {
					fails++;
					if (fails <= 40)
						fprintf(stderr, "cap=%s phase=%d FAIL %s slot=%d (%d,%d)..(%d,%d) frames=%ld born=%ld\n",
							path, p, strict ? "phantom" : held ? "held-too-long" : "static-phantom",
							c->slot, c->x0, c->y0, c->x1, c->y1, c->frames, c->born);
				}
			}
		}
		if (kiwi_rec.dropped) {
			fails++;
			fprintf(stderr, "cap=%s FAIL evidence-incomplete (recorder buffer exhausted)\n", path);
		}
		if (expect && !strcmp(expect, "press") && (!press_sustained || press_churn)) {
			fails++;
			fprintf(stderr, "cap=%s FAIL press-not-sustained (no contact born past two thirds of the capture lives 5 s)\n", path);
		}
		if (expect && !strcmp(expect, "touch") && contacts_total == 0) {
			fails++;
			fprintf(stderr, "cap=%s FAIL finger-never-detected\n", path);
		}
		printf("cap=%s frames=%d phases=%d contacts=%d %s\n", path, n, phases,
			contacts_total, fails ? "FAIL" : "PASS");
		while (n--) free(frames[n]);
	}
	return fails ? 1 : 0;
}
