#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/drivers/can.h>

#include <stdlib.h>
#include <string.h>

#include "app.h"

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	static struct bm_proto s;

	app_proto_snapshot(&s);
	const struct bm_pack_state *st = &s.st;

	shell_print(sh, "link %s, %u frames, can state %d, dropped %u, tx err %u",
		    s.link_up ? "up" : "down", (unsigned int)s.frames_total, app.can_state,
		    (unsigned int)app.can_rx_dropped, (unsigned int)app.can_tx_errors);
	shell_print(sh, "proto: %s / %s / \"%s\" mfr=\"%s\" conf=%u hints=0x%03x",
		    bm_family_name(s.det.family), bm_vendor_name(s.det.vendor), s.det.flavor,
		    st->manufacturer, s.det.confidence, (unsigned int)s.det.hints);
	shell_print(sh, "inverter: %s ident=\"%s\" heartbeats=%u", s.inv.seen ? "seen" : "none",
		    s.inv.ident, (unsigned int)s.inv.hb_count);
	shell_print(sh, "pack: %d mV  %d mA  %d.%d C  SoC %u%%  SoH %u%%  modules %u",
		    (int)st->pack_mv, (int)st->pack_ma, (int)st->temp_dc / 10,
		    (int)(st->temp_dc < 0 ? -st->temp_dc : st->temp_dc) % 10, st->soc_pct,
		    st->soh_pct, st->module_count);
	shell_print(sh, "limits: CVL %d mV  DVL %d mV  CCL %d mA  DCL %d mA", (int)st->cvl_mv,
		    (int)st->dvl_mv, (int)st->ccl_ma, (int)st->dcl_ma);
	shell_print(sh, "flags: chg %d dis %d force %d/%d full %d  prot %02x%02x alarm %02x%02x",
		    st->charge_en, st->discharge_en, st->force_charge_1, st->force_charge_2,
		    st->full_charge_req, st->prot[0], st->prot[1], st->alarm[0], st->alarm[1]);
	shell_print(sh, "net: %s %s  mqtt %s  published %u  errors %u", app.net_up ? "up" : "down",
		    app.ip, app.mqtt_connected ? "connected" : "offline",
		    (unsigned int)app.mqtt_pub_count, (unsigned int)app.mqtt_errors);
	return 0;
}

static int cmd_ids(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	static struct bm_proto s;

	app_proto_snapshot(&s);
	uint32_t now = app_uptime_ms();

	for (unsigned int i = 0; i < s.n_ids; i++) {
		const struct bm_id_stat *x = &s.ids[i];

		shell_print(sh, "%s%03x  dlc %u  n=%u  age %u ms", x->ext ? "x" : " ",
			    (unsigned int)x->id, x->dlc, (unsigned int)x->count,
			    (unsigned int)(now - x->last_ms));
	}
	return 0;
}

static int cmd_json(const struct shell *sh, size_t argc, char **argv)
{
	static char buf[APP_JSON_MAX];
	bool flat = argc > 1 && strcmp(argv[1], "flat") == 0;
	int n = app_json(buf, sizeof(buf), flat);

	if (n < 0) {
		shell_error(sh, "buffer too small");
		return -ENOMEM;
	}
	shell_print(sh, "%s", buf);
	return 0;
}

static int cmd_raw(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(sh, "raw log %s", app.raw_log ? "on" : "off");
		return 0;
	}
	app.raw_log = strcmp(argv[1], "on") == 0;
	shell_print(sh, "raw log %s", app.raw_log ? "on" : "off");
	return 0;
}

static int cmd_hb(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(sh, "bench heartbeat %s", app.bench_heartbeat ? "on" : "off");
		return 0;
	}
	int rc = can_rx_set_heartbeat(strcmp(argv[1], "on") == 0);

	if (rc == -EACCES) {
		shell_error(sh, "not available: built with BATMON_CAN_LISTEN_ONLY");
		return rc;
	}
	shell_print(sh, "bench heartbeat %s", app.bench_heartbeat ? "on" : "off");
	return 0;
}

static int cmd_reset(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	k_mutex_lock(&app.lock, K_FOREVER);
	bm_proto_init(&app.proto);
	k_mutex_unlock(&app.lock);
	shell_print(sh, "protocol state cleared");
	return 0;
}

#ifdef CONFIG_BATMON_MQTT
static int cmd_publish(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	net_mqtt_publish_now();
	shell_print(sh, "publish requested");
	return 0;
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(sub_batmon,
	SHELL_CMD(status, NULL, "Decoded battery state and link status", cmd_status),
	SHELL_CMD(ids, NULL, "CAN identifiers seen so far", cmd_ids),
	SHELL_CMD_ARG(json, NULL, "Telemetry JSON [flat]", cmd_json, 1, 1),
	SHELL_CMD_ARG(raw, NULL, "Raw frame logging on|off", cmd_raw, 1, 1),
	SHELL_CMD_ARG(hb, NULL, "Bench heartbeat 0x305 on|off", cmd_hb, 1, 1),
	SHELL_CMD(reset, NULL, "Clear decoded state and detection", cmd_reset),
#ifdef CONFIG_BATMON_MQTT
	SHELL_CMD(publish, NULL, "Publish telemetry now", cmd_publish),
#endif
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(batmon, &sub_batmon, "batmon battery monitor", NULL);
