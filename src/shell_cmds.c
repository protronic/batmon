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
	if (app.rs485_present) {
		shell_print(sh, "rs485: %s (%s, link %s)", app.serial.flavor_str,
			    bm_serial_phase_name(app.serial.phase), app.serial.link_up ? "up" : "down");
	}
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

#ifdef CONFIG_BATMON_RS485
static void print_fixed(char *out, size_t len, int32_t v, uint32_t div, int digits)
{
	bool neg = v < 0;
	uint32_t u = neg ? (uint32_t)(-(int64_t)v) : (uint32_t)v;

	snprintf(out, len, "%s%u.%0*u", neg ? "-" : "", (unsigned int)(u / div), digits,
		 (unsigned int)(u % div));
}

static int cmd_rs_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	static struct bm_serial s;
	char a[16], b[16], c[16], d[16];

	app_serial_snapshot(&s);
	const struct bm_serial_state *st = &s.st;

	shell_print(sh, "rs485: %s", s.flavor_str);
	shell_print(sh, "phase %s  link %s  baud %u  ver %02x  addr %u", bm_serial_phase_name(s.phase),
		    s.link_up ? "up" : "down", (unsigned int)s.baud, s.ver, s.adr);
	shell_print(sh, "ok %u  err %u  timeouts %u  crc %u  sniffed %u  last error: %s",
		    (unsigned int)s.ok_count, (unsigned int)s.err_count,
		    (unsigned int)s.timeout_count, (unsigned int)s.crc_errors,
		    (unsigned int)s.sniff_frames, s.last_err[0] ? s.last_err : "-");
	if (st->valid & BM_SV_IDENT) {
		shell_print(sh, "model \"%s\"  manufacturer \"%s\"  sw %s  serial \"%s\"", st->model,
			    st->manufacturer, st->sw_version, st->serial);
	}
	if (st->valid & BM_SV_MEAS) {
		print_fixed(a, sizeof(a), st->pack_mv / 10, 100, 2);
		print_fixed(b, sizeof(b), st->pack_ma / 10, 100, 2);
		shell_print(sh, "pack %s V  %s A", a, b);
	}
	if (st->valid & (BM_SV_SOC | BM_SV_SOH)) {
		print_fixed(a, sizeof(a), st->soc_dpct, 10, 1);
		print_fixed(b, sizeof(b), st->soh_dpct, 10, 1);
		shell_print(sh, "SoC %s %%%s  SoH %s %%", a, st->soc_derived ? " (derived)" : "",
			    (st->valid & BM_SV_SOH) ? b : "-");
	}
	if (st->valid & BM_SV_CAP) {
		print_fixed(a, sizeof(a), (int32_t)(st->remain_mah / 10), 100, 2);
		print_fixed(b, sizeof(b), (int32_t)(st->full_mah / 10), 100, 2);
		print_fixed(c, sizeof(c), (int32_t)(st->rated_mah / 10), 100, 2);
		shell_print(sh, "capacity %s / %s Ah (rated %s)  cycles %u", a, b, c, st->cycles);
	}
	if (st->valid & BM_SV_CELLS) {
		unsigned int imin = 0, imax = 0;

		for (unsigned int i = 1; i < st->n_cells; i++) {
			if (st->cell_mv[i] < st->cell_mv[imin]) {
				imin = i;
			}
			if (st->cell_mv[i] > st->cell_mv[imax]) {
				imax = i;
			}
		}
		shell_print(sh, "cells %u: min %u mV (#%u)  max %u mV (#%u)  diff %u mV", st->n_cells,
			    st->cell_mv[imin], imin + 1, st->cell_mv[imax], imax + 1,
			    st->cell_mv[imax] - st->cell_mv[imin]);
	}
	if (st->valid & BM_SV_TEMPS) {
		char line[96] = "temps:";
		size_t len = strlen(line);

		for (unsigned int i = 0; i < st->n_temps && len < sizeof(line) - 10; i++) {
			print_fixed(a, sizeof(a), st->temp_dc[i], 10, 1);
			len += (size_t)snprintf(line + len, sizeof(line) - len, " %s", a);
		}
		shell_print(sh, "%s C", line);
	}
	if (st->valid & BM_SV_LIMITS) {
		print_fixed(a, sizeof(a), st->cvl_mv / 10, 100, 2);
		print_fixed(b, sizeof(b), st->dvl_mv / 10, 100, 2);
		print_fixed(c, sizeof(c), st->ccl_ma / 100, 10, 1);
		print_fixed(d, sizeof(d), st->dcl_ma / 100, 10, 1);
		shell_print(sh, "limits CVL %s V  DVL %s V  CCL %s A  DCL %s A  flags %02x", a, b, c, d,
			    st->req_flags);
	}
	if (st->valid & BM_SV_STATUS) {
		char hex[2 * BM_SER_STATUS_RAW + 1];

		bm_hex_encode(st->status_raw, st->n_status_raw, hex, sizeof(hex));
		shell_print(sh, "status raw: %s", hex);
	}
	if (st->valid & BM_SV_JKFLAGS) {
		shell_print(sh, "jk alarm %04x  status %04x", st->jk_alarm, st->jk_status);
	}
	return 0;
}

static int cmd_rs_cells(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	static struct bm_serial s;

	app_serial_snapshot(&s);
	if (!(s.st.valid & BM_SV_CELLS)) {
		shell_print(sh, "no cell data");
		return 0;
	}
	for (unsigned int i = 0; i < s.st.n_cells; i++) {
		shell_print(sh, "cell %2u  %u mV%s", i + 1, s.st.cell_mv[i],
			    (s.st.valid & BM_SV_STATUS) && s.st.cell_status[i] ? "  (warn)" : "");
	}
	return 0;
}

static int cmd_rs_probe(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	k_mutex_lock(&app.lock, K_FOREVER);
	bm_serial_restart(&app.serial);
	k_mutex_unlock(&app.lock);
	shell_print(sh, "rs485 detection restarted");
	return 0;
}

static void print_reply(const struct shell *sh, const uint8_t *rx, int n)
{
	if (n <= 0) {
		shell_print(sh, "no reply (%d)", n);
		return;
	}
	if (rx[0] == '~') {
		struct bm_ydt_frame f;
		static uint8_t info[BM_YDT_MAX_INFO];
		static char hex[2 * BM_YDT_MAX_INFO + 1];

		shell_print(sh, "%.*s", n - (rx[n - 1] == '\r' ? 1 : 0), (const char *)rx);
		int rc = bm_ydt_parse((const char *)rx, (size_t)n, &f, info, sizeof(info));

		if (rc) {
			shell_error(sh, "frame error %d", rc);
			return;
		}
		bm_hex_encode(info, f.info_len, hex, sizeof(hex));
		shell_print(sh, "ver %02x addr %02x cid1 %02x rtn %02x (%s) info %u bytes: %s", f.ver,
			    f.adr, f.cid1, f.cid2, bm_ydt_rtn_name(f.cid2), (unsigned int)f.info_len,
			    hex);
		return;
	}
	shell_hexdump(sh, rx, (size_t)n);
}

static int cmd_rs_cmd(const struct shell *sh, size_t argc, char **argv)
{
	static uint8_t tx[BM_SER_REQ_MAX * 4], rx[BM_SER_MAX_FRAME], info[64];
	static struct bm_serial s;
	size_t info_len = 0;

	app_serial_snapshot(&s);
	uint8_t cid2 = (uint8_t)strtoul(argv[1], NULL, 16);

	if (argc > 2) {
		int n = bm_hex_decode(argv[2], strlen(argv[2]), info, sizeof(info));

		if (n < 0) {
			shell_error(sh, "info must be hex bytes");
			return -EINVAL;
		}
		info_len = (size_t)n;
	}
	int n = bm_serial_build_cmd(&s, cid2, info, info_len, tx, sizeof(tx));

	if (n < 0) {
		return -EINVAL;
	}
	uint32_t baud = s.baud ? s.baud : 9600;

	shell_print(sh, "-> %.*s  (%u bit/s)", n - 1, (const char *)tx, (unsigned int)baud);
	int r = rs485_transact(tx, (size_t)n, BM_SER_KIND_ASCII, baud, rx, sizeof(rx),
			       bm_serial_timeout_ms(baud, BM_SER_KIND_ASCII));

	if (r == -EACCES) {
		shell_error(sh, "listen-only build");
		return r;
	}
	print_reply(sh, rx, r);
	return 0;
}

static int cmd_rs_send(const struct shell *sh, size_t argc, char **argv)
{
	static uint8_t tx[BM_SER_MAX_FRAME], rx[BM_SER_MAX_FRAME];
	size_t len;
	enum bm_ser_kind kind;
	uint32_t baud = app.serial.baud ? app.serial.baud : 9600;

	if (argc > 2) {
		baud = (uint32_t)strtoul(argv[2], NULL, 10);
	}
	if (argv[1][0] == '~') {
		len = strlen(argv[1]);
		if (len + 1 >= sizeof(tx)) {
			return -EINVAL;
		}
		memcpy(tx, argv[1], len);
		tx[len++] = '\r';
		kind = BM_SER_KIND_ASCII;
	} else {
		int n = bm_hex_decode(argv[1], strlen(argv[1]), tx, sizeof(tx));

		if (n <= 0) {
			shell_error(sh, "argument must start with ~ or be hex bytes");
			return -EINVAL;
		}
		len = (size_t)n;
		kind = (tx[0] == 0x4E) ? BM_SER_KIND_JK : BM_SER_KIND_MODBUS;
	}
	int r = rs485_transact(tx, len, kind, baud, rx, sizeof(rx), bm_serial_timeout_ms(baud, kind));

	if (r == -EACCES) {
		shell_error(sh, "listen-only build");
		return r;
	}
	print_reply(sh, rx, r);
	return 0;
}

static int cmd_rs_last(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	static struct bm_serial s;

	app_serial_snapshot(&s);
	if (s.last_rx_len == 0) {
		shell_print(sh, "nothing received yet");
		return 0;
	}
	print_reply(sh, s.last_rx, (int)s.last_rx_len);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_rs485,
	SHELL_CMD(status, NULL, "RS485 link, detection and decoded values", cmd_rs_status),
	SHELL_CMD(cells, NULL, "Cell voltages", cmd_rs_cells),
	SHELL_CMD(probe, NULL, "Restart protocol detection", cmd_rs_probe),
	SHELL_CMD_ARG(cmd, NULL, "Send <cid2 hex> [info hex] with the detected VER/ADR", cmd_rs_cmd, 2, 1),
	SHELL_CMD_ARG(send, NULL, "Send raw: ~ASCII frame (CR added) or hex bytes [baud]", cmd_rs_send,
		      2, 1),
	SHELL_CMD(last, NULL, "Last received frame", cmd_rs_last),
	SHELL_SUBCMD_SET_END);
#endif /* CONFIG_BATMON_RS485 */

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
#ifdef CONFIG_BATMON_RS485
	SHELL_CMD(rs485, &sub_rs485, "RS485 BMS link", NULL),
#endif
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(batmon, &sub_batmon, "batmon battery monitor", NULL);
