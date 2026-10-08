/*
 * Shared protocol test cases. Compiled twice:
 *  - tests/host  : plain C, CHECK() prints failures (gcc, no Zephyr needed)
 *  - tests/proto : ztest, CHECK() maps to zassert_true (twister, native_sim)
 */
#include "bm_proto.h"
#include "check.h"
#include "test_cases.h"

#include <stdio.h>
#include <string.h>

static void feed(struct bm_proto *p, uint32_t id, const char *hex, uint32_t t)
{
	uint8_t d[8];
	uint8_t n = 0;

	char pair[3] = { 0 };
	int k = 0;

	for (const char *s = hex; *s && n < 8; s++) {
		if (*s == ' ') {
			continue;
		}
		pair[k++] = *s;
		if (k == 2) {
			unsigned int v;

			sscanf(pair, "%2x", &v);
			d[n++] = (uint8_t)v;
			k = 0;
		}
	}
	bm_proto_feed(p, id, id > 0x7FF, d, n, t);
}

static void pylon_set(struct bm_proto *p, uint32_t t, bool pn_marker, const char *mfr_hex)
{
	feed(p, 0x351, "1402e803e803d601", t);        /* 53.2 V, 100 A, 100 A, 47.0 V */
	feed(p, 0x355, "4e006400", t + 1);            /* 78 %, 100 % */
	feed(p, 0x356, "3813e0ffeb00", t + 2);        /* 49.20 V, -3.2 A, 23.5 C */
	feed(p, 0x359, pn_marker ? "02 00 00 00 02 50 4e" : "02 00 00 00", t + 3);
	feed(p, 0x35C, "c000", t + 4);
	feed(p, 0x35E, mfr_hex, t + 5);
}

void test_pylontech_original(void)
{
	struct bm_proto p;

	bm_proto_init(&p);
	for (int i = 0; i < 5; i++) {
		pylon_set(&p, 1000u * (uint32_t)i, true, "50594c4f4e202020"); /* "PYLON   " */
	}
	bm_proto_tick(&p, 5000);

	CHECK(p.st.cvl_mv == 53200);
	CHECK(p.st.ccl_ma == 100000);
	CHECK(p.st.dcl_ma == 100000);
	CHECK(p.st.dvl_mv == 47000);
	CHECK(p.st.soc_pct == 78 && p.st.soh_pct == 100);
	CHECK(p.st.pack_mv == 49200);
	CHECK(p.st.pack_ma == -3200);
	CHECK(p.st.temp_dc == 235);
	CHECK(p.st.module_count == 2);
	CHECK(p.st.charge_en && p.st.discharge_en && !p.st.force_charge_1);
	CHECK(p.st.prot[0] == 0x02);
	CHECK(strcmp(p.st.manufacturer, "PYLON") == 0);
	CHECK(p.det.family == BM_FAMILY_PYLON_LV);
	CHECK(p.det.vendor == BM_VENDOR_PYLONTECH);
	CHECK(p.det.hints & BM_HINT_PN_MARKER);
	CHECK(p.det.hints & BM_HINT_351_DLC8);
	CHECK(p.det.hints & BM_HINT_MULTI_MODULE);
	CHECK(p.det.confidence == 100);
	CHECK(strcmp(p.det.flavor, "Pylontech US-Serie (Protokoll V1.3)") == 0);
	CHECK(p.link_up);

	char json[1024];
	int n = bm_json_telemetry(&p, 5000, json, sizeof(json));

	CHECK(n > 0);
	CHECK(strstr(json, "\"family\":\"pylon_lv\"") != NULL);
	CHECK(strstr(json, "\"u\":49.20") != NULL);
	CHECK(strstr(json, "\"i\":-3.2") != NULL);
	CHECK(strstr(json, "\"t\":23.5") != NULL);
	CHECK(strstr(json, "\"prot\":[\"cell_overvoltage\"]") != NULL);
	CHECK(strstr(json, "\"alarm\":[]") != NULL);
	CHECK(json[n - 1] == '}');
	CHECK(bm_json_telemetry(&p, 5000, json, 64) == -1);

	bm_proto_tick(&p, 20000);
	CHECK(!p.link_up);
}

void test_emulation_and_vendors(void)
{
	struct bm_proto p;

	bm_proto_init(&p);
	pylon_set(&p, 0, false, "50594c4f4e202020");
	bm_detect_run(&p);
	CHECK(p.det.family == BM_FAMILY_PYLON_LV);
	CHECK(p.det.hints & BM_HINT_359_SHORT);
	CHECK(!(p.det.hints & BM_HINT_PN_MARKER));
	CHECK(strstr(p.det.flavor, "Emulation") != NULL);

	bm_proto_init(&p);
	pylon_set(&p, 0, false, "50414345202020"); /* "PACE" */
	bm_detect_run(&p);
	CHECK(p.det.vendor == BM_VENDOR_PACE);
	CHECK(strstr(p.det.flavor, "PACE") != NULL);

	bm_proto_init(&p);
	pylon_set(&p, 0, false, "46454c4943495459"); /* "FELICITY" */
	bm_detect_run(&p);
	CHECK(p.det.vendor == BM_VENDOR_FELICITY);

	CHECK(bm_vendor_from_string("seplos") == BM_VENDOR_SEPLOS);
	CHECK(bm_vendor_from_string("JK-BMS") == BM_VENDOR_JK);
	CHECK(bm_vendor_from_string("") == BM_VENDOR_UNKNOWN);
	CHECK(bm_vendor_from_string("Foo") == BM_VENDOR_UNKNOWN);
}

void test_victron_extension(void)
{
	struct bm_proto p;

	bm_proto_init(&p);
	pylon_set(&p, 0, false, "42594420202020"); /* "BYD" */
	feed(&p, 0x370, "4259442042617474", 10);   /* "BYD Batt" */
	feed(&p, 0x371, "6572792d426f7820", 11);   /* "ery-Box " */
	feed(&p, 0x372, "0400010000000000", 12);   /* 4 online, 1 blocking charge */
	feed(&p, 0x373, "e60cf80c27012b01", 13);   /* 3302 mV, 3320 mV, 295 K, 299 K */
	feed(&p, 0x374, "4331000000000000", 14);   /* "C1" */
	feed(&p, 0x375, "4331360000000000", 15);   /* "C16" */
	feed(&p, 0x379, "9001000000000000", 16);   /* 400 Ah */
	bm_detect_run(&p);
	CHECK(p.det.family == BM_FAMILY_VICTRON_EXT);
	CHECK(p.det.vendor == BM_VENDOR_BYD);
	CHECK(strcmp(p.st.name, "BYD Battery-Box") == 0);
	CHECK(p.st.mod_online == 4 && p.st.mod_block_chg == 1);
	CHECK(p.st.cell_min_mv == 3302 && p.st.cell_max_mv == 3320);
	CHECK(p.st.temp_min_dc == 219 && p.st.temp_max_dc == 259);
	CHECK(strcmp(p.st.cell_min_name, "C1") == 0 && strcmp(p.st.cell_max_name, "C16") == 0);
	CHECK(p.st.installed_ah == 400);
	CHECK(strstr(p.det.flavor, "BYD") != NULL);
}

void test_sma_dialect(void)
{
	struct bm_proto p;

	bm_proto_init(&p);
	feed(&p, 0x351, "1402e803e803d601", 0);
	feed(&p, 0x355, "4e006400", 1);
	feed(&p, 0x356, "3813e0ffeb00", 2);
	feed(&p, 0x35A, "0000000000000000", 3);
	feed(&p, 0x35E, "42594420202020", 4);
	feed(&p, 0x35F, "0100 0a00 9001 1402", 5); /* chem 1, hw 10, 400 Ah, sw 532 */
	bm_detect_run(&p);
	CHECK(p.det.family == BM_FAMILY_SMA_LV);
	CHECK(p.st.capacity_ah == 400);
	CHECK(p.st.sw_version == 532);
	CHECK(p.det.hints & BM_HINT_35F_PRESENT);
}

void test_pylon_hv(void)
{
	struct bm_proto p;

	bm_proto_init(&p);
	feed(&p, 0x4210, "0f00756204e23763", 0);  /* 384.0 V, +5.0 A, 25.0 C, 55 %, 99 % */
	feed(&p, 0x4220, "11940bb875f8762a", 1);  /* 450.0 V, 300.0 V, 20.0 A, 25.0 A */
	feed(&p, 0x4230, "0d480cfe00100001", 2);  /* 3400 mV #16, 3326 mV #1 */
	feed(&p, 0x4280, "00aa000000000000", 3);  /* discharge forbidden */
	bm_detect_run(&p);
	CHECK(p.det.family == BM_FAMILY_PYLON_HV);
	CHECK(p.st.pack_mv == 384000);
	CHECK(p.st.pack_ma == 5000);
	CHECK(p.st.temp_dc == 250);
	CHECK(p.st.soc_pct == 55 && p.st.soh_pct == 99);
	CHECK(p.st.cvl_mv == 450000 && p.st.dvl_mv == 300000);
	CHECK(p.st.ccl_ma == 20000 && p.st.dcl_ma == 25000);
	CHECK(p.st.cell_max_mv == 3400 && p.st.cell_min_mv == 3326);
	CHECK(strcmp(p.st.cell_max_name, "16") == 0);
	CHECK(p.st.charge_en && !p.st.discharge_en);
	CHECK(p.det.hints & BM_HINT_HV_IDS);
}

void test_inverter_and_unknown(void)
{
	struct bm_proto p;

	bm_proto_init(&p);
	feed(&p, 0x305, "0000000000000000", 0);
	feed(&p, 0x307, "564943", 1); /* "VIC" */
	bm_detect_run(&p);
	CHECK(p.inv.seen);
	CHECK(p.inv.hb_count == 1);
	CHECK(strcmp(p.inv.ident, "VIC") == 0);
	CHECK(p.frames_total == 0);
	CHECK(p.det.family == BM_FAMILY_UNKNOWN);
	CHECK(strcmp(p.det.flavor, "kein Batterieverkehr") == 0);

	feed(&p, 0x311, "0000000000000000", 2);
	feed(&p, 0x313, "0000000000000000", 3);
	bm_detect_run(&p);
	CHECK(p.det.family == BM_FAMILY_GROWATT_LV);

	uint8_t hb[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

	bm_build_heartbeat(hb);
	CHECK(hb[0] == 0 && hb[7] == 0);
}

void test_id_table_overflow(void)
{
	struct bm_proto p;

	bm_proto_init(&p);
	for (uint32_t id = 0x100; id < 0x100 + BM_MAX_IDS + 10; id++) {
		feed(&p, id, "00", id);
	}
	CHECK(p.n_ids == BM_MAX_IDS);
	CHECK(p.frames_total == BM_MAX_IDS + 10);
}
