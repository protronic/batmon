#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <app_version.h>

#include "app.h"

LOG_MODULE_REGISTER(batmon, LOG_LEVEL_INF);

struct app_state app;

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios, {0});

void app_proto_snapshot(struct bm_proto *dst)
{
	k_mutex_lock(&app.lock, K_FOREVER);
	*dst = app.proto;
	k_mutex_unlock(&app.lock);
}

int app_json(char *buf, size_t len, bool flat)
{
	k_mutex_lock(&app.lock, K_FOREVER);
	int n = flat ? bm_json_flat(&app.proto, app_uptime_ms(), buf, len)
		     : bm_json_telemetry(&app.proto, app_uptime_ms(), buf, len);
	k_mutex_unlock(&app.lock);
	return n;
}

int main(void)
{
	k_mutex_init(&app.lock);
	bm_proto_init(&app.proto);
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
