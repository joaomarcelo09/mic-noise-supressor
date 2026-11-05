CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2
PLUGIN_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
REPO_ROOT  := $(abspath $(PLUGIN_DIR)/..)
BUILD_DIR  ?= $(REPO_ROOT)/build

RNNOISE_CFLAGS := $(shell pkg-config --cflags rnnoise 2>/dev/null || echo "-I/usr/include")
RNNOISE_LIBS   := $(shell pkg-config --libs rnnoise 2>/dev/null || echo "-lrnnoise")

TARGET_SO := $(BUILD_DIR)/librnnoise_gate_ladspa.so
TEST_BIN  := $(BUILD_DIR)/test_plugin

PLUGIN_SRC := $(PLUGIN_DIR)/rnnoise_gate.c
TEST_SRC   := $(PLUGIN_DIR)/test_plugin.c

all: $(TARGET_SO)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET_SO): $(PLUGIN_SRC) $(PLUGIN_DIR)/ladspa.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -fPIC -I$(PLUGIN_DIR) $(RNNOISE_CFLAGS) -shared -o $@ $< $(RNNOISE_LIBS) -lm

test: $(TARGET_SO) $(TEST_SRC)
	$(CC) $(CFLAGS) -I$(PLUGIN_DIR) -o $(TEST_BIN) $(TEST_SRC) -ldl -lm
	$(TEST_BIN) $(TARGET_SO)

clean:
	rm -f $(TARGET_SO) $(TEST_BIN)

.PHONY: all test clean
