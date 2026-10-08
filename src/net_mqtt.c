#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/random/random.h>

#include <stdio.h>
#include <string.h>

#include "app.h"

LOG_MODULE_REGISTER(batmon_net, LOG_LEVEL_INF);

#define MQTT_THREAD_STACK 4096
#define MQTT_THREAD_PRIO  7
#define RX_BUF_SIZE       512
#define TX_BUF_SIZE       4096
#define RECONNECT_MIN_MS  2000
#define RECONNECT_MAX_MS  60000

static K_THREAD_STACK_DEFINE(batmon_mqtt_stack, MQTT_THREAD_STACK);
static struct k_thread mqtt_thread_data;
static K_SEM_DEFINE(net_ready, 0, 1);
static K_SEM_DEFINE(publish_req, 0, 1);

static struct net_mgmt_event_callback mgmt_cb;
static struct mqtt_client client;
static struct sockaddr_storage broker;
static uint8_t rx_buf[RX_BUF_SIZE];
static uint8_t tx_buf[TX_BUF_SIZE];
static char payload[APP_JSON_MAX];
static bool connected;

#ifdef CONFIG_BATMON_MQTT_MODE_THINGSBOARD
#define TOPIC_TELEMETRY "v1/devices/me/telemetry"
#define TOPIC_ATTRS     "v1/devices/me/attributes"
#else
#define TOPIC_PREFIX    CONFIG_BATMON_MQTT_TOPIC_PREFIX "/" CONFIG_BATMON_DEVICE_ID
#define TOPIC_STATE     TOPIC_PREFIX "/state"
#define TOPIC_STATUS    TOPIC_PREFIX "/status"
#endif

static void net_event(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
	ARG_UNUSED(cb);
	if (event == NET_EVENT_IPV4_ADDR_ADD) {
		struct in_addr *addr = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);

		if (addr) {
			net_addr_ntop(AF_INET, addr, app.ip, sizeof(app.ip));
		}
		app.net_up = true;
		LOG_INF("IPv4 address %s", app.ip);
		k_sem_give(&net_ready);
	} else if (event == NET_EVENT_IF_DOWN) {
		app.net_up = false;
		LOG_WRN("network interface down");
	}
}

static void mqtt_evt(struct mqtt_client *c, const struct mqtt_evt *evt)
{
	ARG_UNUSED(c);
	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		if (evt->result == 0) {
			connected = true;
			app.mqtt_connected = true;
			LOG_INF("MQTT connected");
		} else {
			LOG_ERR("MQTT connect refused (%d)", evt->result);
		}
		break;
	case MQTT_EVT_DISCONNECT:
		connected = false;
		app.mqtt_connected = false;
		LOG_WRN("MQTT disconnected (%d)", evt->result);
		break;
	case MQTT_EVT_PUBACK:
	case MQTT_EVT_PINGRESP:
	default:
		break;
	}
}

static int resolve_broker(void)
{
	struct zsock_addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
	struct zsock_addrinfo *res = NULL;
	char port[8];

	snprintf(port, sizeof(port), "%d", CONFIG_BATMON_MQTT_BROKER_PORT);
	int rc = zsock_getaddrinfo(CONFIG_BATMON_MQTT_BROKER_HOST, port, &hints, &res);

	if (rc != 0 || res == NULL) {
		LOG_ERR("cannot resolve %s (%d)", CONFIG_BATMON_MQTT_BROKER_HOST, rc);
		return -EHOSTUNREACH;
	}
	memcpy(&broker, res->ai_addr, MIN(res->ai_addrlen, sizeof(broker)));
	zsock_freeaddrinfo(res);
	return 0;
}

static int publish(const char *topic, const char *data, size_t len, bool retain)
{
	struct mqtt_publish_param param = {
		.message.topic.topic.utf8 = (uint8_t *)topic,
		.message.topic.topic.size = strlen(topic),
		.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE,
		.message.payload.data = (uint8_t *)data,
		.message.payload.len = len,
		.message_id = (uint16_t)sys_rand32_get(),
		.dup_flag = 0,
		.retain_flag = retain ? 1 : 0,
	};
	int rc = mqtt_publish(&client, &param);

	if (rc) {
		app.mqtt_errors++;
	} else {
		app.mqtt_pub_count++;
	}
	return rc;
}

#if defined(CONFIG_BATMON_HA_DISCOVERY)
struct ha_entity {
	const char *key;
	const char *name;
	const char *tpl;
	const char *unit;
	const char *dev_class;
};

static const struct ha_entity ha_entities[] = {
	{ "soc", "SoC", "{{ value_json.pack.soc }}", "%", "battery" },
	{ "soh", "SoH", "{{ value_json.pack.soh }}", "%", NULL },
	{ "voltage", "Spannung", "{{ value_json.pack.u }}", "V", "voltage" },
	{ "current", "Strom", "{{ value_json.pack.i }}", "A", "current" },
	{ "temperature", "Temperatur", "{{ value_json.pack.t }}", "°C", "temperature" },
	{ "cvl", "Ladespannungsgrenze", "{{ value_json.pack.cvl }}", "V", "voltage" },
	{ "ccl", "Ladestromgrenze", "{{ value_json.pack.ccl }}", "A", "current" },
	{ "dcl", "Entladestromgrenze", "{{ value_json.pack.dcl }}", "A", "current" },
	{ "flavor", "Protokoll", "{{ value_json.proto.flavor }}", NULL, NULL },
	{ "manufacturer", "Hersteller", "{{ value_json.proto.mfr }}", NULL, NULL },
};

static void publish_ha_discovery(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(ha_entities); i++) {
		const struct ha_entity *e = &ha_entities[i];
		char topic[96];

		snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_%s/config",
			 CONFIG_BATMON_DEVICE_ID, e->key);
		int n = snprintf(payload, sizeof(payload),
				 "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"stat_t\":\"%s\","
				 "\"val_tpl\":\"%s\",\"avty_t\":\"%s\","
				 "\"dev\":{\"ids\":[\"%s\"],\"name\":\"batmon %s\","
				 "\"mf\":\"PROTRONIC\",\"mdl\":\"batmon STM32H573I-DK\"}",
				 e->name, CONFIG_BATMON_DEVICE_ID, e->key, TOPIC_STATE, e->tpl,
				 TOPIC_STATUS, CONFIG_BATMON_DEVICE_ID, CONFIG_BATMON_DEVICE_ID);
		if (e->unit) {
			n += snprintf(payload + n, sizeof(payload) - n, ",\"unit_of_meas\":\"%s\"",
				      e->unit);
		}
		if (e->dev_class) {
			n += snprintf(payload + n, sizeof(payload) - n, ",\"dev_cla\":\"%s\"",
				      e->dev_class);
		}
		n += snprintf(payload + n, sizeof(payload) - n, "}");
		publish(topic, payload, n, true);
		mqtt_input(&client);
	}
}
#endif

static int connect_broker(void)
{
	int rc = resolve_broker();

	if (rc) {
		return rc;
	}
	mqtt_client_init(&client);
	client.broker = &broker;
	client.evt_cb = mqtt_evt;
	client.client_id.utf8 = (uint8_t *)CONFIG_BATMON_DEVICE_ID;
	client.client_id.size = strlen(CONFIG_BATMON_DEVICE_ID);
	client.protocol_version = MQTT_VERSION_3_1_1;
	client.rx_buf = rx_buf;
	client.rx_buf_size = sizeof(rx_buf);
	client.tx_buf = tx_buf;
	client.tx_buf_size = sizeof(tx_buf);
	client.transport.type = MQTT_TRANSPORT_NON_SECURE;
	client.clean_session = 1;

	static struct mqtt_utf8 user, pass;

	if (strlen(CONFIG_BATMON_MQTT_USERNAME) > 0) {
		user.utf8 = (uint8_t *)CONFIG_BATMON_MQTT_USERNAME;
		user.size = strlen(CONFIG_BATMON_MQTT_USERNAME);
		client.user_name = &user;
	}
	if (strlen(CONFIG_BATMON_MQTT_PASSWORD) > 0) {
		pass.utf8 = (uint8_t *)CONFIG_BATMON_MQTT_PASSWORD;
		pass.size = strlen(CONFIG_BATMON_MQTT_PASSWORD);
		client.password = &pass;
	}
#ifndef CONFIG_BATMON_MQTT_MODE_THINGSBOARD
	static struct mqtt_topic will_topic = {
		.topic = { .utf8 = (uint8_t *)TOPIC_STATUS, .size = sizeof(TOPIC_STATUS) - 1 },
		.qos = MQTT_QOS_0_AT_MOST_ONCE,
	};
	static struct mqtt_utf8 will_msg = { .utf8 = (uint8_t *)"offline", .size = 7 };

	client.will_topic = &will_topic;
	client.will_message = &will_msg;
	client.will_retain = 1;
#endif
	rc = mqtt_connect(&client);
	if (rc) {
		LOG_ERR("mqtt_connect: %d", rc);
		return rc;
	}
	/* wait for CONNACK */
	struct zsock_pollfd fd = { .fd = client.transport.tcp.sock, .events = ZSOCK_POLLIN };

	for (int i = 0; i < 10 && !connected; i++) {
		if (zsock_poll(&fd, 1, 500) > 0) {
			mqtt_input(&client);
		}
	}
	if (!connected) {
		mqtt_abort(&client);
		return -ETIMEDOUT;
	}
#ifndef CONFIG_BATMON_MQTT_MODE_THINGSBOARD
	publish(TOPIC_STATUS, "online", 6, true);
#if defined(CONFIG_BATMON_HA_DISCOVERY)
	publish_ha_discovery();
#endif
#else
	int n = snprintf(payload, sizeof(payload),
			 "{\"fw\":\"%s\",\"board\":\"%s\",\"device_id\":\"%s\"}", APP_VERSION_STRING,
			 CONFIG_BOARD, CONFIG_BATMON_DEVICE_ID);
	publish(TOPIC_ATTRS, payload, n, false);
#endif
	return 0;
}

static int publish_telemetry(void)
{
#ifdef CONFIG_BATMON_MQTT_MODE_THINGSBOARD
	int n = app_json(payload, sizeof(payload), true);
	const char *topic = TOPIC_TELEMETRY;
#else
	int n = app_json(payload, sizeof(payload), false);
	const char *topic = TOPIC_STATE;
#endif
	if (n < 0) {
		LOG_ERR("telemetry JSON too large");
		return -ENOMEM;
	}
	return publish(topic, payload, n, false);
}

int net_mqtt_publish_now(void)
{
	k_sem_give(&publish_req);
	return 0;
}

static void mqtt_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	uint32_t backoff = RECONNECT_MIN_MS;

	k_sem_take(&net_ready, K_FOREVER);

	while (1) {
		if (connect_broker() != 0) {
			LOG_WRN("broker %s:%d unreachable, retry in %u ms",
				CONFIG_BATMON_MQTT_BROKER_HOST, CONFIG_BATMON_MQTT_BROKER_PORT, backoff);
			k_sleep(K_MSEC(backoff));
			backoff = MIN(backoff * 2, RECONNECT_MAX_MS);
			continue;
		}
		backoff = RECONNECT_MIN_MS;
		int64_t next_pub = k_uptime_get();

		while (connected) {
			struct zsock_pollfd fd = { .fd = client.transport.tcp.sock,
						   .events = ZSOCK_POLLIN };
			int64_t wait = next_pub - k_uptime_get();

			if (wait < 0) {
				wait = 0;
			}
			int rc = zsock_poll(&fd, 1, (int)MIN(wait, 1000));

			if (rc > 0) {
				if (mqtt_input(&client) != 0) {
					break;
				}
			} else if (rc < 0) {
				break;
			}
			if (k_sem_take(&publish_req, K_NO_WAIT) == 0 || k_uptime_get() >= next_pub) {
				if (publish_telemetry() != 0) {
					break;
				}
				next_pub = k_uptime_get() + CONFIG_BATMON_MQTT_INTERVAL_SEC * 1000;
			}
			rc = mqtt_live(&client);
			if (rc != 0 && rc != -EAGAIN) {
				break;
			}
		}
		mqtt_abort(&client);
		connected = false;
		app.mqtt_connected = false;
		k_sleep(K_MSEC(backoff));
	}
}

int net_mqtt_init(void)
{
	struct net_if *iface = net_if_get_default();

	if (iface == NULL) {
		return -ENODEV;
	}
	net_mgmt_init_event_callback(&mgmt_cb, net_event,
				     NET_EVENT_IPV4_ADDR_ADD | NET_EVENT_IF_DOWN);
	net_mgmt_add_event_callback(&mgmt_cb);
	net_dhcpv4_start(iface);

	k_thread_create(&mqtt_thread_data, batmon_mqtt_stack, K_THREAD_STACK_SIZEOF(batmon_mqtt_stack), mqtt_thread,
			NULL, NULL, NULL, MQTT_THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&mqtt_thread_data, "mqtt");
	LOG_INF("DHCP started, MQTT broker %s:%d", CONFIG_BATMON_MQTT_BROKER_HOST,
		CONFIG_BATMON_MQTT_BROKER_PORT);
	return 0;
}
