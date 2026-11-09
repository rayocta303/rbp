#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Long press on a track in the browser shows "MAKE STEMS in coda: <title>" (developer
# build, notice only: nothing is queued yet). The code lives in the core's shared
# object; this module only switches it on. Needs a browse feature that watches the
# list (key-match does); without it the long press stays silent.

module_begin browse-longpress browse_longpress

browse_longpress_prepare()
{
    [ -r "$CORE_OBJECT" ] || return 0
    module_disabled_by_switch browse-longpress && return 0
    # Off by default until the gesture is right: only RX3_RUNTIME/longpress-on switches it on
    # (and RX3_RUNTIME/longpress-off always wins).
    if [ -e "$USB/RX3_RUNTIME/longpress-off" ] || [ ! -e "$USB/RX3_RUNTIME/longpress-on" ]; then
        say "browse-longpress: off (create RX3_RUNTIME/longpress-on to try it; longpress-off wins)"
        return 0
    fi
    module_export RX3_BROWSE_LONGPRESS 1 "Browser long press"
    return 0
}
register_prepare_hook browse_longpress_prepare
