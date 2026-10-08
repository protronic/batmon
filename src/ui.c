/*
 * LVGL user interface on the 240x240 touch display.
 *
 * Three tabs: CAN (inverter protocol view), RS485 (BMS view) and Zellen (cell
 * voltages). Swipe or tap the tab bar, or press the user button to switch.
 * Without interaction the tab with a live link is shown.
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

#define UI_THREAD_STACK 6144
#define UI_THREAD_PRIO  8
#define UI_REFRESH_MS   500
#define MANUAL_HOLD_MS  30000
#define DEBOUNCE_MS     250
#define TAB_BAR_H       30
#define TAB_CAN         0
#define TAB_RS485       1
#define TAB_CELLS       2

#define COL_BLUE   0x10238c
#define COL_BLUE2  0x0e509a
#define COL_YELLOW 0xf4cf18
#define COL_BG     0xffffff
#define COL_TEXT   0x1a1a1a
#define COL_GREY   0x6b7280
#define COL_LIGHT  0xe5e7eb
#define COL_RED    0xc0392b
#define COL_GREEN  0x1e8449

static K_THREAD_STACK_DEFINE(batmon_ui_stack, UI_THREAD_STACK);
static struct k_thread ui_thread_data;

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET_OR(DT_ALIAS(sw0), gpios, {0});
static struct gpio_callback button_cb;
static volatile uint32_t button_presses;
static uint32_t button_last_ms;

static lv_obj_t *tabview;
static uint32_t manual_until;
static int auto_tab = -1;

struct gauge {
	lv_obj_t *arc;
	lv_obj_t *value;
	lv_obj_t *caption;
};

/* CAN tab */
static struct gauge g_can;
static lv_obj_t *c_link, *c_u, *c_i, *c_t, *c_limits, *c_flags, *c_proto, *c_net;
/* RS485 tab */
static struct gauge g_rs;
static lv_obj_t *r_link, *r_u, *r_i, *r_soh, *r_desc, *r_cells, *r_cap, *r_limits, *r_stats;
/* cells tab */
static lv_obj_t *z_chart, *z_info, *z_temps, *z_src;
static lv_chart_series_t *z_series;

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int x, int y)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
	lv_obj_set_pos(l, x, y);
	lv_label_set_text(l, "");
	return l;
}

static lv_obj_t *wrapped(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int x, int y,
			 int w)
{
	lv_obj_t *l = label(parent, font, color, x, y);

	lv_obj_set_width(l, w);
	lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
	return l;
}

static void gauge_create(struct gauge *g, lv_obj_t *parent, int x, int y, const char *caption)
{
	g->arc = lv_arc_create(parent);
	lv_obj_set_size(g->arc, 100, 100);
	lv_obj_set_pos(g->arc, x, y);
	lv_arc_set_rotation(g->arc, 135);
	lv_arc_set_bg_angles(g->arc, 0, 270);
	lv_arc_set_range(g->arc, 0, 100);
	lv_arc_set_value(g->arc, 0);
	lv_obj_remove_style(g->arc, NULL, LV_PART_KNOB);
	lv_obj_remove_flag(g->arc, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_arc_width(g->arc, 11, LV_PART_MAIN);
	lv_obj_set_style_arc_width(g->arc, 11, LV_PART_INDICATOR);
	lv_obj_set_style_arc_color(g->arc, lv_color_hex(COL_LIGHT), LV_PART_MAIN);
	lv_obj_set_style_arc_color(g->arc, lv_color_hex(COL_BLUE2), LV_PART_INDICATOR);
	lv_obj_set_style_arc_rounded(g->arc, true, LV_PART_INDICATOR);

	g->value = label(g->arc, &lv_font_montserrat_20, COL_BLUE, 0, 0);
	lv_obj_set_style_text_align(g->value, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(g->value, LV_ALIGN_CENTER, 0, -6);
	g->caption = label(g->arc, &lv_font_montserrat_14, COL_GREY, 0, 0);
	lv_label_set_text(g->caption, caption);
	lv_obj_align(g->caption, LV_ALIGN_CENTER, 0, 14);
}

static void gauge_set(struct gauge *g, int value_dpct, bool valid, const char *suffix)
{
	char buf[16];

	if (!valid) {
		lv_arc_set_value(g->arc, 0);
		lv_label_set_text(g->value, "--");
	} else {
		lv_arc_set_value(g->arc, value_dpct / 10);
		if (value_dpct % 10) {
			snprintf(buf, sizeof(buf), "%d.%d%s", value_dpct / 10, value_dpct % 10, suffix);
		} else {
			snprintf(buf, sizeof(buf), "%d%s", value_dpct / 10, suffix);
		}
		lv_label_set_text(g->value, buf);
	}
	lv_obj_align(g->value, LV_ALIGN_CENTER, 0, -6);
}

static void page_setup(lv_obj_t *page)
{
	lv_obj_set_style_pad_all(page, 0, 0);
	lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_color(page, lv_color_hex(COL_BG), 0);
	lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
}

static void build_can_tab(lv_obj_t *page)
{
	page_setup(page);
	gauge_create(&g_can, page, 6, 4, "SoC");
	c_link = label(page, &lv_font_montserrat_14, COL_GREY, 114, 6);
	c_u = label(page, &lv_font_montserrat_20, COL_TEXT, 114, 26);
	c_i = label(page, &lv_font_montserrat_20, COL_TEXT, 114, 52);
	c_t = label(page, &lv_font_montserrat_14, COL_GREY, 114, 82);
	c_limits = label(page, &lv_font_montserrat_14, COL_GREY, 8, 108);
	c_flags = label(page, &lv_font_montserrat_14, COL_GREEN, 8, 128);
	c_proto = wrapped(page, &lv_font_montserrat_14, COL_BLUE2, 8, 148, 224);
	c_net = label(page, &lv_font_montserrat_14, COL_GREY, 8, 190);
}

static void build_rs485_tab(lv_obj_t *page)
{
	page_setup(page);
	gauge_create(&g_rs, page, 6, 4, "SoC");
	r_link = label(page, &lv_font_montserrat_14, COL_GREY, 114, 6);
	r_u = label(page, &lv_font_montserrat_20, COL_TEXT, 114, 26);
	r_i = label(page, &lv_font_montserrat_20, COL_TEXT, 114, 52);
	r_soh = label(page, &lv_font_montserrat_14, COL_GREY, 114, 82);
	r_desc = wrapped(page, &lv_font_montserrat_14, COL_BLUE2, 8, 108, 224);
	r_cells = label(page, &lv_font_montserrat_14, COL_TEXT, 8, 146);
	r_cap = label(page, &lv_font_montserrat_14, COL_TEXT, 8, 164);
	r_limits = label(page, &lv_font_montserrat_14, COL_GREY, 8, 182);
	r_stats = label(page, &lv_font_montserrat_14, COL_GREY, 8, 200);
}

static void build_cells_tab(lv_obj_t *page)
{
	page_setup(page);
	z_chart = lv_chart_create(page);
	lv_obj_set_size(z_chart, 226, 118);
	lv_obj_set_pos(z_chart, 6, 6);
	lv_chart_set_type(z_chart, LV_CHART_TYPE_BAR);
	lv_chart_set_point_count(z_chart, 16);
	lv_chart_set_axis_range(z_chart, LV_CHART_AXIS_PRIMARY_Y, 2800, 3700);
	lv_chart_set_div_line_count(z_chart, 4, 0);
	lv_obj_set_style_pad_all(z_chart, 4, 0);
	lv_obj_set_style_pad_column(z_chart, 2, 0);
	lv_obj_set_style_border_color(z_chart, lv_color_hex(COL_LIGHT), 0);
	lv_obj_set_style_line_color(z_chart, lv_color_hex(COL_LIGHT), 0);
	z_series = lv_chart_add_series(z_chart, lv_color_hex(COL_BLUE2), LV_CHART_AXIS_PRIMARY_Y);
	lv_chart_set_all_values(z_chart, z_series, LV_CHART_POINT_NONE);

	z_info = wrapped(page, &lv_font_montserrat_14, COL_TEXT, 8, 130, 224);
	z_temps = wrapped(page, &lv_font_montserrat_14, COL_GREY, 8, 166, 224);
	z_src = label(page, &lv_font_montserrat_14, COL_BLUE2, 8, 194);
}

static void style_tab_bar(void)
{
	lv_obj_t *bar = lv_tabview_get_tab_bar(tabview);

	lv_obj_set_style_bg_color(bar, lv_color_hex(COL_BLUE), 0);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(bar, 0, 0);
	lv_obj_set_style_pad_all(bar, 0, 0);
	for (uint32_t i = 0; i < lv_tabview_get_tab_count(tabview); i++) {
		lv_obj_t *btn = lv_tabview_get_tab_button(tabview, (int32_t)i);

		if (btn == NULL) {
			continue;
		}
		lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
		lv_obj_set_style_bg_color(btn, lv_color_hex(COL_BLUE2), LV_STATE_CHECKED);
		lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_CHECKED);
		lv_obj_set_style_text_color(btn, lv_color_hex(COL_BG), 0);
		lv_obj_set_style_text_color(btn, lv_color_hex(COL_YELLOW), LV_STATE_CHECKED);
		lv_obj_set_style_text_font(btn, &lv_font_montserrat_14, 0);
		lv_obj_set_style_border_width(btn, 0, 0);
		lv_obj_set_style_border_width(btn, 3, LV_STATE_CHECKED);
		lv_obj_set_style_border_color(btn, lv_color_hex(COL_YELLOW), LV_STATE_CHECKED);
		lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_BOTTOM, LV_STATE_CHECKED);
		lv_obj_set_style_radius(btn, 0, 0);
		lv_obj_set_style_shadow_width(btn, 0, 0);
	}
}

static void tab_changed(lv_event_t *e)
{
	ARG_UNUSED(e);
	/* touch interaction: keep the chosen tab for a while */
	manual_until = k_uptime_get_32() + MANUAL_HOLD_MS;
}

static void build_ui(void)
{
	lv_obj_t *scr = lv_screen_active();

	lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

	tabview = lv_tabview_create(scr);
	lv_obj_set_size(tabview, 240, 240);
	lv_tabview_set_tab_bar_position(tabview, LV_DIR_TOP);
	lv_tabview_set_tab_bar_size(tabview, TAB_BAR_H);
	lv_obj_set_style_bg_color(tabview, lv_color_hex(COL_BG), 0);

	build_can_tab(lv_tabview_add_tab(tabview, "CAN"));
	build_rs485_tab(lv_tabview_add_tab(tabview, "RS485"));
	build_cells_tab(lv_tabview_add_tab(tabview, "Zellen"));
	style_tab_bar();
	lv_obj_add_event_cb(tabview, tab_changed, LV_EVENT_VALUE_CHANGED, NULL);
}

static void fmt_fixed(char *out, size_t len, int32_t v, uint32_t div, int digits)
{
	bool neg = v < 0;
	uint32_t u = neg ? (uint32_t)(-(int64_t)v) : (uint32_t)v;

	snprintf(out, len, "%s%u.%0*u", neg ? "-" : "", (unsigned int)(u / div), digits,
		 (unsigned int)(u % div));
}

static void set_link(lv_obj_t *lbl, bool up, const char *name)
{
	char buf[32];

	snprintf(buf, sizeof(buf), "%s %s", name, up ? "aktiv" : "kein Signal");
	lv_obj_set_style_text_color(lbl, lv_color_hex(up ? COL_GREEN : COL_RED), 0);
	lv_label_set_text(lbl, buf);
}

static void refresh_can(const struct bm_proto *snap)
{
	char buf[96], a[16], b[16], c[16];
	const struct bm_pack_state *st = &snap->st;

	set_link(c_link, snap->link_up, "CAN");
	gauge_set(&g_can, st->soc_pct * 10, (st->valid & BM_VALID_SOC) != 0, " %");

	if (st->valid & BM_VALID_MEAS) {
		fmt_fixed(a, sizeof(a), st->pack_mv / 10, 100, 2);
		fmt_fixed(b, sizeof(b), st->pack_ma / 100, 10, 1);
		fmt_fixed(c, sizeof(c), st->temp_dc, 10, 1);
		snprintf(buf, sizeof(buf), "%s V", a);
		lv_label_set_text(c_u, buf);
		snprintf(buf, sizeof(buf), "%s A", b);
		lv_label_set_text(c_i, buf);
		snprintf(buf, sizeof(buf), "%s C   SoH %u%%", c, st->soh_pct);
		lv_label_set_text(c_t, buf);
	} else {
		lv_label_set_text(c_u, "-- V");
		lv_label_set_text(c_i, "-- A");
		lv_label_set_text(c_t, "keine Messwerte");
	}

	if (st->valid & BM_VALID_LIMITS) {
		fmt_fixed(a, sizeof(a), st->cvl_mv / 10, 100, 2);
		fmt_fixed(b, sizeof(b), st->ccl_ma / 100, 10, 1);
		fmt_fixed(c, sizeof(c), st->dcl_ma / 100, 10, 1);
		snprintf(buf, sizeof(buf), "CVL %s V  CCL %s A  DCL %s A", a, b, c);
	} else {
		snprintf(buf, sizeof(buf), "Limits: --");
	}
	lv_label_set_text(c_limits, buf);

	bool fault = st->prot[0] || st->prot[1];

	if (fault) {
		snprintf(buf, sizeof(buf), "SCHUTZ AKTIV  prot %02x%02x", st->prot[0], st->prot[1]);
	} else if (st->valid & BM_VALID_REQUEST) {
		snprintf(buf, sizeof(buf), "Laden %s   Entladen %s%s%s", st->charge_en ? "ja" : "nein",
			 st->discharge_en ? "ja" : "nein",
			 (st->force_charge_1 || st->force_charge_2) ? "   ZWANGSLADUNG" : "",
			 st->full_charge_req ? "   Vollladung" : "");
	} else {
		snprintf(buf, sizeof(buf), "Freigaben: --   Module %u", st->module_count);
	}
	lv_obj_set_style_text_color(c_flags, lv_color_hex(fault ? COL_RED : COL_GREEN), 0);
	lv_label_set_text(c_flags, buf);

	snprintf(buf, sizeof(buf), "%s\n%s (%u%%)", snap->det.flavor,
		 st->manufacturer[0] ? st->manufacturer : bm_family_name(snap->det.family),
		 snap->det.confidence);
	lv_label_set_text(c_proto, buf);

	snprintf(buf, sizeof(buf), "%s  MQTT %s  %u fr", app.net_up ? app.ip : "kein Netz",
		 app.mqtt_connected ? "ok" : "--", (unsigned int)snap->frames_total);
	lv_label_set_text(c_net, buf);
}

static void cells_minmax(const struct bm_serial_state *st, unsigned int *imin, unsigned int *imax)
{
	*imin = 0;
	*imax = 0;
	for (unsigned int i = 1; i < st->n_cells; i++) {
		if (st->cell_mv[i] < st->cell_mv[*imin]) {
			*imin = i;
		}
		if (st->cell_mv[i] > st->cell_mv[*imax]) {
			*imax = i;
		}
	}
}

static void refresh_rs485(const struct bm_serial *s)
{
	char buf[96], a[16], b[16], c[16];
	const struct bm_serial_state *st = &s->st;

	set_link(r_link, s->link_up, "RS485");
	gauge_set(&g_rs, st->soc_dpct, (st->valid & BM_SV_SOC) != 0, st->soc_derived ? "%*" : " %");

	if (st->valid & BM_SV_MEAS) {
		fmt_fixed(a, sizeof(a), st->pack_mv / 10, 100, 2);
		fmt_fixed(b, sizeof(b), st->pack_ma / 10, 100, 2);
		snprintf(buf, sizeof(buf), "%s V", a);
		lv_label_set_text(r_u, buf);
		snprintf(buf, sizeof(buf), "%s A", b);
		lv_label_set_text(r_i, buf);
	} else {
		lv_label_set_text(r_u, "-- V");
		lv_label_set_text(r_i, "-- A");
	}
	if (st->valid & BM_SV_SOH) {
		fmt_fixed(a, sizeof(a), st->soh_dpct, 10, 1);
		snprintf(buf, sizeof(buf), "SoH %s %%", a);
	} else if (st->valid & BM_SV_CYCLES) {
		snprintf(buf, sizeof(buf), "Zyklen %u", st->cycles);
	} else {
		snprintf(buf, sizeof(buf), "%s", bm_serial_phase_name(s->phase));
	}
	lv_label_set_text(r_soh, buf);
	lv_label_set_text(r_desc, s->flavor_str);

	if ((st->valid & BM_SV_CELLS) && st->n_cells) {
		unsigned int imin, imax;

		cells_minmax(st, &imin, &imax);
		snprintf(buf, sizeof(buf), "%u Zellen  %u.%03u-%u.%03u V  dV %u mV", st->n_cells,
			 st->cell_mv[imin] / 1000, st->cell_mv[imin] % 1000, st->cell_mv[imax] / 1000,
			 st->cell_mv[imax] % 1000, st->cell_mv[imax] - st->cell_mv[imin]);
	} else {
		snprintf(buf, sizeof(buf), "Zellen: --");
	}
	lv_label_set_text(r_cells, buf);

	if (st->valid & BM_SV_CAP) {
		fmt_fixed(a, sizeof(a), (int32_t)(st->remain_mah / 100), 10, 1);
		fmt_fixed(b, sizeof(b), (int32_t)(st->full_mah / 100), 10, 1);
		snprintf(buf, sizeof(buf), "%s / %s Ah   Zyklen %u", a, b, st->cycles);
	} else {
		snprintf(buf, sizeof(buf), "Kapazitaet: --");
	}
	lv_label_set_text(r_cap, buf);

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
		snprintf(buf, sizeof(buf), "%s", (st->valid & BM_SV_SERIAL) ? st->serial : "");
	}
	lv_label_set_text(r_limits, buf);

	snprintf(buf, sizeof(buf), "%u bd  ok %u  err %u  %s", (unsigned int)s->baud,
		 (unsigned int)s->ok_count, (unsigned int)s->err_count, s->last_err);
	lv_label_set_text(r_stats, buf);
}

static void refresh_cells(const struct bm_proto *snap, const struct bm_serial *s)
{
	char buf[128], a[16], b[16];
	const struct bm_serial_state *st = &s->st;

	if (app.rs485_present && (st->valid & BM_SV_CELLS) && st->n_cells) {
		unsigned int imin, imax;

		cells_minmax(st, &imin, &imax);
		lv_obj_remove_flag(z_chart, LV_OBJ_FLAG_HIDDEN);
		if (lv_chart_get_point_count(z_chart) != st->n_cells) {
			lv_chart_set_point_count(z_chart, st->n_cells);
		}
		int32_t lo = st->cell_mv[imin], hi = st->cell_mv[imax];
		int32_t span = hi - lo < 100 ? 100 : hi - lo;

		lv_chart_set_axis_range(z_chart, LV_CHART_AXIS_PRIMARY_Y, lo - span / 2, hi + span / 2);
		for (unsigned int i = 0; i < st->n_cells; i++) {
			lv_chart_set_value_by_id(z_chart, z_series, i, st->cell_mv[i]);
		}
		lv_chart_refresh(z_chart);
		snprintf(buf, sizeof(buf), "min %u.%03u V (#%u)   max %u.%03u V (#%u)\ndV %u mV",
			 st->cell_mv[imin] / 1000, st->cell_mv[imin] % 1000, imin + 1,
			 st->cell_mv[imax] / 1000, st->cell_mv[imax] % 1000, imax + 1,
			 st->cell_mv[imax] - st->cell_mv[imin]);
		lv_label_set_text(z_info, buf);

		size_t len = (size_t)snprintf(buf, sizeof(buf), "Temp:");

		for (unsigned int i = 0; i < st->n_temps && len < sizeof(buf) - 12; i++) {
			fmt_fixed(a, sizeof(a), st->temp_dc[i], 10, 1);
			len += (size_t)snprintf(buf + len, sizeof(buf) - len, " %s", a);
		}
		lv_label_set_text(z_temps, st->n_temps ? buf : "");
		snprintf(buf, sizeof(buf), "Quelle: RS485 %s", bm_serial_flavor_name(s->flavor));
		lv_label_set_text(z_src, buf);
		return;
	}

	lv_obj_add_flag(z_chart, LV_OBJ_FLAG_HIDDEN);
	if (snap->st.valid & BM_VALID_CELLS) {
		const struct bm_pack_state *p = &snap->st;

		snprintf(buf, sizeof(buf), "min %u mV %s   max %u mV %s\ndV %d mV", p->cell_min_mv,
			 p->cell_min_name, p->cell_max_mv, p->cell_max_name,
			 (int)p->cell_max_mv - (int)p->cell_min_mv);
		lv_label_set_text(z_info, buf);
		fmt_fixed(a, sizeof(a), p->temp_min_dc, 10, 1);
		fmt_fixed(b, sizeof(b), p->temp_max_dc, 10, 1);
		snprintf(buf, sizeof(buf), "Temp %s ... %s C", a, b);
		lv_label_set_text(z_temps, buf);
		lv_label_set_text(z_src, "Quelle: CAN (nur Minimum/Maximum)");
	} else {
		lv_label_set_text(z_info, "Keine Zelldaten.\nZellspannungen kommen ueber RS485 oder die\n"
					  "Victron-/HV-Erweiterung am CAN.");
		lv_label_set_text(z_temps, "");
		lv_label_set_text(z_src, "");
	}
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

static void select_tab(const struct bm_proto *snap, const struct bm_serial *ser, uint32_t now)
{
	static uint32_t presses_seen;

	if (button_presses != presses_seen) {
		presses_seen = button_presses;
		uint32_t next = (lv_tabview_get_tab_active(tabview) + 1) % lv_tabview_get_tab_count(tabview);

		lv_tabview_set_active(tabview, next, LV_ANIM_ON);
		manual_until = now + MANUAL_HOLD_MS;
		return;
	}
	if ((int32_t)(now - manual_until) <= 0) {
		return;
	}
	int want = -1;

	if (snap->link_up) {
		want = TAB_CAN;
	} else if (app.rs485_present && ser->link_up) {
		want = TAB_RS485;
	}
	if (want >= 0 && want != auto_tab) {
		auto_tab = want;
		lv_tabview_set_active(tabview, (uint32_t)want, LV_ANIM_ON);
	}
}

static void refresh(void)
{
	static struct bm_proto snap;
	static struct bm_serial ser;
	uint32_t now = k_uptime_get_32();

	app_proto_snapshot(&snap);
	if (app.rs485_present) {
		app_serial_snapshot(&ser);
	}
	select_tab(&snap, &ser, now);
	switch (lv_tabview_get_tab_active(tabview)) {
	case TAB_RS485:
		refresh_rs485(&ser);
		break;
	case TAB_CELLS:
		refresh_cells(&snap, &ser);
		break;
	default:
		refresh_can(&snap);
		break;
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
	build_ui();
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
