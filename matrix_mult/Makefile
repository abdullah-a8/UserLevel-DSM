# Makefile for distributed matrix multiplication demo

# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -g -I../include -I.
LDFLAGS = -L../build -ldsm -lpthread -lm

# Source files
SRCS = matrix_main.c matrix_state.c matrix_compute.c
OBJS = $(SRCS:.c=.o)
TARGET = matrix_mult

# Build directory
BUILD_DIR = ../build

# Default target
all: $(TARGET)

# Link the executable
$(TARGET): $(OBJS)
	@echo "Linking $(TARGET)..."
	$(CC) $(OBJS) $(LDFLAGS) -o $(BUILD_DIR)/$(TARGET)
	@echo "Build complete: $(BUILD_DIR)/$(TARGET)"

# Compile source files
%.o: %.c matrix_mult.h
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	@echo "Cleaning..."
	rm -f $(OBJS) $(BUILD_DIR)/$(TARGET)
	@echo "Clean complete"

# Phony targets
.PHONY: all clean
