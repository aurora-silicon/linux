/* SPDX-License-Identifier: ISC */
#ifndef BRCMF_SCAN_TIMEOUT_H
#define BRCMF_SCAN_TIMEOUT_H

#include <linux/minmax.h>
#include <net/cfg80211.h>

#define BRCMF_SCAN_PASSIVE_TIME		120
#define BRCMF_ESCAN_TIMER_INTERVAL_MS	10000
#define BRCMF_ESCAN_TIMER_MAX_MS		60000

static inline unsigned int
brcmf_scan_timeout_ms(struct wiphy *wiphy,
		      const struct cfg80211_scan_request *request)
{
	u64 n_channels = request->n_channels;
	u64 timeout;
	int band;

	/* An empty firmware channel list means all supported channels. */
	if (!n_channels) {
		for (band = 0; band < NUM_NL80211_BANDS; band++) {
			if (wiphy->bands[band])
				n_channels += wiphy->bands[band]->n_channels;
		}
	}

	/*
	 * The regular scan builders leave dwell times at -1, using the defaults
	 * set by brcmf_dongle_scantime(). They do not implement request->duration.
	 * Even an active request may scan channels passively, so budget every
	 * channel at the passive dwell. Allow another dwell per channel for
	 * switching/home-channel visits, plus one second for firmware overhead.
	 * Keep the old minimum and bound recovery time for a stuck firmware scan.
	 */
	timeout = n_channels * BRCMF_SCAN_PASSIVE_TIME * 2 + 1000;
	return clamp_t(u64, timeout, BRCMF_ESCAN_TIMER_INTERVAL_MS,
		       BRCMF_ESCAN_TIMER_MAX_MS);
}

#endif /* BRCMF_SCAN_TIMEOUT_H */
