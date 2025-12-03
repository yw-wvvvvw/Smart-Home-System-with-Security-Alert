/* Smart Home System Firmware
 *
 * Single-file firmware for:
 * - Home Light (LIGHTBULB) with "Power" param - GPIO 2
 * - Alarm System (SWITCH) with "Power" param - enables/disables alarm
 * - Door Sensor Status (read-only) - GPIO 3 (IR sensor)
 *   - "Door Status" param (OPENED/CLOSED)
 * - IR sensor task that triggers alarm/buzzer/LED
 *  - Buzzer on GPIO 4
 *
 * Make sure to re-provision / re-link after flashing so Google Home picks up the corrected device.
 */

#include <string.h>
#include <stdbool.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>

#include <esp_rmaker_core.h>
#include <esp_rmaker_standard_types.h>
#include <esp_rmaker_standard_devices.h>

/* --- ADDED FOR DASHBOARD EVENTS --- */
#include <esp_diagnostics.h> 

#include "app_network.h"
#include "app_insights.h"
#include "app_priv.h"

static const char *TAG = "app_main";

/* Hardware pins */
#define LED_GPIO         GPIO_NUM_2
#define IR_SENSOR_GPIO   GPIO_NUM_3
#define BUZZER_GPIO      GPIO_NUM_4

/* RTOS task config */
#define IR_TASK_STACK    2048
#define IR_TASK_PRIO     5

/* Global flags */
static volatile bool alarm_enabled = false;
static volatile bool led_state = false;  // store current LED state (last commanded light state)

/* RainMaker params (global handles for updates from tasks) */
static esp_rmaker_param_t *door_status_param = NULL;
static esp_rmaker_param_t *alarm_trigger_param = NULL;

/* ---------------- Hardware init ---------------- */
void app_driver_init(void)
{
    // LED (used as Home Light and also toggled during alarm)
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, 0); // OFF initially
    led_state = false;

    // IR sensor input
    gpio_reset_pin(IR_SENSOR_GPIO);
    gpio_set_direction(IR_SENSOR_GPIO, GPIO_MODE_INPUT);

    // Buzzer output
    gpio_reset_pin(BUZZER_GPIO);
    gpio_set_direction(BUZZER_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BUZZER_GPIO, 0); // OFF initially
}

/* ---------------- Driver helper ----------------
 * Handles GPIO changes triggered by RainMaker write requests.
 * Only handles the Light "Power" parameter here.
 */
esp_err_t app_driver_set_gpio(const char *param_name, bool value)
{
    if (strcmp(param_name, "Power") == 0) {
        gpio_set_level(LED_GPIO, value ? 1 : 0);
        led_state = value;
        
        ESP_DIAG_EVENT("LIGHT_ACTION", "Light Power -> %s", value ? "ON" : "OFF");
        return ESP_OK;
    }
    return ESP_FAIL;
}

/* ---------------- RainMaker write callback ----------------
 * This handles write requests coming from cloud / Google Home / app.
 * Check device name + parameter name to route actions.
 */
static esp_err_t write_cb(const esp_rmaker_device_t *device,
                          const esp_rmaker_param_t *param,
                          const esp_rmaker_param_val_t val,
                          void *priv_data,
                          esp_rmaker_write_ctx_t *ctx)
{
    const char *dev_name = esp_rmaker_device_get_name(device);
    const char *param_name = esp_rmaker_param_get_name(param);

    /* --- Home Light handling (toggle LED) --- */
    if (strcmp(dev_name, "Home Light") == 0 && strcmp(param_name, "Power") == 0) {
        bool new_val = val.val.b;
        if (app_driver_set_gpio(param_name, new_val) == ESP_OK) {
            esp_rmaker_param_update(param, val); // sync back to cloud
        } else {
            ESP_LOGW(TAG, "Failed to apply power for Home Light");
        }
        return ESP_OK;
    }

    /* --- Alarm System handling --- */
    if (strcmp(dev_name, "Alarm System") == 0 && strcmp(param_name, "Power") == 0) {
        alarm_enabled = val.val.b;
        
        ESP_DIAG_EVENT("ALARM_ACTION", "Alarm System set to: %s", alarm_enabled ? "ON" : "OFF");

        if (!alarm_enabled) {
            // Reset door and alarm status when alarm is turned off
            if (door_status_param) {
                esp_rmaker_param_update(door_status_param, esp_rmaker_str("CLOSED"));
            }
            if (alarm_trigger_param) {
                esp_rmaker_param_update(alarm_trigger_param, esp_rmaker_bool(false));
            }
            gpio_set_level(BUZZER_GPIO, 0);
            // restore LED to last commanded state
            gpio_set_level(LED_GPIO, led_state ? 1 : 0);
        }

        esp_rmaker_param_update(param, val); // sync state in cloud
        return ESP_OK;
    }

    return ESP_OK;
}

/* ---------------- IR sensor + buzzer task ----------------
 * Monitors IR_SENSOR_GPIO:
 * - Updates Door Status param (OPENED/CLOSED)
 * - If alarm enabled and door opens => update alarm trigger, blink LED & buzzer, send alert
 */
void ir_sensor_task(void *arg)
{
    int previous_sensor_state = -1;  // -1 = unknown
    bool notification_sent = false;

    while (1) {
        int sensor_value = gpio_get_level(IR_SENSOR_GPIO);  // 1=open, 0=closed

        /* -----------------------------
         * 1. DOOR STATE HANDLING
         * ----------------------------- */
        if (sensor_value != previous_sensor_state) {
            if (sensor_value == 1) {
                // Door OPENED
                ESP_DIAG_EVENT("DOOR_ACTION", "Door Sensor: OPENED");
                if (door_status_param) {
                    esp_rmaker_param_update(door_status_param, esp_rmaker_str("OPENED"));
                }
                notification_sent = false;  // allow new notification
            } else {
                // Door CLOSED
                ESP_DIAG_EVENT("DOOR_ACTION", "Door Sensor: CLOSED");
                if (door_status_param) {
                    esp_rmaker_param_update(door_status_param, esp_rmaker_str("CLOSED"));
                }
                if (alarm_trigger_param) {
                    esp_rmaker_param_update(alarm_trigger_param, esp_rmaker_bool(false));
                }
                notification_sent = false;
            }

            previous_sensor_state = sensor_value;
        }

        /* -----------------------------
         * 2. ALARM BEHAVIOR
         * ----------------------------- */
        if (alarm_enabled) {
            if (sensor_value == 1) {
                // Door OPEN => alarm triggered
                if (alarm_trigger_param) {
                    esp_rmaker_param_update(alarm_trigger_param, esp_rmaker_bool(true));
                }

                // Blink LED + buzzer
                gpio_set_level(BUZZER_GPIO, 1);
                gpio_set_level(LED_GPIO, !led_state);
                vTaskDelay(pdMS_TO_TICKS(150));
                gpio_set_level(LED_GPIO, led_state);
                vTaskDelay(pdMS_TO_TICKS(150));

                if (!notification_sent) {
                    esp_rmaker_raise_alert("Door opened while alarm is ON!");
                    ESP_DIAG_EVENT("SECURITY_ALERT", "Intrusion detected");
                    notification_sent = true;
                }
                continue;  // skip the bottom delay
            } else {
                // Door closed while alarm ON
                gpio_set_level(BUZZER_GPIO, 0);
                gpio_set_level(LED_GPIO, led_state);
            }
        } else {
            /* -----------------------------
             * 3. ALARM OFF => full reset
             * ----------------------------- */
            if (door_status_param) {
                esp_rmaker_param_update(door_status_param, esp_rmaker_str("CLOSED"));
            }
            if (alarm_trigger_param) {
                esp_rmaker_param_update(alarm_trigger_param, esp_rmaker_bool(false));
            }
            gpio_set_level(BUZZER_GPIO, 0);
            gpio_set_level(LED_GPIO, led_state);
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}


/* ---------------- Main ---------------- */
void app_main()
{
    // Hardware init 
    app_driver_init();

    // NVS init
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Network init (provisioning/connect)
    app_network_init();

    //RainMaker init
    esp_rmaker_config_t rainmaker_cfg = {
        .enable_time_sync = false,
    };

    esp_rmaker_node_t *node = esp_rmaker_node_init(&rainmaker_cfg, "SmartHomeNode", "Smart Home Node");
    if (!node) {
        ESP_LOGE(TAG, "RainMaker node init failed!");
        abort();
    }

    /* ---------------- Home Light device ----------------
     * Device type: LIGHTBULB
     * Parameter: "Power" with ESP_RMAKER_PARAM_POWER (standard)
     */
    esp_rmaker_device_t *light_dev = esp_rmaker_device_create("Home Light", ESP_RMAKER_DEVICE_LIGHTBULB, NULL);
    esp_rmaker_device_add_cb(light_dev, write_cb, NULL);

    esp_rmaker_param_t *light_param = esp_rmaker_param_create(
        "Power",
        ESP_RMAKER_PARAM_POWER,
        esp_rmaker_bool(false),
        PROP_FLAG_READ | PROP_FLAG_WRITE
    );
    esp_rmaker_param_add_ui_type(light_param, ESP_RMAKER_UI_TOGGLE);
    esp_rmaker_device_add_param(light_dev, light_param);
    esp_rmaker_node_add_device(node, light_dev);

    /* ---------------- Alarm System device ----------------
     * Device type: SWITCH (keeps semantics simple)
     * Parameter: "Power" with ESP_RMAKER_PARAM_POWER (standard)
     */
    esp_rmaker_device_t *alarm_dev = esp_rmaker_device_create("Alarm System", ESP_RMAKER_DEVICE_SWITCH, NULL);
    esp_rmaker_device_add_cb(alarm_dev, write_cb, NULL);

    esp_rmaker_param_t *alarm_param = esp_rmaker_param_create(
        "Power",
        ESP_RMAKER_PARAM_POWER,
        esp_rmaker_bool(false),
        PROP_FLAG_READ | PROP_FLAG_WRITE
    );
    esp_rmaker_param_add_ui_type(alarm_param, ESP_RMAKER_UI_TOGGLE);
    esp_rmaker_device_add_param(alarm_dev, alarm_param);
    esp_rmaker_node_add_device(node, alarm_dev);

    /* ---------------- Door Sensor Status device ----------------
     * Read-only params: Door Status (OPENED/CLOSED) and Alarm Triggered (bool)
     */
    esp_rmaker_device_t *door_dev = esp_rmaker_device_create("Door Sensor Status", ESP_RMAKER_DEVICE_OTHER, NULL);

    door_status_param = esp_rmaker_param_create("Door Status", NULL, esp_rmaker_str("CLOSED"),
                                                                PROP_FLAG_READ);
    alarm_trigger_param = esp_rmaker_param_create("Alarm Triggered", NULL, esp_rmaker_bool(false),
                                                                  PROP_FLAG_READ);

    esp_rmaker_device_add_param(door_dev, door_status_param);
    esp_rmaker_device_add_param(door_dev, alarm_trigger_param);
    esp_rmaker_node_add_device(node, door_dev);

    /* ---------------- OTA + Insights ---------------- */
    esp_rmaker_ota_enable_default();
    
    // Enable ESP Insights
    app_insights_enable();

    // Start RainMaker agent 
    esp_rmaker_start();

    // Start network (provisioning or connect)
    err = app_network_start(POP_TYPE_RANDOM);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start failed!");
        abort();
    }

    // Create IR sensor task 
    BaseType_t x = xTaskCreate(ir_sensor_task, "ir_sensor_task", IR_TASK_STACK, NULL, IR_TASK_PRIO, NULL);
    if (x != pdPASS) {
        ESP_LOGE(TAG, "Failed to create IR sensor task");
    }

    ESP_LOGI(TAG, "Smart Home System running.");
}