#ifndef BATMON_APP_H
#define BATMON_APP_H

#include <zephyr/kernel.h>
#include <zephyr/drivers/can.h>

#include "bm_proto.h"
#include "bm_serial.h"

#define APP_JSON_MAX 2560

struct app_state {
	struct k_mutex lock;
	struct bm_proto proto;
	struct bm_serial serial;
	bool rs485_present;

	/* CAN */
	enum can_state can_state;
	uint32_t can_rx_total;
	uint32_t can_rx_dropped;
	uint32_t can_tx_errors;
	bool raw_log;
	bool bench_heartbeat;

	/* network */
	bool net_up;
	char ip[16];
	bool mqtt_connected;
	uint32_t mqtt_pub_count;
	uint32_t mqtt_errors;
};

extern struct app_state app;

static inline uint32_t app_uptime_ms(void)
{
	return k_uptime_get_32();
}

/* Snapshots of the protocol states under lock. */
void app_proto_snapshot(struct bm_proto *dst);
void app_serial_snapshot(struct bm_serial *dst);

/* Telemetry JSON (nested or flat) built under lock; returns length or -1. */
int app_json(char *buf, size_t len, bool flat);

/* can_rx.c */
int can_rx_init(void);
int can_rx_set_heartbeat(bool enable);
const struct device *can_rx_device(void);

/* rs485.c */
int rs485_init(void);
/* One request/response on the bus (shell). Returns reply length, 0 on timeout, -errno. */
int rs485_transact(const uint8_t *tx, size_t tx_len, enum bm_ser_kind kind, uint32_t baud,
		   uint8_t *rx, size_t rx_max, uint32_t timeout_ms);

/* net_mqtt.c */
int net_mqtt_init(void);
int net_mqtt_publish_now(void);

/* ui.c */
int ui_init(void);

#endif /* BATMON_APP_H */
