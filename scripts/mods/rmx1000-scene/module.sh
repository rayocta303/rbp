#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# RMX-1000 SCENE FX filters on the BEAT FX HELIX position (developer build). The code
# lives in the performance core's shared object; this module reads its configuration and
# exports it. The configuration is plain data, read line by line and never sourced as
# shell code.

module_begin rmx1000-scene rmx1000_scene

RMX1000_SCENE_READY=0

# key=value lines; comments, blank lines and unknown keys are skipped, and a
# bad value keeps what was there before.
rmx1000_scene_read()
{
    [ -r "$1" ] || return 0
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in
            type=hpf|type=lpf|type=bpf) RMX1000_SCENE_TYPE=${line#type=} ;;
            echo=on|echo=off) RMX1000_SCENE_ECHO=${line#echo=} ;;
            selector=on|selector=off) RMX1000_SCENE_SELECTOR=${line#selector=} ;;
            policy=reset|policy=keep) RMX1000_SCENE_POLICY=${line#policy=} ;;
            switch=now|switch=off) RMX1000_SCENE_SWITCH=${line#switch=} ;;
            unavailable=show|unavailable=hide) RMX1000_SCENE_UNAVAILABLE=${line#unavailable=} ;;
            autoopen=on|autoopen=off) RMX1000_SCENE_AUTOOPEN=${line#autoopen=} ;;
            arrival=list|arrival=quick) RMX1000_SCENE_ARRIVAL=${line#arrival=} ;;
            start=helix|start=hpf-echo|start=lpf-echo|start=bpf-echo) RMX1000_SCENE_START=${line#start=} ;;
            feedback=*)
                value=${line#feedback=}
                case "$value" in
                    ''|*[!0-9]*) ;;
                    *) [ ${#value} -le 2 ] && [ "$value" -le 90 ] && RMX1000_SCENE_FEEDBACK=$value ;;
                esac
                ;;
            level=*)
                value=${line#level=}
                case "$value" in
                    ''|*[!0-9]*) ;;
                    *) [ ${#value} -le 3 ] && [ "$value" -le 100 ] && RMX1000_SCENE_LEVEL=$value ;;
                esac
                ;;
        esac
    done < "$1"
}

rmx1000_scene_prepare()
{
    [ -r "$CORE_OBJECT" ] || {
        say "RMX-1000 SCENE FX disabled: the performance core is not selected"
        return 0
    }
    module_disabled_by_switch rmx1000-scene && return 0
    RMX1000_SCENE_TYPE=hpf
    RMX1000_SCENE_ECHO=off
    RMX1000_SCENE_FEEDBACK=45
    RMX1000_SCENE_LEVEL=70
    RMX1000_SCENE_SELECTOR=on
    RMX1000_SCENE_POLICY=reset
    RMX1000_SCENE_SWITCH=now
    RMX1000_SCENE_UNAVAILABLE=show
    RMX1000_SCENE_AUTOOPEN=on
    RMX1000_SCENE_ARRIVAL=list
    RMX1000_SCENE_START=helix
    # The image's defaults first, then the drive, which the operator can
    # edit without building the mod again.
    rmx1000_scene_read /mnt/iso/modules/rmx1000-scene/config.txt
    rmx1000_scene_read "$USB/RX3_RUNTIME/rmx1000-scene.txt"
    echo_on=0
    [ "$RMX1000_SCENE_ECHO" = on ] && echo_on=1
    module_export RX3_RMX_SCENE 1 "RMX-1000 SCENE FX" || :
    module_export RX3_RMX_SCENE_TYPE "$RMX1000_SCENE_TYPE" "RMX-1000 SCENE FX type" || :
    module_export RX3_RMX_SCENE_ECHO "$echo_on" "RMX-1000 SCENE FX echo" || :
    module_export RX3_RMX_SCENE_FEEDBACK "$RMX1000_SCENE_FEEDBACK" "RMX-1000 SCENE FX echo feedback" || :
    module_export RX3_RMX_SCENE_LEVEL "$RMX1000_SCENE_LEVEL" "RMX-1000 SCENE FX echo level" || :
    chooser_on=0
    [ "$RMX1000_SCENE_SELECTOR" = on ] && chooser_on=1
    auto_on=0
    [ "$RMX1000_SCENE_AUTOOPEN" = on ] && auto_on=1
    module_export RX3_RMX_SCENE_SELECTOR "$chooser_on" "RMX-1000 SCENE FX effect chooser" || :
    module_export RX3_RMX_SCENE_START "$RMX1000_SCENE_START" "RMX-1000 SCENE FX start effect" || :
    module_export RX3_RMX_SCENE_POLICY "$RMX1000_SCENE_POLICY" "RMX-1000 SCENE FX reset or keep on leaving the position" || :
    module_export RX3_RMX_SCENE_SWITCH "$RMX1000_SCENE_SWITCH" "RMX-1000 SCENE FX when a new choice takes over" || :
    module_export RX3_RMX_SCENE_UNAVAILABLE "$RMX1000_SCENE_UNAVAILABLE" "RMX-1000 SCENE FX effects not written yet" || :
    module_export RX3_RMX_SCENE_ARRIVAL "$RMX1000_SCENE_ARRIVAL" "RMX-1000 SCENE FX first view" || :
    module_export RX3_RMX_SCENE_AUTOOPEN "$auto_on" "RMX-1000 SCENE FX chooser opens on position 4" || :
    RMX1000_SCENE_READY=1
    say "RMX-1000 SCENE FX prepared: type $RMX1000_SCENE_TYPE, echo $RMX1000_SCENE_ECHO"
    return 0
}

rmx1000_scene_after_launch()
{
    [ "$RMX1000_SCENE_READY" = "1" ] || return 0
    say "RMX-1000 SCENE FX: BEAT FX position 4 plays the $RMX1000_SCENE_TYPE scene, Lv/Depth slides it; hardware validation pending"
}

register_prepare_hook rmx1000_scene_prepare
register_after_launch_hook rmx1000_scene_after_launch
