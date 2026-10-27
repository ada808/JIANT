#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 hahahaudio
# Co-Operator's Vave module: JIANT as a sound module (web/coop/vave_coop.c), compiled to WebAssembly as the browser
# emulator is (web/emu/build.sh), with its worklet and the catalogue of its engines and presets.
#   web/coop/build.sh   ->  build/coop/{vave.wasm, vave-worklet.js, vave-catalog.json, LICENSE, SOURCE.txt}
# Run after one generation of build/gen (tools/build.py's generate()). 4 MiB of memory a module (COOP_MEM): the firmware
# allocates nothing; its RAM, the 1 MiB flash image and a 1 MiB stack fit, measured over every preset.
set -e
cd "$(dirname "$0")/../.."
[ -f build/gen/felucca_tables.h ] || { echo "coop: generate build/gen first"; exit 1; }
command -v emcc >/dev/null 2>&1 || { echo "coop: no emcc (Emscripten)"; exit 1; }
VERSION=$(sed -n 's/^#define FELUCCA_VERSION "\([^"]*\)".*/\1/p' firmware/src/felucca.c)
mkdir -p build/coop
# shellcheck disable=SC2086
emcc ${COOP_OPT:--O2} -ffp-contract=off -std=gnu99 -w ${COOP_CFLAGS:-} \
    "-DFELUCCA_VERSION=\"$VERSION\"" -Ibuild/gen -Ifirmware/src \
    --no-entry -sSTANDALONE_WASM -sSTACK_SIZE=1048576 -sINITIAL_MEMORY=${COOP_MEM:-4194304} -sFILESYSTEM=0 \
    -o build/coop/vave.wasm web/coop/vave_coop.c
cp web/coop/vave-worklet.js LICENSE build/coop/
node web/coop/catalog.mjs build/coop/vave.wasm > build/coop/vave-catalog.json
printf '%s\n' "Co-Operator's Vave module: JIANT FM $VERSION (GPL-3.0-only) as a sound module." \
  "Its complete source: https://github.com/ada808/JIANT/tree/coop-vave (web/coop/, web/coop/build.sh)." \
  "JIANT: https://github.com/juanjiant-bit/JIANT. Licences: LICENSE (GPL-3.0) and the repository's LICENSING.md." > build/coop/SOURCE.txt
echo "coop: build/coop ($(wc -c < build/coop/vave.wasm | tr -d ' ') B wasm, $VERSION)"
