# INA219 MQTT Sensor Agent

A compact C-based tool that reads sensor data from an INA219 current/voltage sensor on Raspberry Pi and replies with values over MQTT when requested. It supports configuration via file and optional command-line arguments.

---

## 🔧 Features

- Reads **bus voltage**, **shunt voltage**, **current (mA)**, and **power (mW)**
- Publishes data to MQTT only on request (via a topic like `ina219/get`)
- Configurable via `.conf` file or `--key=value` and `-k=value` style CLI arguments
- Supports `--scan` to list detected I²C devices and confirm INA219 presence
- Uses Paho MQTT C Client and wiringPi I2C API

---

## 📦 Dependencies

Install these before compiling:

```bash
sudo apt install libi2c-dev i2c-tools build-essential libpaho-mqtt-dev
```

Or manually build [paho.mqtt.c](https://github.com/eclipse/paho.mqtt.c) if needed.

---

## 🛠️ Wiring

Connect INA219 to Raspberry Pi:

| INA219 Pin | Raspberry Pi GPIO |
|------------|-------------------|
| VCC        | 3.3V or 5V        |
| GND        | GND               |
| SDA        | GPIO 2 (SDA)      |
| SCL        | GPIO 3 (SCL)      |

And wire the **shunt resistor inline** with the device whose current you want to monitor:

```
[Power +] ──> [INA219 Vin+] ──> [Load] ──> [INA219 Vin-] ──> GND
```

Alternatively, if you only want to measure voltage (not current), you can connect:

```
[Voltage Source +] ──> [INA219 Vin+]
[Voltage Source -] ──> [INA219 Vin- and GND]
```

In this setup, the shunt voltage and current will be close to zero, but **bus voltage** will still be accurately measured.

---

## ⚙️ Configuration

Create a config file named `ina.conf`:

```ini
shunt_ohms=0.1
max_current=1.0
mqtt_broker=tcp://localhost:1883
mqtt_client_id=ina219-sensor
mqtt_topic_get=ina219/get
mqtt_topic_reply=ina219/status
```

---

## 🚀 Compilation

```bash
gcc -o ina219_mqtt ina219_mqtt.c -lpaho-mqtt3c -lm
```

---

## ✅ Usage

### Run with config file (default: `ina.conf`)
```bash
./ina219_mqtt
```

### Override values on command line:
```bash
./ina219_mqtt --shunt=0.2 --current=2.0 --broker=tcp://192.168.1.10:1883
```

### Use a different config file:
```bash
./ina219_mqtt --conf=prod.conf
```

### Show help:
```bash
./ina219_mqtt --help
```

### Scan I²C bus and detect INA219:
```bash
./ina219_mqtt --scan
```

---

## 🧪 MQTT Testing

From another terminal:

### Trigger reading:
```bash
mosquitto_pub -t ina219/get -n
```

### Listen for reply:
```bash
mosquitto_sub -t ina219/status
```

Example response:
```json
{"voltage":5.012,"current_mA":127.345,"power_mW":638.221,"shunt_mV":6.382}
```

---

## ⚙️ Operating Modes and Measurement Options

The INA219 supports multiple use cases:

### 🔹 1. Voltage-Only Mode (no current measurement)
- Only connect **Vin+** and **Vin-** across a voltage source.
- No load current will pass through the shunt resistor.
- Only **bus voltage** will report meaningful values.

Useful for: monitoring power supply voltage without load monitoring.

### 🔹 2. Full Mode (Voltage + Load Current Measurement)
- Wire the load inline between **Vin+** and **Vin-**.
- Measures:
  - **Bus Voltage** (voltage seen by the load)
  - **Shunt Voltage** (across the resistor)
  - **Current** (calculated using shunt and calibration)
  - **Power** (voltage × current)

Useful for: monitoring actual power usage by a device.

---

## 🧠 Logic Overview

1. Load defaults from config file (e.g. `ina219.conf`)
2. Parse `--key=value` or `-k=value` arguments to override
3. Setup I2C connection to INA219 and calibrate it
4. Connect to MQTT broker and subscribe to the request topic
5. On message, read data from INA219 and publish JSON response

---

## 📌 Notes

- Requires `root` or I2C-enabled user to access `/dev/i2c-*`
- Uses 1.25ms conversion time per default INA219 config (continuous mode)
- Program runs indefinitely and listens for MQTT requests
- `--scan` helps detect addressable I²C devices and verify INA219 is present at `0x40`

---

## 📂 Files

- `ina219_mqtt.c` — main C program
- `ina219.conf` — configuration file (optional)

---

## 📈 Optional Improvements

You can extend this program to:

- Publish periodically instead of only on request (`--interval=5s`)
- Log to CSV or file
- Support multiple INA219 sensors on different I2C addresses
- Use TLS-secured MQTT with authentication
- Display detected device capabilities with `--scan`

---

Made for Raspberry Pi, tested on Debian Bookworm.
