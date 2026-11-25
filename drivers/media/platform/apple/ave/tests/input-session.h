/* SPDX-License-Identifier: GPL-2.0-only */
/* Only the session state ave_sess_input() reads. */
struct ave_session {
	struct ave_params p;
	struct ave_slot slot[1];
	bool copy_noted;
};

struct ave_dev { struct device *dev; };
struct ave_ctx { struct ave_dev *ave; struct ave_session *sess; };
