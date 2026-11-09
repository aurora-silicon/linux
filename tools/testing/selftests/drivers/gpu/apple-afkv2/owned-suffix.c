// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Aurora Silicon */
int main(void)
{
	u8 rx[16], tx[16], output[16];
	struct apple_dcp dcp = { .fw_compat = DCP_FIRMWARE_UNKNOWN };
	struct apple_dcp_afkep ep = { .wire_v2 = true, .dcp = &dcp };
	struct apple_epic_service service = { .ep = &ep };
	struct epic_cmd_info *cmd = &service.cmds[0];

	memset(rx, 0x12, sizeof(rx));
	memset(tx, 0x34, sizeof(tx));
	memset(output, 0x56, sizeof(output));
	cmd->rxbuf = rx;
	cmd->rxbuf_dma = 0x10000001000ULL;
	cmd->rxlen = sizeof(rx);
	cmd->txbuf = tx;
	cmd->txbuf_dma = 0x10000002000ULL;
	cmd->txlen = sizeof(tx);
	assert(afk_v2_copy_owned(&service, cmd->rxbuf_dma, output, 4, false) ==
	       -EACCES);
	service.cmd_map[0] = 1;
	assert(afk_v2_copy_owned(&service, cmd->rxbuf_dma + 12, output, 4,
				 false) == 0);
	assert(output[0] == 0x12 && output[3] == 0x12 && output[4] == 0x56);
	assert(afk_v2_copy_owned(&service, cmd->txbuf_dma + 4, output, 4,
				 false) == 0);
	assert(output[0] == 0x34 && output[4] == 0x56);
	memset(output, 0x78, 4);
	assert(afk_v2_copy_owned(&service, cmd->rxbuf_dma + 8, output, 4,
				 true) == 0);
	assert(rx[7] == 0x12 && rx[8] == 0x78 && rx[11] == 0x78 &&
	       rx[12] == 0x12);
	assert(afk_v2_copy_owned(&service, cmd->rxbuf_dma - 1, output, 1,
				 true) == -EACCES);
	assert(afk_v2_copy_owned(&service, cmd->rxbuf_dma + 13, output, 4,
				 true) == -EACCES);
	assert(afk_v2_copy_owned(&service, cmd->txbuf_dma + 16, output, 1,
				 true) == -EACCES);
	cmd->done = true;
	assert(afk_v2_copy_owned(&service, cmd->rxbuf_dma, output, 1, false) ==
	       -EACCES);
	cmd->done = false;
	ep.wire_v2 = false;
	assert(afk_v2_copy_owned(&service, cmd->rxbuf_dma, output, 1, false) ==
	       -EINVAL);
	ep.wire_v2 = true;
	assert(afk_v2_copy_owned(&service, cmd->rxbuf_dma, NULL, 1, false) ==
	       -EINVAL);
	assert(afk_v2_copy_owned(&service, 0, NULL, 0, false) == 0);
	assert(afk_v2_access_owned(&service, cmd->rxbuf_dma + 1, NULL, 15, true,
				   false) == 0);
	assert(afk_v2_access_owned(&service, cmd->rxbuf_dma + 1, NULL, 16, true,
				   false) == -EACCES);
	/* Only firmware 27 falls back to its retained mappings, and only when unowned. */
	assert(remote_calls == 0);
	dcp.fw_compat = DCP_FIRMWARE_V_27_0;
	assert(afk_v2_copy_owned(&service, cmd->rxbuf_dma + 12, output, 4,
				 false) == 0);
	assert(remote_calls == 0);
	assert(afk_v2_copy_owned(&service, 0x20000003000ULL, output, 4, true) ==
	       -EACCES);
	assert(remote_calls == 1 && remote_dva == 0x20000003000ULL &&
	       remote_size == 4 && remote_to_remote && remote_copy);
	remote_ret = 0;
	assert(afk_v2_access_owned(&service, 0x20000003000ULL, NULL, 8, false,
				   false) == 0);
	assert(remote_calls == 2 && remote_size == 8 && !remote_to_remote &&
	       !remote_copy);
	assert(afk_v2_copy_owned(&service, 0x20000003000ULL, NULL, 1, false) ==
	       -EINVAL);
	assert(remote_calls == 2);
	puts("PASS owned-copy boundaries and firmware-27 fallback (actual extracted function; lock/barrier and fallback mocks)");
	return 0;
}
