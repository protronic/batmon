#include "bm_serial.h"

#include <stdio.h>
#include <string.h>

#define GAP_MS             60    /* between commands of one poll round */
#define PROBE_GAP_MS       40
#define PROBE_ROUND_PAUSE  1500  /* after the whole probe table failed */
#define IDENT_TRIES        2
#define FAILS_LINK_DOWN    3
#define FAILS_REPROBE      10
#define TEMP_K_OFFSET      2731

#define CID1_BATTERY   0x46
#define CID2_ANALOG    0x42
#define CID2_ALARM     0x44
#define CID2_SYSPARAM  0x47
#define CID2_VERSION   0x4F
#define CID2_MFRINFO   0x51
#define CID2_CHGMGMT   0x92
#define CID2_SERIALNO  0x93

/* bm_serial.unsupported bits */
#define UNSUP_ALARM    (1u << 0)
#define UNSUP_CHGMGMT  (1u << 1)
#define UNSUP_SERIALNO (1u << 2)
#define UNSUP_MFRINFO  (1u << 3)

#define REG_SEPLOS_PIA  0x1000
#define REG_SEPLOS_PIA_N 18
#define REG_SEPLOS_PIB  0x1100
#define REG_SEPLOS_PIB_N 26

static const uint8_t jk_read_all[] = { 0x4E, 0x57, 0x00, 0x13, 0x00, 0x00, 0x00, 0x00, 0x06, 0x03, 0x00,
				       0x00, 0x00, 0x00, 0x00, 0x00, 0x68, 0x00, 0x00, 0x01, 0x29 };

struct probe {
	uint8_t flavor;
	uint32_t baud;
	uint8_t ver;
	uint8_t adr;
};

static const struct probe probes[] = {
	{ BM_SER_PYLON_ASCII, 115200, 0x20, 2 },
	{ BM_SER_PACE_ASCII, 9600, 0x25, 1 },
	{ BM_SER_PYLON_ASCII, 9600, 0x20, 2 },
	{ BM_SER_SEPLOS_ASCII, 9600, 0x20, 0 },
	{ BM_SER_JK_BIN, 115200, 0, 0 },
	{ BM_SER_SEPLOS_MODBUS, 19200, 0, 0 },
	{ BM_SER_PACE_ASCII, 115200, 0x25, 1 },
	{ BM_SER_PYLON_ASCII, 115200, 0x20, 1 },
	{ BM_SER_SEPLOS_ASCII, 19200, 0x20, 0 },
	{ BM_SER_PACE_ASCII, 9600, 0x20, 1 },
	{ BM_SER_SEPLOS_MODBUS, 19200, 0, 1 },
	{ BM_SER_JK_BIN, 9600, 0, 0 },
	{ BM_SER_PYLON_ASCII, 9600, 0x20, 1 },
	{ BM_SER_PACE_ASCII, 9600, 0x25, 0 },
};
#define N_PROBES (sizeof(probes) / sizeof(probes[0]))

/* ---------------------------------------------------------------- helpers */

static uint16_t be16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static int16_t sbe16(const uint8_t *p)
{
	return (int16_t)be16(p);
}

static uint32_t be24(const uint8_t *p)
{
	return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void copy_trim(char *dst, size_t dst_len, const uint8_t *src, size_t n)
{
	size_t len = 0;

	for (size_t i = 0; i < n && len + 1 < dst_len; i++) {
		char c = (char)src[i];

		if (c == '\0') {
			break;
		}
		if (c < 0x20 || c > 0x7E) {
			c = '?';
		}
		dst[len++] = c;
	}
	while (len > 0 && dst[len - 1] == ' ') {
		len--;
	}
	dst[len] = '\0';
}

static void str_upper(char *s)
{
	for (; *s; s++) {
		if (*s >= 'a' && *s <= 'z') {
			*s = (char)(*s - 'a' + 'A');
		}
	}
}

static int16_t temp_from_raw(uint16_t v)
{
	/* 0.1 K with 273.1 offset (YD/T family); small values are already 0.1 °C */
	if (v >= 1000) {
		return (int16_t)((int)v - TEMP_K_OFFSET);
	}
	return (int16_t)v;
}

static bool is_ascii_flavor(enum bm_ser_flavor f)
{
	return f == BM_SER_PYLON_ASCII || f == BM_SER_PACE_ASCII || f == BM_SER_SEPLOS_ASCII ||
	       f == BM_SER_GENERIC_ASCII;
}

static enum bm_ser_kind kind_of(enum bm_ser_flavor f)
{
	if (f == BM_SER_JK_BIN) {
		return BM_SER_KIND_JK;
	}
	if (f == BM_SER_SEPLOS_MODBUS) {
		return BM_SER_KIND_MODBUS;
	}
	return BM_SER_KIND_ASCII;
}

static uint32_t default_baud(enum bm_ser_flavor f)
{
	switch (f) {
	case BM_SER_PYLON_ASCII: return 115200;
	case BM_SER_JK_BIN: return 115200;
	case BM_SER_SEPLOS_MODBUS: return 19200;
	default: return 9600;
	}
}

static uint8_t default_adr(enum bm_ser_flavor f)
{
	switch (f) {
	case BM_SER_PYLON_ASCII: return 2;
	case BM_SER_PACE_ASCII: return 1;
	default: return 0;
	}
}

const char *bm_serial_flavor_name(enum bm_ser_flavor f)
{
	static const char *const names[BM_SER_FLAVOR_COUNT] = {
		"none", "pylontech_ascii", "pace_ascii", "seplos_ascii", "generic_ascii", "jk_rs485",
		"seplos_modbus",
	};

	return (f < BM_SER_FLAVOR_COUNT) ? names[f] : "?";
}

const char *bm_serial_phase_name(enum bm_ser_phase p)
{
	switch (p) {
	case BM_SER_PROBING: return "probing";
	case BM_SER_IDENTIFY: return "identify";
	case BM_SER_POLLING: return "polling";
	default: return "?";
	}
}

uint16_t bm_serial_timeout_ms(uint32_t baud, enum bm_ser_kind kind)
{
	uint32_t base = (kind == BM_SER_KIND_ASCII) ? 250 : 150;

	if (baud == 0) {
		baud = 9600;
	}
	/* a long ASCII reply is ~400 characters; add the time it needs on the wire */
	return (uint16_t)(base + 4000000u / baud);
}

static void set_flavor_str(struct bm_serial *s)
{
	const char *label;

	switch (s->flavor) {
	case BM_SER_PYLON_ASCII: label = "Pylontech-RS485"; break;
	case BM_SER_PACE_ASCII: label = "PACE-RS485"; break;
	case BM_SER_SEPLOS_ASCII: label = "Seplos-V2-RS485"; break;
	case BM_SER_GENERIC_ASCII: label = "YD/T-ASCII (Layout geraten)"; break;
	case BM_SER_JK_BIN: label = "JK-BMS-RS485"; break;
	case BM_SER_SEPLOS_MODBUS: label = "Seplos-V3-Modbus"; break;
	default: label = "suche BMS"; break;
	}
	if (s->st.model[0] || s->st.manufacturer[0]) {
		snprintf(s->flavor_str, sizeof(s->flavor_str), "%s %s %s", label, s->st.model,
			 s->st.manufacturer);
	} else {
		snprintf(s->flavor_str, sizeof(s->flavor_str), "%s", label);
	}
}

/* ------------------------------------------------------------ life cycle */

void bm_serial_restart(struct bm_serial *s)
{
	struct bm_serial_cfg cfg = s->cfg;
	uint32_t ok = s->ok_count, err = s->err_count, to = s->timeout_count, crc = s->crc_errors;
	uint32_t sniff = s->sniff_frames;

	memset(s, 0, sizeof(*s));
	s->cfg = cfg;
	s->ok_count = ok;
	s->err_count = err;
	s->timeout_count = to;
	s->crc_errors = crc;
	s->sniff_frames = sniff;
	s->phase = BM_SER_PROBING;
	s->flavor = BM_SER_NONE;

	if (cfg.fixed_flavor != BM_SER_NONE) {
		s->flavor = cfg.fixed_flavor;
		s->baud = cfg.fixed_baud ? cfg.fixed_baud : default_baud(cfg.fixed_flavor);
		s->ver = (cfg.fixed_flavor == BM_SER_PACE_ASCII) ? 0x25 : 0x20;
		s->adr = (cfg.fixed_addr >= 0) ? (uint8_t)cfg.fixed_addr : default_adr(cfg.fixed_flavor);
		s->phase = is_ascii_flavor(cfg.fixed_flavor) ? BM_SER_IDENTIFY : BM_SER_POLLING;
	}
	set_flavor_str(s);
}

void bm_serial_init(struct bm_serial *s, const struct bm_serial_cfg *cfg)
{
	memset(s, 0, sizeof(*s));
	if (cfg) {
		s->cfg = *cfg;
	} else {
		s->cfg.poll_ms = 2000;
		s->cfg.fixed_addr = -1;
	}
	if (s->cfg.poll_ms == 0) {
		s->cfg.poll_ms = 2000;
	}
	bm_serial_restart(s);
}

/* --------------------------------------------------------- request build */

static int build_ascii(struct bm_serial *s, uint8_t cid2, bool with_addr, struct bm_serial_req *req)
{
	uint8_t info[1] = { s->adr };
	char tmp[BM_SER_REQ_MAX];
	int n = bm_ydt_build(s->ver, s->adr, CID1_BATTERY, cid2, with_addr ? info : NULL,
			     with_addr ? 1 : 0, tmp, sizeof(tmp));

	if (n < 0) {
		return -1;
	}
	memcpy(req->data, tmp, (size_t)n);
	req->len = (size_t)n;
	req->kind = BM_SER_KIND_ASCII;
	req->cid2 = cid2;
	return 0;
}

static void build_modbus(struct bm_serial *s, uint16_t reg, uint16_t count, struct bm_serial_req *req)
{
	uint8_t *d = req->data;

	d[0] = s->adr;
	d[1] = 0x04;
	d[2] = (uint8_t)(reg >> 8);
	d[3] = (uint8_t)reg;
	d[4] = (uint8_t)(count >> 8);
	d[5] = (uint8_t)count;
	uint16_t crc = bm_modbus_crc(d, 6);

	d[6] = (uint8_t)(crc & 0xFF);
	d[7] = (uint8_t)(crc >> 8);
	req->len = 8;
	req->kind = BM_SER_KIND_MODBUS;
	req->expect_len = (uint16_t)(5 + 2 * count);
	s->last_reg = reg;
}

static void build_jk(struct bm_serial_req *req)
{
	memcpy(req->data, jk_read_all, sizeof(jk_read_all));
	req->len = sizeof(jk_read_all);
	req->kind = BM_SER_KIND_JK;
}

int bm_serial_build_cmd(const struct bm_serial *s, uint8_t cid2, const uint8_t *info,
			size_t info_len, uint8_t *out, size_t out_max)
{
	uint8_t ver = s->ver ? s->ver : 0x20;

	return bm_ydt_build(ver, s->adr, CID1_BATTERY, cid2, info, info_len, (char *)out, out_max);
}

static const uint8_t poll_pylon[] = { CID2_ANALOG, CID2_ALARM, CID2_CHGMGMT, CID2_SERIALNO };
static const uint8_t poll_pace[] = { CID2_ANALOG, CID2_ALARM };

static int poll_cmds(enum bm_ser_flavor f, const uint8_t **cmds)
{
	if (f == BM_SER_PYLON_ASCII) {
		*cmds = poll_pylon;
		return (int)sizeof(poll_pylon);
	}
	*cmds = poll_pace;
	return (int)sizeof(poll_pace);
}

static bool cmd_unsupported(const struct bm_serial *s, uint8_t cid2)
{
	switch (cid2) {
	case CID2_ALARM: return (s->unsupported & UNSUP_ALARM) != 0;
	case CID2_CHGMGMT: return (s->unsupported & UNSUP_CHGMGMT) != 0;
	case CID2_SERIALNO:
		return (s->unsupported & UNSUP_SERIALNO) != 0 || (s->st.valid & BM_SV_SERIAL) != 0;
	default: return false;
	}
}

static void mark_unsupported(struct bm_serial *s, uint8_t cid2)
{
	switch (cid2) {
	case CID2_ALARM: s->unsupported |= UNSUP_ALARM; break;
	case CID2_CHGMGMT: s->unsupported |= UNSUP_CHGMGMT; break;
	case CID2_SERIALNO: s->unsupported |= UNSUP_SERIALNO; break;
	case CID2_MFRINFO: s->unsupported |= UNSUP_MFRINFO; break;
	default: break;
	}
}

int bm_serial_next_request(struct bm_serial *s, uint32_t now_ms, struct bm_serial_req *req)
{
	memset(req, 0, sizeof(*req));
	if ((int32_t)(now_ms - s->next_ms) < 0) {
		return 0;
	}

	if (s->phase == BM_SER_PROBING) {
		/* skip probes that contradict a fixed baud rate */
		for (unsigned int tries = 0; tries < N_PROBES; tries++) {
			const struct probe *p = &probes[s->probe_idx % N_PROBES];

			if (s->cfg.fixed_baud && p->baud != s->cfg.fixed_baud) {
				s->probe_idx++;
				continue;
			}
			s->flavor = (enum bm_ser_flavor)p->flavor;
			s->baud = p->baud;
			s->ver = p->ver;
			s->adr = (s->cfg.fixed_addr >= 0) ? (uint8_t)s->cfg.fixed_addr : p->adr;
			break;
		}
		switch (kind_of(s->flavor)) {
		case BM_SER_KIND_JK:
			build_jk(req);
			break;
		case BM_SER_KIND_MODBUS:
			build_modbus(s, REG_SEPLOS_PIA, REG_SEPLOS_PIA_N, req);
			break;
		default:
			if (build_ascii(s, CID2_MFRINFO, false, req) < 0) {
				return 0;
			}
			break;
		}
	} else if (s->phase == BM_SER_IDENTIFY) {
		if (build_ascii(s, CID2_MFRINFO, false, req) < 0) {
			return 0;
		}
	} else { /* polling */
		switch (kind_of(s->flavor)) {
		case BM_SER_KIND_JK:
			build_jk(req);
			break;
		case BM_SER_KIND_MODBUS:
			if (s->poll_step == 0) {
				build_modbus(s, REG_SEPLOS_PIA, REG_SEPLOS_PIA_N, req);
			} else {
				build_modbus(s, REG_SEPLOS_PIB, REG_SEPLOS_PIB_N, req);
			}
			break;
		default: {
			const uint8_t *cmds;
			int n = poll_cmds(s->flavor, &cmds);

			while (s->poll_step < (unsigned int)n && cmd_unsupported(s, cmds[s->poll_step])) {
				s->poll_step++;
			}
			if (s->poll_step >= (unsigned int)n) {
				s->poll_step = 0;
			}
			if (build_ascii(s, cmds[s->poll_step], true, req) < 0) {
				return 0;
			}
			break;
		}
		}
	}
	req->baud = s->baud;
	req->timeout_ms = bm_serial_timeout_ms(s->baud, req->kind);
	s->last_kind = req->kind;
	s->last_cid2 = req->cid2;
	return 1;
}

/* ---------------------------------------------------------------- decode */

static int dec_prefix(struct bm_serial_state *st, const uint8_t *d, size_t n, size_t *pos)
{
	if (n < 3) {
		return -1;
	}
	size_t p = 2;
	uint8_t ncells = d[p++];

	if (ncells == 0 || ncells > BM_SER_MAX_CELLS || p + 2u * ncells + 1 > n) {
		return -1;
	}
	st->n_cells = ncells;
	for (unsigned int i = 0; i < ncells; i++) {
		st->cell_mv[i] = be16(d + p);
		p += 2;
	}
	uint8_t ntemps = d[p++];

	if (ntemps > 16 || p + 2u * ntemps > n) {
		return -1;
	}
	st->n_temps = (ntemps > BM_SER_MAX_TEMPS) ? BM_SER_MAX_TEMPS : ntemps;
	for (unsigned int i = 0; i < ntemps; i++) {
		if (i < BM_SER_MAX_TEMPS) {
			st->temp_dc[i] = temp_from_raw(be16(d + p));
		}
		p += 2;
	}
	st->valid |= BM_SV_CELLS | (ntemps ? BM_SV_TEMPS : 0);
	*pos = p;
	return 0;
}

static void derive_soc(struct bm_serial_state *st)
{
	if (!(st->valid & BM_SV_SOC) && st->full_mah > 0) {
		uint64_t v = (uint64_t)st->remain_mah * 1000u / st->full_mah;

		st->soc_dpct = (uint16_t)(v > 1000 ? 1000 : v);
		st->soc_derived = true;
		st->valid |= BM_SV_SOC;
	}
}

static int dec_analog_pylon(struct bm_serial_state *st, const uint8_t *d, size_t n)
{
	size_t p;

	if (dec_prefix(st, d, n, &p) < 0 || p + 11 > n) {
		return -1;
	}
	st->pack_ma = (int32_t)sbe16(d + p) * 100;   /* 0.1 A */
	st->pack_mv = be16(d + p + 2);               /* mV */
	st->remain_mah = be16(d + p + 4);            /* mAh */
	st->user_defined = d[p + 6];
	st->full_mah = be16(d + p + 7);
	st->cycles = be16(d + p + 9);
	p += 11;
	if (st->user_defined >= 4 && p + 6 <= n) {   /* US3000C/US5000: 3-byte capacities */
		st->remain_mah = be24(d + p);
		st->full_mah = be24(d + p + 3);
	}
	st->valid |= BM_SV_MEAS | BM_SV_CAP | BM_SV_CYCLES;
	st->valid &= ~BM_SV_SOC;
	derive_soc(st);
	return 0;
}

static int dec_analog_pace(struct bm_serial_state *st, const uint8_t *d, size_t n)
{
	size_t p;

	if (dec_prefix(st, d, n, &p) < 0 || p + 11 > n) {
		return -1;
	}
	st->pack_ma = (int32_t)sbe16(d + p) * 10;    /* 0.01 A */
	st->pack_mv = be16(d + p + 2);               /* mV */
	st->remain_mah = (uint32_t)be16(d + p + 4) * 10; /* 0.01 Ah */
	st->user_defined = d[p + 6];
	st->full_mah = (uint32_t)be16(d + p + 7) * 10;
	st->cycles = be16(d + p + 9);
	p += 11;
	if (p + 2 <= n) {
		st->rated_mah = (uint32_t)be16(d + p) * 10;
	}
	st->valid |= BM_SV_MEAS | BM_SV_CAP | BM_SV_CYCLES;
	st->valid &= ~BM_SV_SOC;
	derive_soc(st);
	return 0;
}

static int dec_analog_seplos(struct bm_serial_state *st, const uint8_t *d, size_t n)
{
	size_t p;

	if (dec_prefix(st, d, n, &p) < 0 || p + 7 > n) {
		return -1;
	}
	st->pack_ma = (int32_t)sbe16(d + p) * 10;        /* 0.01 A */
	st->pack_mv = (int32_t)be16(d + p + 2) * 10;     /* 0.01 V */
	st->remain_mah = (uint32_t)be16(d + p + 4) * 10; /* 0.01 Ah */
	st->user_defined = d[p + 6];
	p += 7;
	st->valid |= BM_SV_MEAS | BM_SV_CAP;
	if (p + 12 <= n) {
		st->full_mah = (uint32_t)be16(d + p) * 10;
		st->soc_dpct = be16(d + p + 2);
		st->rated_mah = (uint32_t)be16(d + p + 4) * 10;
		st->cycles = be16(d + p + 6);
		st->soh_dpct = be16(d + p + 8);
		st->port_mv = (int32_t)be16(d + p + 10) * 10;
		st->soc_derived = false;
		st->valid |= BM_SV_SOC | BM_SV_SOH | BM_SV_CYCLES | BM_SV_PORT_U;
	} else {
		derive_soc(st);
	}
	return 0;
}

/* Unknown vendor: pick the layout from the "user defined" byte or the voltage scale. */
static enum bm_ser_flavor guess_layout(const struct bm_serial *s, const uint8_t *d, size_t n)
{
	if (n < 3) {
		return BM_SER_PYLON_ASCII;
	}
	size_t p = 3 + 2u * d[2];

	if (p >= n) {
		return BM_SER_PYLON_ASCII;
	}
	p += 1 + 2u * d[p];
	if (p + 7 > n) {
		return BM_SER_PYLON_ASCII;
	}
	switch (d[p + 6]) {
	case 0x0A: return BM_SER_SEPLOS_ASCII;
	case 0x03: return BM_SER_PACE_ASCII;
	case 0x02:
	case 0x04: return BM_SER_PYLON_ASCII;
	default: break;
	}
	uint32_t sum = 0;

	for (unsigned int i = 0; i < d[2]; i++) {
		sum += be16(d + 3 + 2 * i);
	}
	uint32_t v = be16(d + p + 2);

	if (v * 10 > sum * 9 / 10 && v * 10 < sum * 11 / 10) {
		return BM_SER_SEPLOS_ASCII;
	}
	return (s->ver == 0x25) ? BM_SER_PACE_ASCII : BM_SER_PYLON_ASCII;
}

static int dec_alarm(struct bm_serial_state *st, const uint8_t *d, size_t n)
{
	if (n < 3) {
		return -1;
	}
	size_t p = 2;
	uint8_t ncells = d[p++];

	if (ncells > BM_SER_MAX_CELLS || p + ncells + 1 > n) {
		return -1;
	}
	memcpy(st->cell_status, d + p, ncells);
	p += ncells;
	uint8_t ntemps = d[p++];

	if (p + ntemps > n) {
		return -1;
	}
	for (unsigned int i = 0; i < ntemps && i < BM_SER_MAX_TEMPS; i++) {
		st->temp_status[i] = d[p + i];
	}
	p += ntemps;
	size_t rest = n - p;

	if (rest > BM_SER_STATUS_RAW) {
		rest = BM_SER_STATUS_RAW;
	}
	memcpy(st->status_raw, d + p, rest);
	st->n_status_raw = (uint8_t)rest;
	st->valid |= BM_SV_STATUS;
	return 0;
}

static int dec_mgmt(struct bm_serial_state *st, const uint8_t *d, size_t n)
{
	if (n < 10) {
		return -1;
	}
	st->cvl_mv = be16(d + 1);
	st->dvl_mv = be16(d + 3);
	st->ccl_ma = (int32_t)sbe16(d + 5) * 100;
	st->dcl_ma = (int32_t)sbe16(d + 7) * 100;
	st->req_flags = d[9];
	st->valid |= BM_SV_LIMITS;
	return 0;
}

static int dec_ident(struct bm_serial_state *st, const uint8_t *d, size_t n)
{
	if (n < 10) {
		return -1;
	}
	copy_trim(st->model, sizeof(st->model), d, 10);
	if (n >= 12) {
		snprintf(st->sw_version, sizeof(st->sw_version), "%u.%u", d[10], d[11]);
	}
	if (n > 12) {
		copy_trim(st->manufacturer, sizeof(st->manufacturer), d + 12, n - 12 > 20 ? 20 : n - 12);
	}
	st->valid |= BM_SV_IDENT;
	return 0;
}

static int dec_serial(struct bm_serial_state *st, const uint8_t *d, size_t n)
{
	if (n < 2) {
		return -1;
	}
	copy_trim(st->serial, sizeof(st->serial), d + 1, n - 1 > 20 ? 20 : n - 1);
	st->valid |= BM_SV_SERIAL;
	return 0;
}

int bm_serial_decode_ascii(struct bm_serial *s, enum bm_ser_flavor fl, uint8_t cid2,
			   const uint8_t *info, size_t len)
{
	struct bm_serial_state *st = &s->st;

	switch (cid2) {
	case CID2_ANALOG:
		if (fl == BM_SER_GENERIC_ASCII) {
			fl = guess_layout(s, info, len);
		}
		if (fl == BM_SER_SEPLOS_ASCII) {
			return dec_analog_seplos(st, info, len);
		}
		if (fl == BM_SER_PACE_ASCII) {
			return dec_analog_pace(st, info, len);
		}
		return dec_analog_pylon(st, info, len);
	case CID2_ALARM:
		return dec_alarm(st, info, len);
	case CID2_CHGMGMT:
		return dec_mgmt(st, info, len);
	case CID2_MFRINFO:
		return dec_ident(st, info, len);
	case CID2_SERIALNO:
		return dec_serial(st, info, len);
	default:
		return -2;
	}
}

static int jk_field_size(uint8_t id)
{
	switch (id) {
	case 0x79: return -1; /* variable */
	case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: return 2;
	case 0x85: case 0x86: return 1;
	case 0x87: return 2;
	case 0x89: return 4;
	case 0x8A: case 0x8B: case 0x8C: case 0x8E: case 0x8F: case 0x90: case 0x91: case 0x92:
	case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: case 0x98: case 0x99: case 0x9A:
	case 0x9B: case 0x9C: return 2;
	case 0x9D: return 1;
	case 0x9E: case 0x9F: case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5:
	case 0xA6: case 0xA7: case 0xA8: return 2;
	case 0xA9: return 1;
	case 0xAA: return 4;
	case 0xAB: case 0xAC: return 1;
	case 0xAD: return 2;
	case 0xAE: case 0xAF: return 1;
	case 0xB0: return 2;
	case 0xB1: return 1;
	case 0xB2: return 10;
	case 0xB3: return 1;
	case 0xB4: return 8;
	case 0xB5: return 4;
	case 0xB6: return 4;
	case 0xB7: return 15;
	case 0xB8: return 1;
	case 0xB9: return 4;
	case 0xBA: return 24;
	case 0xC0: return 1;
	default: return -2; /* unknown: cannot skip */
	}
}

static int16_t jk_temp(uint16_t v)
{
	int t = (v > 100) ? -(int)(v - 100) : (int)v;

	return (int16_t)(t * 10);
}

int bm_serial_decode_jk(struct bm_serial *s, const uint8_t *d, size_t len)
{
	struct bm_serial_state *st = &s->st;

	if (len < 21 || d[0] != 0x4E || d[1] != 0x57) {
		return -1;
	}
	size_t total = (size_t)be16(d + 2) + 2;

	if (total != len || d[len - 5] != 0x68) {
		return -1;
	}
	uint32_t sum = 0;

	for (size_t i = 0; i + 4 < len; i++) {
		sum += d[i];
	}
	if ((sum & 0xFFFF) != be16(d + len - 2)) {
		return -3;
	}
	size_t p = 11, end = len - 9;

	while (p < end) {
		uint8_t id = d[p++];
		int sz = jk_field_size(id);

		if (sz == -2) {
			break;
		}
		if (sz == -1) {
			if (p >= end) {
				break;
			}
			uint8_t bytes = d[p++];
			unsigned int ncells = bytes / 3;

			if (p + bytes > end) {
				return -1;
			}
			st->n_cells = (uint8_t)(ncells > BM_SER_MAX_CELLS ? BM_SER_MAX_CELLS : ncells);
			for (unsigned int i = 0; i < ncells; i++) {
				if (i < BM_SER_MAX_CELLS) {
					st->cell_mv[i] = be16(d + p + 3 * i + 1);
				}
			}
			p += bytes;
			st->valid |= BM_SV_CELLS;
			continue;
		}
		if (p + (size_t)sz > end) {
			return -1;
		}
		const uint8_t *v = d + p;

		switch (id) {
		case 0x80: st->temp_dc[2] = jk_temp(be16(v)); break; /* MOSFET */
		case 0x81: st->temp_dc[0] = jk_temp(be16(v)); break;
		case 0x82: st->temp_dc[1] = jk_temp(be16(v)); break;
		case 0x83: st->pack_mv = (int32_t)be16(v) * 10; break;
		case 0x84: {
			uint16_t raw = be16(v);

			if (s->cfg.jk_current_legacy) {
				st->pack_ma = (10000 - (int32_t)raw) * 10;
			} else {
				st->pack_ma = (raw & 0x8000) ? (int32_t)(raw & 0x7FFF) * 10
							     : -(int32_t)raw * 10;
			}
			break;
		}
		case 0x85:
			st->soc_dpct = (uint16_t)(v[0] * 10);
			st->soc_derived = false;
			st->valid |= BM_SV_SOC;
			break;
		case 0x86: st->n_temps = (uint8_t)(v[0] > 2 ? 3 : v[0] + 1); break;
		case 0x87: st->cycles = be16(v); st->valid |= BM_SV_CYCLES; break;
		case 0x8A: break;
		case 0x8B: st->jk_alarm = be16(v); st->valid |= BM_SV_JKFLAGS; break;
		case 0x8C: st->jk_status = be16(v); st->valid |= BM_SV_JKFLAGS; break;
		case 0xAA: st->full_mah = be32(v) * 1000u; st->valid |= BM_SV_CAP; break;
		case 0xB4: copy_trim(st->model, sizeof(st->model), v, 8); st->valid |= BM_SV_IDENT; break;
		case 0xB7: copy_trim(st->sw_version, sizeof(st->sw_version), v, 15); break;
		case 0xB9: st->remain_mah = be32(v) * 1000u; break;
		case 0xBA:
			copy_trim(st->manufacturer, sizeof(st->manufacturer), v, 20);
			st->valid |= BM_SV_IDENT;
			break;
		default: break;
		}
		p += (size_t)sz;
	}
	if (st->n_temps == 0) {
		st->n_temps = 3;
	}
	st->valid |= BM_SV_MEAS | BM_SV_TEMPS;
	if (st->full_mah && st->remain_mah) {
		st->valid |= BM_SV_CAP;
	}
	return 0;
}

int bm_serial_decode_modbus(struct bm_serial *s, uint16_t reg, const uint8_t *regs, size_t nregs)
{
	struct bm_serial_state *st = &s->st;

	if (reg == REG_SEPLOS_PIA) {
		if (nregs < 8) {
			return -1;
		}
		st->pack_mv = (int32_t)be16(regs) * 10;
		st->pack_ma = (int32_t)sbe16(regs + 2) * 10;
		st->remain_mah = (uint32_t)be16(regs + 4) * 10;
		st->full_mah = (uint32_t)be16(regs + 6) * 10;
		st->soc_dpct = be16(regs + 10);
		st->soh_dpct = be16(regs + 12);
		st->cycles = be16(regs + 14);
		st->soc_derived = false;
		st->valid |= BM_SV_MEAS | BM_SV_CAP | BM_SV_SOC | BM_SV_SOH | BM_SV_CYCLES;
		return 0;
	}
	if (reg == REG_SEPLOS_PIB) {
		if (nregs < 16) {
			return -1;
		}
		st->n_cells = 16;
		for (unsigned int i = 0; i < 16; i++) {
			st->cell_mv[i] = be16(regs + 2 * i);
		}
		st->valid |= BM_SV_CELLS;
		if (nregs >= 20) {
			for (unsigned int i = 0; i < 4; i++) {
				st->temp_dc[i] = temp_from_raw(be16(regs + 2 * (16 + i)));
			}
			st->n_temps = 4;
			st->valid |= BM_SV_TEMPS;
		}
		if (nregs >= 26) {
			st->temp_dc[4] = temp_from_raw(be16(regs + 2 * 24)); /* MOSFET */
			st->temp_dc[5] = temp_from_raw(be16(regs + 2 * 25)); /* ambient */
			st->n_temps = 6;
		}
		return 0;
	}
	return -2;
}

/* ------------------------------------------------------- response handling */

static void note_error(struct bm_serial *s, const char *msg)
{
	snprintf(s->last_err, sizeof(s->last_err), "%s", msg);
}

static void advance_probe(struct bm_serial *s, uint32_t now_ms)
{
	s->probe_idx++;
	if (s->probe_idx % N_PROBES == 0) {
		s->next_ms = now_ms + PROBE_ROUND_PAUSE;
	} else {
		s->next_ms = now_ms + PROBE_GAP_MS;
	}
}

static void on_failure(struct bm_serial *s, uint32_t now_ms, const char *msg)
{
	s->err_count++;
	note_error(s, msg);
	if (s->phase == BM_SER_PROBING) {
		advance_probe(s, now_ms);
		return;
	}
	s->fails_in_row++;
	if (s->fails_in_row >= FAILS_LINK_DOWN) {
		s->link_up = false;
	}
	if (s->fails_in_row >= FAILS_REPROBE && s->cfg.fixed_flavor == BM_SER_NONE) {
		bm_serial_restart(s);
		s->next_ms = now_ms + PROBE_GAP_MS;
		return;
	}
	if (s->phase == BM_SER_IDENTIFY) {
		if (++s->ident_tries >= IDENT_TRIES) {
			/* device answers but not to 0x51: poll anyway */
			if (s->flavor == BM_SER_NONE) {
				s->flavor = BM_SER_GENERIC_ASCII;
			}
			s->phase = BM_SER_POLLING;
			s->poll_step = 0;
			set_flavor_str(s);
		}
		s->next_ms = now_ms + GAP_MS;
		return;
	}
	s->poll_step = 0;
	s->next_ms = now_ms + s->cfg.poll_ms;
}

static void on_success(struct bm_serial *s, uint32_t now_ms)
{
	s->ok_count++;
	s->fails_in_row = 0;
	s->last_ok_ms = now_ms;
	s->link_up = true;
	s->last_err[0] = '\0';
}

static enum bm_ser_flavor flavor_from_ident(const struct bm_serial *s, enum bm_ser_flavor assumed)
{
	char model[sizeof(s->st.model)], mfr[sizeof(s->st.manufacturer)];

	snprintf(model, sizeof(model), "%s", s->st.model);
	snprintf(mfr, sizeof(mfr), "%s", s->st.manufacturer);
	str_upper(model);
	str_upper(mfr);

	if (strstr(mfr, "PYLON") || strstr(model, "US2000") || strstr(model, "US3000") ||
	    strstr(model, "US5000") || strstr(model, "UP5000") || strstr(model, "UP2500") ||
	    strstr(model, "FORCE")) {
		return BM_SER_PYLON_ASCII;
	}
	if (strstr(mfr, "SEPLOS") || strstr(model, "SEPLOS") || strstr(model, "1101-") ||
	    strstr(model, "1102-")) {
		return BM_SER_SEPLOS_ASCII;
	}
	if (strstr(mfr, "PACE") || strstr(model, "P16S") || strstr(model, "P15S") ||
	    strstr(model, "P08S") || strstr(model, "DLT") || s->ver == 0x25) {
		return BM_SER_PACE_ASCII;
	}
	return (assumed == BM_SER_NONE) ? BM_SER_GENERIC_ASCII : assumed;
}

static void finish_poll_step(struct bm_serial *s, uint32_t now_ms)
{
	const uint8_t *cmds;
	int n = poll_cmds(s->flavor, &cmds);

	s->poll_step++;
	while (s->poll_step < (unsigned int)n && cmd_unsupported(s, cmds[s->poll_step])) {
		s->poll_step++;
	}
	if (s->poll_step >= (unsigned int)n) {
		s->poll_step = 0;
		s->next_ms = now_ms + s->cfg.poll_ms;
	} else {
		s->next_ms = now_ms + GAP_MS;
	}
}

static void handle_ascii(struct bm_serial *s, const uint8_t *data, size_t len, uint32_t now_ms)
{
	struct bm_ydt_frame f;
	uint8_t info[BM_YDT_MAX_INFO];
	int rc = bm_ydt_parse((const char *)data, len, &f, info, sizeof(info));

	if (rc == -3) {
		s->crc_errors++;
		on_failure(s, now_ms, "ascii checksum");
		return;
	}
	if (rc != 0) {
		on_failure(s, now_ms, "ascii frame");
		return;
	}
	/* a well formed frame means: there is a YD/T device at this baud/address */
	if (s->phase == BM_SER_PROBING) {
		s->ver = f.ver;
		s->adr = (s->cfg.fixed_addr >= 0) ? (uint8_t)s->cfg.fixed_addr : f.adr;
		s->phase = BM_SER_IDENTIFY;
		s->ident_tries = 0;
		s->fails_in_row = 0;
	}
	if (f.cid2 != 0x00) {
		char msg[40];

		snprintf(msg, sizeof(msg), "rtn %02x %s", f.cid2, bm_ydt_rtn_name(f.cid2));
		if (f.cid2 == 0x01 && f.ver != s->ver) {
			s->ver = f.ver; /* retry with the version the device speaks */
		}
		if (s->phase == BM_SER_POLLING && s->last_cid2 != CID2_ANALOG) {
			mark_unsupported(s, s->last_cid2);
			note_error(s, msg);
			finish_poll_step(s, now_ms);
			return;
		}
		if (s->phase == BM_SER_IDENTIFY) {
			mark_unsupported(s, CID2_MFRINFO);
		}
		on_failure(s, now_ms, msg);
		return;
	}
	if (s->phase == BM_SER_IDENTIFY) {
		if (bm_serial_decode_ascii(s, s->flavor, CID2_MFRINFO, info, f.info_len) == 0) {
			s->flavor = flavor_from_ident(s, s->flavor);
		} else if (s->flavor == BM_SER_NONE) {
			s->flavor = BM_SER_GENERIC_ASCII;
		}
		s->phase = BM_SER_POLLING;
		s->poll_step = 0;
		set_flavor_str(s);
		on_success(s, now_ms);
		s->next_ms = now_ms + GAP_MS;
		return;
	}
	if (bm_serial_decode_ascii(s, s->flavor, s->last_cid2, info, f.info_len) != 0) {
		on_failure(s, now_ms, "payload layout");
		return;
	}
	on_success(s, now_ms);
	finish_poll_step(s, now_ms);
}

static void handle_jk(struct bm_serial *s, const uint8_t *data, size_t len, uint32_t now_ms)
{
	int rc = bm_serial_decode_jk(s, data, len);

	if (rc == -3) {
		s->crc_errors++;
		on_failure(s, now_ms, "jk checksum");
		return;
	}
	if (rc != 0) {
		on_failure(s, now_ms, "jk frame");
		return;
	}
	if (s->phase != BM_SER_POLLING) {
		s->phase = BM_SER_POLLING;
		s->flavor = BM_SER_JK_BIN;
		set_flavor_str(s);
	}
	on_success(s, now_ms);
	s->next_ms = now_ms + s->cfg.poll_ms;
}

static void handle_modbus(struct bm_serial *s, const uint8_t *data, size_t len, uint32_t now_ms)
{
	if (len < 5 || data[1] != 0x04 || len != (size_t)data[2] + 5) {
		on_failure(s, now_ms, "modbus frame");
		return;
	}
	uint16_t crc = bm_modbus_crc(data, len - 2);

	if (data[len - 2] != (crc & 0xFF) || data[len - 1] != (crc >> 8)) {
		s->crc_errors++;
		on_failure(s, now_ms, "modbus crc");
		return;
	}
	if (bm_serial_decode_modbus(s, s->last_reg, data + 3, data[2] / 2) != 0) {
		on_failure(s, now_ms, "modbus layout");
		return;
	}
	if (s->phase != BM_SER_POLLING) {
		s->phase = BM_SER_POLLING;
		s->flavor = BM_SER_SEPLOS_MODBUS;
		s->poll_step = 0;
		set_flavor_str(s);
	}
	on_success(s, now_ms);
	if (s->last_reg == REG_SEPLOS_PIA) {
		s->poll_step = 1;
		s->next_ms = now_ms + GAP_MS;
	} else {
		s->poll_step = 0;
		s->next_ms = now_ms + s->cfg.poll_ms;
	}
}

void bm_serial_response(struct bm_serial *s, const uint8_t *data, size_t len, uint32_t now_ms)
{
	s->last_rx_len = len > sizeof(s->last_rx) ? sizeof(s->last_rx) : len;
	if (s->last_rx_len) {
		memcpy(s->last_rx, data, s->last_rx_len);
	}
	if (len == 0) {
		s->timeout_count++;
		on_failure(s, now_ms, "timeout");
		return;
	}
	switch (s->last_kind) {
	case BM_SER_KIND_JK:
		handle_jk(s, data, len, now_ms);
		break;
	case BM_SER_KIND_MODBUS:
		handle_modbus(s, data, len, now_ms);
		break;
	default:
		handle_ascii(s, data, len, now_ms);
		break;
	}
}

/* ------------------------------------------------------------- sniffing */

static bool is_request_cid2(uint8_t cid2)
{
	switch (cid2) {
	case CID2_ANALOG: case CID2_ALARM: case CID2_SYSPARAM: case CID2_VERSION:
	case CID2_MFRINFO: case CID2_CHGMGMT: case CID2_SERIALNO:
		return true;
	default:
		return false;
	}
}

int bm_serial_sniff(struct bm_serial *s, const uint8_t *data, size_t len, uint32_t now_ms)
{
	s->sniff_frames++;
	if (len >= 16 && data[0] == '~') {
		struct bm_ydt_frame f;
		uint8_t info[BM_YDT_MAX_INFO];

		if (bm_ydt_parse((const char *)data, len, &f, info, sizeof(info)) != 0) {
			s->crc_errors++;
			return 0;
		}
		if (is_request_cid2(f.cid2) && f.info_len <= 2) {
			s->sniff_req_cid2 = f.cid2;
			s->ver = f.ver;
			s->adr = f.adr;
			return 1;
		}
		if (f.cid2 != 0x00 || s->sniff_req_cid2 == 0) {
			return 1;
		}
		if (!is_ascii_flavor(s->flavor)) {
			s->flavor = (f.ver == 0x25) ? BM_SER_PACE_ASCII : BM_SER_GENERIC_ASCII;
		}
		if (s->sniff_req_cid2 == CID2_MFRINFO) {
			if (bm_serial_decode_ascii(s, s->flavor, CID2_MFRINFO, info, f.info_len) == 0) {
				s->flavor = flavor_from_ident(s, s->flavor);
			}
		} else if (bm_serial_decode_ascii(s, s->flavor, s->sniff_req_cid2, info, f.info_len) != 0) {
			return 0;
		}
		s->phase = BM_SER_POLLING;
		set_flavor_str(s);
		on_success(s, now_ms);
		return 1;
	}
	if (len >= 21 && data[0] == 0x4E && data[1] == 0x57) {
		if (data[8] != 0x06 || data[9] == 0x03) {
			return 1; /* request from the master */
		}
		if (bm_serial_decode_jk(s, data, len) != 0) {
			return 0;
		}
		s->flavor = BM_SER_JK_BIN;
		s->phase = BM_SER_POLLING;
		set_flavor_str(s);
		on_success(s, now_ms);
		return 1;
	}
	if (len >= 5 && data[1] == 0x04) {
		uint16_t crc = bm_modbus_crc(data, len - 2);

		if (data[len - 2] != (crc & 0xFF) || data[len - 1] != (crc >> 8)) {
			return 0;
		}
		if (len == 8) {
			s->last_reg = be16(data + 2);
			s->adr = data[0];
			return 1;
		}
		if (len == (size_t)data[2] + 5 &&
		    bm_serial_decode_modbus(s, s->last_reg, data + 3, data[2] / 2) == 0) {
			s->flavor = BM_SER_SEPLOS_MODBUS;
			s->phase = BM_SER_POLLING;
			set_flavor_str(s);
			on_success(s, now_ms);
			return 1;
		}
	}
	return 0;
}
