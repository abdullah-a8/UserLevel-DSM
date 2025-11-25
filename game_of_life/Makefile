# Conway's Game of Life - DSM Implementation - Makefile

# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -pthread -g -O2 -I../include
LDFLAGS = -pthread -L../build -ldsm

# Directories
BUILD_DIR = build
DSM_LIB = ../build/libdsm.a

# Source files
SRCS = gol_main.c gol_state.c gol_rules.c
OBJS = $(SRCS:.c=.o)
TARGET = $(BUILD_DIR)/game_of_life

# Default target
.PHONY: all clean run-manager run-worker help

all: $(TARGET)

# Build game_of_life binary
$(TARGET): $(OBJS) $(DSM_LIB) | $(BUILD_DIR)
	@echo "Linking game_of_life..."
	$(CC) $(OBJS) $(LDFLAGS) -o $@
	@echo "Built: $@"

# Compile source files
%.o: %.c gol_config.h gol_state.h gol_rules.h
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# Create build directory
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Ensure DSM library is built
$(DSM_LIB):
	@echo "Building DSM library..."
	cd .. && $(MAKE) all

# Clean build artifacts
clean:
	rm -f $(OBJS)
	rm -rf $(BUILD_DIR)
	@echo "Cleaned game_of_life build artifacts"

# Helper targets for running
run-manager:
	$(TARGET) --manager --nodes 2 --grid 100x100 --generations 10

run-worker:
	$(TARGET) --worker --node-id 1 --manager-host localhost --nodes 2

# Help
help:
	@echo "Conway's Game of Life - DSM Implementation"
	@echo ""
	@echo "Targets:"
	@echo "  all          - Build game_of_life binary (default)"
	@echo "  clean        - Remove build artifacts"
	@echo "  run-manager  - Run as manager (Node 0) with 2 nodes"
	@echo "  run-worker   - Run as worker (Node 1) connecting to localhost"
	@echo "  help         - Show this help message"
	@echo ""
	@echo "Manual Usage:"
	@echo "  Manager: $(TARGET) --manager --nodes <N> [options]"
	@echo "  Worker:  $(TARGET) --worker --node-id <ID> --manager-host <HOST> [options]"
	@echo ""
	@echo "Run '$(TARGET) --help' for full options"
