# Smart Home System with Voice Control & Insights

This project demonstrates a production-ready Smart Home Node implemented using **ESP RainMaker** and **ESP Insights**. It simulates a security system that can be controlled via **Phone App**, **Voice (Google Home/Alexa)**, and automated sensors, all while reporting detailed health diagnostics to the cloud.

## Key Features

* **Voice Control:** Fully compatible with **Google Home** and **Amazon Alexa**.
    * "Hey Google, turn on the Home Light."
    * "Hey Google, turn on the Alarm System."
* **Multi-Device Control:** Implements three logical devices in a single node:
    * **Home Light:** Smart LED (GPIO 2) with toggle control.
    * **Alarm System:** Smart Switch (Security Arming) with toggle control. Activate blinking LED (GPIO 2) and buzzer (GPIO 4).
    * **Door Sensor:** Contact sensor (GPIO 3) reporting "Opened/Closed" status.
* **Cloud Diagnostics:** Integrated **ESP Insights** for professional observability:
    * **Async Events:** Logs specific actions like `DOOR_ACTION` or `SECURITY_ALERT`.
    * **System Health:** Real-time RTOS metrics (Heap memory, Wi-Fi signal).
    * **Crash Analysis:** Captures core dumps in a dedicated flash partition.
* **Security Logic:** Local RTOS task (`ir_sensor_task`) monitors the sensor and triggers a buzzer/alert if the alarm is armed.

---

## Dashboards

To fully utilize the monitoring capabilities of this firmware, use the following Espressif dashboards:

1.  **[ESP RainMaker Dashboard](https://dashboard.rainmaker.espressif.com/)**
    * *Use for:* Device management, User Linking, and OTA updates.
2.  **[ESP Insights Dashboard](https://dashboard.insights.espressif.com/)**
    * *Use for:* Observability. View the **Timeline** of events, analyze **System Metrics** graphs, and debug errors or crashes.

---

## Hardware Setup

The firmware is configured for the following pinout (modifiable in `app_main.c`):

| Component | GPIO Pin | Description |
| :--- | :--- | :--- |
| **Home Light (LED)** | **GPIO 2** | Output. Toggles via App/Voice or blinks on alarm. |
| **IR Sensor** | **GPIO 3** | Input. Simulates a door sensor (High = Open). |
| **Buzzer** | **GPIO 4** | Output. Sounds when an intrusion is detected. |

*(Note: Pins may vary depending on your specific ESP32/C3 board definition).*

---

## Build and Flash Firmware

Follow the ESP RainMaker Documentation [Get Started](https://rainmaker.espressif.com/docs/get-started.html) section to setup your environment.

### 1. Configuration (`menuconfig`)
Before building, you **must** configure the project to enable the required components:

Run `idf.py menuconfig` and ensure the following are enabled:

* **ESP RainMaker Config**
    * `[x]` Enable ESP RainMaker
    * `[x]` Enable Wi-Fi Provisioning
* **ESP Insights**
    * `[x]` Enable ESP Insights
    * `[x]` Enable system metrics (Heap, Wi-Fi)
* **ESP Diagnostics**
    * `[x]` Enable ESP Diagnostics

#### Custom App Insights Configuration
This project includes a custom configuration menu for fine-tuning log reporting:
* `Component config` -> `App Insights` -> **Enable all log types**
    * **Enabled (Default):** Reports Errors, Warnings, and Custom Events.
    * **Disabled:** Reports *only* Error logs to save bandwidth.

### 2. Partitions
This project uses a custom `partitions.csv` to support advanced diagnostics. It includes a **64KB `coredump` partition** to save crash data for upload to the Insights Dashboard.

### 3. Flash
Build and flash the project:
`idf.py build flash monitor`

### What to expect in this example?
Once flashed and provisioned, you can link the device to your Google Home or Alexa account via the RainMaker app.

- Voice & App Control:

Toggle the "Home Light" or "Alarm System" using the mobile app.

Voice Command: Ask your voice assistant (Google Home is suggested) to control the devices directly.

- Security Logic:

If you enable the Alarm System and trigger the Door Sensor, the device will locally trigger the Buzzer, blink the LED, and send a Critical Alert to your phone.

### Observability:

The ESP Insights Agent runs in the background, reporting health metrics.

You will see messages like these in the serial monitor, confirming successful cloud logging:
```
I (670808) esp_insights: Data message send success, msg_id:65267.
I (690678) heap_metrics: free:0x1b3a0 lfb:0x18000 min_free_ever:0x159f4
I (690678) wifi_metrics: rssi:-50 min_rssi_ever:-65
I (702968) esp_rmaker_param: Received params: {"Home Light":{"Power":false}}
I (702968) esp_rmaker_param: Found 1 params in write request for Home Light
I (702968) LIGHT_ACTION: Light Power -> OFF
I (702968) esp_rmaker_param: Reporting params: {"Home Light":{"Power":false},"Door Sensor Status":{"Door Status":"CLOSED","Alarm Triggered":false}}
I (720678) heap_metrics: free:0x1b3b8 lfb:0x18000 min_free_ever:0x159f4
I (720678) wifi_metrics: rssi:-53 min_rssi_ever:-65
I (728838) DOOR_ACTION: Door Sensor: OPENED
I (730838) DOOR_ACTION: Door Sensor: CLOSED
```

### Reset to Factory
Press and hold the BOOT button for 10 seconds to reset the board to factory defaults. This erases Wi-Fi credentials and RainMaker mapping. You will have to provision the board again to use it.
