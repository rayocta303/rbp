#!/bin/sh
# SPDX-License-Identifier: MIT
# load-mods.sh — translates /opt/rblive4/mods.conf into RX3_* mod environment variables
# and configures librx3_core.so in RB_LD_PRELOAD.

MOD_CONF="${RB_DEPLOY_ROOT:-/opt/rblive4}/mods.conf"
[ -f "$MOD_CONF" ] && . "$MOD_CONF"

# Default fallback if unset
MOD_CORE="${MOD_CORE:-1}"
CORE_LIB="/root/pdj/librx3_core.so"
CORE_FILE="${RB_CHROOT:-/opt/rblive4/rbx3-run}$CORE_LIB"

MOD_ENV_ARGS=""

add_env() {
    export "$1=$2"
    if [ -z "$MOD_ENV_ARGS" ]; then
        MOD_ENV_ARGS="$1=$2"
    else
        MOD_ENV_ARGS="$MOD_ENV_ARGS $1=$2"
    fi
}

if [ "$MOD_CORE" = "1" ] && [ -f "$CORE_FILE" ]; then
    # Inject librx3_core.so into RB_LD_PRELOAD if not already present
    case ":$RB_LD_PRELOAD:" in
        *":$CORE_LIB:"*) ;;
        *)
            # Place after crashcatch.so if present, or at beginning
            case "$RB_LD_PRELOAD" in
                /usr/lib/crashcatch.so:*)
                    RB_LD_PRELOAD="/usr/lib/crashcatch.so:$CORE_LIB:${RB_LD_PRELOAD#/usr/lib/crashcatch.so:}"
                    ;;
                *)
                    RB_LD_PRELOAD="$CORE_LIB:$RB_LD_PRELOAD"
                    ;;
            esac
            ;;
    esac

    # Custom tabs in performance view are moved to top header bar to prevent
    # obscuring native ZOOM, GRID, STATUS and BEAT FX controls.
    # Tab asset slots are disabled here so librx3_core leaves native tabs untouched.

    # Pad Glyph Atlases
    add_env "RX3_PAD_ATLAS" "/root/pdj/rx3-glyph-atlas-dark.rgb565"
    add_env "RX3_PAD_ATLAS_LIGHT" "/root/pdj/rx3-glyph-atlas-light.rgb565"

    # Harmonic Mixing
    [ "${MOD_KEYSHIFT:-1}" = "1" ] && add_env "RX3_KEYSHIFT" "1"
    [ "${MOD_KEY_SYNC:-1}" = "1" ] && {
        add_env "RX3_KEY_SYNC" "1"
        add_env "RX3_KEY_SYNC_RANGE" "${MOD_KEY_SYNC_RANGE:-1}"
        add_env "RX3_KEY_SYNC_MODE" "${MOD_KEY_SYNC_MODE:-identical}"
    }
    [ "${MOD_KEY_MATCH:-1}" = "1" ] && {
        add_env "RX3_KEY_MATCH" "1"
        add_env "RX3_KEY_MATCH_RULES" "${MOD_KEY_MATCH_RULES:-2}"
    }

    # Sound Color FX
    [ "${MOD_COLORFX_DUAL:-1}" = "1" ] && add_env "RX3_COLORFX_DUAL" "1"

    # Sampler & Stems
    [ "${MOD_SAMPLES:-1}" = "1" ] && {
        add_env "RX3_SAMPLES_CONFIG" "/root/pdj/rx3-samples.ini"
        add_env "RX3_SAMPLES_DIR" "/root/pdj/samples"
    }
    [ "${MOD_STEMS:-1}" = "1" ] && {
        mkdir -p /tmp/rx3-stems 2>/dev/null
        add_env "RX3_STEMS_DIR" "/tmp/rx3-stems"
    }
    [ "${MOD_STEMS_LOCAL:-1}" = "1" ] && {
        mkdir -p /tmp/stems-local 2>/dev/null
        add_env "RX3_STEMS_LOCAL" "1"
    }

    # RMX-1000 Roll & Scene
    [ "${MOD_RMX_ROLL:-1}" = "1" ] && {
        add_env "RX3_RMX_ROLL" "1"
        add_env "RX3_RMX_SOUNDS" "/root/pdj/roll"
        add_env "RX3_RMX_ROLL_MODE" "${MOD_RMX_ROLL_MODE:-aux}"
        add_env "RX3_RMX_ROLL_PADS" "beat-loop"
        add_env "RX3_RMX_ROLL_PAD_LIGHTS" "1"
        add_env "RX3_RMX_ROLL_PANEL" "1"
    }
    [ "${MOD_RMX_SCENE:-1}" = "1" ] && {
        add_env "RX3_RMX_SCENE" "1"
        add_env "RX3_RMX_SCENE_TYPE" "${MOD_RMX_SCENE_TYPE:-hpf}"
    }

    # Beat Jump Custom
    if [ "${MOD_BEATJUMP_CUSTOM:-1}" = "1" ]; then
        BJ_INI="${RB_CHROOT:-/opt/rblive4/rbx3-run}/root/pdj/rx3-beatjump.ini"
        if [ ! -f "$BJ_INI" ]; then
            printf 'version=1\npad9=-8\npad10=+8\npad11=-16\npad12=+16\npad13=-32\npad14=+32\npad15=-64\npad16=+64\n' > "$BJ_INI" 2>/dev/null || true
        fi
        add_env "RX3_BEATJUMP_CONFIG" "/root/pdj/rx3-beatjump.ini"
    fi

    # Display & Browser
    [ "${MOD_THEME_WHITE:-1}" = "1" ] && {
        add_env "RX3_THEME" "${MOD_THEME_MODE:-s}"
        add_env "RX3_THEME_BAR" "1"
    }
    [ "${MOD_BROWSE_COLUMNS:-1}" = "1" ] && {
        add_env "RX3_BROWSE_COLUMNS" "1"
        add_env "RX3_BROWSE_FIELD" "${MOD_BROWSE_FIELD:-13}"
    }
    [ "${MOD_BROWSE_LONGPRESS:-1}" = "1" ] && add_env "RX3_BROWSE_LONGPRESS" "1"
    [ "${MOD_SEARCH_LATIN:-1}" = "1" ] && add_env "RX3_SEARCH_LATIN" "1"
    [ "${MOD_ASSHOLE_MODE:-1}" = "1" ] && add_env "RX3_ASSHOLE_MODE" "1"
    [ "${MOD_LOGO:-0}" = "1" ] && add_env "RX3_LOGO" "1"

    # Streaming / Now Playing
    [ "${MOD_NOW_PLAYING:-1}" = "1" ] && {
        add_env "RX3_NOW_PLAYING" "1"
        [ "${MOD_NET_ANNOUNCE:-1}" = "1" ] && add_env "RX3_NET_ANNOUNCE" "1"
    }

    echo "load-mods: performance core enabled with librx3_core.so"
else
    echo "load-mods: performance core disabled (MOD_CORE=$MOD_CORE)"
fi

