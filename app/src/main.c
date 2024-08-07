/*
 * Copyright (c) 2023-2024 Jamie M.
 *
 * All right reserved. This code is not apache or FOSS/copyleft licensed.
 */

#include <stddef.h>
#include <errno.h>
#include <stdio.h>
#include <app_version.h>
#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/addr.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/shell/shell.h>
#include <zephyr/pm/device.h>
#include <zephyr/dt-bindings/gpio/nordic-nrf-gpio.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(abe, CONFIG_APPLICATION_LOG_LEVEL);

#define SENSOR_THREAD_STACK_SIZE 2048
#define SENSOR_THREAD_PRIORITY 1

#define FAN_THREAD_STACK_SIZE 1024
#define FAN_THREAD_PRIORITY 1
#define PWM_MAX_PERIOD PWM_SEC(1U) / 64U

enum device_state_t {
	STATE_IDLE = 0,
	STATE_CONNECTING,
	STATE_CONNECTED,
	STATE_DISCOVERING,
	STATE_ACTIVE,
};

enum handle_status_t {
	FIND_ESS_SERVICE = 0,
#ifdef CONFIG_APP_ESS_TEMPERATURE
	FIND_TEMPERATURE,
	FIND_TEMPERATURE_CCC,
#endif
#ifdef CONFIG_APP_ESS_HUMIDITY
	FIND_HUMIDITY,
	FIND_HUMIDITY_CCC,
#endif
#ifdef CONFIG_APP_ESS_PRESSURE
	FIND_PRESSURE,
	FIND_PRESSURE_CCC,
#endif
#ifdef CONFIG_APP_ESS_DEW_POINT
	FIND_DEW_POINT,
	FIND_DEW_POINT_CCC,
#endif
#ifdef CONFIG_APP_BATTERY_LEVEL
	FIND_BATTERY_SERVICE,
	FIND_BATTERY_LEVEL,
	FIND_BATTERY_LEVEL_CCC,
#endif
#ifdef CONFIG_APP_ESS_TEMPERATURE
	SUBSCRIBE_TEMPERATURE,
#endif
#ifdef CONFIG_APP_ESS_HUMIDITY
	SUBSCRIBE_HUMDIITY,
#endif
#ifdef CONFIG_APP_ESS_PRESSURE
	SUBSCRIBE_PRESSURE,
#endif
#ifdef CONFIG_APP_ESS_DEW_POINT
	SUBSCRIBE_DEW_POINT,
#endif
#ifdef CONFIG_APP_BATTERY_LEVEL
	SUBSCRIBE_BATTERY_LEVEL,
#endif
	AWAITING_READINGS,
};

enum readings_received_t {
	RECEIVED_NONE = 0,
#ifdef CONFIG_APP_ESS_TEMPERATURE
	RECEIVED_TEMPERATURE = BIT(0),
#endif
#ifdef CONFIG_APP_ESS_HUMIDITY
	RECEIVED_HUMIDITY = BIT(1),
#endif
#ifdef CONFIG_APP_ESS_PRESSURE
	RECEIVED_PRESSURE = BIT(2),
#endif
#ifdef CONFIG_APP_ESS_DEW_POINT
	RECEIVED_DEW_POINT = BIT(3),
#endif
#ifdef CONFIG_APP_BATTERY_LEVEL
	RECEIVED_BATTERY_LEVEL = BIT(4),
#endif
	RECEIVED_ALL = (
#ifdef CONFIG_APP_ESS_TEMPERATURE
			RECEIVED_TEMPERATURE +
#endif
#ifdef CONFIG_APP_ESS_HUMIDITY
			RECEIVED_HUMIDITY +
#endif
#ifdef CONFIG_APP_ESS_PRESSURE
			RECEIVED_PRESSURE +
#endif
#ifdef CONFIG_APP_ESS_DEW_POINT
			RECEIVED_DEW_POINT +
#endif
#ifdef CONFIG_APP_BATTERY_LEVEL
			RECEIVED_BATTERY_LEVEL +
#endif
			0),
};

struct device_handles {
	enum handle_status_t status;
	uint16_t service;
	uint16_t battery_service;
#ifdef CONFIG_APP_ESS_TEMPERATURE
	struct bt_gatt_subscribe_params temperature;
#endif
#ifdef CONFIG_APP_ESS_HUMIDITY
	struct bt_gatt_subscribe_params humidity;
#endif
#ifdef CONFIG_APP_ESS_PRESSURE
	struct bt_gatt_subscribe_params pressure;
#endif
#ifdef CONFIG_APP_ESS_DEW_POINT
	struct bt_gatt_subscribe_params dew_point;
#endif
#ifdef CONFIG_APP_BATTERY_LEVEL
	struct bt_gatt_subscribe_params battery_level;
#endif
};

struct device_readings {
#ifdef CONFIG_APP_ESS_TEMPERATURE
	double temperature;
#endif
#ifdef CONFIG_APP_ESS_HUMIDITY
	double humidity;
#endif
#ifdef CONFIG_APP_ESS_PRESSURE
	double pressure;
#endif
#ifdef CONFIG_APP_ESS_DEW_POINT
	int8_t dew_point;
#endif
#ifdef CONFIG_APP_BATTERY_LEVEL
	uint8_t battery_level;
#endif
	enum readings_received_t received;
};

struct device_params {
	bt_addr_le_t address;
	enum device_state_t state;
	struct bt_conn *connection;
	struct device_handles handles;
	struct device_readings readings;
	const char *name;
};

static const uint8_t device_id_value_offset = 1;

static struct device_params devices[3] = {
	{
		.address = {
			.type = BT_ADDR_LE_RANDOM,
			.a.val = { 0x22, 0x07, 0x7b, 0x1c, 0xb2, 0xf7 },
		},
		.name = "Server Room",
	},
	{
		.address = {
			.type = BT_ADDR_LE_RANDOM,
			.a.val = { 0xc5, 0x2a, 0xc2, 0x37, 0x3e, 0xe2 },
		},
		.name = "Plant area",
	},
	{
		.address = {
			.type = BT_ADDR_LE_RANDOM,
			.a.val = { 0x05, 0x55, 0x92, 0xa8, 0x8a, 0xe3 },
		},
		.name = "Northwind area",
	},
};

#define DEVICE_COUNT ARRAY_SIZE(devices)
static uint8_t current_index = 0;
static bool disabled = false; /* If true, prevents connecting to sensors */
static bool busy = false; /* If true, application is busy connecting/subscribing to a device and will wait before connecting to next device */

static struct bt_uuid_16 uuid = BT_UUID_INIT_16(0);
static struct bt_gatt_discover_params discover_params;
static struct k_sem next_action_sem;
static struct k_sem fan_sem;

K_THREAD_STACK_DEFINE(sensor_thread_stack, SENSOR_THREAD_STACK_SIZE);
static k_tid_t sensor_thread_id;
static struct k_thread sensor_thread;
static struct k_work subscribe_workqueue;

K_THREAD_STACK_DEFINE(fan_thread_stack, FAN_THREAD_STACK_SIZE);
static k_tid_t fan_thread_id;
static struct k_thread fan_thread;

static const char tick_character[] = {0xe2, 0x9c, 0x93, 0x00};

static bool pwm_enabled = true;
static uint8_t fan_speed = 0;
static uint8_t current_fan_speed = 0;
static bool half_fan_speed = false;
static bool current_half_fan_speed = false;

static const struct device *const dht22 = DEVICE_DT_GET_ONE(aosong_dht);
static const struct pwm_dt_spec fan_pwm = PWM_DT_SPEC_GET(DT_NODELABEL(fan_pwm));
static const struct gpio_dt_spec reset = GPIO_DT_SPEC_GET(DT_NODELABEL(reset_pin), gpios);
static const struct gpio_dt_spec fan_pin = GPIO_DT_SPEC_GET(DT_NODELABEL(fan_pin), gpios);
static bool last_dht_reading_pass = false;
static uint8_t connection_failures = 0;

static uint8_t notify_func(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
			   const void *data, uint16_t length)
{
	uint8_t i = 0;

	if (!data) {
		LOG_ERR("[UNSUBSCRIBED]");
		params->value_handle = 0U;
		return BT_GATT_ITER_STOP;
	}

	while (i < DEVICE_COUNT) {
		if (devices[i].connection == conn) {
			break;
		}
		++i;
	}

	if (i == DEVICE_COUNT) {
		LOG_ERR("ERROR! INVALID CONNECTION!");
		return BT_GATT_ITER_STOP;
	}

	LOG_ERR("[NOTIFICATION] from %d data %p length %u", i, data, length);
//	LOG_HEXDUMP_ERR(data, length, "Value");

	if (0) {
#ifdef CONFIG_APP_ESS_TEMPERATURE
	} else if (params == &devices[i].handles.temperature) {
		uint16_t value;
		double fp_value;

		value = sys_get_le16(&((uint8_t *)data)[0]);
		fp_value = ((double)value) / 100.0;

		devices[i].readings.temperature = fp_value;
		devices[i].readings.received |= RECEIVED_TEMPERATURE;
LOG_ERR("temp = %fc", fp_value);
#endif
#ifdef CONFIG_APP_ESS_HUMIDITY
	} else if (params == &devices[i].handles.humidity) {
		uint16_t value;
		double fp_value;

		value = sys_get_le16(&((uint8_t *)data)[0]);
		fp_value = ((double)value) / 100.0;

		devices[i].readings.humidity = fp_value;
		devices[i].readings.received |= RECEIVED_HUMIDITY;

LOG_ERR("hum = %f%c", fp_value, '%');
#endif
#ifdef CONFIG_APP_ESS_PRESSURE
	} else if (params == &devices[i].handles.pressure) {
		uint32_t value;
		double fp_value;

		value = sys_get_le32(&((uint8_t *)data)[0]);
		fp_value = (double)value;

		devices[i].readings.pressure = fp_value;
		devices[i].readings.received |= RECEIVED_PRESSURE;

LOG_ERR("press = %fPa", fp_value);
#endif
#ifdef CONFIG_APP_ESS_DEW_POINT
	} else if (params == &devices[i].handles.dew_point) {
		devices[i].readings.dew_point = ((int8_t *)data)[0];
		devices[i].readings.received |= RECEIVED_DEW_POINT;

LOG_ERR("dew = %dc", ((int8_t *)data)[0]);
#endif
#ifdef CONFIG_APP_BATTERY_LEVEL
	} else if (params == &devices[i].handles.battery_level) {
		devices[i].readings.battery_level = ((int8_t *)data)[0];
		devices[i].readings.received |= RECEIVED_BATTERY_LEVEL;

LOG_ERR("battery = %u%c", ((uint8_t *)data)[0], '%');
#endif
	} else {
LOG_ERR("not valid");
	}

	return BT_GATT_ITER_CONTINUE;
}

static void subscribe_func(struct bt_conn *conn, uint8_t err,
			   struct bt_gatt_subscribe_params *params)
{
	if (err) {
		int err;

		LOG_ERR("Gonna matey");
		err = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	} else {
		k_work_submit(&subscribe_workqueue);
	}
}

static void next_action(struct bt_conn *conn, const struct bt_gatt_attr *attr)
{
	int err;
	uint8_t action = 0;
	uint8_t service = 0;
	struct bt_gatt_subscribe_params *param = NULL;

	/* Increment to next state */
	++devices[current_index].handles.status;

	if (devices[current_index].handles.status == AWAITING_READINGS) {
		/* Finished the setup state machine */
		LOG_ERR("All finished!");
		busy = false;
		devices[current_index].state = STATE_ACTIVE;
		devices[current_index].handles.status = AWAITING_READINGS;
		k_sem_give(&next_action_sem);
		return;
	}

	switch (devices[current_index].handles.status) {
#ifdef CONFIG_APP_ESS_TEMPERATURE
		case FIND_TEMPERATURE:
		{
			memcpy(&uuid, BT_UUID_TEMPERATURE, sizeof(uuid));
			break;
		}
#endif
#ifdef CONFIG_APP_ESS_HUMIDITY
		case FIND_HUMIDITY:
		{
			memcpy(&uuid, BT_UUID_HUMIDITY, sizeof(uuid));
			break;
		}
#endif
#ifdef CONFIG_APP_ESS_PRESSURE
		case FIND_PRESSURE:
		{
			memcpy(&uuid, BT_UUID_PRESSURE, sizeof(uuid));
			break;
		}
#endif
#ifdef CONFIG_APP_ESS_DEW_POINT
		case FIND_DEW_POINT:
		{
			memcpy(&uuid, BT_UUID_DEW_POINT, sizeof(uuid));
			break;
		}
#endif
#ifdef CONFIG_APP_BATTERY_LEVEL
		case FIND_BATTERY_SERVICE:
		{
			memcpy(&uuid, BT_UUID_BAS, sizeof(uuid));
			action = 1;
			break;
		}
		case FIND_BATTERY_LEVEL:
		{
			memcpy(&uuid, BT_UUID_BAS_BATTERY_LEVEL, sizeof(uuid));
			service = 1;
			break;
		}
#endif
#ifdef CONFIG_APP_ESS_TEMPERATURE
		case FIND_TEMPERATURE_CCC:
#endif
#ifdef CONFIG_APP_ESS_HUMIDITY
		case FIND_HUMIDITY_CCC:
#endif
#ifdef CONFIG_APP_ESS_PRESSURE
		case FIND_PRESSURE_CCC:
#endif
#ifdef CONFIG_APP_ESS_DEW_POINT
		case FIND_DEW_POINT_CCC:
#endif
#ifdef CONFIG_APP_BATTERY_LEVEL
		case FIND_BATTERY_LEVEL_CCC:
#endif
		{
			memcpy(&uuid, BT_UUID_GATT_CCC, sizeof(uuid));
			action = 2;
			break;
		}
#ifdef CONFIG_APP_ESS_TEMPERATURE
		case SUBSCRIBE_TEMPERATURE:
		{
			param = &devices[current_index].handles.temperature;
			action = 3;
			break;
		}
#endif
#ifdef CONFIG_APP_ESS_HUMIDITY
		case SUBSCRIBE_HUMDIITY:
		{
			param = &devices[current_index].handles.humidity;
			action = 3;
			break;
		}
#endif
#ifdef CONFIG_APP_ESS_PRESSURE
		case SUBSCRIBE_PRESSURE:
		{
			param = &devices[current_index].handles.pressure;
			action = 3;
			break;
		}
#endif
#ifdef CONFIG_APP_ESS_DEW_POINT
		case SUBSCRIBE_DEW_POINT:
		{
			param = &devices[current_index].handles.dew_point;
			action = 3;
			break;
		}
#endif
#ifdef CONFIG_APP_BATTERY_LEVEL
//todo
		case SUBSCRIBE_BATTERY_LEVEL:
		{
			param = &devices[current_index].handles.battery_level;
			action = 3;
			break;
		}
#endif
		default:
		{
			LOG_ERR("Invalid state execution attempted: %d, maximum is %d (AWAITING_READINGS)", devices[current_index].handles.status, AWAITING_READINGS);
			return;
		}

	};

LOG_ERR("action is %d, state is %d", action, devices[current_index].handles.status);

	if (action == 0) {
		/* Find characteristic of service */
		if (service == 0) {
			discover_params.start_handle = devices[current_index].handles.service + 1;
		} else {
			discover_params.start_handle = devices[current_index].handles.battery_service + 1;
		}
		discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;
	} else if (action == 1) {
		/* Find service */
		discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
		discover_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
		discover_params.type = BT_GATT_DISCOVER_PRIMARY;
	} else if (action == 2) {
		/* Find descriptor of discovered characteristic */
		discover_params.start_handle = attr->handle + 2;
		discover_params.type = BT_GATT_DISCOVER_DESCRIPTOR;
	} else if (action == 3) {
		/* Subscribe for notifications */
		param->subscribe = subscribe_func;
		param->notify = notify_func;
		param->value = BT_GATT_CCC_NOTIFY;

		err = bt_gatt_subscribe(conn, param);

		if (err && err != -EALREADY) {
			LOG_ERR("Subscribe failed (err %d)", err);
		} else {
			LOG_ERR("[SUBSCRIBED]");
		}
	}

	if (action == 0 || action == 1 || action == 2) {
		discover_params.uuid = &uuid.uuid;
		err = bt_gatt_discover(conn, &discover_params);

		if (err) {
			LOG_ERR("Discover failed (err %d)", err);
		}
	}
}

static void subscribe_work(struct k_work *work)
{
	next_action(devices[current_index].connection, NULL);
}

static uint8_t discover_func(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			     struct bt_gatt_discover_params *params)
{
	int err = 0;

	if (!attr) {
		LOG_ERR("Discover complete");
		(void)memset(params, 0, sizeof(*params));
		return BT_GATT_ITER_STOP;
	}

	LOG_ERR("[ATTRIBUTE] handle %u", attr->handle);

	if (!bt_uuid_cmp(discover_params.uuid, BT_UUID_ESS)) {
		devices[current_index].state = STATE_DISCOVERING;
		devices[current_index].handles.service = attr->handle;
#ifdef CONFIG_APP_ESS_TEMPERATURE
	} else if (!bt_uuid_cmp(discover_params.uuid, BT_UUID_TEMPERATURE)) {
		devices[current_index].handles.temperature.value_handle =
								bt_gatt_attr_value_handle(attr);
	} else if (!bt_uuid_cmp(discover_params.uuid, BT_UUID_GATT_CCC) &&
		   devices[current_index].handles.status == FIND_TEMPERATURE_CCC) {
		devices[current_index].handles.temperature.ccc_handle = attr->handle;
#endif
#ifdef CONFIG_APP_ESS_HUMIDITY
	} else if (!bt_uuid_cmp(discover_params.uuid, BT_UUID_HUMIDITY)) {
		devices[current_index].handles.humidity.value_handle =
								bt_gatt_attr_value_handle(attr);
	} else if (!bt_uuid_cmp(discover_params.uuid, BT_UUID_GATT_CCC) &&
		   devices[current_index].handles.status == FIND_HUMIDITY_CCC) {
		devices[current_index].handles.humidity.ccc_handle = attr->handle;
#endif
#ifdef CONFIG_APP_ESS_PRESSURE
	} else if (!bt_uuid_cmp(discover_params.uuid, BT_UUID_PRESSURE)) {
		devices[current_index].handles.pressure.value_handle =
								bt_gatt_attr_value_handle(attr);
	} else if (!bt_uuid_cmp(discover_params.uuid, BT_UUID_GATT_CCC) &&
		   devices[current_index].handles.status == FIND_PRESSURE_CCC) {
		devices[current_index].handles.pressure.ccc_handle = attr->handle;
#endif
#ifdef CONFIG_APP_ESS_DEW_POINT
	} else if (!bt_uuid_cmp(discover_params.uuid, BT_UUID_DEW_POINT)) {
		devices[current_index].handles.dew_point.value_handle =
								bt_gatt_attr_value_handle(attr);
	} else if (!bt_uuid_cmp(discover_params.uuid, BT_UUID_GATT_CCC) &&
		   devices[current_index].handles.status == FIND_DEW_POINT_CCC) {
		devices[current_index].handles.dew_point.ccc_handle = attr->handle;
#endif
#ifdef CONFIG_APP_BATTERY_LEVEL
	} else if (!bt_uuid_cmp(discover_params.uuid, BT_UUID_BAS)) {
		devices[current_index].handles.battery_service = attr->handle;
	} else if (!bt_uuid_cmp(discover_params.uuid, BT_UUID_BAS_BATTERY_LEVEL)) {
		devices[current_index].handles.battery_level.value_handle =
								bt_gatt_attr_value_handle(attr);
	} else if (!bt_uuid_cmp(discover_params.uuid, BT_UUID_GATT_CCC) &&
		   devices[current_index].handles.status == FIND_BATTERY_LEVEL_CCC) {
		devices[current_index].handles.battery_level.ccc_handle = attr->handle;
#endif
	}

	if (err) {
		err = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	} else {
		next_action(conn, attr);
	}

	return BT_GATT_ITER_STOP;
}

static void connected(struct bt_conn *conn, uint8_t conn_err)
{
	char addr[BT_ADDR_LE_STR_LEN];
	int err;

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (conn_err) {
		LOG_ERR("Failed to connect to %s (%u)", addr, conn_err);

		/* Mark as not busy and advance state machine */
		bt_conn_unref(conn);
		devices[current_index].state = STATE_IDLE;
		busy = false;

		if (connection_failures < 30) {
			++connection_failures;
		}

		++current_index;

		if (current_index >= DEVICE_COUNT) {
			current_index = 0;
		}

		k_sem_give(&next_action_sem);

		return;
	}

	connection_failures = 0;

	devices[current_index].state = STATE_CONNECTED;
	memset(&devices[current_index].handles, 0, sizeof(struct device_handles));

	LOG_ERR("Connected: %s", addr);

	memcpy(&uuid, BT_UUID_ESS, sizeof(uuid));
	devices[current_index].handles.status = FIND_ESS_SERVICE;

	discover_params.uuid = &uuid.uuid;
	discover_params.func = discover_func;
	discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	discover_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	discover_params.type = BT_GATT_DISCOVER_PRIMARY;

	err = bt_gatt_discover(conn, &discover_params);

	if (err) {
		LOG_ERR("Discover failed(err %d)", err);
		err = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	uint8_t i = 0;
	char addr[BT_ADDR_LE_STR_LEN];
	bool increment = false;

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_ERR("Disconnected: %s (reason 0x%02x)", addr, reason);

	/* Check if this was the device currently being serviced */
	if (devices[current_index].connection == conn) {
		i = current_index;
		increment = true;

		if (devices[i].state != STATE_ACTIVE && busy) {
			/* We are no longer busy, allow state machine to connect to next device */
			busy = false;
		}
	}

	/* Search for the instance */
	while (i < DEVICE_COUNT) {
		if (devices[i].connection == conn) {
			if (devices[i].state == STATE_ACTIVE) {
				/* Reset connection failure count to allow fast reconnection to device */
				connection_failures = 0;
			}

			devices[i].state = STATE_IDLE;
			devices[i].connection = NULL;
			devices[i].handles.status = 0;
			memset(&devices[i].readings, 0, sizeof(struct device_readings));
			break;
		}

		++i;
	}

	if (increment) {
		++current_index;

		if (current_index >= DEVICE_COUNT) {
			current_index = 0;
		}
	}

	bt_conn_unref(conn);
	k_sem_give(&next_action_sem);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

static void sensor_function(void *, void *, void *)
{
	int err;
	struct bt_le_conn_param *param = BT_LE_CONN_PARAM_DEFAULT;

	while (1) {
		k_sem_take(&next_action_sem, K_FOREVER);

		if (disabled || busy) {
			continue;
		}

		/* Check if there are any devices with states that require attention */
		uint8_t i = 0;
		while (i < DEVICE_COUNT) {
			if (devices[i].state == STATE_IDLE) {
				break;
			}

			++i;
		}

		if (i == DEVICE_COUNT) {
			continue;
		}

		while (devices[current_index].state != STATE_IDLE) {
			++current_index;

			if (current_index >= DEVICE_COUNT) {
				current_index = 0;
			}
		}

		busy = true;
		devices[current_index].state = STATE_CONNECTING;

		/* If we have problems connecting then delay new connection attempts as the device is likely offline */
		if (connection_failures >= 28) {
			k_sleep(K_SECONDS(10));
		} else if (connection_failures > 16) {
			k_sleep(K_SECONDS(4));
		} else if (connection_failures > 8) {
			k_sleep(K_MSEC(800));
		}

		err = bt_conn_le_create(&devices[current_index].address, BT_CONN_LE_CREATE_CONN,
					param, &devices[current_index].connection);

		if (err) {
			LOG_ERR("Got error: %d", err);
			busy = false;
		}
	}
}

static void fan_function(void *, void *, void *)
{
	int err;

	while (1) {
		k_sem_take(&fan_sem, K_FOREVER);

		if (!pwm_is_ready_dt(&fan_pwm)) {
			LOG_ERR("Fan PWM is not ready");
			continue;
		}

		if (fan_speed == current_fan_speed && half_fan_speed == current_half_fan_speed) {
			/* We're already done */
			continue;
		} else if (fan_speed == 0) {
			/* Disable, easy. Do this by disabling PWM mode and using GPIO mode */
			if (pwm_enabled) {
				err = pwm_set_dt(&fan_pwm, PWM_MAX_PERIOD, 0);
				err = pm_device_action_run(fan_pwm.dev, PM_DEVICE_ACTION_SUSPEND);

				if (!err) {
					pwm_enabled = false;
				} else {
					LOG_ERR("PWM disable failed: %d", err);
				}
			}

			err = gpio_pin_configure_dt(&fan_pin, GPIO_OUTPUT_INACTIVE | NRF_GPIO_DRIVE_H0H1);

			if (err) {
				LOG_ERR("GPIO configure failed: %d", err);
			} else {
				current_fan_speed = fan_speed;
			}
		} else {
			int8_t change_increment;
			uint8_t change_amount;

			if (fan_speed > current_fan_speed) {
				/* Increase speed gradually until we hit the target */
				change_increment = 1;
				change_amount = (fan_speed - current_fan_speed);
			} else {
				/* Decrease speed gradually until we hit the target */
				change_increment = -1;
				change_amount = (current_fan_speed - fan_speed);
			}

			if (!pwm_enabled) {
				err = gpio_pin_configure_dt(&fan_pin, GPIO_OUTPUT_INACTIVE);
				err = pm_device_action_run(fan_pwm.dev, PM_DEVICE_ACTION_RESUME);

				if (!err) {
					pwm_enabled = true;
				} else {
					LOG_ERR("PWM enable failed: %d", err);
				}
			}

			while (change_amount > 0) {
				current_fan_speed = (uint8_t)((int8_t)current_fan_speed + change_increment);
				err = pwm_set_dt(&fan_pwm, PWM_MAX_PERIOD, (PWM_MAX_PERIOD * current_fan_speed / (half_fan_speed == true ? 200U : 100U)));

				if (err) {
					LOG_ERR("PWM set failed: %d (speed: %d)", err, current_fan_speed);
				}

				--change_amount;
				k_sleep(K_MSEC(50));
			}

			if (current_fan_speed == 100) {
				/* Disable PWM mode and use GPIO mode */
				if (pwm_enabled) {
					err = pwm_set_dt(&fan_pwm, PWM_MAX_PERIOD, 0);
					err = pm_device_action_run(fan_pwm.dev, PM_DEVICE_ACTION_SUSPEND);

					if (!err) {
						pwm_enabled = false;
					} else {
						LOG_ERR("PWM disable failed: %d", err);
					}
				}

				err = gpio_pin_configure_dt(&fan_pin, GPIO_OUTPUT_ACTIVE | NRF_GPIO_DRIVE_H0H1);

				if (err) {
					LOG_ERR("GPIO configure failed: %d", err);
				}
			}

			current_half_fan_speed = half_fan_speed;
		}
	}
}

int main(void)
{
	int err;

	if (!pwm_is_ready_dt(&fan_pwm)) {
		LOG_ERR("PWM init failed");
	} else {
		/* Disable PWM and use as GPIO */
		err = pwm_set_dt(&fan_pwm, PWM_MAX_PERIOD, 0);

		err = pm_device_action_run(fan_pwm.dev, PM_DEVICE_ACTION_SUSPEND);

		if (!err) {
			pwm_enabled = false;
		}

		err = gpio_pin_configure_dt(&fan_pin, GPIO_OUTPUT_INACTIVE);

	}

	err = bt_enable(NULL);

	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
		return 0;
	}

	LOG_ERR("Bluetooth initialized");

	k_sem_init(&next_action_sem, 1, 1);
	k_sem_init(&fan_sem, 0, 1);
	k_work_init(&subscribe_workqueue, subscribe_work);

/* */
	current_index = 0;

	while (current_index < DEVICE_COUNT) {
		devices[current_index].state = STATE_IDLE;
		devices[current_index].handles.status = 0;
		memset(&devices[current_index].handles, 0, sizeof(struct device_handles));
		++current_index;
	}

	current_index = 0;

#ifndef CONFIG_APP_START_BOOTUP
	disabled = true;
#endif

	/* Setup threads */
	sensor_thread_id = k_thread_create(&sensor_thread, sensor_thread_stack,
					   K_THREAD_STACK_SIZEOF(sensor_thread_stack),
					   sensor_function, NULL, NULL, NULL,
					   SENSOR_THREAD_PRIORITY, 0, K_NO_WAIT);

	fan_thread_id = k_thread_create(&fan_thread, fan_thread_stack,
					K_THREAD_STACK_SIZEOF(fan_thread_stack),
					fan_function, NULL, NULL, NULL,
					FAN_THREAD_PRIORITY, 0, K_NO_WAIT);

	if (!device_is_ready(dht22)) {
		LOG_ERR("Sensor init failed");
	} else {
		/* Read 5 sets of readings due to sensor being of incredibly shit quality and
		 * giving many bogus readings
		 */
		uint8_t i = 0;

		while (i < 3) {
			/* Don't bother checking return code, not like the sensor manufacturer
			 * cares to check if their product is of any actual use...
			 */
			err = sensor_sample_fetch(dht22);
			k_sleep(K_MSEC(1200));

			++i;
		}

		last_dht_reading_pass = (err ? false : true);
	}

	return 0;
}

#if defined(CONFIG_APP_OUTPUT_FORMAT_CUSTOM)
/* Outputs ESS readings in the following format:
 * Start delimiter: ##
 * { for each device with data:
 *     Index number: e.g. 0
 *     Temperature reading: e.g. 25.12
 *     Pressure reading: e.g. 1000270
 *     Humidity reading: e.g. 52.04
 *     Dew point reading: e.g. 8
 * }
 * End delimiter: ^^
 *
 * Each value has a comma (,) separator.
 */
static int ess_readings_handler(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t i = 0;
	uint8_t buffer[128] = {0};

	while (i < DEVICE_COUNT) {
		if (devices[i].state == STATE_ACTIVE &&
		    devices[i].readings.received == RECEIVED_ALL) {
			sprintf(&buffer[strlen(buffer)], "%d,"
#ifdef CONFIG_APP_ESS_TEMPERATURE
				"%.2f,"
#endif
#ifdef CONFIG_APP_ESS_HUMIDITY
				"%.0f,"
#endif
#ifdef CONFIG_APP_ESS_PRESSURE
				"%.2f,"
#endif
#ifdef CONFIG_APP_ESS_DEW_POINT
				"%d,"
#endif
#ifdef CONFIG_APP_ESS_BATTERY_LEVEL
				"%d,"
#endif
				, i
#ifdef CONFIG_APP_ESS_TEMPERATURE
				, devices[i].readings.temperature
#endif
#ifdef CONFIG_APP_ESS_HUMIDITY
				, devices[i].readings.humidity
#endif
#ifdef CONFIG_APP_ESS_PRESSURE
				, devices[i].readings.pressure
#endif
#ifdef CONFIG_APP_ESS_DEW_POINT
				, devices[i].readings.dew_point
#endif
#ifdef CONFIG_APP_ESS_BATTERY_LEVEL
				, devices[i].readings.battery_level
#endif
				);
			devices[i].readings.received = RECEIVED_NONE;
		}

		++i;
	}

/* Should do sht22 here */

	if (strlen(buffer) > 0) {
		buffer[(strlen(buffer) - 1)] = 0;
	}

	shell_print(sh, "##%s^^", buffer);

	return 0;
}
#elif defined(CONFIG_APP_OUTPUT_FORMAT_CSV)
/* Outputs ESS readings in CSV format, with headings */
static int ess_readings_handler(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t i = 0;
	uint8_t buffer[384] = {0};
	int err;
	struct sensor_value humidity;
	struct sensor_value temperature;

	sprintf(&buffer[0], "device,"
#if defined(CONFIG_APP_OUTPUT_DEVICE_ADDRESS)
		"address,"
#endif
#if defined(CONFIG_APP_OUTPUT_DEVICE_NAME)
		"name,"
#endif
#ifdef CONFIG_APP_ESS_TEMPERATURE
		"temperature,"
#endif
#ifdef CONFIG_APP_ESS_HUMIDITY
		"humidity,"
#endif
#ifdef CONFIG_APP_ESS_PRESSURE
		"pressure,"
#endif
#ifdef CONFIG_APP_ESS_DEW_POINT
		"dewpoint,"
#endif
#ifdef CONFIG_APP_BATTERY_LEVEL
		"battery,"
#endif
		"\n");

	/* Read rubbish connected sensor */
	err = sensor_sample_fetch(dht22);

	while (i < DEVICE_COUNT) {
		if (devices[i].state == STATE_ACTIVE &&
		    devices[i].readings.received == RECEIVED_ALL) {
			sprintf(&buffer[strlen(buffer)], "%d,"
#if defined(CONFIG_APP_OUTPUT_DEVICE_ADDRESS)
				"%02x%02x%02x%02x%02x%02x%02x,"
#endif
#if defined(CONFIG_APP_OUTPUT_DEVICE_NAME)
				"%s,"
#endif
#ifdef CONFIG_APP_ESS_TEMPERATURE
				"%.2f,"
#endif
#ifdef CONFIG_APP_ESS_HUMIDITY
				"%.2f,"
#endif
#ifdef CONFIG_APP_ESS_PRESSURE
				"%.0f,"
#endif
#ifdef CONFIG_APP_ESS_DEW_POINT
				"%d,"
#endif
#ifdef CONFIG_APP_BATTERY_LEVEL
				"%d,"
#endif
				"\n", (device_id_value_offset + i)
#if defined(CONFIG_APP_OUTPUT_DEVICE_ADDRESS)
				, devices[i].address.type, devices[i].address.a.val[5],
				devices[i].address.a.val[4], devices[i].address.a.val[3],
				devices[i].address.a.val[2], devices[i].address.a.val[1],
				devices[i].address.a.val[0]
#endif
#if defined(CONFIG_APP_OUTPUT_DEVICE_NAME)
				, devices[i].name
#endif
#ifdef CONFIG_APP_ESS_TEMPERATURE
				, devices[i].readings.temperature
#endif
#ifdef CONFIG_APP_ESS_HUMIDITY
				, devices[i].readings.humidity
#endif
#ifdef CONFIG_APP_ESS_PRESSURE
				, devices[i].readings.pressure
#endif
#ifdef CONFIG_APP_ESS_DEW_POINT
				, devices[i].readings.dew_point
#endif
#ifdef CONFIG_APP_BATTERY_LEVEL
				, devices[i].readings.battery_level
#endif
				);
			devices[i].readings.received = RECEIVED_NONE;
		}

		++i;
	}

	if (err) {
		/* Wait a short period of time and try again */
		k_sleep(K_MSEC(300));
		err = sensor_sample_fetch(dht22);
	}

	last_dht_reading_pass = (err ? false : true);

	if (!err) {
		(void)sensor_channel_get(dht22, SENSOR_CHAN_AMBIENT_TEMP, &temperature);
		(void)sensor_channel_get(dht22, SENSOR_CHAN_HUMIDITY, &humidity);

		sprintf(&buffer[strlen(buffer)], "%d,"
#if defined(CONFIG_APP_OUTPUT_DEVICE_ADDRESS)
			"LOCAL,"
#endif
#if defined(CONFIG_APP_OUTPUT_DEVICE_NAME)
			"Loft,"
#endif
#ifdef CONFIG_APP_ESS_TEMPERATURE
			"%.2f,"
#endif
#ifdef CONFIG_APP_ESS_HUMIDITY
			"%.2f,"
#endif
#ifdef CONFIG_APP_ESS_PRESSURE
			"0,"
#endif
#ifdef CONFIG_APP_ESS_DEW_POINT
			"0,"
#endif
#ifdef CONFIG_APP_BATTERY_LEVEL
			"0,"
#endif
			"\n", (device_id_value_offset + i)
#ifdef CONFIG_APP_ESS_TEMPERATURE
			, sensor_value_to_double(&temperature)
#endif
#ifdef CONFIG_APP_ESS_HUMIDITY
			, sensor_value_to_double(&humidity)
#endif
			);
	}

	shell_print(sh, "%s\n", buffer);

	return 0;
}
#else
#error "Invalid output format selected"
#endif

static int ess_disconnect_handler(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t i = 0;

	while (i < DEVICE_COUNT) {
		if (devices[i].state != STATE_IDLE && devices[i].connection != NULL) {
			int32_t err = bt_conn_disconnect(devices[i].connection, BT_HCI_ERR_REMOTE_USER_TERM_CONN);

			if (err != 0) {
				shell_error(sh, "Error whilst disconnecting from #%d: %d", i, err);
			}
		}

		++i;
	}

	shell_print(sh, "Disconnected from all devices");

	return 0;
}

static int ess_disable_handler(const struct shell *sh, size_t argc, char **argv)
{
	if (!disabled) {
		uint8_t i = 0;
		disabled = true;

		while (i < DEVICE_COUNT) {
			if (devices[i].state != STATE_IDLE && devices[i].connection != NULL) {
				int32_t err = bt_conn_disconnect(devices[i].connection, BT_HCI_ERR_REMOTE_USER_TERM_CONN);

				if (err != 0) {
					shell_error(sh, "Error whilst disconnecting from #%d: %d", i, err);
				}
			}

			++i;
		}

		shell_print(sh, "Application state changed to disabled.");

		return 0;
	}

	shell_error(sh, "Application is already disabled.");

	return -EPERM;
}

static int ess_enable_handler(const struct shell *sh, size_t argc, char **argv)
{
	if (disabled) {
		disabled = false;
		shell_print(sh, "Application state changed to enabled.");
		k_sem_give(&next_action_sem);
		return 0;
	}

	shell_error(sh, "Application is already enabled.");

	return -EPERM;
}

static char *state_to_text(uint8_t state)
{
	if (state == STATE_IDLE) {
		return "Idle";
	} else if (state == STATE_CONNECTING) {
		return "Connecting";
	} else if (state == STATE_CONNECTED) {
		return "Connected";
	} else if (state == STATE_DISCOVERING) {
		return "Discovering";
	} else if (state == STATE_ACTIVE) {
		return "Active";
	}

	return "Unknown";
}

static int ess_status_handler(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t i = 0;
	uint8_t largest_name = 0;
	int8_t repeat_size;

	while (i < DEVICE_COUNT) {
		uint8_t string_size;

		string_size = (uint8_t)strlen(devices[i].name);

		if (string_size > largest_name) {
			largest_name = string_size;
		}

		++i;
	}


	repeat_size = (int8_t)largest_name - 4;
	if (repeat_size < 0) {
		repeat_size = 0;
	}

	shell_print(sh, "# | Address        | Name%.*s | State       | Readings", repeat_size, "                  ");
	shell_print(sh, "--|----------------|-----%.*s-|-------------|---------", repeat_size, "------------------");

	i = 0;

	while (i < DEVICE_COUNT) {
		char *state = state_to_text(devices[i].state);

		shell_print(sh, "%d | %02x%02x%02x%02x%02x%02x%02x | %s%.*s | %s%.*s | 0x%x %s",
			    (device_id_value_offset + i),
			    devices[i].address.type, devices[i].address.a.val[5],
			    devices[i].address.a.val[4], devices[i].address.a.val[3],
			    devices[i].address.a.val[2], devices[i].address.a.val[1],
			    devices[i].address.a.val[0], devices[i].name,
			    (largest_name - strlen(devices[i].name)), "                  ",
			    state, (11 - strlen(state)), "                  ",
			    devices[i].readings.received,
			    (devices[i].readings.received == RECEIVED_ALL ? tick_character : ""));
		++i;
	}

	if (device_is_ready(dht22) && last_dht_reading_pass == true) {
		shell_print(sh, "%d | LOCAL          | Loft%.*s | Active      | 0x%x %s", (device_id_value_offset + i),
			    (largest_name - 4), "                  ", (0
#ifdef CONFIG_APP_ESS_TEMPERATURE
			    + RECEIVED_TEMPERATURE
#endif
#ifdef CONFIG_APP_ESS_HUMIDITY
			    + RECEIVED_HUMIDITY
#endif
			    ), tick_character);
	} else {
		shell_print(sh, "%d | LOCAL          | Loft%.*s | Error       | 0x0", (device_id_value_offset + i),
			    (largest_name - 4), "                  ");
	}

	return 0;
}

static int fan_speed_handler(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 1) {
		if (half_fan_speed == true) {
			shell_print(sh, "Fan speed: %u (half)", fan_speed);
		} else {
			shell_print(sh, "Fan speed: %u", fan_speed);
		}
	} else if (strcmp(argv[1], "actual") == 0) {
		if (half_fan_speed == true) {
			shell_print(sh, "Actual fan speed: %u (half)", current_fan_speed);
		} else {
			shell_print(sh, "Actual fan speed: %u", current_fan_speed);
		}
	} else {
		uint32_t speed = strtoul(argv[1], NULL, 0);

		if (speed > 100) {
			shell_print(sh, "Invalid speed, must be between 0-100");
		} else {
			if (argc == 3) {
				if (strcmp(argv[2], "half") == 0) {
					half_fan_speed = true;
				} else {
					shell_error(sh, "Invalid option");
					return -EINVAL;
				}
			} else {
				half_fan_speed = false;
			}

			fan_speed = (uint8_t)speed;
			k_sem_give(&fan_sem);
			shell_print(sh, "Fan speed set");
		}
	}

	return 0;
}

static int app_reboot_handler(const struct shell *sh, size_t argc, char **argv)
{
        sys_reboot(SYS_REBOOT_COLD);

	return 0;
}

static int app_bootloader_handler(const struct shell *sh, size_t argc, char **argv)
{
	int err;

	if (!gpio_is_ready_dt(&reset)) {
		shell_print(sh, "GPIO is not ready");
	} else {
		err = gpio_pin_configure_dt(&reset, GPIO_OUTPUT_ACTIVE);

		if (err < 0) {
			shell_print(sh, "GPIO set failed");
		}
	}

	return 0;
}

static int app_version_handler(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "Version: %s", APP_VERSION_TWEAK_STRING);

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(ess_cmd,
	/* Command handlers */
	SHELL_CMD(readings, NULL, "Output ESS values", ess_readings_handler),
	SHELL_CMD(disconnect, NULL, "Disconnect from all devices", ess_disconnect_handler),
	SHELL_CMD(disable, NULL, "Disable fetching readings", ess_disable_handler),
	SHELL_CMD(enable, NULL, "Enable fetching readings", ess_enable_handler),
	SHELL_CMD(status, NULL, "Show device status", ess_status_handler),

	/* Array terminator. */
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(ess, &ess_cmd, "ESS profile commands", NULL);

SHELL_STATIC_SUBCMD_SET_CREATE(fan_cmd,
	/* Command handlers */
	SHELL_CMD(speed, NULL, "Change fan speed", fan_speed_handler),

	/* Array terminator. */
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(fan, &fan_cmd, "Fan commands", NULL);

SHELL_STATIC_SUBCMD_SET_CREATE(app_cmd,
	/* Command handlers */
	SHELL_CMD(reboot, NULL, "Reboot", app_reboot_handler),
	SHELL_CMD(bootloader, NULL, "Enter bootloader", app_bootloader_handler),
	SHELL_CMD(version, NULL, "Show version", app_version_handler),

	/* Array terminator. */
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(app, &app_cmd, "Application commands", NULL);
