/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __APPLE_DCP_AFK_TICKET_H__
#define __APPLE_DCP_AFK_TICKET_H__

#include <linux/types.h>

/*
 * What became of a V2 command whose sender follows it past its timeout
 * (afk_service_call_ticket()). Sending it makes the ticket SENT. If it
 * times out, its slot is kept for its late reply (LATE), and that reply
 * makes it DONE, or REFUSED for a nonzero return code. An answer in time,
 * or a failure to send, is returned by the call as usual, and the ticket
 * then tells nothing. The ticket follows one command at a time: sending
 * another with it numbers it anew (@seq), and the late reply of an earlier
 * command leaves it alone. The transport writes it under the lock of the
 * command's service.
 */
enum afk_ticket_state {
	AFK_TICKET_IDLE,
	AFK_TICKET_SENT,
	AFK_TICKET_LATE,
	AFK_TICKET_DONE,
	AFK_TICKET_REFUSED,
};

struct afk_ticket {
	enum afk_ticket_state state;
	u32 seq;
};

#endif
