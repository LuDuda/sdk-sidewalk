/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/*
 * "mac" command group of the Sidewalk DUT CLI.
 *
 * The commands (mac gcfg, scfg) are used by the Sidewalk compliance test procedures to
 * read/modify the MAC level configuration of the end device. The behaviour follows the reference
 * sid_config_cli (MAC v2) implementation, adapted to the Zephyr shell.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#include <sid_api.h>
#include <sid_900_cfg.h>
#include <sid_error.h>
#include <sid_sdk_config.h>
#include <sid_pal_storage_kv_ifc.h>
#include <cli/mac_shell.h>

#define MAC_ID_SIZE 5
#define MAC_KEY_SIZE 16
#define MAC_MAX_HEX_STR_LEN (2 * MAC_KEY_SIZE)

#define MAC_SUBGHZ_SUPPORTED IS_ENABLED(CONFIG_SIDEWALK_SUBGHZ_SUPPORT)

#define CHECK_ARGUMENT_COUNT(argc, required, optional)                                             \
	if ((argc < required) || (argc > (required + optional))) {                                 \
		return -EINVAL;                                                                    \
	}

/*
 * KV group used by the prebuilt Sidewalk library for the protocol records.
 * It has to match STORAGE_KV_INTERNAL_PROTOCOL_GROUP_ID in sid_storage.c (0),
 * not the 0x2000 default from sid_pal_storage_kv_internal_group_ids.h.
 */
#define MAC_KV_PROTOCOL_GROUP 0

enum mac_kv_key {
	MAC_KV_PAN_ID = 3,
	MAC_KV_P2P_BROADCAST_KEY = 4,
	MAC_KV_GROUP_NUM = 8,
	MAC_KV_LDR_CHANNEL = 21,
	MAC_KV_WAN_MASTER_KEY = 28,
};

#define MAC_DEFAULT_PAN_ID                                                                         \
	{                                                                                          \
		0xA0, 0x00, 0x00, 0x02, 0x00                                                       \
	}
#define MAC_DEFAULT_LDR_CHANNEL 26
#define MAC_DEFAULT_MAX_BCN_MISS 3

#define MAC_ADDRESS_SIZE_MAX 7
#define MAC_ADDRESS_TYPE_DEVICE 1
struct mac_sid_address {
	uint8_t type;
	uint8_t addr[MAC_ADDRESS_SIZE_MAX];
};

struct mac_fsk_slot_params {
	uint8_t ul_periodicity;
	uint8_t ul_offset;
	uint8_t dl_periodicity;
	uint8_t dl_offset;
};

enum mac_lora_ll_threshold_idx {
	MAC_LORA_LL_THRESHOLD_MIN_IDX = 1,
	MAC_LORA_LL_THRESHOLD_MAX_IDX = 7,
};

struct mac_lora_ll_params {
	bool enabled;
	uint8_t repetition;
	uint8_t rate_limit;
	enum mac_lora_ll_threshold_idx latency_threshold_idx;
};

enum mac_reg_region_code {
	MAC_REG_RC_NONE = 0,
	MAC_REG_RC_NA = 1,
	MAC_REG_RC_EU = 2,
	MAC_REG_RC_JP = 3,
	MAC_REG_RC_MAX,
};

struct mac_reg_regional {
	enum mac_reg_region_code region;
	char iso_country[3];
};

/* Internal Sidewalk stack symbols provided by the Sidewalk libraries. */
extern bool sid_state_is_device_registered(void);
extern struct mac_sid_address sid_address_get_local(void);

#if MAC_SUBGHZ_SUPPORTED
extern struct mac_reg_regional *sid_reg_get_regional_info(void);
extern bool sid_network_link_lora_is_started(void);
extern struct sid_device_profile *sid_network_link_lora_get_profile(void);
extern sid_error_t sid_network_link_lora_set_channel(uint16_t channel);
extern sid_error_t sid_network_link_get_low_latency_mac_params(void *params);
extern bool sid_network_link_fsk_is_started(void);
extern struct sid_device_profile *sid_network_link_fsk_get_profile(void);
extern sid_error_t sid_network_link_fsk_get_fsk_slot_params(struct mac_fsk_slot_params *params);
extern sid_error_t sid_network_link_fsk_connect_prefer_gw(struct mac_sid_address gw_addr,
							  bool connect);
extern sid_error_t sid_network_link_fsk_bcn_miss_params(uint8_t *max_bcn_miss, bool set);
#endif /* MAC_SUBGHZ_SUPPORTED */

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_mac_services,
	SHELL_CMD_ARG(gcfg, NULL, CMD_MAC_GCFG_DESCRIPTION, cmd_mac_gcfg, CMD_MAC_GCFG_ARG_REQUIRED,
		      CMD_MAC_GCFG_ARG_OPTIONAL),
	SHELL_CMD_ARG(scfg, NULL, CMD_MAC_SCFG_DESCRIPTION, cmd_mac_scfg, CMD_MAC_SCFG_ARG_REQUIRED,
		      CMD_MAC_SCFG_ARG_OPTIONAL),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(mac, &sub_mac_services, "Sidewalk MAC config CLI", NULL);

/* utils */

#if MAC_SUBGHZ_SUPPORTED
static bool mac_parse_ulong(const char *str, unsigned long max, unsigned long *out)
{
	char *end = NULL;

	if (str == NULL || *str == '\0') {
		return false;
	}

	/* Same as the reference utils_parse_input_num(): decimal unless 0x/x prefix. */
	int base = 10;

	if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
		base = 16;
		str += 2;
	} else if (str[0] == 'x' || str[0] == 'X') {
		base = 16;
		str += 1;
	}

	if (*str == '\0') {
		return false;
	}

	unsigned long val = strtoul(str, &end, base);

	if (*end != '\0' || val > max) {
		return false;
	}

	*out = val;
	return true;
}

static int mac_parse_hex(const char *str, uint8_t *out, size_t out_size)
{
	if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
		str += 2;
	}

	size_t str_len = strlen(str);

	if (str_len == 0 || (str_len % 2) != 0 || str_len > MAC_MAX_HEX_STR_LEN ||
	    (str_len / 2) > out_size) {
		return -EINVAL;
	}

	if (hex2bin(str, str_len, out, out_size) != (str_len / 2)) {
		return -EINVAL;
	}

	return (int)(str_len / 2);
}
#endif /* MAC_SUBGHZ_SUPPORTED */

static void mac_hex_str(char *out, const uint8_t *data, size_t len)
{
	static const char hex[] = "0123456789ABCDEF";

	for (size_t i = 0; i < len; i++) {
		out[2 * i] = hex[data[i] >> 4];
		out[2 * i + 1] = hex[data[i] & 0x0f];
	}
	out[2 * len] = '\0';
}

static void mac_print_key(const struct shell *shell, const char *name, uint16_t kv_key)
{
	uint8_t key[MAC_KEY_SIZE];
	char key_str[2 * MAC_KEY_SIZE + 1];

	if (sid_pal_storage_kv_record_get(MAC_KV_PROTOCOL_GROUP, kv_key,
					  key, sizeof(key)) != SID_ERROR_NONE) {
		shell_error(shell, "%s key read failed", name);
		return;
	}

#ifdef CONFIG_SIDEWALK_CRYPTO_PSA_KEY_STORAGE
	/* With PSA key storage the KV read returns a key ID, not the key material. */
	shell_print(shell, "%s Master Key stored in PSA (not readable)", name);
	ARG_UNUSED(key_str);
#else
	mac_hex_str(key_str, key, sizeof(key));
	shell_print(shell, "%s Master Key = 0x%s", name, key_str);
#endif
}

#if MAC_SUBGHZ_SUPPORTED
static void mac_print_lora_cfg(const struct shell *shell)
{
	uint16_t ldr_channel = MAC_DEFAULT_LDR_CHANNEL;

	/* The record may not exist yet, then the default is printed. */
	(void)sid_pal_storage_kv_record_get(MAC_KV_PROTOCOL_GROUP,
					    MAC_KV_LDR_CHANNEL, &ldr_channel, sizeof(ldr_channel));
	shell_print(shell, "LDRChannel = %d", ldr_channel);

	struct sid_device_profile *profile = sid_network_link_lora_get_profile();

	if (profile == NULL) {
		shell_error(shell, "LoRa device profile not available");
		return;
	}

	shell_print(shell, "Async Device Profile = %d", profile->unicast_params.device_profile_id);
	shell_print(shell, "Async LDR Periodicity = %d",
		    profile->unicast_params.unicast_window_interval.async_rx_interval_ms);
	shell_print(shell, "Async RX Count = %d", profile->unicast_params.rx_window_count);

	struct mac_lora_ll_params ll_params = {0};

	if (sid_network_link_get_low_latency_mac_params(&ll_params) == SID_ERROR_NONE) {
		shell_print(shell, "Low Latency enabled %d", ll_params.enabled);
		shell_print(shell, "Low Latency latency_threshold_idx %d",
			    ll_params.latency_threshold_idx);
		shell_print(shell, "Low Latency rate_limit %d", ll_params.rate_limit);
		shell_print(shell, "Low Latency repetition %d", ll_params.repetition);
	}
}

static void mac_print_fsk_cfg(const struct shell *shell)
{
	struct mac_fsk_slot_params slot_params = {0};
	struct sid_device_profile *profile = sid_network_link_fsk_get_profile();

	(void)sid_network_link_fsk_get_fsk_slot_params(&slot_params);
	shell_print(shell, "Unicast TX Periodicity = %d", slot_params.ul_periodicity);
	shell_print(shell, "Unicast TX Offset = %d", slot_params.ul_offset);
	shell_print(shell, "Unicast RX Periodicity = %d", slot_params.dl_periodicity);
	shell_print(shell, "Unicast RX Offset = %d", slot_params.dl_offset);

	if (profile == NULL) {
		shell_error(shell, "FSK device profile not available");
		return;
	}

	shell_print(shell, "Unicast TX Enabled = %d",
		    (profile->unicast_params.wakeup_type & SID_TX_ONLY_WAKEUP) ? 1 : 0);
	shell_print(shell, "Unicast RX Enabled = %d",
		    (profile->unicast_params.wakeup_type & SID_RX_ONLY_WAKEUP) ? 1 : 0);
}
#endif /* MAC_SUBGHZ_SUPPORTED */

int cmd_mac_gcfg(const struct shell *shell, int32_t argc, const char **argv)
{
	CHECK_ARGUMENT_COUNT(argc, CMD_MAC_GCFG_ARG_REQUIRED, CMD_MAC_GCFG_ARG_OPTIONAL);

	char id_str[2 * MAC_ID_SIZE + 1];
	uint8_t group_num = 0;
	uint8_t pan_id[MAC_ID_SIZE] = MAC_DEFAULT_PAN_ID;

	/* The records may not exist yet, then the defaults are printed. */
	(void)sid_pal_storage_kv_record_get(MAC_KV_PROTOCOL_GROUP,
					    MAC_KV_GROUP_NUM, &group_num, sizeof(group_num));
	shell_print(shell, "GroupNum = %d", group_num);

	(void)sid_pal_storage_kv_record_get(MAC_KV_PROTOCOL_GROUP,
					    MAC_KV_PAN_ID, pan_id, sizeof(pan_id));
	mac_hex_str(id_str, pan_id, sizeof(pan_id));
	shell_print(shell, "PanID = 0x%s", id_str);

	struct mac_sid_address local_addr = sid_address_get_local();

	mac_hex_str(id_str, local_addr.addr, MAC_ID_SIZE);
	shell_print(shell, "DevID = 0x%s", id_str);

	mac_print_key(shell, "PAN", MAC_KV_P2P_BROADCAST_KEY);
	mac_print_key(shell, "WAN", MAC_KV_WAN_MASTER_KEY);

	const int8_t rf_power[] = SID_SDK_CONFIG_RF_POWER_TABLE;

	for (size_t i = 0; i < ARRAY_SIZE(rf_power); i++) {
		shell_print(shell, "DR %d PWR %d", (int)(i + 1), rf_power[i]);
	}

#if MAC_SUBGHZ_SUPPORTED
	uint8_t max_bcn_miss = MAC_DEFAULT_MAX_BCN_MISS;

	(void)sid_network_link_fsk_bcn_miss_params(&max_bcn_miss, false);
	shell_print(shell, "MaxBeaconMiss = %d", max_bcn_miss);
#endif /* MAC_SUBGHZ_SUPPORTED */

	shell_print(shell, "PairingStateStatus = %d", sid_state_is_device_registered() ? 1 : 0);

#if MAC_SUBGHZ_SUPPORTED
	if (sid_network_link_lora_is_started()) {
		mac_print_lora_cfg(shell);
	}

	if (sid_network_link_fsk_is_started()) {
		mac_print_fsk_cfg(shell);
	}

	struct mac_reg_regional *regional = sid_reg_get_regional_info();

	if (regional != NULL) {
		shell_print(shell, "Region = %d", regional->region);
	}
#endif /* MAC_SUBGHZ_SUPPORTED */

	return 0;
}

/* mac scfg */

#if MAC_SUBGHZ_SUPPORTED
static int mac_scfg_ldr_channel(const struct shell *shell, const char *val)
{
	unsigned long channel;

	if (!mac_parse_ulong(val, UINT16_MAX, &channel)) {
		shell_error(shell, "Invalid LDR channel [%s]", val);
		return -EINVAL;
	}

	sid_error_t err = sid_network_link_lora_set_channel((uint16_t)channel);

	if (err != SID_ERROR_NONE) {
		shell_error(shell, "Failed to set LDR channel, err %d", err);
		return -EIO;
	}
	return 0;
}

static int mac_scfg_force_connect_gw(const struct shell *shell, int32_t argc, const char **argv)
{
	unsigned long connect;
	struct mac_sid_address gw_addr = {
		.type = MAC_ADDRESS_TYPE_DEVICE,
	};
	int consumed = 1;

	if (argc < 1 || !mac_parse_ulong(argv[0], 1, &connect)) {
		shell_error(shell, "Invalid connect flag, use 0 or 1");
		return -EINVAL;
	}

	if (connect) {
		if (argc < 2 || mac_parse_hex(argv[1], gw_addr.addr, MAC_ID_SIZE) != MAC_ID_SIZE) {
			shell_error(shell, "Gateway ID format is not valid");
			return -EINVAL;
		}
		consumed = 2;
	}

	sid_error_t err = sid_network_link_fsk_connect_prefer_gw(gw_addr, connect != 0);

	if (err != SID_ERROR_NONE) {
		shell_error(shell, "Failed to %s preferred gateway, err %d",
			    connect ? "connect to" : "disconnect from", err);
		return -EIO;
	}

	shell_print(shell, "SetCfg connect prefer gw status %d", err);
	return consumed;
}
#endif /* MAC_SUBGHZ_SUPPORTED */

int cmd_mac_scfg(const struct shell *shell, int32_t argc, const char **argv)
{
	CHECK_ARGUMENT_COUNT(argc, CMD_MAC_SCFG_ARG_REQUIRED, CMD_MAC_SCFG_ARG_OPTIONAL);

#if MAC_SUBGHZ_SUPPORTED
	int32_t idx = 1;

	while (idx < argc) {
		const char *flag = argv[idx++];
		int ret;

		if (flag[0] == '\0' || flag[1] != '\0') {
			shell_error(shell, "Unknown flag [%s]", flag);
			return -EINVAL;
		}

		/* Every flag is followed by at least one value. */
		if (idx >= argc) {
			shell_error(shell, "Missing value for flag %s", flag);
			return -EINVAL;
		}

		switch (flag[0]) {
		case 'R':
			ret = mac_scfg_ldr_channel(shell, argv[idx++]);
			break;
		case 'C':
			ret = mac_scfg_force_connect_gw(shell, argc - idx, &argv[idx]);
			if (ret > 0) {
				idx += ret;
				ret = 0;
			}
			break;
		default:
			shell_error(shell, "Unknown flag [%s]", flag);
			return -EINVAL;
		}

		if (ret < 0) {
			shell_error(shell, "Failed to set cfg parameter %s", flag);
			return ret;
		}
	}

	shell_print(shell, "Successfully set cfg parameters.");
	return 0;
#else
	shell_error(shell, "Sub-GHz links are not supported in this build");
	return -ENOTSUP;
#endif
}
