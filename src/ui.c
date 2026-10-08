/*
 * Status screens on the 240x240 display. Page 1: CAN view, page 2: RS485 view.
 * The user button toggles, otherwise the page with a live link is shown.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>

#include <stdio.h>

#include "app.h"

LOG_MODULE_REGISTER(batmon_ui, LOG_LEVEL_INF);

#define UI_THREAD_STACK 4096
#define UI_THREAD_PRIO  8
#define UI_REFRESH_MS   500
#define MANUAL_HOLD_MS  30000
#define DEBOUNCE_MS     250

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

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET_OR(DT_ALIAS(sw0), gpios, {0});
static struct gpio_callback button_cb;
static volatile uint32_t button_presses;
static uint32_t button_last_ms;

/* page 1 */
static lv_obj_t *scr1, *lbl_title, *lbl_soc, *bar_soc, *lbl_meas, *lbl_limits, *lbl_flags,
	*lbl_proto, *lbl_net, *lbl_link;
/* page 2 */
static lv_obj_t *scr2, *lbl2_link, *lbl2_desc, *lbl2_meas, *lbl2_soc, *lbl2_cells, *lbl2_cells2,
	*lbl2_cap, *lbl2_limits, *lbl2_stats, *lbl2_net;

static unsigned int page;
static uint32_t manual_until;

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int x, int y)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
	lv_obj_set_pos(l, x, y);
	lv_label_set_text(l, "");
	return l;
}

static lv_obj_t *header(lv_obj_t *scr, const char *title, lv_obj_t **link_lbl)
{
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

	lv_obj_t *t = label(head, &lv_font_montserrat_14, COL_BG, 6, 6);

	lv_label_set_text(t, title);
	*link_lbl = label(head, &lv_font_montserrat_14, COL_YELLOW, 190, 6);
	return head;
}

static void build_page1(void)
{
	scr1 = lv_screen_active();
	header(scr1, "batmon  " CONFIG_BATMON_DEVICE_ID, &lbl_link);
	lbl_title = NULL;

	lbl_soc = label(scr1, &lv_font_montserrat_32, COL_BLUE, 8, 38);
	bar_soc = lv_bar_create(scr1);
	lv_obj_set_size(bar_soc, 110, 16);
	lv_obj_set_pos(bar_soc, 120, 50);
	lv_bar_set_range(bar_soc, 0, 100);
	lv_obj_set_style_bg_color(bar_soc, lv_color_hex(0xe5e7eb), LV_PART_MAIN);
	lv_obj_set_style_bg_color(bar_soc, lv_color_hex(COL_BLUE2), LV_PART_INDICATOR);

	lbl_meas = label(scr1, &lv_font_montserrat_20, COL_TEXT, 8, 82);
	lbl_limits = label(scr1, &lv_font_montserrat_14, COL_GREY, 8, 112);
	lbl_flags = label(scr1, &lv_font_montserrat_14, COL_TEXT, 8, 150);
	lbl_proto = label(scr1, &lv_font_montserrat_14, COL_BLUE2, 8, 176);
	lv_obj_set_width(lbl_proto, 224);
	lv_label_set_long_mode(lbl_proto, LV_LABEL_LONG_MODE_WRAP);
	lbl_net = label(scr1, &lv_font_montserrat_14, COL_GREY, 8, 220);
}

static void build_page2(void)
{
	scr2 = lv_obj_create(NULL);
	header(scr2, "batmon  RS485", &lbl2_link);

	lbl2_desc = label(scr2, &lv_font_montserrat_14, COL_BLUE2, 8, 36);
	lv_obj_set_width(lbl2_desc, 224);
	lv_label_set_long_mode(lbl2_desc, LV_LABEL_LONG_MODE_WRAP);
	lbl2_meas = label(scr2, &lv_font_montserrat_20, COL_TEXT, 8, 76);
	lbl2_soc = label(scr2, &lv_font_montserrat_14, COL_TEXT, 8, 104);
	lbl2_cells = label(scr2, &lv_font_montserrat_14, COL_TEXT, 8, 124);
	lbl2_cells2 = label(scr2, &lv_font_montserrat_14, COL_TEXT, 8, 144);
	lbl2_cap = label(scr2, &lv_font_montserrat_14, COL_TEXT, 8, 164);
	lbl2_limits = label(scr2, &lv_font_montserrat_14, COL_GREY, 8, 184);
	lbl2_stats = label(scr2, &lv_font_montserrat_14, COL_GREY, 8, 202);
	lbl2_net = label(scr2, &lv_font_montserrat_14, COL_GREY, 8, 220);
}

static void fmt_fixed(char *out, size_t len, int32_t v, uint32_t div, int digits)
{
	bool neg = v < 0;
	uint32_t u = neg ? (uint32_t)(-(int64_t)v) : (uint32_t)v;

	snprintf(out, len, "%s%u.%0*u", neg ? "-" : "", (unsigned int)(u / div), digits,
		 (unsigned int)(u % div));
}

static void net_line(lv_obj_t *lbl, uint32_t frames)
{
	char buf[64];

	snprintf(buf, sizeof(buf), "%s  MQTT %s  %u fr", app.net_up ? app.ip : "kein Netz",
		 app.mqtt_connected ? "ok" : "--", (unsigned int)frames);
	lv_label_set_text(lbl, buf);
}

static void refresh_page1(const struct bm_proto *snap)
{
	char buf[96], u[16], i[16], t[16], a[16], b[16], c[16];
	const struct bm_pack_state *st = &snap->st;

	lv_label_set_text(lbl_link, snap->link_up ? "CAN" : "---");
	lv_obj_set_style_text_color(lbl_link, lv_color_hex(snap->link_up ? COL_YELLOW : COL_RED), 0);

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

	snprintf(buf, sizeof(buf), "%s\n%s (%u%%)", snap->det.flavor,
		 st->manufacturer[0] ? st->manufacturer : bm_family_name(snap->det.family),
		 snap->det.confidence);
	lv_label_set_text(lbl_proto, buf);
	net_line(lbl_net, snap->frames_total);
}

static void refresh_page2(const struct bm_serial *s, uint32_t can_frames)
{
	char buf[96], a[16], b[16], c[16];
	const struct bm_serial_state *st = &s->st;

	lv_label_set_text(lbl2_link, s->link_up ? "485" : "---");
	lv_obj_set_style_text_color(lbl2_link, lv_color_hex(s->link_up ? COL_YELLOW : COL_RED), 0);
	lv_label_set_text(lbl2_desc, s->flavor_str);

	if (st->valid & BM_SV_MEAS) {
		fmt_fixed(a, sizeof(a), st->pack_mv / 10, 100, 2);
		fmt_fixed(b, sizeof(b), st->pack_ma / 10, 100, 2);
		snprintf(buf, sizeof(buf), "%s V   %s A", a, b);
	} else {
		snprintf(buf, sizeof(buf), "keine Messwerte");
	}
	lv_label_set_text(lbl2_meas, buf);

	if (st->valid & BM_SV_SOC) {
		fmt_fixed(a, sizeof(a), st->soc_dpct, 10, 1);
		fmt_fixed(b, sizeof(b), st->soh_dpct, 10, 1);
		snprintf(buf, sizeof(buf), "SoC %s %%%s   SoH %s %%", a, st->soc_derived ? "*" : "",
			 (st->valid & BM_SV_SOH) ? b : "--");
	} else {
		snprintf(buf, sizeof(buf), "SoC --");
	}
	lv_label_set_text(lbl2_soc, buf);

	if ((st->valid & BM_SV_CELLS) && st->n_cells) {
		unsigned int imin = 0, imax = 0;

		for (unsigned int i = 1; i < st->n_cells; i++) {
			if (st->cell_mv[i] < st->cell_mv[imin]) {
				imin = i;
			}
			if (st->cell_mv[i] > st->cell_mv[imax]) {
				imax = i;
			}
		}
		snprintf(buf, sizeof(buf), "Zellen %u: %u.%03u - %u.%03u V", st->n_cells,
			 st->cell_mv[imin] / 1000, st->cell_mv[imin] % 1000, st->cell_mv[imax] / 1000,
			 st->cell_mv[imax] % 1000);
		lv_label_set_text(lbl2_cells, buf);
		int16_t tmax = st->temp_dc[0];

		for (unsigned int i = 1; i < st->n_temps; i++) {
			if (st->temp_dc[i] > tmax) {
				tmax = st->temp_dc[i];
			}
		}
		fmt_fixed(a, sizeof(a), tmax, 10, 1);
		snprintf(buf, sizeof(buf), "dV %u mV (#%u/#%u)   Tmax %s C",
			 st->cell_mv[imax] - st->cell_mv[imin], imin + 1, imax + 1,
			 (st->valid & BM_SV_TEMPS) ? a : "--");
		lv_label_set_text(lbl2_cells2, buf);
	} else {
		lv_label_set_text(lbl2_cells, "Zellen: --");
		lv_label_set_text(lbl2_cells2, "");
	}

	if (st->valid & BM_SV_CAP) {
		fmt_fixed(a, sizeof(a), (int32_t)(st->remain_mah / 100), 10, 1);
		fmt_fixed(b, sizeof(b), (int32_t)(st->full_mah / 100), 10, 1);
		snprintf(buf, sizeof(buf), "%s / %s Ah   Zyklen %u", a, b, st->cycles);
	} else if (st->valid & BM_SV_CYCLES) {
		snprintf(buf, sizeof(buf), "Zyklen %u", st->cycles);
	} else {
		snprintf(buf, sizeof(buf), "Kapazitaet: --");
	}
	lv_label_set_text(lbl2_cap, buf);

	if (st->valid & BM_SV_LIMITS) {
		fmt_fixed(a, sizeof(a), st->cvl_mv / 10, 100, 2);
		fmt_fixed(b, sizeof(b), st->ccl_ma / 100, 10, 1);
		fmt_fixed(c, sizeof(c), st->dcl_ma / 100, 10, 1);
		snprintf(buf, sizeof(buf), "CVL %s V  CCL %s A  DCL %s A", a, b, c);
	} else if (st->valid & BM_SV_STATUS) {
		char hex[2 * BM_SER_STATUS_RAW + 1];

		bm_hex_encode(st->status_raw, st->n_status_raw > 11 ? 11 : st->n_status_raw, hex,
			      sizeof(hex));
		snprintf(buf, sizeof(buf), "Status %s", hex);
	} else if (st->valid & BM_SV_JKFLAGS) {
		snprintf(buf, sizeof(buf), "Alarm %04x  Status %04x", st->jk_alarm, st->jk_status);
	} else {
		snprintf(buf, sizeof(buf), "%s", (st->valid & BM_SV_IDENT) ? st->serial : "");
	}
	lv_label_set_text(lbl2_limits, buf);

	snprintf(buf, sizeof(buf), "%u bd  ok %u  err %u  %s", (unsigned int)s->baud,
		 (unsigned int)s->ok_count, (unsigned int)s->err_count, s->last_err);
	lv_label_set_text(lbl2_stats, buf);
	net_line(lbl2_net, can_frames);
}

static void button_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	uint32_t now = k_uptime_get_32();

	if (now - button_last_ms > DEBOUNCE_MS) {
		button_presses++;
	}
	button_last_ms = now;
}

static void show_page(unsigned int p)
{
	if (p == page) {
		return;
	}
	page = p;
	lv_screen_load(p == 1 ? scr2 : scr1);
}

static void refresh(void)
{
	static struct bm_proto snap;
	static struct bm_serial ser;
	static uint32_t presses_seen;
	uint32_t now = k_uptime_get_32();

	app_proto_snapshot(&snap);
	if (app.rs485_present) {
		app_serial_snapshot(&ser);
	}
	if (button_presses != presses_seen) {
		presses_seen = button_presses;
		if (app.rs485_present) {
			show_page(page ^ 1u);
			manual_until = now + MANUAL_HOLD_MS;
		}
	} else if (app.rs485_present && (int32_t)(now - manual_until) > 0) {
		/* automatic choice: the side with a live link, CAN preferred */
		if (!snap.link_up && ser.link_up) {
			show_page(1);
		} else if (snap.link_up) {
			show_page(0);
		}
	}
	if (page == 0) {
		refresh_page1(&snap);
	} else {
		refresh_page2(&ser, snap.frames_total);
	}
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
	build_page1();
	build_page2();
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
	if (button.port != NULL && device_is_ready(button.port)) {
		gpio_pin_configure_dt(&button, GPIO_INPUT);
		gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
		gpio_init_callback(&button_cb, button_isr, BIT(button.pin));
		gpio_add_callback(button.port, &button_cb);
	}
	k_thread_create(&ui_thread_data, batmon_ui_stack, K_THREAD_STACK_SIZEOF(batmon_ui_stack),
			ui_thread, NULL, NULL, NULL, UI_THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&ui_thread_data, "ui");
	return 0;
}
