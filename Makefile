CC      := gcc
CFLAGS  := -O2 -Wall -Wextra -std=c11 -Iinclude
LDFLAGS := -lm -lpthread

SRC_DIR := src
BIN_DIR := bin

COMMON_OBJS := \
    $(BIN_DIR)/io_unit.o \
    $(BIN_DIR)/memory_unit.o \
    $(BIN_DIR)/compute_unit.o \
    $(BIN_DIR)/sensor_hw.o \
    $(BIN_DIR)/instrumentation.o \
    $(BIN_DIR)/pipeline_workload.o

.PHONY: all clean run

all: $(BIN_DIR)/critter_main $(BIN_DIR)/run_experiments $(BIN_DIR)/analyze_embedded

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN_DIR)/%.o: $(SRC_DIR)/%.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN_DIR)/critter_main: $(BIN_DIR)/main.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BIN_DIR)/run_experiments: $(BIN_DIR)/run_experiments.o $(BIN_DIR)/power_config.o $(BIN_DIR)/power_monitor.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BIN_DIR)/analyze_embedded: $(BIN_DIR)/analyze_embedded.o $(BIN_DIR)/power_monitor.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

run: all
	./$(BIN_DIR)/critter_main

clean:
	rm -rf $(BIN_DIR) results.csv embedded_results.csv embedded_results_all.csv
