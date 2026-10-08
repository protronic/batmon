#include "bm_proto.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

struct ap {
	char *buf;
	size_t len;
	size_t pos;
	bool ovf;
};

static void ap_raw(struct ap *a, const char *fmt, ...)
{
	if (a->ovf) {
		return;
	}
	va_list args;

	va_start(args, fmt);
	int n = vsnprintf(a->buf + a->pos, a->len - a->pos, fmt, args);
	va_end(args);
	if (n < 0 || (size_t)n >= a->len - a->pos) {
		a->ovf = true;
		a->pos = a->len - 1;
		a->buf[a->pos] = '\0';
		return;
	}
	a->pos += (size_t)n;
}

/* value is in units of 1/div; prints with `digits` decimals */
static void ap_fixed(struct ap *a, const char *key, int32_t v, uint32_t div, int digits)
{
	bool neg = v < 0;
	uint32_t u = neg ? (uint32_t)(-(int64_t)v) : (uint32_t)v;

	ap_raw(a, "\"%s\":%s%u.%0*u,", key, neg ? "-" : "", (unsigned int)(u / div), digits,
	       (unsigned int)(u % div));
}

static void ap_int(struct ap *a, const char *key, int32_t v)
{
	ap_raw(a, "\"%s\":%d,", key, (int)v);
}

static void ap_bool(struct ap *a, const char *key, bool v)
{
	ap_raw(a, "\"%s\":%s,", key, v ? "true" : "false");
}

static void ap_str(struct ap *a, const char *key, const char *s)
{
	ap_raw(a, "\"%s\":\"", key);
	for (; *s && !a->ovf; s++) {
		if (*s == '"' || *s == '\\') {
			ap_raw(a, "\\%c", *s);
		} else {
			ap_raw(a, "%c", *s);
		}
	}
	ap_raw(a, "\",");
}

static void ap_open(struct ap *a, const char *key, char c)
{
	if (key) {
		ap_raw(a, "\"%s\":%c", key, c);
	} else {
		ap_raw(a, "%c", c);
	}
}

static void ap_close(struct ap *a, char c)
{
	if (!a->ovf && a->pos > 0 && a->buf[a->pos - 1] == ',') {
		a->pos--;
	}
	ap_raw(a, "%c,", c);
}

static void ap_bits(struct ap *a, const char *key, const uint8_t bytes[2],
		    const char *(*name)(unsigned int, unsigned int))
{
	ap_open(a, key, '[');
	for (unsigned int b = 0; b < 2; b++) {
		for (unsigned int bit = 0; bit < 8; bit++) {
			if (bytes[b] & (1u << bit)) {
				const char *n = name(b, bit);

				if (n) {
					ap_raw(a, "\"%s\",", n);
				} else {
					ap_raw(a, "\"b%u.%u\",", b, bit);
				}
			}
		}
	}
	ap_close(a, ']');
}

int bm_json_telemetry(const struct bm_proto *p, uint32_t now_ms, char *buf, size_t len)
{
	if (len < 2) {
		return -1;
	}
	struct ap a = { .buf = buf, .len = len, .pos = 0, .ovf = false };
	const struct bm_pack_state *st = &p->st;
	const struct bm_detect *det = &p->det;

	ap_open(&a, NULL, '{');
	ap_int(&a, "ts", (int32_t)now_ms);
	ap_bool(&a, "link", p->link_up);
	ap_int(&a, "frames", (int32_t)p->frames_total);
	if (p->frames_total > 1 && p->last_ms > p->first_ms) {
		ap_fixed(&a, "rate", (int32_t)((uint64_t)p->frames_total * 10000u /
						(p->last_ms - p->first_ms)), 10, 1);
	}

	ap_open(&a, "proto", '{');
	ap_str(&a, "family", bm_family_name(det->family));
	ap_str(&a, "vendor", bm_vendor_name(det->vendor));
	ap_str(&a, "flavor", det->flavor);
	ap_str(&a, "mfr", st->manufacturer);
	ap_int(&a, "conf", det->confidence);
	ap_raw(&a, "\"hints\":\"0x%03x\",", (unsigned int)det->hints);
	ap_open(&a, "ids", '[');
	for (unsigned int i = 0; i < p->n_ids; i++) {
		ap_raw(&a, "\"%x:%u\",", (unsigned int)p->ids[i].id, (unsigned int)p->ids[i].count);
	}
	ap_close(&a, ']');
	ap_close(&a, '}');

	ap_open(&a, "inv", '{');
	ap_bool(&a, "seen", p->inv.seen);
	ap_str(&a, "ident", p->inv.ident);
	ap_int(&a, "hb", (int32_t)p->inv.hb_count);
	ap_close(&a, '}');

	ap_open(&a, "pack", '{');
	if (st->valid & BM_VALID_MEAS) {
		ap_fixed(&a, "u", st->pack_mv / 10, 100, 2);
		ap_fixed(&a, "i", st->pack_ma / 100, 10, 1);
		ap_fixed(&a, "t", st->temp_dc, 10, 1);
	}
	if (st->valid & BM_VALID_SOC) {
		ap_int(&a, "soc", st->soc_pct);
		ap_int(&a, "soh", st->soh_pct);
	}
	if (st->valid & BM_VALID_LIMITS) {
		ap_fixed(&a, "cvl", st->cvl_mv / 10, 100, 2);
		ap_fixed(&a, "dvl", st->dvl_mv / 10, 100, 2);
		ap_fixed(&a, "ccl", st->ccl_ma / 100, 10, 1);
		ap_fixed(&a, "dcl", st->dcl_ma / 100, 10, 1);
	}
	if (st->valid & BM_VALID_REQUEST) {
		ap_bool(&a, "charge_en", st->charge_en);
		ap_bool(&a, "discharge_en", st->discharge_en);
		ap_bool(&a, "force_charge", st->force_charge_1 || st->force_charge_2);
		ap_bool(&a, "full_req", st->full_charge_req);
	}
	if (st->valid & BM_VALID_STATUS) {
		ap_int(&a, "modules", st->module_count);
		ap_raw(&a, "\"prot_raw\":\"%02x%02x\",\"alarm_raw\":\"%02x%02x\",", st->prot[0],
		       st->prot[1], st->alarm[0], st->alarm[1]);
		if (det->family != BM_FAMILY_PYLON_HV) {
			ap_bits(&a, "prot", st->prot, bm_prot_bit_name);
			ap_bits(&a, "alarm", st->alarm, bm_alarm_bit_name);
		}
	}
	if (det->hints & BM_HINT_35A_PRESENT) {
		ap_raw(&a, "\"alarm35a\":\"");
		for (int i = 0; i < 8; i++) {
			ap_raw(&a, "%02x", st->alarm35a[i]);
		}
		ap_raw(&a, "\",");
	}
	ap_close(&a, '}');

	if (st->valid & BM_VALID_CELLS) {
		ap_open(&a, "cells", '{');
		ap_int(&a, "min_mv", st->cell_min_mv);
		ap_int(&a, "max_mv", st->cell_max_mv);
		ap_str(&a, "min_id", st->cell_min_name);
		ap_str(&a, "max_id", st->cell_max_name);
		ap_fixed(&a, "tmin", st->temp_min_dc, 10, 1);
		ap_fixed(&a, "tmax", st->temp_max_dc, 10, 1);
		ap_str(&a, "tmin_id", st->temp_min_name);
		ap_str(&a, "tmax_id", st->temp_max_name);
		ap_close(&a, '}');
	}
	if (st->valid & BM_VALID_MODULES) {
		ap_open(&a, "mod", '{');
		ap_int(&a, "online", st->mod_online);
		ap_int(&a, "block_chg", st->mod_block_chg);
		ap_int(&a, "block_dis", st->mod_block_dis);
		ap_int(&a, "offline", st->mod_offline);
		ap_close(&a, '}');
	}
	if (st->valid & (BM_VALID_INFO | BM_VALID_NAME | BM_VALID_CAPACITY | BM_VALID_ENERGY |
			 BM_VALID_HV_STATUS)) {
		ap_open(&a, "info", '{');
		if (st->valid & BM_VALID_NAME) {
			ap_str(&a, "name", st->name);
		}
		if (st->valid & BM_VALID_INFO) {
			ap_int(&a, "chem", st->chemistry);
			ap_int(&a, "hw", st->hw_version);
			ap_int(&a, "sw", st->sw_version);
			ap_int(&a, "cap_ah", st->capacity_ah);
		}
		if (st->valid & BM_VALID_CAPACITY) {
			ap_int(&a, "installed_ah", st->installed_ah);
		}
		if (st->valid & BM_VALID_ENERGY) {
			ap_int(&a, "e_chg_raw", (int32_t)st->energy_chg_raw);
			ap_int(&a, "e_dis_raw", (int32_t)st->energy_dis_raw);
		}
		if (st->valid & BM_VALID_HV_STATUS) {
			ap_int(&a, "hv_state", st->hv_state);
			ap_int(&a, "hv_cycles", st->hv_cycles);
		}
		ap_close(&a, '}');
	}
	ap_close(&a, '}');
	if (a.ovf) {
		return -1;
	}
	/* drop the trailing comma left by the final ap_close */
	if (a.pos > 0 && a.buf[a.pos - 1] == ',') {
		a.buf[--a.pos] = '\0';
	}
	return (int)a.pos;
}

int bm_json_flat(const struct bm_proto *p, uint32_t now_ms, char *buf, size_t len)
{
	if (len < 2) {
		return -1;
	}
	struct ap a = { .buf = buf, .len = len, .pos = 0, .ovf = false };
	const struct bm_pack_state *st = &p->st;
	const struct bm_detect *det = &p->det;

	ap_open(&a, NULL, '{');
	ap_int(&a, "uptime_s", (int32_t)(now_ms / 1000u));
	ap_bool(&a, "link", p->link_up);
	ap_int(&a, "frames", (int32_t)p->frames_total);
	ap_str(&a, "family", bm_family_name(det->family));
	ap_str(&a, "vendor", bm_vendor_name(det->vendor));
	ap_str(&a, "flavor", det->flavor);
	ap_str(&a, "mfr", st->manufacturer);
	ap_int(&a, "conf", det->confidence);
	ap_bool(&a, "inv_seen", p->inv.seen);
	if (st->valid & BM_VALID_MEAS) {
		ap_fixed(&a, "u", st->pack_mv / 10, 100, 2);
		ap_fixed(&a, "i", st->pack_ma / 100, 10, 1);
		ap_fixed(&a, "t", st->temp_dc, 10, 1);
	}
	if (st->valid & BM_VALID_SOC) {
		ap_int(&a, "soc", st->soc_pct);
		ap_int(&a, "soh", st->soh_pct);
	}
	if (st->valid & BM_VALID_LIMITS) {
		ap_fixed(&a, "cvl", st->cvl_mv / 10, 100, 2);
		ap_fixed(&a, "dvl", st->dvl_mv / 10, 100, 2);
		ap_fixed(&a, "ccl", st->ccl_ma / 100, 10, 1);
		ap_fixed(&a, "dcl", st->dcl_ma / 100, 10, 1);
	}
	if (st->valid & BM_VALID_REQUEST) {
		ap_bool(&a, "charge_en", st->charge_en);
		ap_bool(&a, "discharge_en", st->discharge_en);
		ap_bool(&a, "force_charge", st->force_charge_1 || st->force_charge_2);
	}
	if (st->valid & BM_VALID_STATUS) {
		ap_int(&a, "modules", st->module_count);
		ap_raw(&a, "\"prot_raw\":\"%02x%02x\",\"alarm_raw\":\"%02x%02x\",", st->prot[0],
		       st->prot[1], st->alarm[0], st->alarm[1]);
	}
	if (st->valid & BM_VALID_CELLS) {
		ap_int(&a, "cell_min_mv", st->cell_min_mv);
		ap_int(&a, "cell_max_mv", st->cell_max_mv);
	}
	ap_close(&a, '}');
	if (a.ovf) {
		return -1;
	}
	if (a.pos > 0 && a.buf[a.pos - 1] == ',') {
		a.buf[--a.pos] = '\0';
	}
	return (int)a.pos;
}
