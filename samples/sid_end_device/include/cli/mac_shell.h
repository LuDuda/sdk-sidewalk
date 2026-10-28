/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef MAC_SHELL_H
#define MAC_SHELL_H

#include <stdint.h>
#include <zephyr/shell/shell.h>

#define CMD_MAC_GCFG_DESCRIPTION                                                                   \
	"\n"                                                                                       \
	"Get MAC config parameters (group, PAN/device ID, keys, registration state, link params)."

#define CMD_MAC_SCFG_DESCRIPTION                                                                   \
	"<flag> <value> [<flag> <value> ...]\n"                                                    \
	"Set MAC config parameters.\n"                                                             \
	"  R <channel> - LDR channel, e.g. mac scfg R 0\n"                                         \
	"  C 1 <gw_id> - force FSK end node to sync to the given gateway\n"                        \
	"  C 0         - stop forcing FSK end node to a gateway"

#define CMD_MAC_GCFG_ARG_REQUIRED 1
#define CMD_MAC_GCFG_ARG_OPTIONAL 0
#define CMD_MAC_SCFG_ARG_REQUIRED 3
#define CMD_MAC_SCFG_ARG_OPTIONAL 3

int cmd_mac_gcfg(const struct shell *shell, int32_t argc, const char **argv);
int cmd_mac_scfg(const struct shell *shell, int32_t argc, const char **argv);

#endif /* MAC_SHELL_H */
