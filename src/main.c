#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <app_version.h>

#include "app.h"

LOG_MODULE_REGISTER(batmon, LOG_LEVEL_INF);

struct app_state app;

#if defined(CONFIG_BATMON_RS485_FLAVOR_PYLON)
#define RS485_FIXED_FLAVOR BM_SER_PYLON_ASCII
#elif defined(CONFIG_BATMON_RS485_FLAVOR_PACE)
#define RS485_FIXED_FLAVOR BM_SER_PACE_ASCII
#elif defined(CONFIG_BATMON_RS485_FLAVOR_SEPLOS)
#define RS485_FIXED_FLAVOR BM_SER_SEPLOS_ASCII
#elif defined(CONFIG_BATMON_RS485_FLAVOR_JK)
#define RS485_FIXED_FLAVOR BM_SER_JK_BIN
#elif defined(CONFIG_BATMON_RS485_FLAVOR_SEPLOS_V3)
#define RS485_FIXED_FLAVOR BM_SER_SEPLOS_MODBUS
#else
#define RS485_FIXED_FLAVOR BM_SER_NONE
#endif

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios, {0});

void app_proto_snapshot(struct bm_proto *dst)
{
	k_mutex_lock(&app.lock, K_FOREVER);
	*dst = app.proto;
	k_mutex_unlock(&app.lock);
}

void app_serial_snapshot(struct bm_serial *dst)
{
	k_mutex_lock(&app.lock, K_FOREVER);
	*dst = app.serial;
	k_mutex_unlock(&app.lock);
}

int app_json(char *buf, size_t len, bool flat)
{
	k_mutex_lock(&app.lock, K_FOREVER);
	const struct bm_serial *ser = app.rs485_present ? &app.serial : NULL;
	int n = flat ? bm_json_flat_ex(&app.proto, ser, app_uptime_ms(), buf, len)
		     : bm_json_telemetry_ex(&app.proto, ser, app_uptime_ms(), buf, len);
	k_mutex_unlock(&app.lock);
	return n;
}

int main(void)
{
	k_mutex_init(&app.lock);
	bm_proto_init(&app.proto);
#ifdef CONFIG_BATMON_RS485
	struct bm_serial_cfg scfg = {
		.poll_ms = CONFIG_BATMON_RS485_POLL_MS,
		.fixed_baud = CONFIG_BATMON_RS485_BAUD,
		.fixed_addr = CONFIG_BATMON_RS485_ADDR,
		.fixed_flavor = RS485_FIXED_FLAVOR,
		.jk_current_legacy = IS_ENABLED(CONFIG_BATMON_JK_CURRENT_LEGACY),
	};

	bm_serial_init(&app.serial, &scfg);
#else
	bm_serial_init(&app.serial, NULL);
#endif
	app.raw_log = IS_ENABLED(CONFIG_BATMON_RAW_LOG);
	app.bench_heartbeat = IS_ENABLED(CONFIG_BATMON_BENCH_HEARTBEAT);

	if (led.port != NULL && device_is_ready(led.port)) {
		gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	}

	LOG_INF("batmon %s starting, device id %s", APP_VERSION_STRING, CONFIG_BATMON_DEVICE_ID);

	int rc = can_rx_init();

	if (rc) {
		LOG_ERR("CAN init failed (%d)", rc);
	}
#ifdef CONFIG_BATMON_RS485
	rc = rs485_init();
	if (rc) {
		LOG_ERR("RS485 init failed (%d)", rc);
	}
#endif
#ifdef CONFIG_BATMON_UI
	rc = ui_init();
	if (rc) {
		LOG_ERR("UI init failed (%d)", rc);
	}
#endif
#ifdef CONFIG_BATMON_MQTT
	rc = net_mqtt_init();
	if (rc) {
		LOG_ERR("network init failed (%d)", rc);
	}
#endif

	bool on = false;

	while (1) {
		k_mutex_lock(&app.lock, K_FOREVER);
		bm_proto_tick(&app.proto, app_uptime_ms());
		bool link = app.proto.link_up;
		k_mutex_unlock(&app.lock);

		/* LED: steady when battery frames arrive, blinking while waiting */
		if (led.port != NULL) {
			on = link ? true : !on;
			gpio_pin_set_dt(&led, on);
		}
		k_sleep(K_MSEC(link ? 1000 : 500));
	}
	return 0;
}
