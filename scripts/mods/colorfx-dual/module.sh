#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Several SOUND COLOR FX.
# The code lives in the performance core's shared object; this module decides
# whether it runs, and owns its documentation and tests.

module_begin colorfx-dual colorfx_dual

COLORFX_DUAL_READY=0

colorfx_dual_prepare()
{
    [ -r "$CORE_OBJECT" ] || {
        say "Several SOUND COLOR FX disabled: the performance core is not selected"
        return 0
    }
    export RX3_COLORFX_DUAL=1
    COLORFX_DUAL_READY=1
    running=$(rbp_environment_value RX3_COLORFX_DUAL)
    if [ "$running" != "1" ]; then
        say "Several SOUND COLOR FX needs a restart: running rbp carries RX3_COLORFX_DUAL=[${running:-none}]"
        request_rbp_restart
    fi
    say "Several SOUND COLOR FX prepared"
}

colorfx_dual_after_launch()
{
    [ "$COLORFX_DUAL_READY" = "1" ] || return 0
    say "Several SOUND COLOR FX active"
}

register_prepare_hook colorfx_dual_prepare
register_after_launch_hook colorfx_dual_after_launch
