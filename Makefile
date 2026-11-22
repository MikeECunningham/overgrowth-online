PROCS ?= $(shell nproc)
BUILD_DIR ?= Build
GAME_BINARY ?= $(abspath $(BUILD_DIR))/OvergrowthOnline.bin.x86_64
CMAKE_ARGS ?=

.PHONY: default setup setup-release setup-telemetry setup-debug setup-clang-release activate-ogda deactivate-ogda build run debug test clean

default: build
setup:
	cmake -S Projects -B "$(BUILD_DIR)" -DCMAKE_BUILD_TYPE=Release -DBUILD_OGDA=OFF $(CMAKE_ARGS)
setup-release:
	cmake -S Projects -B "$(BUILD_DIR)" -DCMAKE_BUILD_TYPE=Release -DENABLE_RAD_TELEMETRY=OFF -DBUILD_OGDA=OFF $(CMAKE_ARGS)

setup-telemetry:
	cmake -S Projects -B "$(BUILD_DIR)" -DCMAKE_BUILD_TYPE=Release -DENABLE_RAD_TELEMETRY=ON -DBUILD_OGDA=OFF $(CMAKE_ARGS)

setup-debug:
	cmake -S Projects -B "$(BUILD_DIR)" -DCMAKE_BUILD_TYPE=Debug -DENABLE_RAD_TELEMETRY=OFF -DBUILD_OGDA=OFF $(CMAKE_ARGS)

setup-clang-release:
	cmake -S Projects -B "$(BUILD_DIR)" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DBUILD_OGDA=OFF $(CMAKE_ARGS)

activate-ogda:
	cmake -S Projects -B "$(BUILD_DIR)" -DBUILD_OGDA=ON $(CMAKE_ARGS)

deactivate-ogda:
	cmake -S Projects -B "$(BUILD_DIR)" -DBUILD_OGDA=OFF $(CMAKE_ARGS)

build: setup
	cmake --build "$(BUILD_DIR)" --parallel $(PROCS)
run:
	"$(GAME_BINARY)" --working-dir "$(CURDIR)/"

debug:
	cgdb "$(GAME_BINARY)"

test: 
	"$(GAME_BINARY)" --run-unit-tests --no-dialogues
clean:
	cmake --build "$(BUILD_DIR)" --target clean
