#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Custom Beat Jump: 16 pads (BEAT JUMP + BEAT JUMP 2) up to 64 bars.
# The code lives in the performance core's shared object; this module copies
# the drive's beatjump.ini to RAM and points the core at it. The core reads it
# once, at start, and checks it: a file it refuses leaves the pads stock.

module_begin beatjump-custom beatjump_custom

BEATJUMP_CUSTOM_SOURCE="$USB/RX3_RUNTIME/beatjump/beatjump.ini"
# A fixed path in RAM, so a drive coming back under another device node does
# not change the environment rbp was started with.
BEATJUMP_CUSTOM_COPY=/root/pdj/rx3-beatjump.ini
BEATJUMP_CUSTOM_MAX_BYTES=4096
BEATJUMP_CUSTOM_READY=0

beatjump_custom_disable()
{
    module_export RX3_BEATJUMP_CONFIG "" "Custom Beat Jump" || :
    if [ -e "$BEATJUMP_CUSTOM_COPY" ] || [ -L "$BEATJUMP_CUSTOM_COPY" ]; then
        stage_runtime_removal "$BEATJUMP_CUSTOM_COPY" || return 1
    fi
    return 0
}

beatjump_custom_prepare()
{
    [ -r "$CORE_OBJECT" ] || {
        say "Custom Beat Jump disabled: the performance core is not selected"
        return 0
    }
    if module_disabled_by_switch beatjump-custom; then
        beatjump_custom_disable || return 1
        return 0
    fi
    # Senza beatjump.ini sulla chiavetta: BEAT JUMP resta originale, BEAT JUMP 2
    # diventa 8< 8> 16< 16> / 32< 32> 64< 64> (battiti), scelta dell'utente.
    if [ ! -f "$BEATJUMP_CUSTOM_SOURCE" ]; then
        BEATJUMP_CUSTOM_SOURCE=/tmp/rx3-beatjump-default.ini
        printf 'version=1\npad9=-8\npad10=+8\npad11=-16\npad12=+16\npad13=-32\npad14=+32\npad15=-64\npad16=+64\n' \
            > "$BEATJUMP_CUSTOM_SOURCE" || {
            beatjump_custom_disable || return 1
            return 0
        }
        say "Custom Beat Jump: default layout (BEAT JUMP 2 = 8/16/32/64)"
    fi
    [ -r "$BEATJUMP_CUSTOM_SOURCE" ] || {
        say "Custom Beat Jump: beatjump.ini unreadable, stock pads"
        beatjump_custom_disable || return 1
        return 0
    }
    beatjump_custom_bytes=$(wc -c < "$BEATJUMP_CUSTOM_SOURCE" 2>/dev/null | tr -d ' ')
    case "$beatjump_custom_bytes" in
        ''|*[!0-9]*) beatjump_custom_bytes=0 ;;
    esac
    if [ "$beatjump_custom_bytes" -eq 0 ] ||
       [ "$beatjump_custom_bytes" -gt "$BEATJUMP_CUSTOM_MAX_BYTES" ]; then
        say "Custom Beat Jump: beatjump.ini is empty or too large, stock pads"
        beatjump_custom_disable || return 1
        return 0
    fi
    beatjump_custom_before=$RUNTIME_STAGE_COUNT
    stage_runtime_file "$BEATJUMP_CUSTOM_SOURCE" "$BEATJUMP_CUSTOM_COPY" || return 1
    # rbp reads the file once, at start: a changed file needs a restart even
    # when the path it is exported under stays the same.
    [ "$RUNTIME_STAGE_COUNT" = "$beatjump_custom_before" ] || request_rbp_restart
    module_export RX3_BEATJUMP_CONFIG "$BEATJUMP_CUSTOM_COPY" "Custom Beat Jump" || :
    BEATJUMP_CUSTOM_READY=1
    say "Custom Beat Jump prepared from $BEATJUMP_CUSTOM_SOURCE"
}

beatjump_custom_after_launch()
{
    [ "$BEATJUMP_CUSTOM_READY" = "1" ] || return 0
    say "Custom Beat Jump active; the core log says whether beatjump.ini was accepted"
}

register_prepare_hook beatjump_custom_prepare
register_after_launch_hook beatjump_custom_after_launch
