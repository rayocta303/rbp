#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Stem controls. The code lives in the performance core's shared object; this
# module points it at the stem directory and reports what it found.

module_begin stems stems

STEMS_DIR="$USB/RX3_STEMS"
# A drive does not come back under the same device node: pulled out as sda and
# pushed back in as sdb, it mounts somewhere else, and the absolute path handed
# to rbp on the first insertion goes stale. rbp reads this once at load time, so
# a moved path used to mean stopping and relaunching the player - which freezes
# the screen and empties the media list, the very thing that made the drive get
# pulled again. One fixed path, re-pointed on each insertion, makes the loop go
# away: the running player keeps resolving it, and nothing has to restart.
STEMS_LINK=/tmp/rx3-stems
STEMS_READY=0

# Indice dei nomi per l'icona "S": scritto in un file .new e rinominato, mai letto a metà dal core.
# Con l'argomento "keep" (il controllo periodico) un elenco vuoto non sostituisce un indice che ha già dei nomi: sulla console,
# mentre un brano con stems si carica dalla chiavetta, un elenco della cartella può fallire o tornare vuoto per qualche secondo,
# e l'indice vuoto faceva sparire tutte le S finché non cambiava di nuovo qualcosa.
stems_index_write()
{
    STEMS_INDEX_COUNT=0
    : > /tmp/rx3-stems-index.txt.new 2>/dev/null
    for candidate in "$STEMS_DIR"/*.rx3stem; do
        [ -f "$candidate" ] || continue
        name=${candidate##*/}
        case $name in ._*) continue ;; esac
        STEMS_INDEX_COUNT=$((STEMS_INDEX_COUNT+1))
        printf '%s\n' "${name%.rx3stem}" >> /tmp/rx3-stems-index.txt.new 2>/dev/null
    done
    if [ "$STEMS_INDEX_COUNT" = 0 ] && [ "${1:-}" = keep ] && [ -s /tmp/rx3-stems-index.txt ]; then
        rm -f /tmp/rx3-stems-index.txt.new 2>/dev/null
        return 0
    fi
    mv -f /tmp/rx3-stems-index.txt.new /tmp/rx3-stems-index.txt 2>/dev/null
}

# Un pacchetto che arriva a mod già avviata (copiato o inviato dal Mac) non rilancia la mod: un controllo leggero
# ogni ~3 s (un elenco della cartella, nessuna scansione pesante) rifà l'indice solo se è cambiato; il core lo rilegge da solo.
stems_index_watch()
{
    pid=$(cat /tmp/rx3-stems-index.pid 2>/dev/null)
    [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null && kill "$pid" 2>/dev/null
    (
        last=$(ls "$STEMS_DIR" 2>/dev/null)
        while sleep 3; do
            # Chiavetta tolta (non più in /proc/mounts): si esce e non si tocca più nulla. Un controllo che fallisce solo perché la
            # chiavetta è occupata a leggere un brano si salta e si riprova: non si esce e non si riscrive.
            grep -qF " $USB " "${STEMS_MOUNTS:-/proc/mounts}" 2>/dev/null || exit 0
            [ -d "$STEMS_DIR" ] || continue
            now=$(ls "$STEMS_DIR" 2>/dev/null) || continue
            [ "$now" = "$last" ] && continue
            last=$now
            stems_index_write keep
        done
    ) </dev/null >/dev/null 2>&1 &
    echo $! > /tmp/rx3-stems-index.pid
}

stems_prepare()
{

    [ -r "$CORE_OBJECT" ] || {
        say "Stems disabled: the performance core is not selected"
        return 1
    }
    module_disabled_by_switch stems && return 0
    stage_panel_asset stems 01 stems-selected || return 1
    stage_panel_asset stems 09 single-stems-none || return 1
    stage_panel_asset stems 10 single-stems-selected || return 1

    count=0
    # Elenco dei nomi (senza .rx3stem) per l'icona "S" nel browser (solo build instabile la usa): letto una volta dal core.
    stems_index_write
    count=$STEMS_INDEX_COUNT
    stems_index_watch

    published=$STEMS_DIR
    if [ -L "$STEMS_LINK" ] &&
       [ "$(readlink "$STEMS_LINK")" = "$STEMS_DIR" ]; then
        published=$STEMS_LINK
    elif [ ! -e "$STEMS_LINK" ] || [ -L "$STEMS_LINK" ]; then
        rm -f "$STEMS_LINK"
        ln -s "$STEMS_DIR" "$STEMS_LINK" 2>/dev/null && published=$STEMS_LINK
    fi
    [ "$published" = "$STEMS_LINK" ] || \
        say "Stems: $STEMS_LINK cannot be published, falling back to the mount path"

    STEMS_READY=1
    # Only a change of the published directory needs a restart; the core decides
    # for itself whether the binary changed.
    module_export RX3_STEMS_DIR "$published" Stems
    say "Stems prepared: $count stem(s) at $STEMS_DIR, asynchronous basename lookup"
}

stems_after_launch()
{
    [ "$STEMS_READY" = "1" ] || return 0
    say "SLIP LOOP: pads 1=vocal, 2=harmony/inst, 3=bass, 4=drums when prepared"
    say "SLIP LOOP: pads 5-8 = Echo Out of the part above (one beat from the deck BPM)"
    say "SHIFT+CUE tap: channel ISOLATOR <-> STEMS (HI vocal, MID harmony, LOW bass+drums); hold: STEMS panel"
    say "STEMS tab: VOCAL and INST, plus DRUMS, plus BASS for four-part tracks"
    say "available pads blink while loading; the four-part port needs hardware validation"
    say "without a matching stem, audio and pads remain stock"
}

register_prepare_hook stems_prepare
register_after_launch_hook stems_after_launch
