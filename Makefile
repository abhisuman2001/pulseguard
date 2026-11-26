# PulseGuard build
#
#   make qnx                 cross-compile for QNX 8.0 x86_64 (QEMU)   [needs qnxsdp-env.sh]
#   make qnx ARCH=aarch64le  cross-compile for QNX 8.0 aarch64 (e.g. Raspberry Pi 4)
#   make test                build + run host unit tests
#   make host                host tools: unit tests, simgen, emulator
#   make model               regenerate core/pg_model_weights.h from simulation
#   make clean

ARCH      ?= x86_64
HOSTCC    ?= cc
QCC       ?= qcc
PYTHON    ?= python3

CORE_SRC  := core/pg_core.c core/pg_telemetry.c
INC       := -Iinclude -Icore
WARN      := -Wall -Wextra -Wno-unused-parameter
HOST_CFLAGS := -O2 -std=c11 -D_GNU_SOURCE $(WARN) $(INC)

QNX_OUT   := build/qnx-$(ARCH)
QNX_CFLAGS:= -V gcc_nto$(ARCH) -O2 -std=gnu11 $(WARN) $(INC) -D_QNX_SOURCE
QNX_LIBS  := -lm -lslog2

QNX_BINS  := pg_sensord pg_controld pg_aiguard pg_watchdogd pg_telemetryd pg_stressor

.PHONY: all qnx host test model clean check-qnx

all: host

# ----------------------------------------------------------------- QNX
check-qnx:
	@if [ -z "$$QNX_HOST" ] || [ -z "$$QNX_TARGET" ]; then \
	  echo "QNX SDP environment not set. Run:  source ~/qnx800/qnxsdp-env.sh"; exit 1; fi

qnx: check-qnx $(addprefix $(QNX_OUT)/,$(QNX_BINS))
	@echo "QNX 8.0 $(ARCH) binaries in $(QNX_OUT)/"
	@file $(QNX_OUT)/pg_* 2>/dev/null || true

$(QNX_OUT)/pg_%: src/%/*.c src/common/pg_rt.c $(CORE_SRC) include/*.h core/*.h | $(QNX_OUT)
	$(QCC) $(QNX_CFLAGS) -o $@ src/$*/$*.c src/common/pg_rt.c $(CORE_SRC) $(QNX_LIBS) \
	    $(if $(filter telemetryd,$*),-lsocket,)

$(QNX_OUT):
	mkdir -p $@

# ----------------------------------------------------------------- host
host: build/host/test_core build/host/simgen build/host/pg_emulator

build/host:
	mkdir -p $@

build/host/test_core: tests/test_core.c $(CORE_SRC) core/*.h include/*.h | build/host
	$(HOSTCC) $(HOST_CFLAGS) -o $@ tests/test_core.c $(CORE_SRC) -lm

build/host/test_telemetry: tests/test_telemetry.c $(CORE_SRC) core/*.h include/*.h | build/host
	$(HOSTCC) $(HOST_CFLAGS) -o $@ tests/test_telemetry.c $(CORE_SRC) -lm

build/host/simgen: tools/simgen.c $(CORE_SRC) core/*.h | build/host
	$(HOSTCC) $(HOST_CFLAGS) -o $@ tools/simgen.c $(CORE_SRC) -lm

build/host/pg_emulator: tools/pg_emulator.c $(CORE_SRC) core/*.h include/*.h | build/host
	$(HOSTCC) $(HOST_CFLAGS) -o $@ tools/pg_emulator.c $(CORE_SRC) -lm -lpthread

test: build/host/test_core build/host/test_telemetry
	./build/host/test_core
	./build/host/test_telemetry

model: build/host/simgen
	./build/host/simgen 300 1  > build/host/train.csv
	./build/host/simgen 100 99 > build/host/test.csv
	$(PYTHON) tools/train_model.py build/host/train.csv build/host/test.csv core/pg_model_weights.h

clean:
	rm -rf build

# ----------------------------------------------------------------- benchmark
build/host/rtbench: tools/rtbench.c | build/host
	$(HOSTCC) -O2 -Wall -o $@ tools/rtbench.c -lm -lpthread

$(QNX_OUT)/rtbench: tools/rtbench.c | $(QNX_OUT)
	$(QCC) -V gcc_nto$(ARCH) -O2 -Wall -o $@ tools/rtbench.c -lm

qnx: $(QNX_OUT)/rtbench
host: build/host/rtbench
