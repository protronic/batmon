/*
 * batmon – BMS serial (RS485/UART) protocols: probing, polling and decoding.
 *
 * Supported flavours
 *   - Pylontech RS485 ASCII (US2000/US3000/US5000, Force L): 0x42 analog, 0x44 alarm,
 *     0x51 manufacturer info, 0x92 charge/discharge management, 0x93 serial number
 *   - PACE BMS ASCII (FM-Solar/DelongTop, Gobel, SOK, Jakiper ...): 0x42, 0x44, 0x51
 *   - Seplos V2 ASCII: 0x42 telemetry, 0x44 telecommand, 0x51
 *   - JK-BMS RS485 binary (4E 57 frames, "read all" 0x06)
 *   - Seplos V3 Modbus RTU (input registers 0x1000 PIA, 0x1100 PIB)
 *
 * The module does no I/O: the host asks for the next request to send
 * (bm_serial_next_request), transmits it, collects the reply and hands the
 * bytes back (bm_serial_response). In listen-only setups every frame seen on
 * the bus goes to bm_serial_sniff instead. Plain C99, host-testable.
 */
#ifndef BM_SERIAL_H
#define BM_SERIAL_H

#include "bm_ydt.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BM_SER_MAX_CELLS   32
#define BM_SER_MAX_TEMPS   8
#define BM_SER_MAX_FRAME   1024
#define BM_SER_STATUS_RAW  24
#define BM_SER_REQ_MAX     64

enum bm_ser_flavor {
	BM_SER_NONE = 0,
	BM_SER_PYLON_ASCII,
	BM_SER_PACE_ASCII,
	BM_SER_SEPLOS_ASCII,
	BM_SER_GENERIC_ASCII,   /* answers YD/T frames, payload layout guessed */
	BM_SER_JK_BIN,
	BM_SER_SEPLOS_MODBUS,
	BM_SER_FLAVOR_COUNT
};

enum bm_ser_kind {
	BM_SER_KIND_ASCII = 0,  /* '~' ... '\r' */
	BM_SER_KIND_JK,         /* 4E 57 LEN ... */
	BM_SER_KIND_MODBUS      /* fixed length / idle gap framed */
};

enum bm_ser_phase {
	BM_SER_PROBING = 0,
	BM_SER_IDENTIFY,
	BM_SER_POLLING
};

/* bm_serial_state.valid bits */
#define BM_SV_CELLS    (1u << 0)
#define BM_SV_TEMPS    (1u << 1)
#define BM_SV_MEAS     (1u << 2)
#define BM_SV_CAP      (1u << 3)
#define BM_SV_SOC      (1u << 4)
#define BM_SV_SOH      (1u << 5)
#define BM_SV_CYCLES   (1u << 6)
#define BM_SV_LIMITS   (1u << 7)
#define BM_SV_STATUS   (1u << 8)
#define BM_SV_IDENT    (1u << 9)
#define BM_SV_SERIAL   (1u << 10)
#define BM_SV_JKFLAGS  (1u << 11)
#define BM_SV_PORT_U   (1u << 12)

struct bm_serial_state {
	uint32_t valid;
	uint8_t n_cells;
	uint16_t cell_mv[BM_SER_MAX_CELLS];
	uint8_t n_temps;
	int16_t temp_dc[BM_SER_MAX_TEMPS];   /* 0.1 °C */
	int32_t pack_mv;
	int32_t pack_ma;                     /* + = charging */
	int32_t port_mv;
	uint32_t remain_mah;
	uint32_t full_mah;
	uint32_t rated_mah;
	uint16_t cycles;
	uint16_t soc_dpct;                   /* 0.1 % */
	uint16_t soh_dpct;
	bool soc_derived;                    /* SoC computed from remaining/full */
	int32_t cvl_mv, dvl_mv, ccl_ma, dcl_ma;
	uint8_t req_flags;                   /* Pylontech 0x92 status byte */
	uint8_t cell_status[BM_SER_MAX_CELLS];
	uint8_t temp_status[BM_SER_MAX_TEMPS];
	uint8_t status_raw[BM_SER_STATUS_RAW];
	uint8_t n_status_raw;
	uint16_t jk_alarm;
	uint16_t jk_status;
	uint8_t user_defined;
	char model[17];
	char manufacturer[21];
	char sw_version[12];
	char serial[21];
};

struct bm_serial_req {
	enum bm_ser_kind kind;
	uint32_t baud;
	uint16_t timeout_ms;
	uint16_t expect_len;    /* modbus: expected reply length, 0 = unknown */
	uint8_t cid2;           /* ascii: command code (bookkeeping) */
	size_t len;
	uint8_t data[BM_SER_REQ_MAX];
};

struct bm_serial_cfg {
	uint32_t poll_ms;               /* pause between poll rounds */
	uint32_t fixed_baud;            /* 0 = probe */
	int fixed_addr;                 /* -1 = probe */
	enum bm_ser_flavor fixed_flavor; /* BM_SER_NONE = probe */
	bool jk_current_legacy;         /* old JK firmware: current = 10000 - raw */
};

struct bm_serial {
	struct bm_serial_cfg cfg;
	struct bm_serial_state st;
	enum bm_ser_flavor flavor;
	enum bm_ser_phase phase;
	uint8_t ver;
	uint8_t adr;
	uint32_t baud;
	bool link_up;
	unsigned int probe_idx;
	unsigned int poll_step;
	uint8_t ident_tries;
	uint32_t unsupported;           /* bit per optional command, see bm_serial.c */
	uint32_t next_ms;
	uint32_t last_ok_ms;
	uint32_t ok_count;
	uint32_t err_count;
	uint32_t timeout_count;
	uint32_t crc_errors;
	uint8_t fails_in_row;
	uint8_t last_cid2;
	uint16_t last_reg;
	enum bm_ser_kind last_kind;
	uint8_t sniff_req_cid2;
	uint32_t sniff_frames;
	char flavor_str[72];
	char last_err[40];
	uint8_t last_rx[BM_SER_MAX_FRAME];
	size_t last_rx_len;
};

void bm_serial_init(struct bm_serial *s, const struct bm_serial_cfg *cfg);
void bm_serial_restart(struct bm_serial *s);

/* Fill the next request to transmit. Returns 1 when req is valid, 0 when nothing is due yet. */
int bm_serial_next_request(struct bm_serial *s, uint32_t now_ms, struct bm_serial_req *req);

/* Reply to the last request; len 0 means timeout. */
void bm_serial_response(struct bm_serial *s, const uint8_t *data, size_t len, uint32_t now_ms);

/* Passive mode: any frame seen on the bus. Returns 1 if the frame was understood. */
int bm_serial_sniff(struct bm_serial *s, const uint8_t *data, size_t len, uint32_t now_ms);

/* Build an ASCII command with the detected VER/ADR (shell helper). Returns bytes or -1. */
int bm_serial_build_cmd(const struct bm_serial *s, uint8_t cid2, const uint8_t *info,
			size_t info_len, uint8_t *out, size_t out_max);

const char *bm_serial_flavor_name(enum bm_ser_flavor f);
const char *bm_serial_phase_name(enum bm_ser_phase p);
uint16_t bm_serial_timeout_ms(uint32_t baud, enum bm_ser_kind kind);

/* Decoders, exposed for tests. Return 0 on success. */
int bm_serial_decode_ascii(struct bm_serial *s, enum bm_ser_flavor fl, uint8_t cid2,
			   const uint8_t *info, size_t len);
int bm_serial_decode_jk(struct bm_serial *s, const uint8_t *frame, size_t len);
int bm_serial_decode_modbus(struct bm_serial *s, uint16_t reg, const uint8_t *regs, size_t nregs);

#ifdef __cplusplus
}
#endif

#endif /* BM_SERIAL_H */
