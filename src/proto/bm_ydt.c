#include "bm_ydt.h"

#include <stdio.h>
#include <string.h>

static int hexval(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

int bm_hex_decode(const char *hex, size_t nchars, uint8_t *out, size_t out_max)
{
	if (nchars % 2 != 0 || nchars / 2 > out_max) {
		return -1;
	}
	for (size_t i = 0; i < nchars; i += 2) {
		int hi = hexval(hex[i]);
		int lo = hexval(hex[i + 1]);

		if (hi < 0 || lo < 0) {
			return -1;
		}
		out[i / 2] = (uint8_t)((hi << 4) | lo);
	}
	return (int)(nchars / 2);
}

size_t bm_hex_encode(const uint8_t *in, size_t n, char *out, size_t out_max)
{
	static const char digits[] = "0123456789ABCDEF";
	size_t pos = 0;

	for (size_t i = 0; i < n && pos + 2 < out_max; i++) {
		out[pos++] = digits[in[i] >> 4];
		out[pos++] = digits[in[i] & 0xF];
	}
	if (out_max > 0) {
		out[pos] = '\0';
	}
	return pos;
}

uint16_t bm_ydt_checksum(const char *s, size_t len)
{
	uint32_t sum = 0;

	for (size_t i = 0; i < len; i++) {
		sum += (uint8_t)s[i];
	}
	return (uint16_t)((~sum + 1u) & 0xFFFFu);
}

uint16_t bm_ydt_length_field(size_t info_chars)
{
	uint32_t lenid = (uint32_t)info_chars & 0xFFFu;
	uint32_t sum = (lenid & 0xF) + ((lenid >> 4) & 0xF) + ((lenid >> 8) & 0xF);
	uint32_t lchk = (~sum + 1u) & 0xFu;

	return (uint16_t)((lchk << 12) | lenid);
}

int bm_ydt_build(uint8_t ver, uint8_t adr, uint8_t cid1, uint8_t cid2, const uint8_t *info,
		 size_t info_len, char *out, size_t out_len)
{
	size_t need = 1 + 12 + info_len * 2 + 4 + 1 + 1;

	if (out_len < need || info_len > BM_YDT_MAX_INFO) {
		return -1;
	}
	size_t pos = 0;

	out[pos++] = '~';
	pos += (size_t)snprintf(out + pos, out_len - pos, "%02X%02X%02X%02X%04X", ver, adr, cid1,
				cid2, bm_ydt_length_field(info_len * 2));
	pos += bm_hex_encode(info, info_len, out + pos, out_len - pos);
	uint16_t chk = bm_ydt_checksum(out + 1, pos - 1);

	pos += (size_t)snprintf(out + pos, out_len - pos, "%04X", chk);
	out[pos++] = '\r';
	out[pos] = '\0';
	return (int)pos;
}

int bm_ydt_parse(const char *frame, size_t len, struct bm_ydt_frame *f, uint8_t *info_buf,
		 size_t info_max)
{
	/* strip SOI and trailing CR/LF/NUL */
	if (len > 0 && frame[0] == '~') {
		frame++;
		len--;
	}
	while (len > 0 && (frame[len - 1] == '\r' || frame[len - 1] == '\n' ||
			   frame[len - 1] == '\0')) {
		len--;
	}
	if (len < 16) {
		return -1;
	}
	uint8_t hdr[6];

	if (bm_hex_decode(frame, 12, hdr, sizeof(hdr)) != 6) {
		return -1;
	}
	uint16_t chk_rx;
	uint8_t chk_bytes[2];

	if (bm_hex_decode(frame + len - 4, 4, chk_bytes, 2) != 2) {
		return -1;
	}
	chk_rx = (uint16_t)((chk_bytes[0] << 8) | chk_bytes[1]);
	if (bm_ydt_checksum(frame, len - 4) != chk_rx) {
		return -3;
	}
	uint16_t length = (uint16_t)((hdr[4] << 8) | hdr[5]);
	size_t lenid = length & 0xFFF;
	size_t info_chars = len - 16;

	if (lenid != info_chars) {
		return -2;
	}
	memset(f, 0, sizeof(*f));
	f->ver = hdr[0];
	f->adr = hdr[1];
	f->cid1 = hdr[2];
	f->cid2 = hdr[3];
	f->lchksum_ok = (length == bm_ydt_length_field(info_chars));
	int n = bm_hex_decode(frame + 12, info_chars, info_buf, info_max);

	if (n < 0) {
		return (info_chars / 2 > info_max) ? -4 : -1;
	}
	f->info_len = (size_t)n;
	return 0;
}

const char *bm_ydt_rtn_name(uint8_t rtn)
{
	switch (rtn) {
	case 0x00: return "ok";
	case 0x01: return "version error";
	case 0x02: return "checksum error";
	case 0x03: return "length checksum error";
	case 0x04: return "unsupported command";
	case 0x05: return "command format error";
	case 0x06: return "invalid data";
	case 0x90: return "address error";
	case 0x91: return "communication error";
	default: return "unknown return code";
	}
}

uint16_t bm_modbus_crc(const uint8_t *d, size_t n)
{
	uint16_t crc = 0xFFFF;

	for (size_t i = 0; i < n; i++) {
		crc ^= d[i];
		for (int b = 0; b < 8; b++) {
			if (crc & 1) {
				crc = (uint16_t)((crc >> 1) ^ 0xA001);
			} else {
				crc >>= 1;
			}
		}
	}
	return crc;
}
