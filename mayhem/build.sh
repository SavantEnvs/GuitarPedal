#!/usr/bin/env bash
#
# mayhem/build.sh — build the fuzz harnesses, their standalone reproducers and the test suite.
#
# GuitarPedal is firmware (RP2354 + nRF54) whose pico-sdk/zephyr builds cannot run here. The fuzzed
# code is its header-only, host-compilable parts: nRF54/app/src/link.h (+ uartrx.h) and
# Firmware/midi/midi.h. They are compiled into each harness with the sanitizer, DWARF-3 and SanCov flags,
# so the fuzzed code is itself instrumented. Everything lands under build/ (gitignored upstream).
set -euo pipefail

[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
export SANITIZER_FLAGS DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

cd "$SRC"
OUT="$SRC/build"
mkdir -p "$OUT/obj" "$OUT/tests"

# SanCov on the fuzzed headers, unconditionally (also with SANITIZER_FLAGS empty)
FUZZ_CFLAGS="$SANITIZER_FLAGS $DEBUG_FLAGS -fsanitize=fuzzer-no-link -O1 -I$SRC"

# LSan off for every fuzz/standalone binary (mayhem/lsan_off.cc)
$CXX $SANITIZER_FLAGS $DEBUG_FLAGS -c "$SRC/mayhem/lsan_off.cc" -o "$OUT/obj/lsan_off.o"
$CC $SANITIZER_FLAGS $DEBUG_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o "$OUT/obj/standalone_main.o"

# fuzz_audio is the Validation/bench host build of the audio core: it needs the generated effect map and
# math tables (the same generators the Validation Makefile uses), rebuilt here so a clean tree still builds.
G="$OUT/gen"
mkdir -p "$G"
python3 scripts/gen_effects.py Effects/ "$G/effect_map.h" "$G/effects.js" "$G/dummy_map.md"
python3 scripts/pow2.py "$G/pow2.h" 8
python3 scripts/log2.py "$G/log2.h" 8
python3 scripts/quarter_sine.py "$G/quarter_sine.h" 8
python3 scripts/rat_clamp.py Effects/rat.h -o "$G/rat_clamp.h"
# the shims must come before -I$SRC, or Audio/cycles.h is the real DWT one
AUDIO_CFLAGS="-ffast-math -DSAMPLES_PER_SEC=48000.0f -Wno-unused-function -I$SRC/Validation/bench/shim -I$G -I$SRC/Firmware -I$SRC/Audio"

for t in fuzz_link fuzz_midi_parser fuzz_audio; do
  case $t in fuzz_audio) EXTRA="$AUDIO_CFLAGS"; XLIBS="-lm" ;; *) EXTRA=""; XLIBS="" ;; esac
  $CC $EXTRA $FUZZ_CFLAGS -c "$SRC/mayhem/$t.c" -o "$OUT/obj/$t.o"
  $CC $SANITIZER_FLAGS $DEBUG_FLAGS $LIB_FUZZING_ENGINE "$OUT/obj/$t.o" "$OUT/obj/lsan_off.o" $XLIBS -o "$OUT/$t"
  $CC $SANITIZER_FLAGS $DEBUG_FLAGS "$OUT/obj/$t.o" "$OUT/obj/standalone_main.o" "$OUT/obj/lsan_off.o" $XLIBS -o "$OUT/$t-standalone"
done

# The project's own self-checking tests (Validation/), with its NORMAL flags, plus our known-answer probe.
V="$SRC/Validation"
TFLAGS="-Wall -O2 -I$SRC -I$SRC/Firmware -I$SRC/Firmware/midi $COVERAGE_FLAGS"
$CC $TFLAGS -o "$OUT/tests/test-link"      "$V/test-link.c"
$CC $TFLAGS -o "$OUT/tests/test-uartrx"    "$V/test-uartrx.c"
$CC $TFLAGS -o "$OUT/tests/test-midi-cin"  "$V/test-midi-cin.c"
$CC $TFLAGS -o "$OUT/tests/kat"            "$SRC/mayhem/kat.c"
