#include "bm_proto.h"

#include <stdio.h>
#include <string.h>

#define LINK_TIMEOUT_MS   5000u
#define DETECT_PERIOD_MS  1000u

static uint16_t le16(const uint8_t *d)
{
	return (uint16_t)(d[0] | ((uint16_t)d[1] << 8));
}

static int16_t le16s(const uint8_t *d)
{
	return (int16_t)le16(d);
}

static uint32_t le32(const uint8_t *d)
{
	return (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) |
	       ((uint32_t)d[3] << 24);
}

static uint16_t be16(const uint8_t *d)
{
	return (uint16_t)(((uint16_t)d[0] << 8) | d[1]);
}

static void copy_ascii(char *dst, size_t dst_len, const uint8_t *src, size_t n)
{
	size_t o = 0;

	for (size_t i = 0; i < n && o + 1 < dst_len; i++) {
		uint8_t c = src[i];

		if (c == 0) {
			break;
		}
		if (c < 0x20 || c > 0x7e) {
			c = '?';
		}
		dst[o++] = (char)c;
	}
	while (o > 0 && dst[o - 1] == ' ') {
		o--;
	}
	dst[o] = '\0';
}

static void copy_ascii_at(char *dst, size_t dst_len, size_t at, const uint8_t *src, size_t n)
{
	if (at >= dst_len - 1) {
		return;
	}
	char tmp[BM_STR_LEN];

	copy_ascii(tmp, sizeof(tmp), src, n);
	/* keep already stored prefix, pad with spaces up to the insert position */
	size_t cur = strlen(dst);

	while (cur < at && cur + 1 < dst_len) {
		dst[cur++] = ' ';
	}
	dst[cur] = '\0';
	size_t room = dst_len - 1 - at;
	size_t l = strlen(tmp);

	if (l > room) {
		l = room;
	}
	memcpy(dst + at, tmp, l);
	dst[at + l] = '\0';
}

void bm_proto_init(struct bm_proto *p)
{
	memset(p, 0, sizeof(*p));
	strcpy(p->det.flavor, "unbekannt");
}

static struct bm_id_stat *track_id(struct bm_proto *p, uint32_t id, bool ext, uint8_t dlc,
				   uint32_t now_ms)
{
	for (unsigned int i = 0; i < p->n_ids; i++) {
		struct bm_id_stat *s = &p->ids[i];

		if (s->id == id && s->ext == ext) {
			s->count++;
			s->last_ms = now_ms;
			if (dlc > s->dlc) {
				s->dlc = dlc;
			}
			return s;
		}
	}
	if (p->n_ids >= BM_MAX_IDS) {
		return NULL;
	}
	struct bm_id_stat *s = &p->ids[p->n_ids++];

	s->id = id;
	s->ext = ext;
	s->dlc = dlc;
	s->count = 1;
	s->last_ms = now_ms;
	return s;
}

static const struct bm_id_stat *find_id(const struct bm_proto *p, uint32_t id)
{
	for (unsigned int i = 0; i < p->n_ids; i++) {
		if (p->ids[i].id == id) {
			return &p->ids[i];
		}
	}
	return NULL;
}

static bool seen(const struct bm_proto *p, uint32_t id)
{
	return find_id(p, id) != NULL;
}

static bool seen_range(const struct bm_proto *p, uint32_t lo, uint32_t hi)
{
	for (unsigned int i = 0; i < p->n_ids; i++) {
		if (p->ids[i].id >= lo && p->ids[i].id <= hi) {
			return true;
		}
	}
	return false;
}

/* ---- Pylontech LV / SMA / Victron (standard identifiers) ------------------------------ */

static void dec_351(struct bm_pack_state *st, struct bm_detect *det, const uint8_t *d, uint8_t dlc)
{
	if (dlc < 6) {
		return;
	}
	st->cvl_mv = (int32_t)le16(d) * 100;
	st->ccl_ma = (int32_t)le16s(d + 2) * 100;
	st->dcl_ma = (int32_t)le16s(d + 4) * 100;
	if (dlc >= 8) {
		st->dvl_mv = (int32_t)le16(d + 6) * 100;
		det->hints |= BM_HINT_351_DLC8;
	}
	st->valid |= BM_VALID_LIMITS;
}

static void dec_355(struct bm_pack_state *st, const uint8_t *d, uint8_t dlc)
{
	if (dlc < 2) {
		return;
	}
	uint16_t soc = le16(d);

	st->soc_pct = soc > 100 ? 100 : (uint8_t)soc;
	if (dlc >= 4) {
		uint16_t soh = le16(d + 2);

		st->soh_pct = soh > 100 ? 100 : (uint8_t)soh;
	}
	st->valid |= BM_VALID_SOC;
}

static void dec_356(struct bm_pack_state *st, const uint8_t *d, uint8_t dlc)
{
	if (dlc < 4) {
		return;
	}
	st->pack_mv = (int32_t)le16s(d) * 10;
	st->pack_ma = (int32_t)le16s(d + 2) * 100;
	if (dlc >= 6) {
		st->temp_dc = le16s(d + 4);
	}
	st->valid |= BM_VALID_MEAS;
}

static void dec_359(struct bm_pack_state *st, struct bm_detect *det, const uint8_t *d, uint8_t dlc)
{
	if (dlc < 4) {
		return;
	}
	st->prot[0] = d[0];
	st->prot[1] = d[1];
	st->alarm[0] = d[2];
	st->alarm[1] = d[3];
	if (dlc >= 5) {
		st->module_count = d[4];
		if (d[4] > 1) {
			det->hints |= BM_HINT_MULTI_MODULE;
		}
	}
	if (dlc >= 7 && d[5] == 'P' && d[6] == 'N') {
		det->hints |= BM_HINT_PN_MARKER;
	} else if (dlc < 7) {
		det->hints |= BM_HINT_359_SHORT;
	}
	st->valid |= BM_VALID_STATUS;
}

static void dec_35c(struct bm_pack_state *st, struct bm_detect *det, const uint8_t *d, uint8_t dlc)
{
	if (dlc < 1) {
		return;
	}
	st->charge_en = (d[0] & 0x80) != 0;
	st->discharge_en = (d[0] & 0x40) != 0;
	st->force_charge_1 = (d[0] & 0x20) != 0;
	st->force_charge_2 = (d[0] & 0x10) != 0;
	st->full_charge_req = (d[0] & 0x08) != 0;
	if (dlc > 2) {
		det->hints |= BM_HINT_35C_EXTRA;
	}
	st->valid |= BM_VALID_REQUEST;
}

static void dec_35e(struct bm_pack_state *st, struct bm_detect *det, const uint8_t *d, uint8_t dlc)
{
	copy_ascii(st->manufacturer, sizeof(st->manufacturer), d, dlc);
	if (st->manufacturer[0] != '\0') {
		det->hints |= BM_HINT_MFR_STRING;
	}
	st->valid |= BM_VALID_MFR;
}

static void dec_35f(struct bm_pack_state *st, struct bm_detect *det, const uint8_t *d, uint8_t dlc)
{
	if (dlc < 8) {
		return;
	}
	st->chemistry = le16(d);
	st->hw_version = le16(d + 2);
	st->capacity_ah = le16(d + 4);
	st->sw_version = le16(d + 6);
	det->hints |= BM_HINT_35F_PRESENT;
	st->valid |= BM_VALID_INFO;
}

static int32_t kelvin_to_dc(uint16_t k)
{
	return (int32_t)k * 10 - 2731;
}

static void dec_victron(struct bm_pack_state *st, struct bm_detect *det, uint32_t id, const uint8_t *d,
			uint8_t dlc)
{
	det->hints |= BM_HINT_VICTRON_EXT;
	switch (id) {
	case 0x370:
		copy_ascii_at(st->name, sizeof(st->name), 0, d, dlc);
		st->valid |= BM_VALID_NAME;
		break;
	case 0x371:
		copy_ascii_at(st->name, sizeof(st->name), 8, d, dlc);
		st->valid |= BM_VALID_NAME;
		break;
	case 0x372:
		if (dlc >= 8) {
			st->mod_online = le16(d);
			st->mod_block_chg = le16(d + 2);
			st->mod_block_dis = le16(d + 4);
			st->mod_offline = le16(d + 6);
			st->valid |= BM_VALID_MODULES;
		}
		break;
	case 0x373:
		if (dlc >= 8) {
			st->cell_min_mv = le16(d);
			st->cell_max_mv = le16(d + 2);
			st->temp_min_dc = kelvin_to_dc(le16(d + 4));
			st->temp_max_dc = kelvin_to_dc(le16(d + 6));
			st->valid |= BM_VALID_CELLS;
		}
		break;
	case 0x374:
		copy_ascii(st->cell_min_name, sizeof(st->cell_min_name), d, dlc);
		break;
	case 0x375:
		copy_ascii(st->cell_max_name, sizeof(st->cell_max_name), d, dlc);
		break;
	case 0x376:
		copy_ascii(st->temp_min_name, sizeof(st->temp_min_name), d, dlc);
		break;
	case 0x377:
		copy_ascii(st->temp_max_name, sizeof(st->temp_max_name), d, dlc);
		break;
	case 0x378:
		if (dlc >= 8) {
			st->energy_chg_raw = le32(d);
			st->energy_dis_raw = le32(d + 4);
			st->valid |= BM_VALID_ENERGY;
		}
		break;
	case 0x379:
		if (dlc >= 2) {
			st->installed_ah = le16(d);
			st->valid |= BM_VALID_CAPACITY;
		}
		break;
	default:
		break;
	}
}

/* ---- Pylontech HV (0x4210.., big-endian, offsets 30000 / 1000) ------------------------- */

static void snprintf_id(char *dst, size_t len, unsigned int id)
{
	snprintf(dst, len, "%u", id);
}

static void dec_hv(struct bm_pack_state *st, struct bm_detect *det, uint32_t id, const uint8_t *d,
		   uint8_t dlc)
{
	det->hints |= BM_HINT_HV_IDS;
	if (dlc < 8) {
		return;
	}
	switch (id) {
	case 0x4210:
		st->pack_mv = (int32_t)be16(d) * 100;
		st->pack_ma = ((int32_t)be16(d + 2) - 30000) * 100;
		st->temp_dc = (int32_t)be16(d + 4) - 1000;
		st->soc_pct = d[6] > 100 ? 100 : d[6];
		st->soh_pct = d[7] > 100 ? 100 : d[7];
		st->valid |= BM_VALID_MEAS | BM_VALID_SOC;
		break;
	case 0x4220:
		st->cvl_mv = (int32_t)be16(d) * 100;
		st->dvl_mv = (int32_t)be16(d + 2) * 100;
		st->ccl_ma = ((int32_t)be16(d + 4) - 30000) * 100;
		st->dcl_ma = ((int32_t)be16(d + 6) - 30000) * 100;
		st->valid |= BM_VALID_LIMITS;
		break;
	case 0x4230:
		st->cell_max_mv = be16(d);
		st->cell_min_mv = be16(d + 2);
		snprintf_id(st->cell_max_name, sizeof(st->cell_max_name), be16(d + 4));
		snprintf_id(st->cell_min_name, sizeof(st->cell_min_name), be16(d + 6));
		st->valid |= BM_VALID_CELLS;
		break;
	case 0x4240:
		st->temp_max_dc = (int32_t)be16(d) - 1000;
		st->temp_min_dc = (int32_t)be16(d + 2) - 1000;
		snprintf_id(st->temp_max_name, sizeof(st->temp_max_name), be16(d + 4));
		snprintf_id(st->temp_min_name, sizeof(st->temp_min_name), be16(d + 6));
		st->valid |= BM_VALID_CELLS;
		break;
	case 0x4250:
		st->hv_state = d[0];
		st->hv_cycles = be16(d + 1);
		st->valid |= BM_VALID_HV_STATUS;
		break;
	case 0x4280:
		st->charge_en = d[0] != 0xAA;
		st->discharge_en = d[1] != 0xAA;
		st->valid |= BM_VALID_REQUEST;
		break;
	case 0x4290:
		st->prot[0] = d[0];
		st->prot[1] = d[1];
		st->alarm[0] = d[2];
		st->alarm[1] = d[3];
		st->valid |= BM_VALID_STATUS;
		break;
	default:
		break;
	}
}

void bm_proto_feed(struct bm_proto *p, uint32_t id, bool ext, const uint8_t *data, uint8_t dlc,
		   uint32_t now_ms)
{
	if (dlc > 8) {
		dlc = 8;
	}
	struct bm_pack_state *st = &p->st;
	struct bm_detect *det = &p->det;

	/* inverter side first: it must not count as battery traffic */
	if (!ext && id == 0x305) {
		track_id(p, id, ext, dlc, now_ms);
		p->inv.seen = true;
		p->inv.last_hb_ms = now_ms;
		p->inv.hb_count++;
		det->hints |= BM_HINT_INV_HEARTBEAT;
		return;
	}
	if (!ext && id == 0x307) {
		track_id(p, id, ext, dlc, now_ms);
		p->inv.seen = true;
		copy_ascii(p->inv.ident, sizeof(p->inv.ident), data, dlc);
		det->hints |= BM_HINT_INV_IDENT;
		return;
	}

	track_id(p, id, ext, dlc, now_ms);
	if (p->frames_total == 0) {
		p->first_ms = now_ms;
	}
	p->frames_total++;
	p->last_ms = now_ms;
	p->link_up = true;

	if (ext) {
		if (id >= 0x4210 && id <= 0x42FF) {
			dec_hv(st, det, id, data, dlc);
		}
		return;
	}

	switch (id) {
	case 0x351:
		dec_351(st, det, data, dlc);
		break;
	case 0x355:
		dec_355(st, data, dlc);
		break;
	case 0x356:
		dec_356(st, data, dlc);
		break;
	case 0x359:
		dec_359(st, det, data, dlc);
		break;
	case 0x35A:
		memset(st->alarm35a, 0, sizeof(st->alarm35a));
		memcpy(st->alarm35a, data, dlc);
		det->hints |= BM_HINT_35A_PRESENT;
		break;
	case 0x35C:
		dec_35c(st, det, data, dlc);
		break;
	case 0x35E:
		dec_35e(st, det, data, dlc);
		break;
	case 0x35F:
		dec_35f(st, det, data, dlc);
		break;
	default:
		if (id >= 0x370 && id <= 0x381) {
			dec_victron(st, det, id, data, dlc);
		}
		break;
	}
}

void bm_proto_tick(struct bm_proto *p, uint32_t now_ms)
{
	if (p->frames_total > 0 && (uint32_t)(now_ms - p->last_ms) > LINK_TIMEOUT_MS) {
		p->link_up = false;
	}
	if (p->inv.seen && (uint32_t)(now_ms - p->inv.last_hb_ms) > LINK_TIMEOUT_MS &&
	    p->inv.hb_count > 0) {
		p->inv.seen = false;
	}
	if ((uint32_t)(now_ms - p->last_detect_ms) >= DETECT_PERIOD_MS) {
		p->last_detect_ms = now_ms;
		bm_detect_run(p);
	}
}

/* ---- classification ------------------------------------------------------------------- */

static bool prefix_ci(const char *s, const char *prefix)
{
	for (; *prefix; s++, prefix++) {
		char a = *s, b = *prefix;

		if (a >= 'a' && a <= 'z') {
			a = (char)(a - 'a' + 'A');
		}
		if (a != b) {
			return false;
		}
	}
	return true;
}

enum bm_vendor bm_vendor_from_string(const char *mfr)
{
	static const struct {
		const char *prefix;
		enum bm_vendor vendor;
	} tab[] = {
		{ "PYLON", BM_VENDOR_PYLONTECH }, { "PACE", BM_VENDOR_PACE },
		{ "SEPLOS", BM_VENDOR_SEPLOS },   { "JK", BM_VENDOR_JK },
		{ "JIKONG", BM_VENDOR_JK },       { "FELIC", BM_VENDOR_FELICITY },
		{ "BYD", BM_VENDOR_BYD },         { "SMA", BM_VENDOR_SMA },
		{ "DYNESS", BM_VENDOR_DYNESS },   { "HUBBLE", BM_VENDOR_HUBBLE },
		{ "EG4", BM_VENDOR_EG4 },         { "DEYE", BM_VENDOR_DEYE },
		{ "GROWATT", BM_VENDOR_GROWATT }, { "GOODWE", BM_VENDOR_GOODWE },
		{ "LUX", BM_VENDOR_LUX },
	};

	if (mfr == NULL || mfr[0] == '\0') {
		return BM_VENDOR_UNKNOWN;
	}
	for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
		if (prefix_ci(mfr, tab[i].prefix)) {
			return tab[i].vendor;
		}
	}
	return BM_VENDOR_UNKNOWN;
}

static void set_flavor(struct bm_detect *det, const char *txt)
{
	size_t n = strlen(txt);

	if (n >= sizeof(det->flavor)) {
		n = sizeof(det->flavor) - 1;
	}
	memcpy(det->flavor, txt, n);
	det->flavor[n] = '\0';
}

static uint8_t clamp100(unsigned int v)
{
	return v > 100 ? 100 : (uint8_t)v;
}

void bm_detect_run(struct bm_proto *p)
{
	struct bm_detect *det = &p->det;
	const uint32_t h = det->hints;
	unsigned int conf = 0;

	det->vendor = bm_vendor_from_string(p->st.manufacturer);

	if (p->frames_total == 0) {
		det->family = BM_FAMILY_UNKNOWN;
		det->confidence = 0;
		set_flavor(det, "kein Batterieverkehr");
		return;
	}

	if (seen_range(p, 0x4210, 0x42FF)) {
		det->family = BM_FAMILY_PYLON_HV;
		conf = 40;
		static const uint32_t hv_ids[] = { 0x4210, 0x4220, 0x4230, 0x4240, 0x4250, 0x4280, 0x4290 };

		for (size_t i = 0; i < sizeof(hv_ids) / sizeof(hv_ids[0]); i++) {
			if (seen(p, hv_ids[i])) {
				conf += 8;
			}
		}
		set_flavor(det, det->vendor == BM_VENDOR_UNKNOWN
				      ? "Pylontech-HV (Force H/Powercube) oder kompatibel"
				      : "Pylontech-HV-Dialekt");
	} else if (seen(p, 0x351) && seen(p, 0x355) && seen(p, 0x356)) {
		conf = 50;
		if (seen(p, 0x359)) {
			conf += 10;
		}
		if (seen(p, 0x35C)) {
			conf += 10;
		}
		if (seen(p, 0x35E)) {
			conf += 10;
		}
		if (h & BM_HINT_PN_MARKER) {
			conf += 10;
		}
		if (p->frames_total > 20) {
			conf += 10;
		}

		if (h & BM_HINT_VICTRON_EXT) {
			det->family = BM_FAMILY_VICTRON_EXT;
			switch (det->vendor) {
			case BM_VENDOR_BYD:
				set_flavor(det, "BYD Battery-Box LV (Victron-Dialekt)");
				break;
			case BM_VENDOR_PYLONTECH:
				set_flavor(det, "Pylontech mit Victron-Erweiterung");
				break;
			case BM_VENDOR_UNKNOWN:
				set_flavor(det, "Victron-CAN-BMS-Dialekt, Hersteller unbekannt");
				break;
			default:
				set_flavor(det, "Victron-CAN-BMS-Dialekt");
				break;
			}
		} else if (((h & BM_HINT_35A_PRESENT) || (h & BM_HINT_35F_PRESENT)) && !seen(p, 0x35C) &&
			   !seen(p, 0x359)) {
			det->family = BM_FAMILY_SMA_LV;
			set_flavor(det, det->vendor == BM_VENDOR_BYD ? "BYD (SMA-Li-Ion-Dialekt)"
								     : "SMA-Li-Ion-Dialekt (BYD/LG-Stil)");
		} else {
			det->family = BM_FAMILY_PYLON_LV;
			switch (det->vendor) {
			case BM_VENDOR_PYLONTECH:
				if (h & BM_HINT_PN_MARKER) {
					set_flavor(det, (h & BM_HINT_351_DLC8)
							      ? "Pylontech US-Serie (Protokoll V1.3)"
							      : "Pylontech (Protokoll V1.2)");
				} else {
					set_flavor(det, "Pylontech-kompatibel, Emulation (PACE/Seplos/JK?)");
				}
				break;
			case BM_VENDOR_PACE:
				set_flavor(det, "PACE-BMS (FM-Solar/DelongTop, Gobel, SOK ...)");
				break;
			case BM_VENDOR_SEPLOS:
				set_flavor(det, "Seplos-BMS im Pylontech-Modus");
				break;
			case BM_VENDOR_JK:
				set_flavor(det, "JK-BMS im Pylontech-Modus");
				break;
			case BM_VENDOR_FELICITY:
				set_flavor(det, "Felicity Solar LPBF/LPBA");
				break;
			case BM_VENDOR_DYNESS:
				set_flavor(det, "Dyness (Pylontech-Protokoll)");
				break;
			case BM_VENDOR_HUBBLE:
				set_flavor(det, "Hubble (Pylontech-Protokoll)");
				break;
			case BM_VENDOR_EG4:
				set_flavor(det, "EG4 (Pylontech-Protokoll)");
				break;
			case BM_VENDOR_UNKNOWN:
				set_flavor(det, (h & BM_HINT_MFR_STRING)
						      ? "Pylontech-kompatibel, Hersteller unbekannt"
						      : "Pylontech-kompatibel ohne Herstellerstring");
				break;
			default:
				set_flavor(det, "Pylontech-kompatibel");
				break;
			}
		}
	} else if (seen(p, 0x311) && seen(p, 0x313)) {
		det->family = BM_FAMILY_GROWATT_LV;
		conf = 70;
		set_flavor(det, "Growatt-LV-Protokoll");
	} else if (seen(p, 0x455) && seen(p, 0x456)) {
		det->family = BM_FAMILY_GOODWE_LV;
		conf = 70;
		set_flavor(det, "GoodWe-LV-Protokoll");
	} else {
		det->family = BM_FAMILY_UNKNOWN;
		conf = p->frames_total > 5 ? 10 : 0;
		set_flavor(det, "unbekanntes Protokoll");
	}

	det->confidence = clamp100(conf);
}

const char *bm_family_name(enum bm_family f)
{
	switch (f) {
	case BM_FAMILY_PYLON_LV:
		return "pylon_lv";
	case BM_FAMILY_SMA_LV:
		return "sma_lv";
	case BM_FAMILY_VICTRON_EXT:
		return "victron_ext";
	case BM_FAMILY_PYLON_HV:
		return "pylon_hv";
	case BM_FAMILY_GROWATT_LV:
		return "growatt_lv";
	case BM_FAMILY_GOODWE_LV:
		return "goodwe_lv";
	default:
		return "unknown";
	}
}

const char *bm_vendor_name(enum bm_vendor v)
{
	static const char *const names[BM_VENDOR_COUNT] = {
		"unknown", "pylontech", "pace", "seplos", "jk", "felicity", "byd", "sma",
		"dyness", "hubble", "eg4", "deye", "growatt", "goodwe", "lux",
	};

	return (v < BM_VENDOR_COUNT) ? names[v] : "unknown";
}

const char *bm_prot_bit_name(unsigned int byte_idx, unsigned int bit)
{
	if (byte_idx == 0) {
		switch (bit) {
		case 1: return "cell_overvoltage";
		case 2: return "cell_undervoltage";
		case 3: return "cell_overtemp";
		case 4: return "cell_undertemp";
		case 7: return "discharge_overcurrent";
		default: return NULL;
		}
	}
	if (byte_idx == 1) {
		switch (bit) {
		case 0: return "charge_overcurrent";
		case 3: return "system_error";
		default: return NULL;
		}
	}
	return NULL;
}

const char *bm_alarm_bit_name(unsigned int byte_idx, unsigned int bit)
{
	if (byte_idx == 0) {
		switch (bit) {
		case 1: return "cell_high_voltage";
		case 2: return "cell_low_voltage";
		case 3: return "cell_high_temp";
		case 4: return "cell_low_temp";
		case 7: return "discharge_high_current";
		default: return NULL;
		}
	}
	if (byte_idx == 1) {
		switch (bit) {
		case 0: return "charge_high_current";
		case 3: return "internal_comm_fail";
		default: return NULL;
		}
	}
	return NULL;
}

void bm_build_heartbeat(uint8_t data[8])
{
	memset(data, 0, 8);
}
