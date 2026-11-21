# nwgba
#   make host     the PC version (build/nwgba-host)
CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -std=c11
BUILD = build
CORE = core/gba.c core/cpu.c core/mem.c core/ppu.c core/bios.c core/pack.c core/inflate.c core/save.c core/irqm.c
HEADERS = $(wildcard core/*.h)

.PHONY: all host clean
all: host
host: $(BUILD)/nwgba-host

$(BUILD)/nwgba-host: $(CORE) host/main.c $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -DGBA_STATS $(CORE) host/main.c -o $@

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

# ---- calculator app -------------------------------------------------------
#   make nwa       the app (build/nwgba.nwa), needs arm-none-eabi-gcc + node
#   make nwa-check links it as the installer does, shows the RAM it keeps
NWLINK ?= npx --yes -- nwlink@1.0.0
ARM_CC ?= arm-none-eabi-gcc
ARM_CFLAGS = -std=c11 -O2 -Wall -Wextra -DNWGBA_CALC
ARM_CFLAGS += $(shell $(NWLINK) eadk-cflags-device)
ARM_CFLAGS += -fdata-sections -ffunction-sections
ARM_CFLAGS += -fvisibility=internal
ARM_LDFLAGS = -Wl,--relocatable -nostartfiles --specs=nano.specs
ARM_LDFLAGS += -Wl,-e,main -Wl,-u,eadk_app_name -Wl,-u,eadk_app_icon -Wl,-u,eadk_api_level
ARM_LDFLAGS += -Wl,--gc-sections
APP_SRC = $(CORE) core/flash.c numworks/main.c

.PHONY: nwa nwa-check ramtest
nwa: $(BUILD)/nwgba.nwa

$(BUILD)/icon.o: numworks/icon.png | $(BUILD)
	$(NWLINK) png-icon-o $< $@

# The core is compiled as one piece (core/all.c): the compiler sees it whole,
# and the code that results misses the processor's small cache less often
# than what link-time optimisation made of the separate files.
# With counts of what really runs (pgo/all.gcda, see tools/pgo.sh), the
# compiler keeps the rare parts of each function out of the way.
PGO_FLAGS = $(if $(wildcard pgo/all.gcda),-fbranch-probabilities -Wno-missing-profile -Wno-error=coverage-mismatch)
$(BUILD)/nwa/all.o: core/all.c $(CORE) $(HEADERS) $(wildcard pgo/all.gcda)
	@mkdir -p $(BUILD)/nwa
	@if [ -f pgo/all.gcda ]; then cp pgo/all.gcda $(BUILD)/nwa/all.gcda; fi
	$(ARM_CC) $(ARM_CFLAGS) $(PGO_FLAGS) -c $< -o $@

$(BUILD)/nwgba.nwa: $(BUILD)/nwa/all.o core/flash.c numworks/main.c numworks/app.ld $(HEADERS) $(BUILD)/icon.o
	$(ARM_CC) $(ARM_CFLAGS) $(ARM_LDFLAGS) -Wl,-T,numworks/app.ld $(BUILD)/nwa/all.o core/flash.c numworks/main.c $(BUILD)/icon.o -o $@

# a test app: can code run from RAM on the calculator, and what is it worth (numworks/ramtest.c)
ramtest: $(BUILD)/ramtest.nwa
$(BUILD)/ramtest.nwa: numworks/ramtest.c $(BUILD)/icon.o
	$(ARM_CC) $(ARM_CFLAGS) $(ARM_LDFLAGS) numworks/ramtest.c $(BUILD)/icon.o -o $@

nwa-check: $(BUILD)/nwgba.nwa
	printf 'none' > $(BUILD)/empty.pack
	$(NWLINK) nwa-elf --external-data $(BUILD)/empty.pack $< $(BUILD)/nwgba.elf
	arm-none-eabi-size -A $(BUILD)/nwgba.elf | grep -E "section|\.text|\.rodata|\.data|\.bss"

# ---- the calculator front end, run on the PC against stand-in EADK calls ----
#   make sim  ->  build/numworks-sim GAME.pack FRAMES OUTDIR [RAM-KB] [KEYS] [MS-PER-FRAME]
EADK_INC = $(shell $(NWLINK) eadk-cflags-device | sed 's/.*"-I\([^"]*\)".*/\1/')
.PHONY: sim
sim: $(BUILD)/numworks-sim
$(BUILD)/numworks-sim: $(CORE) core/flash.c numworks/main.c host/numworks_sim.c $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -DNWGBA_CALC -DNW_SIM -DPLATFORM_DEVICE=1 -Dmain=app_main -I"$(EADK_INC)" -c numworks/main.c -o $(BUILD)/sim_main.o
	$(CC) $(CFLAGS) -DNWGBA_CALC -DPLATFORM_DEVICE=1 -I"$(EADK_INC)" $(CORE) core/flash.c host/numworks_sim.c $(BUILD)/sim_main.o -o $@

# ---- instruction count on an emulated Cortex-M7 (see bench/main.c) --------
#   make bench ROM=game.gba [FRAMES=600] [SKIP=0] [KEYS="300:S 420:A"]
#   make profile ...the same... [DRAW=1] [FUNC=name]   where the instructions go
#     (tools/qprof, a QEMU plugin; FUNC: also list that function's machine code)
FRAMES ?= 600
SKIP ?= 0
KEYS ?=
BENCH_CFLAGS = -std=c11 -O2 -mthumb -mcpu=cortex-m7 -mfloat-abi=hard -mfpu=fpv5-sp-d16 \
  -fdata-sections -ffunction-sections -DNWGBA_CALC \
  -DBENCH_FRAMES=$(FRAMES) -DBENCH_SKIP=$(SKIP) '-DBENCH_KEYS="$(KEYS)"' $(BENCH_DEFS)
QEMU = qemu-system-arm -M mps2-an500 -cpu cortex-m7 -nographic -monitor none -serial none \
  -icount shift=0 -semihosting-config enable=on,target=native
.PHONY: bench bench-elf profile
BENCH_CPU = -std=c11 -O2 -g -mthumb -mcpu=cortex-m7 -mfloat-abi=hard -mfpu=fpv5-sp-d16 \
  -fdata-sections -ffunction-sections -DNWGBA_CALC
BENCH_OBJS = $(BUILD)/bench/all.o
$(BUILD)/bench/all.o: core/all.c $(CORE) $(HEADERS) $(wildcard pgo/all.gcda)
	@mkdir -p $(BUILD)/bench
	@if [ -f pgo/all.gcda ]; then cp pgo/all.gcda $(BUILD)/bench/all.gcda; fi
	$(ARM_CC) $(BENCH_CPU) $(PGO_FLAGS) -c $< -o $@
bench-elf: $(BENCH_OBJS) bench/main.c bench/startup.c bench/mps2.ld $(HEADERS) | $(BUILD)
	@test -n "$(ROM)" || (echo "usage: make bench ROM=game.gba"; exit 1)
	printf '.section .game,"a"\n.global bench_rom\n.balign 4\nbench_rom:\n.incbin "%s"\nbench_rom_end:\n.section .rodata\n.global bench_rom_size\n.balign 4\nbench_rom_size:\n.word bench_rom_end - bench_rom\n' "$(ROM)" > $(BUILD)/bench_rom.S
	$(ARM_CC) $(BENCH_CFLAGS) $(BENCH_OBJS) bench/main.c bench/startup.c $(BUILD)/bench_rom.S \
	  -nostartfiles --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections -T bench/mps2.ld -o $(BUILD)/bench.elf
bench: bench-elf
	$(QEMU) -kernel $(BUILD)/bench.elf
$(BUILD)/qprof.so: tools/qprof/qprof.c | $(BUILD)
	gcc -O2 -fPIC -shared -o $@ $<
profile: BENCH_DEFS += -DBENCH_ONE=$(if $(DRAW),1,0)
profile: bench-elf $(BUILD)/qprof.so
	$(QEMU) -plugin $(BUILD)/qprof.so,start=$$(arm-none-eabi-nm $(BUILD)/bench.elf | awk '/bench_mark_start/{print $$1}'),stop=$$(arm-none-eabi-nm $(BUILD)/bench.elf | awk '/bench_mark_stop/{print $$1}'),out=$(BUILD)/qprof.out \
	  -kernel $(BUILD)/bench.elf
	python3 tools/qprof/report.py $(BUILD)/bench.elf $(BUILD)/qprof.out $(FRAMES) $(FUNC)
