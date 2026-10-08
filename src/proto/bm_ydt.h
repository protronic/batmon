/*
 * YD/T 1363 style ASCII frames as used by Pylontech, PACE and Seplos (V2) BMS
 * over RS485/RS232, plus Modbus CRC and hex helpers. Plain C99.
 *
 *   ~ VER ADR CID1 CID2 LENGTH INFO... CHKSUM \r
 *     VER/ADR/CID1/CID2: one byte each as two hex chars
 *     LENGTH: 4 hex chars = LCHKSUM (4 bit) + LENID (12 bit, number of INFO chars)
 *     CHKSUM: two's complement of the sum of all chars between ~ and CHKSUM
 */
#ifndef BM_YDT_H
#define BM_YDT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BM_YDT_MAX_INFO 512

struct bm_ydt_frame {
	uint8_t ver;
	uint8_t adr;
	uint8_t cid1;
	uint8_t cid2;      /* command in a request, return code (RTN) in a response */
	size_t info_len;   /* decoded INFO bytes */
	bool lchksum_ok;   /* length nibble checksum matched (tolerated if not) */
};

/* Build "~...\r" into out. Returns the length (without NUL) or -1. */
int bm_ydt_build(uint8_t ver, uint8_t adr, uint8_t cid1, uint8_t cid2, const uint8_t *info,
		 size_t info_len, char *out, size_t out_len);

/*
 * Parse a frame (leading '~' and trailing CR/LF optional). INFO is hex-decoded
 * into info_buf. Returns 0 on success, -1 malformed, -2 LENGTH mismatch,
 * -3 CHKSUM mismatch, -4 INFO larger than info_max.
 */
int bm_ydt_parse(const char *frame, size_t len, struct bm_ydt_frame *f, uint8_t *info_buf,
		 size_t info_max);

uint16_t bm_ydt_checksum(const char *s, size_t len);
uint16_t bm_ydt_length_field(size_t info_chars);
const char *bm_ydt_rtn_name(uint8_t rtn);

/* Hex helpers: decode returns bytes or -1; encode writes uppercase, NUL terminated. */
int bm_hex_decode(const char *hex, size_t nchars, uint8_t *out, size_t out_max);
size_t bm_hex_encode(const uint8_t *in, size_t n, char *out, size_t out_max);

/* Modbus RTU CRC16 (poly 0xA001, init 0xFFFF). Low byte goes first on the wire. */
uint16_t bm_modbus_crc(const uint8_t *d, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* BM_YDT_H */
