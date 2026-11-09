#!/bin/bash
set -e

MODS_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$MODS_DIR"

CC=arm-linux-gnueabi-gcc
CFLAGS="-O2 -fPIC -march=armv7-a -mfloat-abi=softfp -mfpu=vfpv3 -Wall -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function -D_GNU_SOURCE"
INCLUDES="-I. -Icore -Icore/api -Icore/services -Icore/runtime -Icore/ui -Icore/firmware"

SOURCES=(
    core/rx3_core_hook.c
    core/firmware/rx3_patch.c
    core/runtime/rx3_composition.c
    core/runtime/rx3_modules.c
    core/services/rx3_audio.c
    core/services/rx3_browse.c
    core/services/rx3_dsp.c
    core/services/rx3_hooks.c
    core/services/rx3_images.c
    core/services/rx3_input.c
    core/services/rx3_loader.c
    core/services/rx3_log.c
    core/services/rx3_memory.c
    core/services/rx3_mix_state.c
    core/services/rx3_notice.c
    core/services/rx3_panels.c
    core/services/rx3_titles.c
    logo/rx3_logo_module.c
    theme-white/rx3_theme_module.c
    stems/rx3_stems_module.c
    stems/rx3_stemwave_module.c
    samples/rx3_samples_module.c
    key-match/rx3_key_match_module.c
    browse-columns/rx3_browse_columns_module.c
    keyshift/rx3_keyshift_module.c
    search-latin/rx3_search_module.c
    now-playing/rx3_now_playing_module.c
    asshole-mode/rx3_asshole_module.c
    beatjump-custom/rx3_beatjump_custom_module.c
    colorfx-dual/rx3_colorfx_dual_module.c
    rmx1000-roll/rx3_rmx_roll_module.c
)

echo "Compiling librx3_core.so..."
$CC -shared $CFLAGS $INCLUDES "${SOURCES[@]}" -o librx3_core.so -nostdlib -lgcc

echo "Build successful! librx3_core.so:"
ls -lh librx3_core.so

