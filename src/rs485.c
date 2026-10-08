/*
 * RS485 link to the BMS: interrupt driven UART receive into a ring buffer,
 * polled transmit with a driver-enable GPIO, request/response framing for the
 * YD/T ASCII, JK binary and Modbus RTU protocols, and a listen-only mode that
 * decodes the traffic between an inverter and the BMS.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/logging/log.h>

#include "app.h"

LOG_MODULE_REGISTER(batmon_rs485, LOG_LEVEL_INF);

#define RS485_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(protronic_batmon_rs485)

#define RX_RING_SIZE   2048
#define THREAD_STACK   4096
#define THREAD_PRIO    6
#define INTERBYTE_MS   120   /* ASCII/JK: give up a half received frame after this gap */
#define IDLE_GAP_MS    15    /* Modbus / unknown: frame ends after this silence */
#define SNIFF_WAIT_MS  1000
#define SNIFF_ROTATE_MS 6000 /* listen-only: try the next baud rate when nothing decodes */
#define SNIFF_LOST_MS  20000

static const struct device *const uart_dev = DEVICE_DT_GET(DT_PHANDLE(RS485_NODE, uart));
static const struct gpio_dt_spec de_gpio = GPIO_DT_SPEC_GET_OR(RS485_NODE, de_gpios, {0});

RING_BUF_DECLARE(rs485_rx_rb, RX_RING_SIZE);
static K_SEM_DEFINE(rx_sem, 0, 1);
static K_MUTEX_DEFINE(bus_lock);
static volatile bool rx_discard;
static uint32_t cur_baud;
static uint8_t frame_buf[BM_SER_MAX_FRAME];

static K_THREAD_STACK_DEFINE(batmon_rs485_stack, THREAD_STACK);
static struct k_thread rs485_thread_data;

static void uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);
	uint8_t tmp[32];

	while (uart_irq_update(dev) > 0 && uart_irq_is_pending(dev) > 0) {
		if (uart_irq_rx_ready(dev) <= 0) {
			break;
		}
		int n = uart_fifo_read(dev, tmp, sizeof(tmp));

		if (n <= 0) {
			break;
		}
		if (!rx_discard) {
			ring_buf_put(&rs485_rx_rb, tmp, (uint32_t)n);
			k_sem_give(&rx_sem);
		}
	}
}

static void rx_flush(void)
{
	ring_buf_reset(&rs485_rx_rb);
	k_sem_reset(&rx_sem);
}

/* One byte or -EAGAIN once the absolute deadline (uptime ms) passes. */
static int rx_byte(uint8_t *b, int64_t deadline)
{
	while (1) {
		if (ring_buf_get(&rs485_rx_rb, b, 1) == 1) {
			return 0;
		}
		int64_t left = deadline - k_uptime_get();

		if (left <= 0) {
			return -EAGAIN;
		}
		k_sem_take(&rx_sem, K_MSEC(left));
	}
}

static int set_baud(uint32_t baud)
{
	if (baud == 0 || baud == cur_baud) {
		return 0;
	}
	struct uart_config cfg = {
		.baudrate = baud,
		.parity = UART_CFG_PARITY_NONE,
		.stop_bits = UART_CFG_STOP_BITS_1,
		.data_bits = UART_CFG_DATA_BITS_8,
		.flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
	};
	int rc = uart_configure(uart_dev, &cfg);

	if (rc) {
		LOG_ERR("uart_configure(%u): %d", baud, rc);
		return rc;
	}
	cur_baud = baud;
	return 0;
}

static void tx_frame(const uint8_t *d, size_t n)
{
	rx_discard = true;
	rx_flush();
	if (de_gpio.port != NULL) {
		gpio_pin_set_dt(&de_gpio, 1);
	}
	for (size_t i = 0; i < n; i++) {
		uart_poll_out(uart_dev, d[i]);
	}
	/* uart_poll_out returns once the byte is queued: wait two character times */
	k_sleep(K_USEC(20000000u / (cur_baud ? cur_baud : 9600) + 200));
	if (de_gpio.port != NULL) {
		gpio_pin_set_dt(&de_gpio, 0);
	}
	rx_discard = false;
	rx_flush();
}

static int64_t min_deadline(int64_t a, int64_t b)
{
	return a < b ? a : b;
}

/* Collect one reply of the given kind. Returns its length, 0 on timeout. */
static size_t collect(enum bm_ser_kind kind, size_t expect, uint32_t timeout_ms, uint8_t *buf,
		      size_t max)
{
	int64_t deadline = k_uptime_get() + timeout_ms;
	size_t n = 0;
	uint8_t b;

	switch (kind) {
	case BM_SER_KIND_ASCII:
		do {
			if (rx_byte(&b, deadline)) {
				return 0;
			}
		} while (b != '~');
		buf[n++] = b;
		while (n < max) {
			if (rx_byte(&b, min_deadline(deadline, k_uptime_get() + INTERBYTE_MS))) {
				return 0;
			}
			buf[n++] = b;
			if (b == '\r') {
				return n;
			}
		}
		return 0;

	case BM_SER_KIND_JK:
		while (1) {
			if (rx_byte(&b, deadline)) {
				return 0;
			}
			if (b != 0x4E) {
				continue;
			}
			if (rx_byte(&b, min_deadline(deadline, k_uptime_get() + INTERBYTE_MS))) {
				return 0;
			}
			if (b == 0x57) {
				break;
			}
		}
		buf[0] = 0x4E;
		buf[1] = 0x57;
		n = 2;
		while (n < 4) {
			if (rx_byte(&buf[n], min_deadline(deadline, k_uptime_get() + INTERBYTE_MS))) {
				return 0;
			}
			n++;
		}
		size_t total = ((size_t)buf[2] << 8 | buf[3]) + 2;

		if (total > max || total < 21) {
			return 0;
		}
		while (n < total) {
			if (rx_byte(&buf[n], min_deadline(deadline, k_uptime_get() + INTERBYTE_MS))) {
				return 0;
			}
			n++;
		}
		return n;

	default: /* Modbus: fixed length when known, else idle gap */
		if (rx_byte(&b, deadline)) {
			return 0;
		}
		buf[n++] = b;
		while (n < max && (expect == 0 || n < expect)) {
			if (rx_byte(&b, min_deadline(deadline, k_uptime_get() + IDLE_GAP_MS))) {
				break;
			}
			buf[n++] = b;
		}
		return n;
	}
}

/* Listen-only: any frame on the bus, framed by its own syntax or an idle gap. */
static size_t collect_any(uint8_t *buf, size_t max, uint32_t wait_ms)
{
	int64_t deadline = k_uptime_get() + wait_ms;
	size_t n = 0;
	uint8_t b;

	if (rx_byte(&b, deadline)) {
		return 0;
	}
	buf[n++] = b;
	if (b == '~') {
		while (n < max) {
			if (rx_byte(&b, k_uptime_get() + INTERBYTE_MS)) {
				return 0;
			}
			buf[n++] = b;
			if (b == '\r') {
				return n;
			}
		}
		return 0;
	}
	while (n < max) {
		if (rx_byte(&b, k_uptime_get() + IDLE_GAP_MS)) {
			break;
		}
		buf[n++] = b;
		if (n == 4 && buf[0] == 0x4E && buf[1] == 0x57) {
			size_t total = ((size_t)buf[2] << 8 | buf[3]) + 2;

			if (total > max) {
				return 0;
			}
			while (n < total) {
				if (rx_byte(&buf[n], k_uptime_get() + INTERBYTE_MS)) {
					return 0;
				}
				n++;
			}
			return n;
		}
	}
	return n;
}

int rs485_transact(const uint8_t *tx, size_t tx_len, enum bm_ser_kind kind, uint32_t baud,
		   uint8_t *rx, size_t rx_max, uint32_t timeout_ms)
{
	if (IS_ENABLED(CONFIG_BATMON_RS485_LISTEN_ONLY)) {
		return -EACCES;
	}
	k_mutex_lock(&bus_lock, K_FOREVER);
	int rc = set_baud(baud);

	if (rc) {
		k_mutex_unlock(&bus_lock);
		return rc;
	}
	tx_frame(tx, tx_len);
	size_t n = collect(kind, 0, timeout_ms, rx, rx_max);

	k_mutex_unlock(&bus_lock);
	return (int)n;
}

static void poll_loop(void)
{
	struct bm_serial_req req;

	while (1) {
		uint32_t now = app_uptime_ms();

		k_mutex_lock(&app.lock, K_FOREVER);
		int have = bm_serial_next_request(&app.serial, now, &req);

		k_mutex_unlock(&app.lock);
		if (!have) {
			k_sleep(K_MSEC(20));
			continue;
		}
		k_mutex_lock(&bus_lock, K_FOREVER);
		set_baud(req.baud);
		tx_frame(req.data, req.len);
		size_t n = collect(req.kind, req.expect_len, req.timeout_ms, frame_buf,
				   sizeof(frame_buf));

		k_mutex_unlock(&bus_lock);
		k_mutex_lock(&app.lock, K_FOREVER);
		bm_serial_response(&app.serial, frame_buf, n, app_uptime_ms());
		k_mutex_unlock(&app.lock);
	}
}

static void sniff_loop(void)
{
	static const uint32_t bauds[] = { 9600, 115200, 19200 };
	unsigned int idx = 0;

	while (1) {
		uint32_t baud = CONFIG_BATMON_RS485_BAUD ? CONFIG_BATMON_RS485_BAUD : bauds[idx];

		set_baud(baud);
		rx_flush();
		int64_t start = k_uptime_get(), last_ok = 0;
		unsigned int ok = 0;

		LOG_INF("listening at %u bit/s", baud);
		while (1) {
			size_t n = collect_any(frame_buf, sizeof(frame_buf), SNIFF_WAIT_MS);

			if (n) {
				k_mutex_lock(&app.lock, K_FOREVER);
				int good = bm_serial_sniff(&app.serial, frame_buf, n, app_uptime_ms());

				k_mutex_unlock(&app.lock);
				if (good) {
					ok++;
					last_ok = k_uptime_get();
				}
			}
			if (CONFIG_BATMON_RS485_BAUD) {
				continue;
			}
			if (ok == 0 && k_uptime_get() - start > SNIFF_ROTATE_MS) {
				break;
			}
			if (ok && k_uptime_get() - last_ok > SNIFF_LOST_MS) {
				break;
			}
		}
		idx = (idx + 1) % ARRAY_SIZE(bauds);
	}
}

static void rs485_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	if (IS_ENABLED(CONFIG_BATMON_RS485_LISTEN_ONLY)) {
		sniff_loop();
	} else {
		poll_loop();
	}
}

int rs485_init(void)
{
	if (!device_is_ready(uart_dev)) {
		LOG_ERR("UART not ready");
		return -ENODEV;
	}
	if (de_gpio.port != NULL) {
		if (!device_is_ready(de_gpio.port)) {
			LOG_ERR("DE GPIO not ready");
			return -ENODEV;
		}
		gpio_pin_configure_dt(&de_gpio, GPIO_OUTPUT_INACTIVE);
	}
	set_baud(CONFIG_BATMON_RS485_BAUD ? CONFIG_BATMON_RS485_BAUD : 9600);
	uart_irq_callback_user_data_set(uart_dev, uart_isr, NULL);
	uart_irq_rx_enable(uart_dev);

	k_thread_create(&rs485_thread_data, batmon_rs485_stack, K_THREAD_STACK_SIZEOF(batmon_rs485_stack),
			rs485_thread, NULL, NULL, NULL, THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&rs485_thread_data, "rs485");
	app.rs485_present = true;
	LOG_INF("RS485 on %s, DE %s, %s", uart_dev->name, de_gpio.port ? "via GPIO" : "automatic",
		IS_ENABLED(CONFIG_BATMON_RS485_LISTEN_ONLY) ? "listen-only" : "polling");
	return 0;
}
