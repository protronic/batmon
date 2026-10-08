#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/logging/log.h>

#include "app.h"

LOG_MODULE_REGISTER(batmon_can, LOG_LEVEL_INF);

#define RX_QUEUE_LEN     64
#define RX_THREAD_STACK  2048
#define RX_THREAD_PRIO   5

struct rx_item {
	struct can_frame frame;
	uint32_t ms;
};

K_MSGQ_DEFINE(rx_msgq, sizeof(struct rx_item), RX_QUEUE_LEN, 4);
static K_THREAD_STACK_DEFINE(can_rx_stack, RX_THREAD_STACK);
static struct k_thread rx_thread_data;

static const struct device *const can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));

const struct device *can_rx_device(void)
{
	return can_dev;
}

static void rx_cb(const struct device *dev, struct can_frame *frame, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);
	struct rx_item item = { .frame = *frame, .ms = k_uptime_get_32() };

	if (k_msgq_put(&rx_msgq, &item, K_NO_WAIT) != 0) {
		app.can_rx_dropped++;
	}
}

static void state_cb(const struct device *dev, enum can_state state, struct can_bus_err_cnt err_cnt,
		     void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);
	app.can_state = state;
	LOG_WRN("CAN state %d (tx err %u, rx err %u)", state, err_cnt.tx_err_cnt, err_cnt.rx_err_cnt);
}

static void heartbeat_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(heartbeat_work, heartbeat_work_fn);

static void heartbeat_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	if (!app.bench_heartbeat) {
		return;
	}
	struct can_frame frame = { .id = 0x305, .dlc = 8, .flags = 0 };

	bm_build_heartbeat(frame.data);
	int rc = can_send(can_dev, &frame, K_MSEC(50), NULL, NULL);

	if (rc != 0) {
		app.can_tx_errors++;
	}
	k_work_schedule(&heartbeat_work, K_SECONDS(1));
}

int can_rx_set_heartbeat(bool enable)
{
	if (IS_ENABLED(CONFIG_BATMON_CAN_LISTEN_ONLY)) {
		return -EACCES;
	}
	app.bench_heartbeat = enable;
	if (enable) {
		k_work_schedule(&heartbeat_work, K_NO_WAIT);
	} else {
		k_work_cancel_delayable(&heartbeat_work);
	}
	return 0;
}

static void rx_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	struct rx_item item;

	while (1) {
		if (k_msgq_get(&rx_msgq, &item, K_MSEC(500)) != 0) {
			continue;
		}
		const struct can_frame *f = &item.frame;

		if (f->flags & CAN_FRAME_RTR) {
			continue;
		}
		bool ext = (f->flags & CAN_FRAME_IDE) != 0;
		uint8_t len = can_dlc_to_bytes(f->dlc);

		if (len > 8) {
			len = 8;
		}
		app.can_rx_total++;
		if (app.raw_log) {
			LOG_INF("rx %s%03x [%u] %02x %02x %02x %02x %02x %02x %02x %02x",
				ext ? "x" : "", (unsigned int)f->id, len, f->data[0], f->data[1],
				f->data[2], f->data[3], f->data[4], f->data[5], f->data[6], f->data[7]);
		}
		k_mutex_lock(&app.lock, K_FOREVER);
		bm_proto_feed(&app.proto, f->id, ext, f->data, len, item.ms);
		k_mutex_unlock(&app.lock);
	}
}

int can_rx_init(void)
{
	if (!device_is_ready(can_dev)) {
		LOG_ERR("CAN device not ready");
		return -ENODEV;
	}
	int rc = can_set_bitrate(can_dev, CONFIG_BATMON_CAN_BITRATE);

	if (rc) {
		LOG_ERR("can_set_bitrate(%d): %d", CONFIG_BATMON_CAN_BITRATE, rc);
		return rc;
	}
	can_mode_t mode = IS_ENABLED(CONFIG_BATMON_CAN_LISTEN_ONLY) ? CAN_MODE_LISTENONLY
								     : CAN_MODE_NORMAL;

	rc = can_set_mode(can_dev, mode);
	if (rc) {
		LOG_ERR("can_set_mode: %d", rc);
		return rc;
	}
	can_set_state_change_callback(can_dev, state_cb, NULL);

	const struct can_filter std = { .id = 0, .mask = 0, .flags = 0 };
	const struct can_filter ext = { .id = 0, .mask = 0, .flags = CAN_FILTER_IDE };

	if (can_add_rx_filter(can_dev, rx_cb, NULL, &std) < 0 ||
	    can_add_rx_filter(can_dev, rx_cb, NULL, &ext) < 0) {
		LOG_ERR("cannot add rx filters");
		return -EIO;
	}
	rc = can_start(can_dev);
	if (rc) {
		LOG_ERR("can_start: %d", rc);
		return rc;
	}
	k_thread_create(&rx_thread_data, can_rx_stack, K_THREAD_STACK_SIZEOF(can_rx_stack), rx_thread, NULL,
			NULL, NULL, RX_THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&rx_thread_data, "can_rx");

	if (app.bench_heartbeat) {
		can_rx_set_heartbeat(true);
	}
	LOG_INF("CAN %s at %d bit/s, %s", can_dev->name, CONFIG_BATMON_CAN_BITRATE,
		mode == CAN_MODE_LISTENONLY ? "listen-only" : "normal mode");
	return 0;
}
