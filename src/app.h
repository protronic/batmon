#ifndef BATMON_APP_H
#define BATMON_APP_H

#include <zephyr/kernel.h>
#include <zephyr/drivers/can.h>

#include "bm_proto.h"

#define APP_JSON_MAX 1536

struct app_state {
	struct k_mutex lock;
	struct bm_proto proto;

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

/* Snapshot of the protocol state under lock. */
void app_proto_snapshot(struct bm_proto *dst);

/* Telemetry JSON (nested or flat) built under lock; returns length or -1. */
int app_json(char *buf, size_t len, bool flat);

/* can_rx.c */
int can_rx_init(void);
int can_rx_set_heartbeat(bool enable);
const struct device *can_rx_device(void);

/* net_mqtt.c */
int net_mqtt_init(void);
int net_mqtt_publish_now(void);

/* ui.c */
int ui_init(void);

#endif /* BATMON_APP_H */
