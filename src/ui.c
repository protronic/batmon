#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>

#include <stdio.h>

#include "app.h"

LOG_MODULE_REGISTER(batmon_ui, LOG_LEVEL_INF);

#define UI_THREAD_STACK 4096
#define UI_THREAD_PRIO  8
#define UI_REFRESH_MS   500

#define COL_BLUE   0x10238c
#define COL_BLUE2  0x0e509a
#define COL_YELLOW 0xf4cf18
#define COL_BG     0xffffff
#define COL_TEXT   0x1a1a1a
#define COL_GREY   0x6b7280
#define COL_RED    0xc0392b
#define COL_GREEN  0x1e8449

static K_THREAD_STACK_DEFINE(batmon_ui_stack, UI_THREAD_STACK);
static struct k_thread ui_thread_data;

static lv_obj_t *lbl_title, *lbl_soc, *bar_soc, *lbl_meas, *lbl_limits, *lbl_flags, *lbl_proto,
	*lbl_net, *lbl_link;

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int x, int y)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
	lv_obj_set_pos(l, x, y);
	lv_label_set_text(l, "");
	return l;
}

static void build_screen(void)
{
	lv_obj_t *scr = lv_screen_active();

	lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

	lv_obj_t *head = lv_obj_create(scr);

	lv_obj_remove_style_all(head);
	lv_obj_set_size(head, 240, 28);
	lv_obj_set_pos(head, 0, 0);
	lv_obj_set_style_bg_color(head, lv_color_hex(COL_BLUE), 0);
	lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);

	lv_obj_t *accent = lv_obj_create(scr);

	lv_obj_remove_style_all(accent);
	lv_obj_set_size(accent, 240, 3);
	lv_obj_set_pos(accent, 0, 28);
	lv_obj_set_style_bg_color(accent, lv_color_hex(COL_YELLOW), 0);
	lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);

	lbl_title = label(head, &lv_font_montserrat_14, COL_BG, 6, 6);
	lv_label_set_text(lbl_title, "batmon  " CONFIG_BATMON_DEVICE_ID);
	lbl_link = label(head, &lv_font_montserrat_14, COL_YELLOW, 170, 6);

	lbl_soc = label(scr, &lv_font_montserrat_32, COL_BLUE, 8, 38);
	bar_soc = lv_bar_create(scr);
	lv_obj_set_size(bar_soc, 110, 16);
	lv_obj_set_pos(bar_soc, 120, 50);
	lv_bar_set_range(bar_soc, 0, 100);
	lv_obj_set_style_bg_color(bar_soc, lv_color_hex(0xe5e7eb), LV_PART_MAIN);
	lv_obj_set_style_bg_color(bar_soc, lv_color_hex(COL_BLUE2), LV_PART_INDICATOR);

	lbl_meas = label(scr, &lv_font_montserrat_20, COL_TEXT, 8, 82);
	lbl_limits = label(scr, &lv_font_montserrat_14, COL_GREY, 8, 112);
	lbl_flags = label(scr, &lv_font_montserrat_14, COL_TEXT, 8, 150);
	lbl_proto = label(scr, &lv_font_montserrat_14, COL_BLUE2, 8, 176);
	lv_obj_set_width(lbl_proto, 224);
	lv_label_set_long_mode(lbl_proto, LV_LABEL_LONG_MODE_WRAP);
	lbl_net = label(scr, &lv_font_montserrat_14, COL_GREY, 8, 220);
}

static void fmt_fixed(char *out, size_t len, int32_t v, uint32_t div, int digits)
{
	bool neg = v < 0;
	uint32_t u = neg ? (uint32_t)(-(int64_t)v) : (uint32_t)v;

	snprintf(out, len, "%s%u.%0*u", neg ? "-" : "", (unsigned int)(u / div), digits,
		 (unsigned int)(u % div));
}

static void refresh(void)
{
	static struct bm_proto snap;
	char buf[96], u[16], i[16], t[16], a[16], b[16], c[16];

	app_proto_snapshot(&snap);
	const struct bm_pack_state *st = &snap.st;

	lv_label_set_text(lbl_link, snap.link_up ? "CAN" : "---");
	lv_obj_set_style_text_color(lbl_link, lv_color_hex(snap.link_up ? COL_YELLOW : COL_RED), 0);

	if (st->valid & BM_VALID_SOC) {
		snprintf(buf, sizeof(buf), "%u %%", st->soc_pct);
		lv_bar_set_value(bar_soc, st->soc_pct, LV_ANIM_OFF);
	} else {
		snprintf(buf, sizeof(buf), "-- %%");
		lv_bar_set_value(bar_soc, 0, LV_ANIM_OFF);
	}
	lv_label_set_text(lbl_soc, buf);

	if (st->valid & BM_VALID_MEAS) {
		fmt_fixed(u, sizeof(u), st->pack_mv / 10, 100, 2);
		fmt_fixed(i, sizeof(i), st->pack_ma / 100, 10, 1);
		fmt_fixed(t, sizeof(t), st->temp_dc, 10, 1);
		snprintf(buf, sizeof(buf), "%s V  %s A  %s C", u, i, t);
	} else {
		snprintf(buf, sizeof(buf), "keine Messwerte");
	}
	lv_label_set_text(lbl_meas, buf);

	if (st->valid & BM_VALID_LIMITS) {
		fmt_fixed(a, sizeof(a), st->cvl_mv / 10, 100, 2);
		fmt_fixed(b, sizeof(b), st->ccl_ma / 100, 10, 1);
		fmt_fixed(c, sizeof(c), st->dcl_ma / 100, 10, 1);
		snprintf(buf, sizeof(buf), "CVL %s V\nCCL %s A   DCL %s A", a, b, c);
	} else {
		snprintf(buf, sizeof(buf), "Limits: --");
	}
	lv_label_set_text(lbl_limits, buf);

	if (st->valid & BM_VALID_REQUEST) {
		snprintf(buf, sizeof(buf), "%s %s %s  SoH %u%%  M%u",
			 st->charge_en ? "CHG" : "chg", st->discharge_en ? "DIS" : "dis",
			 (st->force_charge_1 || st->force_charge_2) ? "FORCE" : "",
			 st->soh_pct, st->module_count);
	} else {
		snprintf(buf, sizeof(buf), "Flags: --");
	}
	bool fault = st->prot[0] || st->prot[1];

	lv_obj_set_style_text_color(lbl_flags, lv_color_hex(fault ? COL_RED : COL_GREEN), 0);
	lv_label_set_text(lbl_flags, fault ? "SCHUTZ AKTIV" : buf);

	snprintf(buf, sizeof(buf), "%s\n%s (%u%%)", snap.det.flavor,
		 st->manufacturer[0] ? st->manufacturer : bm_family_name(snap.det.family),
		 snap.det.confidence);
	lv_label_set_text(lbl_proto, buf);

	snprintf(buf, sizeof(buf), "%s  MQTT %s  %u fr", app.net_up ? app.ip : "kein Netz",
		 app.mqtt_connected ? "ok" : "--", (unsigned int)snap.frames_total);
	lv_label_set_text(lbl_net, buf);
}

static void ui_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

	if (!device_is_ready(disp)) {
		LOG_ERR("display not ready");
		return;
	}
	build_screen();
	lv_timer_handler();
	display_blanking_off(disp);

	int64_t next = k_uptime_get();

	while (1) {
		if (k_uptime_get() >= next) {
			refresh();
			next += UI_REFRESH_MS;
		}
		uint32_t sleep = lv_timer_handler();

		k_sleep(K_MSEC(MIN(sleep, 50)));
	}
}

int ui_init(void)
{
	k_thread_create(&ui_thread_data, batmon_ui_stack, K_THREAD_STACK_SIZEOF(batmon_ui_stack), ui_thread, NULL,
			NULL, NULL, UI_THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&ui_thread_data, "ui");
	return 0;
}
