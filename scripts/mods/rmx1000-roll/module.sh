#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# RMX-1000 style Roll on the MASTER bus. The code lives in the performance core's shared
# object; this module reads its configuration, exports it and says where the drum sounds are.
# The configuration is plain data, read line by line and never sourced as shell code.

module_begin rmx1000-roll rmx1000_roll

RMX1000_ROLL_READY=0
# The sounds this module ships (copied to the image with the module) and the link to the
# drive's own kit, made the same way as the Snare Roll's link.
RMX1000_ROLL_BUILT_IN=/mnt/iso/modules/rmx1000-roll
RMX1000_ROLL_LINK=/tmp/rx3-rmx-roll

# key=value lines; comments, blank lines and unknown keys are skipped, and a
# bad value keeps what was there before.
rmx1000_roll_number()
{
    # $1 value, $2 maximum digits, $3 limit; prints the value or nothing.
    case "$1" in
        ''|*[!0-9]*) return 0 ;;
    esac
    [ ${#1} -le "$2" ] && [ "$1" -le "$3" ] && printf '%s' "$1"
    return 0
}

rmx1000_roll_read()
{
    [ -r "$1" ] || return 0
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in
            mode=aux|mode=always|mode=off) RMX1000_ROLL_MODE=${line#mode=} ;;
            pads=off|pads=beat-loop|pads=beat-loop-2|pads=slip-loop) RMX1000_ROLL_PADS=${line#pads=} ;;
            pad-lights=on|pad-lights=off) RMX1000_ROLL_PAD_LIGHTS=${line#pad-lights=} ;;
            panel=on|panel=off) RMX1000_ROLL_PANEL=${line#panel=} ;;
            portable-raw=*) value=$(rmx1000_roll_number "${line#portable-raw=}" 4 1023); [ -n "$value" ] && RMX1000_ROLL_PORTABLE_RAW=$value ;;
            trace=on|trace=off) RMX1000_ROLL_TRACE=${line#trace=} ;;
            trace=*) value=$(rmx1000_roll_number "${line#trace=}" 5 65535); [ -n "$value" ] && RMX1000_ROLL_TRACE=$value ;;
            capture=after|capture=before) RMX1000_ROLL_CAPTURE=${line#capture=} ;;
            in-ms=*) value=$(rmx1000_roll_number "${line#in-ms=}" 3 200); [ -n "$value" ] && RMX1000_ROLL_IN_MS=$value ;;
            out-ms=*) value=$(rmx1000_roll_number "${line#out-ms=}" 4 2000); [ -n "$value" ] && RMX1000_ROLL_OUT_MS=$value ;;
            length=*) value=$(rmx1000_roll_number "${line#length=}" 1 9); [ -n "$value" ] && RMX1000_ROLL_LENGTH=$value ;;
            tempo=auto|tempo=deck) RMX1000_ROLL_TEMPO=${line#tempo=} ;;
            bpm=*) value=$(rmx1000_roll_number "${line#bpm=}" 3 300); [ -n "$value" ] && [ "$value" -ge 40 ] && RMX1000_ROLL_BPM=$value ;;
        esac
    done < "$1"
}

# The four drum sounds. The drive's own kit (RX3_RUNTIME/roll) wins only when all four files are
# there; otherwise the sounds this module ships are used. RX3_RMX_SOUNDS names the folder either way,
# so the Snare Roll module is not needed for them.
rmx1000_roll_sounds()
{
    rmx1000_roll_complete=1
    for rmx1000_roll_name in kick snare clap hat; do
        [ -r "$USB/RX3_RUNTIME/roll/$rmx1000_roll_name.wav" ] || rmx1000_roll_complete=0
    done
    if [ "$rmx1000_roll_complete" = 1 ]; then
        stage_runtime_symlink "$USB/RX3_RUNTIME/roll" "$RMX1000_ROLL_LINK" || return 1
        rmx1000_roll_folder=$RMX1000_ROLL_LINK
        say "RMX-1000 Roll sounds: the drive's own kit, RX3_RUNTIME/roll"
    else
        if [ -L "$RMX1000_ROLL_LINK" ]; then
            stage_runtime_removal "$RMX1000_ROLL_LINK" || return 1
        fi
        rmx1000_roll_folder=$RMX1000_ROLL_BUILT_IN
    fi
    module_export RX3_RMX_SOUNDS "$rmx1000_roll_folder" "RMX-1000 Roll sounds" || :
}

rmx1000_roll_prepare()
{
    [ -r "$CORE_OBJECT" ] || {
        say "RMX-1000 Roll disabled: the performance core is not selected"
        return 0
    }
    module_disabled_by_switch rmx1000-roll && return 0
    RMX1000_ROLL_MODE=aux
    RMX1000_ROLL_PORTABLE_RAW=2
    RMX1000_ROLL_PADS=beat-loop
    RMX1000_ROLL_PAD_LIGHTS=on
    RMX1000_ROLL_PANEL=on
    RMX1000_ROLL_TRACE=off
    RMX1000_ROLL_CAPTURE=after
    RMX1000_ROLL_BEAT_KEYS=normal
    RMX1000_ROLL_IN_MS=3
    RMX1000_ROLL_OUT_MS=8
    RMX1000_ROLL_LENGTH=5
    RMX1000_ROLL_BPM=120
    RMX1000_ROLL_TEMPO=auto
    # The image's defaults first, then the drive, which the operator can
    # edit without building the mod again.
    rmx1000_roll_read /mnt/iso/modules/rmx1000-roll/config.txt
    rmx1000_roll_read "$USB/RX3_RUNTIME/rmx1000-roll.txt"
    module_export RX3_RMX_ROLL 1 "RMX-1000 Roll" || :
    module_export RX3_RMX_ROLL_MODE "$RMX1000_ROLL_MODE" "RMX-1000 Roll mode" || :
    module_export RX3_RMX_ROLL_CAPTURE "$RMX1000_ROLL_CAPTURE" "RMX-1000 Roll capture" || :
    module_export RX3_RMX_ROLL_IN_MS "$RMX1000_ROLL_IN_MS" "RMX-1000 Roll fade in" || :
    module_export RX3_RMX_ROLL_OUT_MS "$RMX1000_ROLL_OUT_MS" "RMX-1000 Roll fade out" || :
    module_export RX3_RMX_ROLL_LENGTH "$RMX1000_ROLL_LENGTH" "RMX-1000 Roll first length" || :
    module_export RX3_RMX_ROLL_BPM "$RMX1000_ROLL_BPM" "RMX-1000 Roll fallback tempo" || :
    module_export RX3_RMX_ROLL_TEMPO "$RMX1000_ROLL_TEMPO" "RMX-1000 Roll tempo source" || :
    module_export RX3_RMX_ROLL_PORTABLE_RAW "$RMX1000_ROLL_PORTABLE_RAW" "RMX-1000 Roll AUX PORTABLE raw" || :
    module_export RX3_RMX_ROLL_PADS "$RMX1000_ROLL_PADS" "RMX-1000 Roll pads" || :
    lights=1
    [ "$RMX1000_ROLL_PAD_LIGHTS" = off ] && lights=0
    module_export RX3_RMX_ROLL_PAD_LIGHTS "$lights" "RMX-1000 Roll pad lights" || :
    panel=1
    [ "$RMX1000_ROLL_PANEL" = off ] && panel=0
    module_export RX3_RMX_ROLL_PANEL "$panel" "RMX-1000 Roll screen row" || :
    # 17417 = 0x4409, the AUX LINE/PORTABLE switch, read by the core's key trace (docs/TEST_SU_RX3.md, 8).
    case "$RMX1000_ROLL_TRACE" in
        on) trace=17417 ;;
        off) trace= ;;
        *) trace=$RMX1000_ROLL_TRACE ;;
    esac
    module_export RX3_KEY_TRACE "$trace" "RMX-1000 Roll key trace" || :
    rmx1000_roll_sounds || say "RMX-1000 Roll: the sounds could not be staged"
    # RX3_RMX_OSC and RX3_RMX_RELEASE are shelved: this module never exports them, so they stay off.
    RMX1000_ROLL_READY=1
    say "RMX-1000 Roll prepared: mode $RMX1000_ROLL_MODE, PORTABLE raw $RMX1000_ROLL_PORTABLE_RAW, pads $RMX1000_ROLL_PADS, capture $RMX1000_ROLL_CAPTURE"
    return 0
}

rmx1000_roll_after_launch()
{
    [ "$RMX1000_ROLL_READY" = "1" ] || return 0
    case "$RMX1000_ROLL_MODE" in
        off) say "RMX-1000 Roll: off in its configuration, BEAT FX native" ;;
        aux) say "RMX-1000 Roll: AUX on PORTABLE = RMX mode, X-PAD and sounds from the screen, AUX knob the level, BEAT FX untouched" ;;
        *) say "RMX-1000 Roll: always on, X-PAD and sounds from the screen" ;;
    esac
}

register_prepare_hook rmx1000_roll_prepare
register_after_launch_hook rmx1000_roll_after_launch
