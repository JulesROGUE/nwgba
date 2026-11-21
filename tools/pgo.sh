#!/bin/sh
# nwgba - telling the compiler what really runs.
#
# The calculator's processor keeps 8 KB of code at hand; a line of code that
# is not there is read again from the flash chip, at the price of two
# hundred instructions. What runs at every frame is about 6 KB, but mixed
# with what almost never runs (the slow ways out of each instruction), it
# is spread over twice that. The compiler can put the rare parts aside if
# it knows which they are: this script plays games on the simulated
# processor with counting code added, and keeps the counts (pgo/all.gcda);
# `make nwa` and `make bench` then build with them.
#
#   tools/pgo.sh train NAME GAME FRAMES SKIP "KEYS"   one training run
#   tools/pgo.sh merge                                all runs -> pgo/all.gcda
#
# The counts go with the source: after a change to core/*.c, train again
# (functions that changed are otherwise compiled without their counts).
set -e
cd "$(dirname "$0")/.."
CC=arm-none-eabi-gcc
CPU="-std=c11 -O2 -mthumb -mcpu=cortex-m7 -mfloat-abi=hard -mfpu=fpv5-sp-d16 -DNWGBA_CALC"
D=build/pgo
case "$1" in
train)
  name=$2; rom=$3; frames=$4; skip=$5; keys=$6
  mkdir -p $D/gen
  $CC $CPU -fprofile-arcs -fprofile-info-section -c core/all.c -o $D/gen/all.o
  printf '.section .game,"a"\n.global bench_rom\n.balign 4\nbench_rom:\n.incbin "%s"\nbench_rom_end:\n.section .rodata\n.global bench_rom_size\n.balign 4\nbench_rom_size:\n.word bench_rom_end - bench_rom\n' "$rom" > $D/rom.S
  $CC $CPU -DBENCH_PGO -DBENCH_RAM_KB=127 -DBENCH_FRAMES=$frames -DBENCH_SKIP=$skip "-DBENCH_KEYS=\"$keys\"" \
    bench/main.c bench/startup.c $D/gen/all.o $D/rom.S \
    -nostartfiles --specs=nano.specs --specs=nosys.specs -T bench/mps2.ld -lgcov -o $D/train.elf
  rm -f $D/gen/*.gcda
  qemu-system-arm -M mps2-an500 -cpu cortex-m7 -nographic -monitor none -serial none \
    -icount shift=0 -semihosting-config enable=on,target=native -kernel $D/train.elf
  mkdir -p $D/runs/$name
  mv $D/gen/all.gcda $D/runs/$name/
  ;;
merge)
  rm -rf $D/all && mkdir -p $D/all pgo
  first=1
  for r in $D/runs/*/; do
    if [ $first = 1 ]; then cp ${r}all.gcda $D/all/; first=0
    else rm -rf $D/tmp && arm-none-eabi-gcov-tool merge $D/all $r -o $D/tmp && cp $D/tmp/all.gcda $D/all/; fi
  done
  cp $D/all/all.gcda pgo/
  ls -la pgo/all.gcda
  ;;
*) sed -n 2,20p "$0"; exit 2;;
esac
