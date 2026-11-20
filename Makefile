# Convenience wrapper around CMake. Usage: make [run|test|dump FILE=x.ska|clean|help]
BUILD_DIR ?= build
CONFIG    ?= Release
JOBS      ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

.PHONY: all build configure run test dump clean help

all: build

# First configure only; `make configure` re-runs it (e.g. CONFIG=Debug).
$(BUILD_DIR)/CMakeCache.txt:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(CONFIG)

configure:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(CONFIG)

build: $(BUILD_DIR)/CMakeCache.txt
	cmake --build $(BUILD_DIR) -j $(JOBS)

run: build
	./$(BUILD_DIR)/skelanim

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

dump: build
	@test -n "$(FILE)" || { echo "usage: make dump FILE=path/to/file.ska"; exit 1; }
	./$(BUILD_DIR)/ska_dump "$(FILE)"

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "make            build everything ($(CONFIG)) into $(BUILD_DIR)/"
	@echo "make run        build and start the editor"
	@echo "make test       build and run all tests"
	@echo "make dump FILE=anim.ska   print a .ska file"
	@echo "make configure CONFIG=Debug   reconfigure (Release/Debug/RelWithDebInfo)"
	@echo "make clean      remove $(BUILD_DIR)/"
