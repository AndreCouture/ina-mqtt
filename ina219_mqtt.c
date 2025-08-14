#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <wiringPiI2C.h>
#include "MQTTClient.h"

#define INA219_ADDR 0x40
#define QOS 1
#define TIMEOUT 10000L

#define INA219_REG_CONFIG         0x00
#define INA219_REG_SHUNT_VOLTAGE  0x01
#define INA219_REG_BUS_VOLTAGE    0x02
#define INA219_REG_POWER          0x03
#define INA219_REG_CURRENT        0x04
#define INA219_REG_CALIBRATION    0x05

#define MAX_LINE 256

typedef struct {
    float shunt_ohms;
    float max_current;
    char mqtt_broker[128];
    char mqtt_client_id[64];
    char mqtt_topic_get[64];
    char mqtt_topic_reply[64];
} AppConfig;

AppConfig config;
MQTTClient client;
int fd;
float current_lsb = 0.0;
float power_lsb = 0.0;

uint16_t read16(int fd, uint8_t reg) {
    uint16_t raw = wiringPiI2CReadReg16(fd, reg);
    return (raw << 8) | (raw >> 8);
}

void write16(int fd, uint8_t reg, uint16_t val) {
    val = (val << 8) | (val >> 8);
    wiringPiI2CWriteReg16(fd, reg, val);
}

void ina219_calibrate(float r_shunt, float max_current) {
    current_lsb = max_current / 32768.0;
    uint16_t calib = (uint16_t)(0.04096 / (current_lsb * r_shunt));
    current_lsb = 0.04096 / (r_shunt * calib);
    power_lsb = current_lsb * 20.0;
    write16(fd, INA219_REG_CALIBRATION, calib);
}

void ina219_read(float *voltage, float *current, float *power, float *shunt) {
    if (voltage) {
        uint16_t bus_raw = read16(fd, INA219_REG_BUS_VOLTAGE);
        *voltage = ((bus_raw >> 3) * 4.0) / 1000.0;
    }
    if (shunt) {
        int16_t raw = (int16_t)read16(fd, INA219_REG_SHUNT_VOLTAGE);
        *shunt = raw * 0.01;
    }
    if (current) {
        int16_t raw = (int16_t)read16(fd, INA219_REG_CURRENT);
        *current = raw * current_lsb * 1000.0;
    }
    if (power) {
        uint16_t raw = read16(fd, INA219_REG_POWER);
        *power = raw * power_lsb * 1000.0;
    }
}

void load_config(const char *filename, AppConfig *cfg) {
    FILE *file = fopen(filename, "r");
    if (!file) return;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), file)) {
        char key[MAX_LINE], value[MAX_LINE];
        if (sscanf(line, "%[^=]=%s", key, value) == 2) {
            if (strcmp(key, "shunt_ohms") == 0) cfg->shunt_ohms = atof(value);
            else if (strcmp(key, "max_current") == 0) cfg->max_current = atof(value);
            else if (strcmp(key, "mqtt_broker") == 0) strncpy(cfg->mqtt_broker, value, sizeof(cfg->mqtt_broker));
            else if (strcmp(key, "mqtt_client_id") == 0) strncpy(cfg->mqtt_client_id, value, sizeof(cfg->mqtt_client_id));
            else if (strcmp(key, "mqtt_topic_get") == 0) strncpy(cfg->mqtt_topic_get, value, sizeof(cfg->mqtt_topic_get));
            else if (strcmp(key, "mqtt_topic_reply") == 0) strncpy(cfg->mqtt_topic_reply, value, sizeof(cfg->mqtt_topic_reply));
        }
    }
    fclose(file);
}

void override_from_args(int argc, char **argv, AppConfig *cfg, const char **conf_path) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [options]\n", argv[0]);
            printf("\nOptions:\n");
            printf("  -c, --conf=path         Config file path (default: ina219.conf)\n");
            printf("  -s, --shunt=value       Shunt resistor in ohms\n");
            printf("  -i, --current=value     Max expected current in amps\n");
            printf("  -b, --broker=uri        MQTT broker URI\n");
            printf("  -d, --client_id=name    MQTT client ID\n");
            printf("  -g, --get=topic         MQTT request topic\n");
            printf("  -r, --reply=topic       MQTT reply topic\n");
            printf("  -S, --scan              Scan I2C bus for devices\n");
            printf("  -h, --help              Show this help and exit\n");
            exit(0);
        }
        if (strcmp(argv[i], "--scan") == 0 || strcmp(argv[i], "-S") == 0) {
            int file = open("/dev/i2c-1", O_RDWR);
            if (file < 0) { perror("I2C open failed"); exit(1); }
            printf("Scanning I2C bus /dev/i2c-1:\n");
            for (int addr = 0x03; addr <= 0x77; addr++) {
                if (ioctl(file, I2C_SLAVE, addr) < 0) continue;
                uint8_t reg = 0x00;
                if (write(file, &reg, 1) == 1) {
                    uint8_t buf[2];
                    if (read(file, buf, 2) == 2) {
                        printf("  Found device at 0x%02X", addr);
                        if (addr == 0x40) printf("  --> INA219 likely present");
                        printf("\n");
                    }
                }
            }
            close(file);
            exit(0);
        }
        char *arg = argv[i];
        if (strncmp(arg, "--conf=", 8) == 0 || strncmp(arg, "-c=", 3) == 0)
            *conf_path = strchr(arg, '=') + 1;
        else if (strncmp(arg, "--shunt=", 8) == 0 || strncmp(arg, "-s=", 3) == 0)
            cfg->shunt_ohms = atof(strchr(arg, '=') + 1);
        else if (strncmp(arg, "--current=", 10) == 0 || strncmp(arg, "-i=", 3) == 0)
            cfg->max_current = atof(strchr(arg, '=') + 1);
        else if (strncmp(arg, "--broker=", 9) == 0 || strncmp(arg, "-b=", 3) == 0)
            strncpy(cfg->mqtt_broker, strchr(arg, '=') + 1, sizeof(cfg->mqtt_broker));
        else if (strncmp(arg, "--client_id=", 12) == 0 || strncmp(arg, "-d=", 3) == 0)
            strncpy(cfg->mqtt_client_id, strchr(arg, '=') + 1, sizeof(cfg->mqtt_client_id));
        else if (strncmp(arg, "--get=", 6) == 0 || strncmp(arg, "-g=", 3) == 0)
            strncpy(cfg->mqtt_topic_get, strchr(arg, '=') + 1, sizeof(cfg->mqtt_topic_get));
        else if (strncmp(arg, "--reply=", 8) == 0 || strncmp(arg, "-r=", 3) == 0)
            strncpy(cfg->mqtt_topic_reply, strchr(arg, '=') + 1, sizeof(cfg->mqtt_topic_reply));
    }
}

int messageArrived(void *context, char *topicName, int topicLen, MQTTClient_message *message) {
    float voltage, current, power, shunt;
    ina219_read(&voltage, &current, &power, &shunt);
    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"voltage\":%.3f,\"current_mA\":%.3f,\"power_mW\":%.3f,\"shunt_mV\":%.3f}",
             voltage, current, power, shunt);
    MQTTClient_message pubmsg = MQTTClient_message_initializer;
    pubmsg.payload = payload;
    pubmsg.payloadlen = strlen(payload);
    pubmsg.qos = QOS;
    pubmsg.retained = 0;
    MQTTClient_deliveryToken token;
    MQTTClient_publishMessage(client, config.mqtt_topic_reply, &pubmsg, &token);
    MQTTClient_waitForCompletion(client, token, TIMEOUT);
    MQTTClient_freeMessage(&message);
    MQTTClient_free(topicName);
    return 1;
}

int main(int argc, char **argv) {
    const char *conf_path = "ina219.conf";
    override_from_args(argc, argv, &config, &conf_path);
    load_config(conf_path, &config);
    override_from_args(argc, argv, &config, &conf_path);
    fd = wiringPiI2CSetup(INA219_ADDR);
    if (fd < 0) { fprintf(stderr, "INA219 not detected\n"); return 1; }
    ina219_calibrate(config.shunt_ohms, config.max_current);
    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    MQTTClient_create(&client, config.mqtt_broker, config.mqtt_client_id, MQTTCLIENT_PERSISTENCE_NONE, NULL);
    MQTTClient_setCallbacks(client, NULL, NULL, messageArrived, NULL);
    if (MQTTClient_connect(client, &conn_opts) != MQTTCLIENT_SUCCESS) {
        fprintf(stderr, "MQTT connection failed\n"); return 1;
    }
    MQTTClient_subscribe(client, config.mqtt_topic_get, QOS);
    printf("Listening for MQTT topic '%s'...\n", config.mqtt_topic_get);
    while (1) sleep(1);
    MQTTClient_disconnect(client, 1000);
    MQTTClient_destroy(&client);
    return 0;
}

