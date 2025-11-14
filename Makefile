# User-Level Distributed Shared Memory (DSM) - Makefile
# Operating Systems Semester Project

# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -pthread -g -O2 -Iinclude
LDFLAGS = -pthread

# Directories
SRC_DIR = src
INC_DIR = include
TEST_DIR = tests
DEMO_DIR = demos
BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj

# Source files
MEMORY_SRC = $(wildcard $(SRC_DIR)/memory/*.c)
NETWORK_SRC = $(wildcard $(SRC_DIR)/network/*.c)
CONSISTENCY_SRC = $(wildcard $(SRC_DIR)/consistency/*.c)
SYNC_SRC = $(wildcard $(SRC_DIR)/sync/*.c)
CORE_SRC = $(wildcard $(SRC_DIR)/core/*.c)

ALL_SRC = $(MEMORY_SRC) $(NETWORK_SRC) $(CONSISTENCY_SRC) $(SYNC_SRC) $(CORE_SRC)

# Object files
MEMORY_OBJ = $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(MEMORY_SRC))
NETWORK_OBJ = $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(NETWORK_SRC))
CONSISTENCY_OBJ = $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(CONSISTENCY_SRC))
SYNC_OBJ = $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SYNC_SRC))
CORE_OBJ = $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(CORE_SRC))

ALL_OBJ = $(MEMORY_OBJ) $(NETWORK_OBJ) $(CONSISTENCY_OBJ) $(SYNC_OBJ) $(CORE_OBJ)

# Test files
TEST_SRC = $(wildcard $(TEST_DIR)/*.c)
TEST_BIN = $(patsubst $(TEST_DIR)/%.c,$(BUILD_DIR)/test_%,$(TEST_SRC))

# Demo files
DEMO_SRC = $(wildcard $(DEMO_DIR)/*.c)
DEMO_BIN = $(patsubst $(DEMO_DIR)/%.c,$(BUILD_DIR)/%,$(DEMO_SRC))

# Library
LIB = $(BUILD_DIR)/libdsm.a

# Targets
.PHONY: all clean test demo help

all: $(LIB)

# Build static library
$(LIB): $(ALL_OBJ) | $(BUILD_DIR)
	@echo "Building DSM library..."
	ar rcs $@ $(ALL_OBJ)
	@echo "Library built: $@"

# Compile source files to object files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# Create build directories
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)/{memory,network,consistency,sync,core}

# Build tests
test: $(LIB) $(TEST_BIN)
	@echo "Running tests..."
	@for test in $(TEST_BIN); do \
		echo "Running $$test..."; \
		$$test || exit 1; \
	done

$(BUILD_DIR)/test_%: $(TEST_DIR)/%.c $(LIB)
	@echo "Building test: $@..."
	$(CC) $(CFLAGS) $< -L$(BUILD_DIR) -ldsm $(LDFLAGS) -o $@

# Build demos
demo: $(LIB) $(DEMO_BIN)

$(BUILD_DIR)/%: $(DEMO_DIR)/%.c $(LIB)
	@echo "Building demo: $@..."
	$(CC) $(CFLAGS) $< -L$(BUILD_DIR) -ldsm $(LDFLAGS) -o $@

# Clean build artifacts
clean:
	@echo "Cleaning build artifacts..."
	rm -rf $(BUILD_DIR)
	@echo "Clean complete."

# Help target
help:
	@echo "User-Level DSM Makefile"
	@echo "======================="
	@echo "Targets:"
	@echo "  all     - Build DSM library (default)"
	@echo "  test    - Build and run all tests"
	@echo "  demo    - Build demo applications"
	@echo "  clean   - Remove all build artifacts"
	@echo "  help    - Show this help message"
	@echo ""
	@echo "Example usage:"
	@echo "  make              # Build library"
	@echo "  make test         # Build and run tests"
	@echo "  make demo         # Build demos"
	@echo "  make clean        # Clean everything"

# Dependency tracking (auto-generated)
-include $(ALL_OBJ:.o=.d)

$(OBJ_DIR)/%.d: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -MM -MT '$(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$<)' $< > $@
