# Entry points for building, running, checking and deploying. CMake does the real work.
#
#   make                build for this machine (build/)
#   make run            run it in a window; make demo: with sample content, no server
#   make lint           clang-format, clang-tidy and shellcheck on build/ (this machine's tools)
#   make format         reformat the sources
#   make device-build   the muOS package: dist/Sendspin/ and dist/Sendspin.zip (Docker)
#   make device-lint    make lint on the device build, with the pinned tools (Docker), as CI runs it
#   make device-deploy  build the package and install it on a device over SSH
#
# Variables: ARGS="--size 720x720 ..." (options for run/demo, see make help-options),
# DEVICE=user@host (default root@muos.local), BUILD=dir (default build).

BUILD ?= build
DEVICE ?= root@muos.local
ARGS ?=

.PHONY: all build run demo help-options lint format device-build device-lint device-deploy clean

# One target at a time (the device ones share build-device/); CMake parallelises the builds.
.NOTPARALLEL:

all: build

$(BUILD)/CMakeCache.txt:
	cmake -S . -B $(BUILD)

build: $(BUILD)/CMakeCache.txt
	cmake --build $(BUILD) --parallel

run: build
	$(BUILD)/muos-sendspin $(ARGS)

demo: build
	$(BUILD)/muos-sendspin --demo $(ARGS)

help-options: build
	$(BUILD)/muos-sendspin --help

lint: $(BUILD)/CMakeCache.txt
	BUILD_DIR=$(BUILD) scripts/lint.sh

format:
	scripts/lint.sh --fix

device-build:
	scripts/device.sh build

device-lint:
	scripts/device.sh lint

device-deploy: device-build
	scripts/device.sh deploy $(DEVICE)

clean:
	rm -rf $(BUILD) build-device dist
