# Makefile for ina219_mqtt with test support

ARCH     := $(shell uname -m)
TARGET   := ina219_mqtt-$(ARCH)
SRC = ina219_mqtt.c
CONF = ina.conf

CC = gcc
CFLAGS = -Wall -O2
LIBS = -lpaho-mqtt3c -lm

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LIBS)

clean:
	rm -f $(TARGET)

test: $(TARGET)
	@echo "Running I2C device scan test..."
	@./$(TARGET) --scan || echo "❌ Scan failed"

	@echo "Running config load + one-shot read test..."
	@./$(TARGET) --conf=$(CONF) --shunt=0.1 --current=1.0 --broker=tcp://localhost:1883 --get=ina219/test --reply=ina219/test &

	@sleep 1
	@mosquitto_pub -t "ina219/test" -n && mosquitto_sub -C 1 -t "ina219/test"
	@pkill $(TARGET) || true
