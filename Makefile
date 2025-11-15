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
.PHONY: all clean test demo help test-tsan test-valgrind test-all

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

# ThreadSanitizer tests
TSAN_DIR = $(BUILD_DIR)/tsan
TSAN_OBJ_DIR = $(TSAN_DIR)/obj
TSAN_LIB = $(TSAN_DIR)/libdsm_tsan.a
TSAN_CFLAGS = $(CFLAGS) -fsanitize=thread -g
TSAN_LDFLAGS = $(LDFLAGS) -fsanitize=thread

TSAN_OBJ = $(patsubst $(SRC_DIR)/%.c,$(TSAN_OBJ_DIR)/%.o,$(ALL_SRC))
TSAN_TEST_BIN = $(patsubst $(TEST_DIR)/%.c,$(TSAN_DIR)/test_%,$(TEST_SRC))

$(TSAN_OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(TSAN_OBJ_DIR)
	@mkdir -p $(dir $@)
	@echo "Compiling with TSAN: $<..."
	$(CC) $(TSAN_CFLAGS) -c $< -o $@

$(TSAN_LIB): $(TSAN_OBJ) | $(TSAN_DIR)
	@echo "Building DSM library with TSAN..."
	ar rcs $@ $(TSAN_OBJ)
	@echo "TSAN library built: $@"

$(TSAN_DIR)/test_%: $(TEST_DIR)/%.c $(TSAN_LIB)
	@echo "Building TSAN test: $@..."
	$(CC) $(TSAN_CFLAGS) $< -L$(TSAN_DIR) -ldsm_tsan $(TSAN_LDFLAGS) -o $@

$(TSAN_DIR):
	mkdir -p $(TSAN_DIR)

$(TSAN_OBJ_DIR):
	mkdir -p $(TSAN_OBJ_DIR)/{memory,network,consistency,sync,core}

test-tsan: $(TSAN_LIB) $(TSAN_TEST_BIN)
	@echo ""
	@echo "========================================"
	@echo "  Running ThreadSanitizer Tests"
	@echo "========================================"
	@echo ""
	@for test in $(TSAN_TEST_BIN); do \
		echo "Running TSAN: $$test..."; \
		TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1" $$test || exit 1; \
		echo ""; \
	done
	@echo "========================================"
	@echo "  All TSAN tests passed! ✓"
	@echo "========================================"

# Valgrind tests
test-valgrind: $(LIB) $(TEST_BIN)
	@echo ""
	@echo "========================================"
	@echo "  Running Valgrind Memory Tests"
	@echo "========================================"
	@echo ""
	@for test in $(TEST_BIN); do \
		echo "Running Valgrind: $$test..."; \
		valgrind --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all \
		         --error-exitcode=1 --track-origins=yes --suppressions=/dev/null \
		         $$test > /dev/null 2>&1 || \
		(echo "Valgrind found issues in $$test" && \
		 valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes $$test && exit 1); \
		echo "  ✓ No memory leaks"; \
		echo ""; \
	done
	@echo "========================================"
	@echo "  All Valgrind tests passed! ✓"
	@echo "========================================"

# Run all tests (regular + TSAN + Valgrind)
test-all: test test-tsan test-valgrind
	@echo ""
	@echo "========================================"
	@echo "  ALL TESTS PASSED! ✓"
	@echo "  - Regular tests: PASS"
	@echo "  - TSAN tests: PASS"
	@echo "  - Valgrind tests: PASS"
	@echo "========================================"

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

clean-tsan:
	@echo "Cleaning TSAN build artifacts..."
	rm -rf $(TSAN_DIR)
	@echo "TSAN clean complete."

# Help target
help:
	@echo "User-Level DSM Makefile"
	@echo "======================="
	@echo "Targets:"
	@echo "  all          - Build DSM library (default)"
	@echo "  test         - Build and run all tests"
	@echo "  test-tsan    - Run tests with ThreadSanitizer (race condition detection)"
	@echo "  test-valgrind- Run tests with Valgrind (memory leak detection)"
	@echo "  test-all     - Run all tests (regular + TSAN + Valgrind)"
	@echo "  demo         - Build demo applications"
	@echo "  clean        - Remove all build artifacts"
	@echo "  clean-tsan   - Remove TSAN build artifacts"
	@echo "  help         - Show this help message"
	@echo ""
	@echo "Example usage:"
	@echo "  make              # Build library"
	@echo "  make test         # Build and run tests"
	@echo "  make test-tsan    # Check for race conditions"
	@echo "  make test-valgrind# Check for memory leaks"
	@echo "  make test-all     # Run all tests"
	@echo "  make demo         # Build demos"
	@echo "  make clean        # Clean everything"

# Dependency tracking (auto-generated)
-include $(ALL_OBJ:.o=.d)

$(OBJ_DIR)/%.d: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -MM -MT '$(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$<)' $< > $@
