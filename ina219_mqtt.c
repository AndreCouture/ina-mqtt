// ina219_mqtt.c
// Full working code for INA219/INA226 MQTT-enabled sensor reader

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>   // <- Needed for O_RDWR
#include <math.h>
#include <time.h>
#include <errno.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <stdbool.h>
#include <ctype.h>
#include <getopt.h>
#include <MQTTClient.h>

#define I2C_BUS "/dev/i2c-1"
#define I2C_ADDR 0x40
#define CONFIG_FILE "ina.conf"
#define QOS 1
#define TIMEOUT 10000L

#define REG_CONFIG        0x00
#define REG_SHUNT_VOLTAGE 0x01
#define REG_BUS_VOLTAGE   0x02
#define REG_POWER         0x03
#define REG_CURRENT       0x04
#define REG_CALIBRATION   0x05
#define REG_MASK          0x06
#define REG_ALERT         0x06
#define REG_MANUFACTURER  0xFE
#define REG_DIE_ID        0xFF

#define MODEL_AUTO   0
#define MODEL_INA219 1
#define MODEL_INA226 2

// Expected values (may vary slightly by vendor)
#define INA219_MANUF_ID  0x2000
#define INA226_MANUF_ID1 0x5449  // "TI"
#define INA226_MANUF_ID2 0x4954  // alternative encoding

#define TOPIC_INA_STATE "ina/state"

typedef struct {
    float shunt_ohms;
    float max_current;
    char broker[128];
    char client_id[64];
    char topic_get[128];
    char topic_reply[128];
    int model;
    int interactive;
    int interval;
} Config;

Config cfg = {
    .shunt_ohms = 0.1,
    .max_current = 2.0,
    .broker = "tcp://localhost:1883",
    .client_id = "ina-sensor",
    .topic_get = "ina/get",
    .topic_reply = "ina/status",
    .model = MODEL_AUTO,
    .interactive = 0,
    .interval = 5
};

MQTTClient client;

float current_lsb = 0.0;
float power_lsb = 0.0;
int i2c_fd = -1;


void trim(char *str) {
    char *end;
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return;
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    *(end + 1) = 0;
}

void load_config(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;

        *eq = 0;
        char *key = line;
        char *val = eq + 1;
        trim(key);
        trim(val);

        if (strcmp(key, "shunt_ohms") == 0) cfg.shunt_ohms = atof(val);
        else if (strcmp(key, "max_current") == 0) cfg.max_current = atof(val);
        else if (strcmp(key, "mqtt_broker") == 0) snprintf(cfg.broker, sizeof(cfg.broker), "%s", val);
        else if (strcmp(key, "mqtt_client_id") == 0) snprintf(cfg.client_id, sizeof(cfg.client_id), "%s", val);
        else if (strcmp(key, "mqtt_topic_get") == 0) snprintf(cfg.topic_get, sizeof(cfg.topic_get), "%s", val);
        else if (strcmp(key, "mqtt_topic_reply") == 0) snprintf(cfg.topic_reply, sizeof(cfg.topic_reply), "%s", val);
        else if (strcmp(key, "model") == 0) {
            if (strcmp(val, "ina219") == 0) cfg.model = MODEL_INA219;
            else if (strcmp(val, "ina226") == 0) cfg.model = MODEL_INA226;
            else cfg.model = MODEL_AUTO;
        }
    }

    fclose(f);
}

int open_i2c(uint8_t addr) {
    i2c_fd = open(I2C_BUS, O_RDWR);
    if (i2c_fd < 0) {
        perror("Failed to open I2C bus");
        return -1;
    }
    if (ioctl(i2c_fd, I2C_SLAVE, addr) < 0) {
        perror("Failed to set I2C address");
        close(i2c_fd);
        return -1;
    }
    return 0;
}

uint16_t read_reg(uint8_t reg) {
    uint8_t buf[2];
    if (write(i2c_fd, &reg, 1) != 1) {
        perror("Write reg failed");
        return 0xFFFF;
    }
    if (read(i2c_fd, buf, 2) != 2) {
        perror("Read reg failed");
        return 0xFFFF;
    }
    return (buf[0] << 8) | buf[1];
}

int write_reg(uint8_t reg, uint16_t value) {
    uint8_t buf[3];
    buf[0] = reg;
    buf[1] = (value >> 8) & 0xFF;
    buf[2] = value & 0xFF;
    if (write(i2c_fd, buf, 3) != 3) {
        perror("Write failed");
        return -1;
    }
    return 0;
}

void calibrate_sensor() {
    uint16_t calib=0;
    current_lsb = cfg.max_current / 32768.0;

    if (cfg.model == MODEL_INA219) {
        calib = (uint16_t)(0.04096 / (current_lsb * cfg.shunt_ohms));
        current_lsb = 0.04096 / (cfg.shunt_ohms * calib);
        power_lsb = current_lsb * 20.0;
    } else {
        calib = (uint16_t)(0.00512 / (current_lsb * cfg.shunt_ohms));
        current_lsb = 0.00512 / (cfg.shunt_ohms * calib);
        power_lsb = current_lsb * 25.0;
    }
    write_reg(REG_CALIBRATION, calib);
}

int detect_model() {
    int mfg = read_reg(REG_MANUFACTURER);
    printf("model: 0x%04X\n", mfg);
    printf("Detected Manufacturer ID: 0x%04X\n", read_reg(REG_MANUFACTURER));
    if (mfg == 0x5449 || mfg == 0x2000) return MODEL_INA219;
    if ((mfg >> 8) == 0x49 || (mfg == 0x2260)) return MODEL_INA226;
    return MODEL_INA226;
}

void ina_read(float *voltage, float *current, float *power, float *shunt) {
    if (voltage) {
        uint16_t bus_raw = read_reg(REG_BUS_VOLTAGE);
        *voltage = ((bus_raw >> 3) * (cfg.model == MODEL_INA219?0.004:0.00125));
    }
    if (shunt) {
        int16_t raw = (int16_t)read_reg(REG_SHUNT_VOLTAGE);
        *shunt = raw * (cfg.model == MODEL_INA219?0.01:0.00025);
    }
    if (current) {
        int16_t raw = (int16_t)read_reg(REG_CURRENT);
        *current = raw * current_lsb * 1000.0;
    }
    if (power) {
        uint16_t raw = read_reg(REG_POWER);
        *power = raw * power_lsb * 1000.0;
    }

}

void print_mqtt_reading() {
    float voltage = 0.0, shunt = 0.0;
    int16_t raw_current = read_reg(REG_CURRENT);
    int16_t raw_power = read_reg(REG_POWER);
    int16_t raw_shunt = read_reg(REG_SHUNT_VOLTAGE);
    uint16_t raw_bus = read_reg(REG_BUS_VOLTAGE);

    if (cfg.model == MODEL_INA219) {
        voltage = (raw_bus >> 3) * 0.004; //4.0e-3;        // 4 mV/bit
        shunt   = raw_shunt * 0.00001; //10.0e-6;     // 10 µV/bit
    } else {
        voltage = (raw_bus) * 0.00125; //1.25e-3;       // 1.25 mV/bit
        shunt   = raw_shunt * 0.0000025; //2.5e-6;      // 2.5 µV/bit
    }

    float current = raw_current * current_lsb * 1000.0; // mA
    float power   = raw_power   * power_lsb   * 1000.0; // mW
    float shunt_mV = shunt * 1000.0;

    // Debug output
//  printf("[DEBUG] Raw: V=0x%04X S=0x%04X C=0x%04X P=0x%04X\n",
//         raw_bus, raw_shunt, raw_current, raw_power);
//
//  printf("[DEBUG] Parsed: V=%.3fV I=%.2fmA P=%.2fmW S=%.2fmV\n",
//      voltage, current, power, shunt_mV);

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"voltage_V\":%.3f,\"current_mA\":%.2f,\"power_mW\":%.2f,\"shunt_mV\":%.2f}",
             voltage, current, power, shunt_mV);
    printf("[INFO] %s\n", payload);
}

//bool ina_connected() {
//  int raw = wiringPiI2CReadReg16(fd, REG_BUS_VOLTAGE);
//  if (raw == -1 || raw == 0xFFFF || raw == 0x0000) {
//      return 0; // Likely disconnected or unreadable
//  }
//  return 1;
//}

int messageArrived(void *context, char *topicName, int topicLen, MQTTClient_message *message) {
    if (!message || !message->payload) return 1;
    
    char *payload = strndup((char *)message->payload, message->payloadlen);
    printf("[DEBUG] Received on topic %s: %s\n", topicName, payload);
    
    if (strcmp(topicName, cfg.topic_get) == 0) {
        float voltage, current, power, shunt;
        ina_read(&voltage, &current, &power, &shunt);
        
        char reply[256];
        snprintf(reply, sizeof(reply),
            "{\"voltage_V\":%.3f,\"current_mA\":%.3f,\"power_mW\":%.3f,\"shunt_mV\":%.3f}",
            voltage, current, power, shunt);
        
        MQTTClient_message pubmsg = MQTTClient_message_initializer;
        pubmsg.payload = reply;
        pubmsg.payloadlen = (int)strlen(reply);
        pubmsg.qos = QOS;
        pubmsg.retained = 0;
        
        MQTTClient_deliveryToken token;
        MQTTClient_publishMessage(client, cfg.topic_reply, &pubmsg, &token);
        MQTTClient_waitForCompletion(client, token, TIMEOUT);
        printf("[INFO] %s => %s\n", cfg.topic_reply, reply);
    } else if (strcmp(topicName, TOPIC_INA_STATE) == 0) {
        const char *reply = "{\"status\":\"alive\"}";
        MQTTClient_message pubmsg = MQTTClient_message_initializer;
        pubmsg.payload = (void *)reply;
        pubmsg.payloadlen = (int)strlen(reply);
        pubmsg.qos = QOS;
        pubmsg.retained = 0;
        MQTTClient_deliveryToken token;
        MQTTClient_publishMessage(client, cfg.topic_reply, &pubmsg, &token);
        MQTTClient_waitForCompletion(client, token, TIMEOUT);
        printf("[INFO] %s => %s\n", cfg.topic_reply, reply);
    } else {
        printf("[WARN] Unknown command: %s\n", payload);
    }
    
    free(payload);
    MQTTClient_freeMessage(&message);
    MQTTClient_free(topicName);
    return 1;
}

void subscribe_topic(char *topic) {
    printf("[INFO] Listening for MQTT topic '%s'\n", topic);
    MQTTClient_subscribe(client, topic, QOS);
}

uint16_t read_reg16(int fd, uint8_t reg) {
    uint8_t buf[2];
    if (write(fd, &reg, 1) != 1) return 0xFFFF;
    if (read(fd, buf, 2) != 2) return 0xFFFF;
    return (buf[0] << 8) | buf[1]; // Big endian
}

void scan_and_detect() {
    printf("🔍 Scanning I²C bus for INA sensors...\n");
    
    for (int addr = 0x03; addr <= 0x77; addr++) {
        int fd = open(I2C_BUS, O_RDWR);
        if (fd < 0) continue;
        
        if (ioctl(fd, I2C_SLAVE, addr) < 0) {
            close(fd);
            continue;
        }
        
        uint16_t mfg = read_reg16(fd, REG_MANUFACTURER);
        close(fd);
        
        if (mfg == INA219_MANUF_ID) {
            printf("✓ Found INA219 at 0x%02X (Manufacturer ID: 0x%04X)\n", addr, mfg);
        } else if (mfg == INA226_MANUF_ID1 || mfg == INA226_MANUF_ID2) {
            printf("✓ Found INA226 at 0x%02X (Manufacturer ID: 0x%04X)\n", addr, mfg);
        } else if (mfg != 0xFFFF) {
            printf("• Device at 0x%02X (Unknown ID: 0x%04X)\n", addr, mfg);
        }
    }
}

void print_help() {
    printf("Usage: ./ina219_mqtt [options]\n");
    printf("  -c, --conf=FILE         Config file path\n");
    printf("  -s, --shunt=OHMS        Shunt resistor value\n");
    printf("  -i, --current=AMPS      Max current\n");
    printf("  -b, --broker=URI        MQTT broker\n");
    printf("  -d, --client_id=ID      MQTT client ID\n");
    printf("  -g, --get=TOPIC         MQTT subscribe topic\n");
    printf("  -r, --reply=TOPIC       MQTT reply topic\n");
    printf("      --model=MODEL       Model (ina219, ina226, auto)\n");
    printf("  -S, --scan              Scan I2C bus\n");
    printf("  --interactive           Interactive console mode\n");
    printf("  --interval=SECONDS      Polling interval\n");
    printf("  -h, --help              Show help\n");
}

int main(int argc, char *argv[]) {
    const char *conf_path = CONFIG_FILE;
    int scan_only = 0;

    static struct option long_opts[] = {
        {"conf", required_argument, 0, 'c'},
        {"shunt", required_argument, 0, 's'},
        {"current", required_argument, 0, 'i'},
        {"broker", required_argument, 0, 'b'},
        {"client_id", required_argument, 0, 'd'},
        {"get", required_argument, 0, 'g'},
        {"reply", required_argument, 0, 'r'},
        {"model", required_argument, 0, 0},
        {"scan", no_argument, 0, 'S'},
        {"help", no_argument, 0, 'h'},
        {"interactive", no_argument, &cfg.interactive, 1},
        {"interval", required_argument, 0, 0},
        {0, 0, 0, 0}
    };

    load_config(conf_path);
    int opt, opt_index = 0;
    while ((opt = getopt_long(argc, argv, "c:s:i:b:d:g:r:Sh", long_opts, &opt_index)) != -1) {
        switch (opt) {
            case 'c':
                conf_path = optarg;
                load_config(conf_path);
                break;
            case 's': cfg.shunt_ohms = atof(optarg); break;
            case 'i': cfg.max_current = atof(optarg); break;
            case 'b': snprintf(cfg.broker, sizeof(cfg.broker), "%s", optarg); break;
            case 'd': snprintf(cfg.client_id, sizeof(cfg.client_id), "%s", optarg); break;
            case 'g': snprintf(cfg.topic_get, sizeof(cfg.topic_get), "%s", optarg); break;
            case 'r': snprintf(cfg.topic_reply, sizeof(cfg.topic_reply), "%s", optarg); break;
            case 'S': scan_only = 1; break;
            case 0:
                if (strcmp(long_opts[opt_index].name, "model") == 0) {
                    if (strcmp(optarg, "ina219") == 0) cfg.model = MODEL_INA219;
                    else if (strcmp(optarg, "ina226") == 0) cfg.model = MODEL_INA226;
                    else cfg.model = MODEL_AUTO;
                } else if (strcmp(long_opts[opt_index].name, "interval") == 0) {
                    cfg.interval = atoi(optarg);
                }
                break;
            case 'h': print_help(); return 0;
        }
    }

    if (scan_only) {
        scan_and_detect();
        return 0;
    }
    if (open_i2c(I2C_ADDR) != 0) {
        perror("I2C Setup Failed"); 
        return 1;
    }
    if (cfg.model == MODEL_AUTO) cfg.model = detect_model();
    printf("Sensor model: %s\n", cfg.model == MODEL_INA226 ? "INA226" : "INA219");
    calibrate_sensor();

    MQTTClient_create(&client, cfg.broker, cfg.client_id, MQTTCLIENT_PERSISTENCE_NONE, NULL);
    MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
    MQTTClient_setCallbacks(client, NULL, NULL, messageArrived, NULL);
    if (MQTTClient_connect(client, &opts) != MQTTCLIENT_SUCCESS) {
        fprintf(stderr, "MQTT connection failed. Defaulting to interactive mode!\n");
        cfg.interactive = 1;
    } else {
        subscribe_topic(cfg.topic_get);
        subscribe_topic(TOPIC_INA_STATE);
    }

    while (1) {

//      char *topic = NULL;
//      int msgid;
//      MQTTClient_message *msg = NULL;
//      if (MQTTClient_receive(client, &topic, &msgid, &msg, 1000) == MQTTCLIENT_SUCCESS && msg) {
//          send_mqtt_reading(client);
//          MQTTClient_freeMessage(&msg);
//          MQTTClient_free(topic);
//      }
        if (cfg.interactive) {
            print_mqtt_reading(client);
            sleep(cfg.interval);
        }

        // Block until message or timeout (1000ms = 1 sec)
        MQTTClient_yield();  // Non-blocking but polite
        usleep(100000);      // Sleep for 100ms between yield cycles
    }

    MQTTClient_disconnect(client, 1000);
    MQTTClient_destroy(&client);
    close(i2c_fd);
    return 0;
}
