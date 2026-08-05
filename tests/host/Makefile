# Host test harness Makefile.
#
# Compiles host GCC or Clang ONLY. Refuses arm-none-eabi-*.
# Provides `test`, `test-sanitize` (ASan + UBSan), per-suite targets via
# the implicit `test_<name>` rule, and `test-fail-demo` to prove a
# deliberately failing assertion exits nonzero.
#
# No network access at test time. No external dependencies. Deterministic
# seed (override with `make SEED=0x12345 ...`).

# ---- Toolchain (host only) ----------------------------------------------
CC      ?= gcc
CXX     ?= g++
ifneq (,$(findstring arm-none-eabi,$(CC)))
$(error CC must be host GCC/Clang, not arm-none-eabi-gcc. Override with: make CC=gcc)
endif
ifneq (,$(findstring arm-none-eabi,$(CXX)))
$(error CXX must be host G++/Clang++, not arm-none-eabi-g++.)
endif

# ---- Compile flags ------------------------------------------------------
CWARN     := -Wall -Wextra -Wconversion -Wshadow -pedantic
CFLAGS    ?= -std=c11 -O2 -g $(CWARN)
CXXFLAGS  ?= -std=c++17 -O2 -g $(CWARN)
CPPFLAGS  += -I. -I../../src -I../../../whad-lib -I../../../whad-lib/inc \
	-I../../../whad-lib/nanopb -I../../../whad-lib/whad/protocol

# ASan + UBSan. -O1 keeps ASan stack traces readable.
SANITIZE_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g

# Compiler to use for sanitized builds. Clang is preferred because its
# ASan runtime is self-bootstrapping; GCC's dynamic libasan needs
# LD_PRELOAD on some glibc installs (notably Arch). Override with
# `make test-sanitize SANITIZE_CC=gcc LD_PRELOAD_ASAN=/usr/lib/libasan.so.8`.
SANITIZE_CC ?= $(shell command -v clang >/dev/null 2>&1 && echo clang || echo gcc)
LD_PRELOAD_ASAN ?=

# Deterministic seed (override on the command line for variation runs).
SEED ?= 0xC0FFEE
CPPFLAGS += -DTEST_SEED=$(SEED)

# ---- Sources ------------------------------------------------------------
MOCKS_DIR  := mocks
MOCK_SRCS  := $(wildcard $(MOCKS_DIR)/*.c)

TEST_SRCS  := $(filter-out test_framework.c,$(wildcard test_*.c))
TEST_BINS  := $(TEST_SRCS:.c=)
TEST_CXX   := $(wildcard test_*.cpp)
TEST_CBINS := $(TEST_CXX:.cpp=)

# Protocol round-trip test needs nanopb + generated protobuf + Board C API.
WHAD_DIR       := ../../../whad-lib
NANOPB_SRCS    := $(WHAD_DIR)/nanopb/pb_common.c \
                  $(WHAD_DIR)/nanopb/pb_encode.c \
                  $(WHAD_DIR)/nanopb/pb_decode.c
PROTO_SRCS     := $(WHAD_DIR)/whad/protocol/whad.pb.c \
                  $(WHAD_DIR)/whad/protocol/device.pb.c \
                  $(WHAD_DIR)/whad/protocol/generic.pb.c \
                  $(WHAD_DIR)/whad/protocol/ble/ble.pb.c \
                  $(WHAD_DIR)/whad/protocol/dot15d4/dot15d4.pb.c \
                  $(WHAD_DIR)/whad/protocol/esb/esb.pb.c \
                  $(WHAD_DIR)/whad/protocol/unifying/unifying.pb.c \
                  $(WHAD_DIR)/whad/protocol/phy/phy.pb.c \
                  $(WHAD_DIR)/whad/protocol/board/board.pb.c
BOARD_API_SRCS := $(WHAD_DIR)/src/domains/board.c

# Transport layer sources used by foundation protocol transport-boundary tests.
TRANSPORT_SRCS := $(WHAD_DIR)/src/transport.c \
                  $(WHAD_DIR)/src/ringbuf.c

# Wave 0-1 foundation test suite — the complete set of tests that validate
# the protocol, runtime, pin registry, message pool, and display foundations.
FOUNDATION_BINS := test_runtime test_pinreg test_timebase \
                   test_message_pool test_display test_neopixel \
                   test_menu test_foundation_protocol test_i2cBus \
                   test_sensors_env \
                   test_qspi test_ble_runtime \
                   test_sensors_imu \
                   test_bond \
                   test_hids \
                   test_qspi_journal \
                   test_motion \
		   test_expert \
		   test_expert_io \
		   test_profiles \
		   test_motion_board \
		   test_calib \
		   test_apds9960 \
		   test_pdm \
		   test_buzzer \
		   test_gatt \
		   test_security \
		   test_advertising

.PHONY: all test test-sanitize test-fail-demo foundation list clean

all: test

# Implicit rule: a binary "test_foo" is built from "test_foo.c".
%: %.c test_framework.c $(MOCK_SRCS)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< test_framework.c $(MOCK_SRCS) -o $@

# Implicit rule: a C++ host binary can link firmware C++ units under test.
%: %.cpp test_framework.c ../../src/messagePool.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $< test_framework.c \
		../../src/messagePool.cpp -o $@

# Implicit rule: a binary "test_foo" is built from "test_foo.cpp".
%: %.cpp test_framework.c $(MOCK_SRCS)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $< test_framework.c $(MOCK_SRCS) -o $@

# Explicit rule: protocol round-trip test links nanopb + proto + Board API.
# C sources compiled with CC (avoids whad.h pulling cpp/whad.hpp in C++ mode).
# Overrides the pattern rules above because make prefers explicit targets.
PROTOCOL_OBJS := pb_common.o pb_encode.o pb_decode.o whad.pb.o device.pb.o \
                 generic.pb.o ble.pb.o dot15d4.pb.o esb.pb.o unifying.pb.o \
                 phy.pb.o board.pb.o board.o transport.o ringbuf.o \
                 test_framework.o

test_foundation_protocol: test_foundation_protocol.cpp test_framework.c \
                          $(NANOPB_SRCS) $(PROTO_SRCS) $(BOARD_API_SRCS) \
                          $(TRANSPORT_SRCS)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $(NANOPB_SRCS) $(PROTO_SRCS) \
		$(BOARD_API_SRCS) $(TRANSPORT_SRCS) test_framework.c
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $< $(PROTOCOL_OBJS) -o $@
	@rm -f $(PROTOCOL_OBJS)

# BLE runtime test links ble_eval.c (pure-C, no SDK deps).
test_ble_runtime: test_ble_runtime.cpp test_framework.c \
                  ../../src/ble/ble_eval.c
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -I../../src/ble $< test_framework.c \
		../../src/ble/ble_eval.c -o $@

# GATT test links gatt_eval.c + ble_eval.c (pure-C, no SDK deps).
test_gatt: test_gatt.cpp test_framework.c \
           ../../src/ble/gatt_eval.c ../../src/ble/ble_eval.c
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -I../../src/ble $< test_framework.c \
		../../src/ble/gatt_eval.c ../../src/ble/ble_eval.c -o $@

# Security test links security_eval.c + ble_eval.c (pure-C, no SDK deps).
test_security: test_security.cpp test_framework.c \
               ../../src/ble/security_eval.c ../../src/ble/ble_eval.c
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -I../../src/ble $< test_framework.c \
		../../src/ble/security_eval.c ../../src/ble/ble_eval.c -o $@

# Advertising test links ble_eval.c (pure-C, no SDK deps).
test_advertising: test_advertising.cpp test_framework.c \
                  ../../src/ble/ble_eval.c
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -I../../src/ble $< test_framework.c \
		../../src/ble/ble_eval.c -o $@

# QSPI storage test links mock_flash.c for NOR semantics.
test_qspi: test_qspi.cpp test_framework.c mocks/mock_flash.c
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $< test_framework.c mocks/mock_flash.c -o $@

# QSPI journal test links mock_flash.c for NOR fault injection.
test_qspi_journal: test_qspi_journal.cpp test_framework.c mocks/mock_flash.c
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $< test_framework.c mocks/mock_flash.c -o $@

# Bond storage test links bond_eval.c (pure-C, no SDK deps).
test_bond: test_bond.cpp test_framework.c \
           ../../src/ble/bond_eval.c
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -I../../src/ble $< test_framework.c \
		../../src/ble/bond_eval.c -o $@

# HIDS test links hids_eval.c (pure-C, no SDK deps).
test_hids: test_hids.cpp test_framework.c \
           ../../src/ble/hids_eval.c
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -I../../src/ble $< test_framework.c \
		../../src/ble/hids_eval.c -o $@

# Motion test includes motion_eval.c directly (pure-C, no SDK deps).
test_motion: test_motion.cpp test_framework.c
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $< test_framework.c -o $@

# Expert GPIO/SAADC test links expert_eval.c (pure-C, no SDK deps).
test_expert: test_expert.cpp test_framework.c \
             ../../src/expert/expert_eval.c
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -I../../src/expert $< test_framework.c \
		../../src/expert/expert_eval.c -o $@

# Expert I2C/SPI test links io_eval.c + expert_force.c (pure-C, no SDK deps).
test_expert_io: test_expert_io.cpp test_framework.c \
                ../../src/expert/io_eval.c ../../src/expert/expert_force.c
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -I../../src/expert $< test_framework.c \
		../../src/expert/io_eval.c ../../src/expert/expert_force.c -o $@

# HID profiles test links profiles_eval.c (pure-C, no SDK deps).
test_profiles: test_profiles.cpp test_framework.c \
               ../../src/ble/profiles_eval.c ../../src/ble/hids_eval.c
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -I../../src/ble $< test_framework.c \
		../../src/ble/profiles_eval.c ../../src/ble/hids_eval.c -o $@

# Motion board dispatch test includes boardMotionEval.c directly (pure-C).
test_motion_board: test_motion_board.cpp test_framework.c
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $< test_framework.c -o $@

# PDM audio test includes pdm_eval.c directly (pure-C, no SDK deps).
test_pdm: test_pdm.cpp test_framework.c
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -I../../src/audio $< test_framework.c -o $@

# Buzzer output test includes buzzer_eval.c directly (pure-C, no SDK deps).
test_buzzer: test_buzzer.cpp test_framework.c
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -I../../src/output $< test_framework.c -o $@

# APDS-9960 test includes apds9960.cpp and boardMotionEval.c directly.
test_apds9960: test_apds9960.cpp test_framework.c
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $< test_framework.c -o $@

# Calibration persistence test links calib_eval.c + journal eval via mock_flash.
test_calib: test_calib.cpp test_framework.c mocks/mock_flash.c
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -I../../src/storage $< test_framework.c mocks/mock_flash.c -o $@

# Run every test_*.c binary. Fails fast on first failure.
test: $(TEST_BINS) $(TEST_CBINS)
	@echo "=== Running host tests (seed=$(SEED)) ==="
	@rc=0; \
	for bin in $(TEST_BINS) $(TEST_CBINS); do \
		echo "[RUN] $$bin"; \
		./"$$bin" || { echo "FAIL in $$bin"; rc=1; break; }; \
	done; \
	if [ $$rc -eq 0 ]; then echo "=== All host tests passed ==="; fi; \
	exit $$rc

# Wave 0-1 foundation test suite: runs all protocol, runtime, pin registry,
# message pool, and display tests. Reports which suite failed by name.
foundation: $(FOUNDATION_BINS)
	@echo "=== Running foundation test suite (seed=$(SEED)) ==="
	@rc=0; \
	for bin in $(FOUNDATION_BINS); do \
		echo "[RUN] $$bin"; \
		./"$$bin" || { echo "FAIL in $$bin"; rc=1; break; }; \
	done; \
	if [ $$rc -eq 0 ]; then echo "=== All foundation tests passed ==="; fi; \
	exit $$rc

# Same suite under ASan/UBSan. Builds .asan binaries so the normal
# `test` target and the sanitize target can coexist in one worktree.
test-sanitize:
	@echo "=== Running host tests with ASan/UBSan (seed=$(SEED), CC=$(SANITIZE_CC)) ==="
	@rc=0; \
	for src in $(TEST_SRCS); do \
		bin=$${src%.c}.asan; \
		echo "[CC-ASAN] $$src"; \
		$(SANITIZE_CC) $(CFLAGS) $(SANITIZE_FLAGS) $(CPPFLAGS) \
			$$src test_framework.c $(MOCK_SRCS) -o $$bin \
			|| { rc=1; break; }; \
		echo "[RUN-ASAN] $$bin"; \
		$(if $(LD_PRELOAD_ASAN),LD_PRELOAD=$(LD_PRELOAD_ASAN)) \
			./"$$bin" || { echo "FAIL in $$bin"; rc=1; break; }; \
	done; \
	if [ $$rc -eq 0 ]; then echo "=== All ASan/UBSan tests passed ==="; fi; \
	exit $$rc

# Build test_smoke with -DRUN_FAILING_TEST and verify nonzero exit.
test-fail-demo:
	@echo "=== Building test_smoke with -DRUN_FAILING_TEST ==="
	@$(CC) $(CFLAGS) $(CPPFLAGS) -DRUN_FAILING_TEST \
		test_smoke.c test_framework.c $(MOCK_SRCS) \
		-o test_smoke_failing || exit 1
	@echo "=== Running (expect nonzero exit) ==="
	@./test_smoke_failing; rc=$$?; \
	if [ $$rc -ne 0 ]; then \
		echo "OK: deliberately failing test exited $$rc (nonzero as expected)"; \
	else \
		echo "FAIL: failing test should have exited nonzero, got 0"; \
		exit 1; \
	fi

list:
	@echo "Test sources:"; printf '  %s\n' $(TEST_SRCS) $(TEST_CXX)
	@echo "Mock sources:"; printf '  %s\n' $(MOCK_SRCS)

clean:
	rm -f $(TEST_BINS) $(TEST_CBINS) *.asan test_smoke_failing
