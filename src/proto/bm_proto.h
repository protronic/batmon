/*
 * batmon – battery CAN protocol decoder and flavour detection.
 * Plain C99, no RTOS dependencies: usable from Zephyr, host tests and tools.
 */
#ifndef BM_PROTO_H
#define BM_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BM_MAX_IDS 48
#define BM_STR_LEN 17

enum bm_family {
	BM_FAMILY_UNKNOWN = 0,
	BM_FAMILY_PYLON_LV,     /* Pylontech LV V1.2/V1.3 and its many emulations */
	BM_FAMILY_SMA_LV,       /* SMA / BYD "Li-Ion BMS" dialect: 0x35A alarms, 0x35F info */
	BM_FAMILY_VICTRON_EXT,  /* Pylon base plus Victron extension block 0x370..0x380 */
	BM_FAMILY_PYLON_HV,     /* Pylontech HV (Force H, Powercube) 0x4210..0x4290 */
	BM_FAMILY_GROWATT_LV,
	BM_FAMILY_GOODWE_LV,
	BM_FAMILY_COUNT
};

enum bm_vendor {
	BM_VENDOR_UNKNOWN = 0,
	BM_VENDOR_PYLONTECH,
	BM_VENDOR_PACE,        /* PACE BMS packs: FM-Solar/DelongTop, Gobel, SOK, Jakiper ... */
	BM_VENDOR_SEPLOS,
	BM_VENDOR_JK,
	BM_VENDOR_FELICITY,
	BM_VENDOR_BYD,
	BM_VENDOR_SMA,
	BM_VENDOR_DYNESS,
	BM_VENDOR_HUBBLE,
	BM_VENDOR_EG4,
	BM_VENDOR_DEYE,
	BM_VENDOR_GROWATT,
	BM_VENDOR_GOODWE,
	BM_VENDOR_LUX,
	BM_VENDOR_COUNT
};

/* Evidence bits collected while listening. */
#define BM_HINT_PN_MARKER        (1u << 0)  /* 0x359 carries "PN" -> Pylontech firmware or faithful clone */
#define BM_HINT_351_DLC8         (1u << 1)  /* 0x351 has discharge voltage limit (V1.3 style) */
#define BM_HINT_35F_PRESENT      (1u << 2)
#define BM_HINT_35A_PRESENT      (1u << 3)
#define BM_HINT_VICTRON_EXT      (1u << 4)
#define BM_HINT_HV_IDS           (1u << 5)
#define BM_HINT_INV_HEARTBEAT    (1u << 6)  /* 0x305 seen: an inverter is on the bus */
#define BM_HINT_INV_IDENT        (1u << 7)  /* 0x307 seen */
#define BM_HINT_MULTI_MODULE     (1u << 8)
#define BM_HINT_MFR_STRING       (1u << 9)
#define BM_HINT_35C_EXTRA        (1u << 10) /* 0x35C longer than 2 bytes */
#define BM_HINT_359_SHORT        (1u << 11) /* 0x359 without module count / marker */

/* Validity bits of struct bm_pack_state. */
#define BM_VALID_LIMITS      (1u << 0)
#define BM_VALID_SOC         (1u << 1)
#define BM_VALID_MEAS        (1u << 2)
#define BM_VALID_STATUS      (1u << 3)
#define BM_VALID_REQUEST     (1u << 4)
#define BM_VALID_MFR         (1u << 5)
#define BM_VALID_INFO        (1u << 6)
#define BM_VALID_CELLS       (1u << 7)
#define BM_VALID_MODULES     (1u << 8)
#define BM_VALID_ENERGY      (1u << 9)
#define BM_VALID_CAPACITY    (1u << 10)
#define BM_VALID_NAME        (1u << 11)
#define BM_VALID_HV_STATUS   (1u << 12)

struct bm_pack_state {
	uint32_t valid;

	/* limits (0x351 / 0x4220) */
	int32_t cvl_mv, dvl_mv;
	int32_t ccl_ma, dcl_ma;

	/* measurements (0x356 / 0x4210), current positive = charging */
	int32_t pack_mv, pack_ma;
	int32_t temp_dc;          /* 0.1 °C */
	uint8_t soc_pct, soh_pct;

	/* status (0x359) */
	uint8_t prot[2], alarm[2];
	uint8_t module_count;

	/* request flags (0x35C) */
	bool charge_en, discharge_en, force_charge_1, force_charge_2, full_charge_req;

	/* SMA/BYD style alarms (0x35A), raw */
	uint8_t alarm35a[8];

	char manufacturer[BM_STR_LEN];   /* 0x35E */
	char name[BM_STR_LEN];           /* 0x370/0x371 */

	/* 0x35F */
	uint16_t chemistry, hw_version, capacity_ah, sw_version;

	/* cells (0x373..0x377 or 0x4230/0x4240) */
	int32_t cell_min_mv, cell_max_mv;
	int32_t temp_min_dc, temp_max_dc;
	char cell_min_name[9], cell_max_name[9];
	char temp_min_name[9], temp_max_name[9];

	/* modules (0x372) */
	uint16_t mod_online, mod_block_chg, mod_block_dis, mod_offline;

	/* energy (0x378), raw counters as sent */
	uint32_t energy_chg_raw, energy_dis_raw;
	uint16_t installed_ah;           /* 0x379 */

	/* HV status (0x4250) */
	uint8_t hv_state;
	uint16_t hv_cycles;
};

struct bm_id_stat {
	uint32_t id;
	uint32_t count;
	uint32_t last_ms;
	uint8_t dlc;
	bool ext;
};

struct bm_inverter {
	bool seen;
	uint32_t last_hb_ms;
	uint32_t hb_count;
	char ident[BM_STR_LEN];          /* 0x307 ASCII, e.g. "VIC" */
};

struct bm_detect {
	enum bm_family family;
	enum bm_vendor vendor;
	uint32_t hints;
	uint8_t confidence;              /* 0..100 */
	char flavor[48];                 /* human readable classification */
};

struct bm_proto {
	struct bm_pack_state st;
	struct bm_detect det;
	struct bm_inverter inv;
	struct bm_id_stat ids[BM_MAX_IDS];
	uint8_t n_ids;
	uint32_t frames_total;
	uint32_t first_ms, last_ms;
	uint32_t last_detect_ms;
	bool link_up;                    /* battery frames within timeout */
};

void bm_proto_init(struct bm_proto *p);

/* Feed one received CAN frame. now_ms is a monotonic millisecond clock. */
void bm_proto_feed(struct bm_proto *p, uint32_t id, bool ext, const uint8_t *data, uint8_t dlc,
		   uint32_t now_ms);

/* Periodic housekeeping: link timeout and (re)classification. Call about once per second. */
void bm_proto_tick(struct bm_proto *p, uint32_t now_ms);

/* Classification helpers (also used by tests). */
void bm_detect_run(struct bm_proto *p);
enum bm_vendor bm_vendor_from_string(const char *mfr);

const char *bm_family_name(enum bm_family f);
const char *bm_vendor_name(enum bm_vendor v);

/* Bit names for 0x359 protection/alarm bytes; returns NULL when the bit has no meaning. */
const char *bm_prot_bit_name(unsigned int byte_idx, unsigned int bit);
const char *bm_alarm_bit_name(unsigned int byte_idx, unsigned int bit);

/* Serialise the current state as compact JSON. Returns bytes written or -1 if buf is too small. */
int bm_json_telemetry(const struct bm_proto *p, uint32_t now_ms, char *buf, size_t len);

/* Flat key/value JSON (no nesting) for ThingsBoard-style telemetry endpoints. */
int bm_json_flat(const struct bm_proto *p, uint32_t now_ms, char *buf, size_t len);

/* Build the inverter-side keep-alive frame (0x305). */
void bm_build_heartbeat(uint8_t data[8]);

#ifdef __cplusplus
}
#endif

#endif /* BM_PROTO_H */
