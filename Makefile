# nwgba
#   make host     the PC version (build/nwgba-host)
CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -std=c11
BUILD = build
CORE = core/gba.c core/cpu.c core/mem.c core/ppu.c core/bios.c
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

# ---- instruction count on an emulated Cortex-M7 (see bench/main.c) --------
#   make bench ROM=game.gba [FRAMES=600] [SKIP=0] [KEYS="300:S 420:A"]
ARM_CC ?= arm-none-eabi-gcc
FRAMES ?= 600
SKIP ?= 0
KEYS ?=
BENCH_CFLAGS = -std=c11 -O2 -mthumb -mcpu=cortex-m7 -mfloat-abi=hard -mfpu=fpv5-sp-d16 \
  -fdata-sections -ffunction-sections -flto -DNWGBA_CALC \
  -DBENCH_FRAMES=$(FRAMES) -DBENCH_SKIP=$(SKIP) '-DBENCH_KEYS="$(KEYS)"' $(BENCH_DEFS)
.PHONY: bench
bench: $(CORE) bench/main.c bench/startup.c bench/mps2.ld $(HEADERS) | $(BUILD)
	@test -n "$(ROM)" || (echo "usage: make bench ROM=game.gba"; exit 1)
	printf '.section .game,"a"\n.global bench_rom\n.balign 4\nbench_rom:\n.incbin "%s"\nbench_rom_end:\n.section .rodata\n.global bench_rom_size\n.balign 4\nbench_rom_size:\n.word bench_rom_end - bench_rom\n' "$(ROM)" > $(BUILD)/bench_rom.S
	$(ARM_CC) $(BENCH_CFLAGS) $(CORE) bench/main.c bench/startup.c $(BUILD)/bench_rom.S \
	  -nostartfiles --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections -T bench/mps2.ld -o $(BUILD)/bench.elf
	qemu-system-arm -M mps2-an500 -cpu cortex-m7 -nographic -monitor none -serial none \
	  -icount shift=0 -semihosting-config enable=on,target=native -kernel $(BUILD)/bench.elf
