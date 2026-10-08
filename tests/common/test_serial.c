/* Serial (RS485) protocol tests: YD/T frames, decoders, probing state machine, sniffing. */
#include "bm_serial.h"
#include "bm_ydt.h"
#include "check.h"
#include "test_cases.h"

#include <stdio.h>
#include <string.h>

static int t_hex2bin(const char *hex, uint8_t *out, size_t max)
{
	size_t n = 0;
	char pair[3] = { 0 };
	int k = 0;

	for (const char *s = hex; *s; s++) {
		if (*s == ' ') {
			continue;
		}
		pair[k++] = *s;
		if (k == 2) {
			unsigned int v;

			if (n >= max || sscanf(pair, "%2x", &v) != 1) {
				return -1;
			}
			out[n++] = (uint8_t)v;
			k = 0;
		}
	}
	return (int)n;
}

/* repeat a hex word n times into a string buffer */
static void rep(char *dst, size_t dst_len, const char *word, int n)
{
	size_t len = strlen(dst);

	for (int i = 0; i < n; i++) {
		len += (size_t)snprintf(dst + len, dst_len - len, "%s ", word);
	}
}

void test_ydt_frames(void)
{
	char buf[64];
	uint8_t a2[1] = { 0x02 }, a1[1] = { 0x01 }, a0[1] = { 0x00 };

	CHECK(bm_ydt_build(0x20, 0x02, 0x46, 0x42, a2, 1, buf, sizeof(buf)) == 20);
	CHECK(strcmp(buf, "~20024642E00202FD33\r") == 0);
	CHECK(bm_ydt_build(0x25, 0x01, 0x46, 0x42, a1, 1, buf, sizeof(buf)) > 0);
	CHECK(strcmp(buf, "~25014642E00201FD30\r") == 0);
	CHECK(bm_ydt_build(0x20, 0x00, 0x46, 0x42, a0, 1, buf, sizeof(buf)) > 0);
	CHECK(strcmp(buf, "~20004642E00200FD37\r") == 0);
	CHECK(bm_ydt_build(0x20, 0x02, 0x46, 0x51, NULL, 0, buf, sizeof(buf)) == 18);
	CHECK(strcmp(buf, "~200246510000FDAC\r") == 0);

	struct bm_ydt_frame f;
	uint8_t info[64];

	CHECK(bm_ydt_parse(buf, strlen(buf), &f, info, sizeof(info)) == 0);
	CHECK(f.ver == 0x20 && f.adr == 0x02 && f.cid1 == 0x46 && f.cid2 == 0x51);
	CHECK(f.info_len == 0 && f.lchksum_ok);

	/* lower case hex and no CR: the checksum covers the characters as sent */
	char lower[32] = "20024642e00202";

	snprintf(lower + 14, sizeof(lower) - 14, "%04x", bm_ydt_checksum(lower, 14));
	CHECK(bm_ydt_parse(lower, strlen(lower), &f, info, sizeof(info)) == 0);
	CHECK(f.info_len == 1 && info[0] == 0x02);

	CHECK(bm_ydt_parse("~20024642E00202FD34\r", 20, &f, info, sizeof(info)) == -3);

	/* LENID says 4 chars but only 2 follow (checksum recomputed to isolate the length check) */
	char bad[32] = "20024642E00402";
	uint16_t chk = bm_ydt_checksum(bad, strlen(bad));

	snprintf(bad + strlen(bad), sizeof(bad) - strlen(bad), "%04X", chk);
	CHECK(bm_ydt_parse(bad, strlen(bad), &f, info, sizeof(info)) == -2);
	CHECK(bm_ydt_parse("~2002", 5, &f, info, sizeof(info)) == -1);

	CHECK(bm_ydt_length_field(0) == 0x0000);
	CHECK(bm_ydt_length_field(2) == 0xE002);
	CHECK(bm_ydt_length_field(150) == 0x1096);

	const uint8_t mb[] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x0A };

	CHECK(bm_modbus_crc(mb, sizeof(mb)) == 0xCDC5);
	CHECK(strcmp(bm_ydt_rtn_name(0x04), "unsupported command") == 0);

	char hex[16];

	CHECK(bm_hex_encode(mb, 3, hex, sizeof(hex)) == 6 && strcmp(hex, "010300") == 0);
}

void test_serial_pylontech(void)
{
	struct bm_serial s;
	uint8_t info[256];
	char hex[512] = "00 02 0F ";

	bm_serial_init(&s, NULL);
	s.flavor = BM_SER_PYLON_ASCII;

	rep(hex, sizeof(hex), "0CE4", 15);
	rep(hex, sizeof(hex), "05", 1);
	rep(hex, sizeof(hex), "0BA5", 5);
	rep(hex, sizeof(hex), "FFE0 C15C 9A4C 02 C350 0005", 1);
	int n = t_hex2bin(hex, info, sizeof(info));

	CHECK(n == 3 + 30 + 1 + 10 + 11);
	CHECK(bm_serial_decode_ascii(&s, BM_SER_PYLON_ASCII, 0x42, info, (size_t)n) == 0);
	CHECK(s.st.n_cells == 15 && s.st.cell_mv[14] == 3300);
	CHECK(s.st.n_temps == 5 && s.st.temp_dc[0] == 250);
	CHECK(s.st.pack_ma == -3200);
	CHECK(s.st.pack_mv == 49500);
	CHECK(s.st.remain_mah == 39500 && s.st.full_mah == 50000);
	CHECK(s.st.cycles == 5);
	CHECK((s.st.valid & BM_SV_SOC) && s.st.soc_derived && s.st.soc_dpct == 790);

	/* US3000C style: user defined 04, 3-byte capacities appended */
	rep(hex, sizeof(hex), "011D28 012110", 1);
	n = t_hex2bin(hex, info, sizeof(info));
	info[3 + 30 + 1 + 10 + 6] = 0x04;
	CHECK(bm_serial_decode_ascii(&s, BM_SER_PYLON_ASCII, 0x42, info, (size_t)n) == 0);
	CHECK(s.st.remain_mah == 73000 && s.st.full_mah == 74000);

	n = t_hex2bin("02 CFD0 B798 00FA 00FA C0", info, sizeof(info));
	CHECK(bm_serial_decode_ascii(&s, BM_SER_PYLON_ASCII, 0x92, info, (size_t)n) == 0);
	CHECK(s.st.cvl_mv == 53200 && s.st.dvl_mv == 47000);
	CHECK(s.st.ccl_ma == 25000 && s.st.dcl_ma == 25000 && s.st.req_flags == 0xC0);

	n = t_hex2bin("55 53 32 30 30 30 43 20 20 20 01 02 50 79 6C 6F 6E 20 20 20 20 20 20 20 20 20 "
		    "20 20 20 20 20 20",
		    info, sizeof(info));
	CHECK(bm_serial_decode_ascii(&s, BM_SER_PYLON_ASCII, 0x51, info, (size_t)n) == 0);
	CHECK(strcmp(s.st.model, "US2000C") == 0);
	CHECK(strcmp(s.st.manufacturer, "Pylon") == 0);
	CHECK(strcmp(s.st.sw_version, "1.2") == 0);

	n = t_hex2bin("02 50 50 54 42 48 30 31 32 33 34 35 36 37 38 39 41", info, sizeof(info));
	CHECK(bm_serial_decode_ascii(&s, BM_SER_PYLON_ASCII, 0x93, info, (size_t)n) == 0);
	CHECK(strcmp(s.st.serial, "PPTBH0123456789A") == 0);

	hex[0] = '\0';
	rep(hex, sizeof(hex), "00 02 0F", 1);
	rep(hex, sizeof(hex), "00", 15);
	rep(hex, sizeof(hex), "05", 1);
	rep(hex, sizeof(hex), "00", 5);
	rep(hex, sizeof(hex), "00 00 00 10 00 00 00 00", 1);
	n = t_hex2bin(hex, info, sizeof(info));
	CHECK(bm_serial_decode_ascii(&s, BM_SER_PYLON_ASCII, 0x44, info, (size_t)n) == 0);
	CHECK(s.st.n_status_raw == 8 && s.st.status_raw[3] == 0x10);
	CHECK(bm_serial_decode_ascii(&s, BM_SER_PYLON_ASCII, 0x47, info, (size_t)n) == -2);
}

void test_serial_seplos(void)
{
	struct bm_serial s;
	uint8_t info[256];
	char hex[512] = "00 01 10 ";

	bm_serial_init(&s, NULL);
	rep(hex, sizeof(hex), "0CD7", 15);
	rep(hex, sizeof(hex), "0CD8 06", 1);
	rep(hex, sizeof(hex), "0BA6", 5);
	rep(hex, sizeof(hex), "0BA2 FD5C 14A0 344E 0A 4268 0313 4650 0046 03E8 149F 00 00 00 00 00 00 00 00", 1);
	int n = t_hex2bin(hex, info, sizeof(info));

	CHECK(n == 75);
	/* generic flavour must recognise the Seplos layout from the 0x0A marker */
	CHECK(bm_serial_decode_ascii(&s, BM_SER_GENERIC_ASCII, 0x42, info, (size_t)n) == 0);
	CHECK(s.st.n_cells == 16 && s.st.cell_mv[0] == 3287 && s.st.cell_mv[15] == 3288);
	CHECK(s.st.n_temps == 6 && s.st.temp_dc[0] == 251 && s.st.temp_dc[5] == 247);
	CHECK(s.st.pack_ma == -6760);
	CHECK(s.st.pack_mv == 52800);
	CHECK(s.st.remain_mah == 133900 && s.st.full_mah == 170000 && s.st.rated_mah == 180000);
	CHECK(s.st.soc_dpct == 787 && !s.st.soc_derived);
	CHECK(s.st.soh_dpct == 1000);
	CHECK(s.st.cycles == 70);
	CHECK(s.st.port_mv == 52790);
	CHECK(s.st.valid & BM_SV_PORT_U);
}

void test_serial_pace_state_machine(void)
{
	struct bm_serial s;
	struct bm_serial_req req;
	struct bm_serial_cfg cfg = { .poll_ms = 2000, .fixed_addr = -1 };
	char frame[600];
	uint8_t info[256];
	uint32_t t = 1000;

	bm_serial_init(&s, &cfg);
	CHECK(s.phase == BM_SER_PROBING);

	/* probe 1: Pylontech at 115200, manufacturer info */
	CHECK(bm_serial_next_request(&s, t, &req) == 1);
	CHECK(req.kind == BM_SER_KIND_ASCII && req.baud == 115200);
	CHECK(memcmp(req.data, "~200246510000FDAC\r", 18) == 0);
	CHECK(req.timeout_ms > 250 && req.timeout_ms < 400);
	bm_serial_response(&s, NULL, 0, t); /* timeout */
	CHECK(s.timeout_count == 1);
	CHECK(bm_serial_next_request(&s, t, &req) == 0); /* probe gap */
	t += 100;

	/* probe 2: PACE at 9600 answers with manufacturer info */
	CHECK(bm_serial_next_request(&s, t, &req) == 1);
	CHECK(req.baud == 9600 && req.data[1] == '2' && req.data[2] == '5');
	int n = t_hex2bin("50 31 36 53 31 30 30 41 20 20 02 05 50 41 43 45 20 20 20 20 20 20 20 20 20 "
			"20 20 20 20 20 20 20",
			info, sizeof(info));
	int len = bm_ydt_build(0x25, 0x01, 0x46, 0x00, info, (size_t)n, frame, sizeof(frame));

	CHECK(len > 0);
	bm_serial_response(&s, (const uint8_t *)frame, (size_t)len, t);
	CHECK(s.phase == BM_SER_POLLING);
	CHECK(s.flavor == BM_SER_PACE_ASCII);
	CHECK(s.link_up);
	CHECK(strcmp(s.st.model, "P16S100A") == 0 && strcmp(s.st.manufacturer, "PACE") == 0);
	CHECK(strstr(s.flavor_str, "PACE-RS485 P16S100A PACE") != NULL);
	t += 100;

	/* poll: analog values */
	CHECK(bm_serial_next_request(&s, t, &req) == 1);
	CHECK(req.cid2 == 0x42);
	CHECK(memcmp(req.data, "~25014642E00201FD30\r", 20) == 0);
	char hex[512] = "00 01 10 ";

	rep(hex, sizeof(hex), "0CF8", 16);
	rep(hex, sizeof(hex), "06", 1);
	rep(hex, sizeof(hex), "0B9A", 6);
	rep(hex, sizeof(hex), "0190 CF80 1388 03 2710 000C 2710", 1);
	n = t_hex2bin(hex, info, sizeof(info));
	len = bm_ydt_build(0x25, 0x01, 0x46, 0x00, info, (size_t)n, frame, sizeof(frame));
	bm_serial_response(&s, (const uint8_t *)frame, (size_t)len, t);
	CHECK(s.st.pack_ma == 4000);
	CHECK(s.st.pack_mv == 53120);
	CHECK(s.st.remain_mah == 50000 && s.st.full_mah == 100000 && s.st.rated_mah == 100000);
	CHECK(s.st.cycles == 12 && s.st.soc_dpct == 500 && s.st.soc_derived);
	CHECK(s.st.temp_dc[0] == 239);
	CHECK(s.ok_count == 2);
	t += 100;

	/* poll: alarm info unsupported -> marked, not a link failure */
	CHECK(bm_serial_next_request(&s, t, &req) == 1);
	CHECK(req.cid2 == 0x44);
	len = bm_ydt_build(0x25, 0x01, 0x46, 0x04, NULL, 0, frame, sizeof(frame));
	bm_serial_response(&s, (const uint8_t *)frame, (size_t)len, t);
	CHECK(s.link_up && s.fails_in_row == 0);
	CHECK(strstr(s.last_err, "unsupported") != NULL);
	/* round finished: next poll only after poll_ms */
	CHECK(bm_serial_next_request(&s, t + 500, &req) == 0);
	CHECK(bm_serial_next_request(&s, t + 2100, &req) == 1);
	CHECK(req.cid2 == 0x42); /* 0x44 skipped from now on */

	/* three timeouts -> link down, ten -> back to probing */
	for (int i = 0; i < 3; i++) {
		t += 3000;
		bm_serial_next_request(&s, t, &req);
		bm_serial_response(&s, NULL, 0, t);
	}
	CHECK(!s.link_up && s.phase == BM_SER_POLLING);
	for (int i = 0; i < 7; i++) {
		t += 3000;
		bm_serial_next_request(&s, t, &req);
		bm_serial_response(&s, NULL, 0, t);
	}
	CHECK(s.phase == BM_SER_PROBING && s.flavor == BM_SER_NONE);
	CHECK(s.st.model[0] == '\0');

	uint8_t cmd[64];

	s.ver = 0x25;
	s.adr = 1;
	CHECK(bm_serial_build_cmd(&s, 0x42, (const uint8_t *)"\x01", 1, cmd, sizeof(cmd)) == 20);
	CHECK(memcmp(cmd, "~25014642E00201FD30\r", 20) == 0);
}

static size_t jk_frame(uint8_t *out, const uint8_t *data, size_t data_len)
{
	size_t total = 11 + data_len + 9;
	size_t p = 0;

	out[p++] = 0x4E;
	out[p++] = 0x57;
	out[p++] = (uint8_t)((total - 2) >> 8);
	out[p++] = (uint8_t)(total - 2);
	memset(out + p, 0, 4);
	p += 4;
	out[p++] = 0x06; /* command: read all */
	out[p++] = 0x01; /* source: BMS */
	out[p++] = 0x02; /* transport type */
	memcpy(out + p, data, data_len);
	p += data_len;
	memset(out + p, 0, 4); /* record number */
	p += 4;
	out[p++] = 0x68;
	uint32_t sum = 0;

	for (size_t i = 0; i < p; i++) {
		sum += out[i];
	}
	out[p++] = 0;
	out[p++] = 0;
	out[p++] = (uint8_t)(sum >> 8);
	out[p++] = (uint8_t)sum;
	return p;
}

void test_serial_jk(void)
{
	struct bm_serial s;
	uint8_t data[200], frame[256];
	const char *hex = "79 09 01 0CE4 02 0CE5 03 0CE6 "
			  "80 001A 81 0019 82 006E 83 03DE 84 8064 85 50 86 02 87 0005 "
			  "89 00000064 8A 0003 8B 0001 8C 0003 "
			  "AA 000000C8 "
			  "B4 4A 4B 2D 42 32 41 38 53 "
			  "B7 31 31 2E 58 57 5F 53 31 31 2E 32 36 5F 5F 5F "
			  "B9 00000064 "
			  "BA 49 6E 70 75 74 20 55 73 65 72 64 61 20 20 20 20 20 20 20 20 20 20 20 20 "
			  "C0 01";
	int n = t_hex2bin(hex, data, sizeof(data));

	CHECK(n > 0);
	size_t len = jk_frame(frame, data, (size_t)n);

	bm_serial_init(&s, NULL);
	CHECK(bm_serial_decode_jk(&s, frame, len) == 0);
	CHECK(s.st.n_cells == 3 && s.st.cell_mv[0] == 3300 && s.st.cell_mv[2] == 3302);
	CHECK(s.st.temp_dc[2] == 260 && s.st.temp_dc[0] == 250 && s.st.temp_dc[1] == -100);
	CHECK(s.st.pack_mv == 9900);
	CHECK(s.st.pack_ma == 1000); /* bit 15 set = charging 1.00 A */
	CHECK(s.st.soc_dpct == 800 && !s.st.soc_derived);
	CHECK(s.st.cycles == 5);
	CHECK(s.st.jk_alarm == 0x0001 && s.st.jk_status == 0x0003);
	CHECK(s.st.full_mah == 200000 && s.st.remain_mah == 100000);
	CHECK(strcmp(s.st.model, "JK-B2A8S") == 0);
	CHECK(strncmp(s.st.manufacturer, "Input Userda", 12) == 0);
	CHECK(strcmp(s.st.sw_version, "11.XW_S11.2") == 0);

	/* legacy current encoding */
	s.cfg.jk_current_legacy = true;
	data[0] = 0x79; /* unchanged, just re-decode */
	CHECK(bm_serial_decode_jk(&s, frame, len) == 0);
	CHECK(s.st.pack_ma == (10000 - 0x8064) * 10);

	/* corrupted checksum */
	frame[len - 1] ^= 0x01;
	CHECK(bm_serial_decode_jk(&s, frame, len) == -3);
	frame[len - 1] ^= 0x01;

	/* the request itself is not a BMS reply */
	struct bm_serial_req req;
	struct bm_serial_cfg cfg = { .poll_ms = 1000, .fixed_addr = -1, .fixed_flavor = BM_SER_JK_BIN };

	bm_serial_init(&s, &cfg);
	CHECK(s.phase == BM_SER_POLLING && s.baud == 115200);
	CHECK(bm_serial_next_request(&s, 10, &req) == 1 && req.kind == BM_SER_KIND_JK && req.len == 21);
	CHECK(req.data[0] == 0x4E && req.data[20] == 0x29);
	bm_serial_response(&s, frame, len, 10);
	CHECK(s.link_up && s.ok_count == 1 && s.st.soc_dpct == 800);
}

void test_serial_modbus(void)
{
	struct bm_serial s;
	struct bm_serial_req req;
	struct bm_serial_cfg cfg = { .poll_ms = 1000, .fixed_addr = -1, .fixed_flavor = BM_SER_SEPLOS_MODBUS };
	uint8_t resp[80];

	bm_serial_init(&s, &cfg);
	CHECK(bm_serial_next_request(&s, 10, &req) == 1);
	CHECK(req.kind == BM_SER_KIND_MODBUS && req.baud == 19200 && req.len == 8);
	CHECK(req.data[0] == 0x00 && req.data[1] == 0x04 && req.data[2] == 0x10 && req.data[5] == 0x12);
	CHECK(req.expect_len == 41);
	uint16_t crc = bm_modbus_crc(req.data, 6);

	CHECK(req.data[6] == (crc & 0xFF) && req.data[7] == (crc >> 8));

	/* PIA reply: 18 registers */
	const char *hex = "00 04 24 14A0 FD5C 344E 4268 0000 0313 03E8 0046 0CE0 0BA6 0CF0 0CD0 0BB0 0BA0 "
			  "0000 0000 0000 0000";
	int n = t_hex2bin(hex, resp, sizeof(resp));

	CHECK(n == 39);
	crc = bm_modbus_crc(resp, (size_t)n);
	resp[n++] = (uint8_t)(crc & 0xFF);
	resp[n++] = (uint8_t)(crc >> 8);
	bm_serial_response(&s, resp, (size_t)n, 20);
	CHECK(s.link_up);
	CHECK(s.st.pack_mv == 52800 && s.st.pack_ma == -6760);
	CHECK(s.st.remain_mah == 133900 && s.st.full_mah == 170000);
	CHECK(s.st.soc_dpct == 787 && s.st.soh_dpct == 1000 && s.st.cycles == 70);

	/* then PIB (cells) */
	CHECK(bm_serial_next_request(&s, 100, &req) == 1);
	CHECK(req.data[2] == 0x11 && req.data[5] == 0x1A);
	char cells[400] = "00 04 34 ";

	rep(cells, sizeof(cells), "0CE4", 16);
	rep(cells, sizeof(cells), "0BA6", 4);
	rep(cells, sizeof(cells), "0000", 4);
	rep(cells, sizeof(cells), "0BC0 0B90", 1);
	n = t_hex2bin(cells, resp, sizeof(resp));
	CHECK(n == 55);
	crc = bm_modbus_crc(resp, (size_t)n);
	resp[n++] = (uint8_t)(crc & 0xFF);
	resp[n++] = (uint8_t)(crc >> 8);
	bm_serial_response(&s, resp, (size_t)n, 110);
	CHECK(s.st.n_cells == 16 && s.st.cell_mv[15] == 3300);
	CHECK(s.st.n_temps == 6 && s.st.temp_dc[0] == 251 && s.st.temp_dc[4] == 277 && s.st.temp_dc[5] == 229);
	CHECK(bm_serial_next_request(&s, 200, &req) == 0); /* waits for poll_ms */

	/* bad CRC counted */
	resp[5] ^= 0xFF;
	bm_serial_next_request(&s, 2000, &req);
	bm_serial_response(&s, resp, (size_t)n, 2000);
	CHECK(s.crc_errors == 1 && s.err_count == 1);
}

void test_serial_sniff(void)
{
	struct bm_serial s;
	char frame[600];
	uint8_t info[256];
	char hex[512] = "00 02 0F ";

	bm_serial_init(&s, NULL);
	/* inverter asks pack 2 for analog values ... */
	int len = bm_ydt_build(0x20, 0x02, 0x46, 0x42, (const uint8_t *)"\x02", 1, frame, sizeof(frame));

	CHECK(bm_serial_sniff(&s, (const uint8_t *)frame, (size_t)len, 5) == 1);
	CHECK(s.sniff_req_cid2 == 0x42 && s.adr == 2);
	/* ... and the battery answers (Pylontech layout, user defined 02) */
	rep(hex, sizeof(hex), "0CE4", 15);
	rep(hex, sizeof(hex), "05", 1);
	rep(hex, sizeof(hex), "0BA5", 5);
	rep(hex, sizeof(hex), "0014 C15C 9A4C 02 C350 0005", 1);
	int n = t_hex2bin(hex, info, sizeof(info));

	len = bm_ydt_build(0x20, 0x02, 0x46, 0x00, info, (size_t)n, frame, sizeof(frame));
	CHECK(bm_serial_sniff(&s, (const uint8_t *)frame, (size_t)len, 40) == 1);
	CHECK(s.link_up && s.phase == BM_SER_POLLING);
	CHECK(s.flavor == BM_SER_GENERIC_ASCII);
	CHECK(s.st.pack_ma == 2000 && s.st.pack_mv == 49500 && s.st.soc_dpct == 790);
	CHECK(s.sniff_frames == 2);

	/* manufacturer info reveals the vendor */
	len = bm_ydt_build(0x20, 0x02, 0x46, 0x51, NULL, 0, frame, sizeof(frame));
	CHECK(bm_serial_sniff(&s, (const uint8_t *)frame, (size_t)len, 50) == 1);
	n = t_hex2bin("55 53 33 30 30 30 43 20 20 20 01 02 50 79 6C 6F 6E", info, sizeof(info));
	len = bm_ydt_build(0x20, 0x02, 0x46, 0x00, info, (size_t)n, frame, sizeof(frame));
	CHECK(bm_serial_sniff(&s, (const uint8_t *)frame, (size_t)len, 60) == 1);
	CHECK(s.flavor == BM_SER_PYLON_ASCII);
	CHECK(strstr(s.flavor_str, "Pylontech-RS485 US3000C Pylon") != NULL);

	/* garbage and a broken checksum are rejected */
	CHECK(bm_serial_sniff(&s, (const uint8_t *)"hello world!!!!!", 16, 70) == 0);
	frame[5] ^= 0x01;
	CHECK(bm_serial_sniff(&s, (const uint8_t *)frame, (size_t)len, 80) == 0);
	CHECK(s.crc_errors == 1);
}
