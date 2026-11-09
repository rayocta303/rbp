#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Sample pads. The code lives in the performance core's shared object; this
# module points it at the bank directory and reports what it found.

module_begin samples samples

SAMPLES_DIR="$USB/RX3_RUNTIME/samples"
# Same reason as stems: a drive comes back under a different device node, and
# rbp reads its environment once. One fixed path, re-pointed on each insertion,
# means a moved drive does not cost a restart.
SAMPLES_LINK=/tmp/rx3-samples
# Deck 2's bank, only when it differs from deck 1's: with one bank for both
# decks there is one link and RX3_SAMPLES_DIR2 stays empty.
SAMPLES_LINK2=/tmp/rx3-samples-2
SAMPLES_READY=0

samples_bank_generation()
{
    samples_bank=$1
    samples_active=$2
    samples_manifest=$TMP/samples-generation.$$
    printf 'bank %s\n' "$samples_active" > "$samples_manifest" || return 1
    for sample_name in settings.ini 1.wav 2.wav 3.wav 4.wav 5.wav 6.wav 7.wav 8.wav; do
        sample_file=$samples_bank/$sample_name
        if [ -e "$sample_file" ] || [ -L "$sample_file" ]; then
            [ -f "$sample_file" ] && [ -r "$sample_file" ] || {
                rm -f "$samples_manifest"
                return 1
            }
            sample_hash=$(sha1sum "$sample_file" 2>/dev/null | awk '{print $1}')
            case "$sample_hash" in
                ""|*[!0-9a-f]*) rm -f "$samples_manifest"; return 1 ;;
            esac
            [ "${#sample_hash}" = 40 ] || { rm -f "$samples_manifest"; return 1; }
            printf '%s %s\n' "$sample_name" "$sample_hash" >> "$samples_manifest" || return 1
        else
            printf '%s absent\n' "$sample_name" >> "$samples_manifest" || return 1
        fi
    done
    samples_generation=$(sha1sum "$samples_manifest" 2>/dev/null | awk '{print $1}')
    rm -f "$samples_manifest"
    [ "${#samples_generation}" = 40 ] || return 1
    printf '%s' "$samples_generation"
}

samples_disable()
{
    module_export RX3_SAMPLES_DIR "" "Sample pads" || :
    module_export RX3_SAMPLES_DIR2 "" "Sample pads" || :
    module_export RX3_SAMPLES_GENERATION "" "Sample pads" || :
    if [ -L "$SAMPLES_LINK" ]; then
        stage_runtime_removal "$SAMPLES_LINK" || return 1
    fi
    if [ -L "$SAMPLES_LINK2" ]; then
        stage_runtime_removal "$SAMPLES_LINK2" || return 1
    fi
}

samples_repoint_live_link()
{
    samples_link=$2
    samples_next=$samples_link.$$
    rm -f "$samples_next"
    ln -s "$1" "$samples_next" || return 1
    samples_previous=$(readlink "$samples_link") || return 1
    rm -f "$samples_link" || return 1
    mv -f "$samples_next" "$samples_link" || {
        rm -f "$samples_next"
        ln -s "$samples_previous" "$samples_link"
        return 1
    }
}

# Point one fixed link at a bank and say which path the player reads. A link
# the running player already uses for the same contents is re-pointed in
# place, so a drive that comes back under another device node costs no
# restart. Arguments: link, bank directory, the setting that names the link.
samples_publish()
{
    samples_link=$1
    samples_dir=$2
    samples_published=$samples_dir
    if [ -L "$samples_link" ] &&
       [ "$(readlink "$samples_link")" = "$samples_dir" ]; then
        samples_published=$samples_link
    elif [ ! -e "$samples_link" ] || [ -L "$samples_link" ]; then
        if [ "$(rbp_environment_value RX3_SAMPLES_GENERATION)" = "$generation" ] &&
           [ "$(rbp_environment_value "$3")" = "$samples_link" ] &&
           [ -L "$samples_link" ]; then
            samples_repoint_live_link "$samples_dir" "$samples_link" || return 1
        else
            stage_runtime_symlink "$samples_dir" "$samples_link" || return 1
        fi
        samples_published=$samples_link
    fi
}

samples_valid_name()
{
    case "$1" in
        ''|.*|*[!A-Za-z0-9_.-]*) return 1 ;;
    esac
    [ -d "$SAMPLES_DIR/banks/$1" ]
}

# `active` names the bank of each deck. The old form, one bank name on the
# first line, still means that bank on both decks. The new form is
#   deck1=<bank>
#   deck2=<bank>
# and a deck whose line is missing, invalid or names a missing bank takes the
# other deck's bank. Sets samples_active1 and samples_active2, both empty when
# no deck has a bank.
samples_read_active()
{
    samples_active1=
    samples_active2=
    samples_legacy=
    samples_keyed=0
    samples_first=1
    [ -r "$SAMPLES_DIR/active" ] || return 0
    while IFS= read -r samples_line || [ -n "$samples_line" ]; do
        case "$samples_line" in
            deck1=*) samples_active1=${samples_line#deck1=}; samples_keyed=1 ;;
            deck2=*) samples_active2=${samples_line#deck2=}; samples_keyed=1 ;;
            *) [ "$samples_first" = 1 ] && samples_legacy=$samples_line ;;
        esac
        samples_first=0
    done < "$SAMPLES_DIR/active"
    if [ "$samples_keyed" = 0 ]; then
        samples_active1=$samples_legacy
        samples_active2=$samples_legacy
    fi
    samples_valid_name "$samples_active1" || samples_active1=
    samples_valid_name "$samples_active2" || samples_active2=
    [ -n "$samples_active1" ] || samples_active1=$samples_active2
    [ -n "$samples_active2" ] || samples_active2=$samples_active1
}

# Snare Roll (RX3_RUNTIME/roll, written by the app; docs/ROLL_INI.md). Its
# own link and settings, independent of the sample banks.
ROLL_DIR="$USB/RX3_RUNTIME/roll"
ROLL_LINK=/tmp/rx3-roll
samples_roll_prepare()
{
    roll_divisions=1,2,4,8,16
    roll_raw=2
    roll_advance=0
    if [ ! -r "$ROLL_DIR/roll.ini" ] || [ ! -r "$ROLL_DIR/snare.wav" ]; then
        module_export RX3_ROLL_FILE "" "Snare Roll" || :
        return 0
    fi
    while IFS= read -r roll_line || [ -n "$roll_line" ]; do
        roll_line=${roll_line%"$(printf '\r')"}
        case "$roll_line" in
            divisions=*)
                roll_value=${roll_line#divisions=}
                case "$roll_value" in
                    ''|*[!0-9,]*) ;;
                    *) [ ${#roll_value} -le 16 ] && roll_divisions=$roll_value ;;
                esac ;;
            portable-raw=0|portable-raw=1|portable-raw=2) roll_raw=${roll_line#portable-raw=} ;;
            anticipo-ms=*)
                roll_value=${roll_line#anticipo-ms=}
                case "$roll_value" in ''|*[!0-9]*) ;; *) [ ${#roll_value} -le 3 ] && roll_advance=$roll_value ;; esac ;;
        esac
    done < "$ROLL_DIR/roll.ini"
    if ! { [ -L "$ROLL_LINK" ] && [ "$(readlink "$ROLL_LINK")" = "$ROLL_DIR" ]; }; then
        stage_runtime_symlink "$ROLL_DIR" "$ROLL_LINK" || return 1
    fi
    module_export RX3_ROLL_FILE "$ROLL_LINK/snare.wav" "Snare Roll" || :
    module_export RX3_ROLL_DIVISIONS "$roll_divisions" "Snare Roll" || :
    module_export RX3_ROLL_PORTABLE_RAW "$roll_raw" "Snare Roll" || :
    module_export RX3_ROLL_ADVANCE_MS "$roll_advance" "Snare Roll" || :
    say "Snare Roll prepared: ROLL tab, volume from AUX LEVEL on PORTABLE (selector value $roll_raw)"
}

samples_prepare()
{

    [ -r "$CORE_OBJECT" ] || {
        say "Sample pads disabled: the performance core is not selected"
        return 1
    }
    if [ -e "$(module_switch_path samples)" ]; then
        samples_disable || say "Sample pads: disabled link could not be staged"
    fi
    module_disabled_by_switch samples && return 0
    samples_roll_prepare || say "Snare Roll: settings could not be staged"
    # With the RMX-1000 Roll module on, the STATUS slot is its window: the tab reads RMX, not ROLL.
    if [ "${RX3_RMX_ROLL:-}" = 1 ]; then
        stage_panel_asset samples 04 samples-rmx-selected || return 1
        stage_panel_asset samples 05 samples-rmx-none-selected || return 1
        stage_panel_asset samples 06 samples-rmx-beatfx-selected || return 1
    else
        stage_panel_asset samples 04 samples-selected || return 1
        stage_panel_asset samples 05 samples-none-selected || return 1
        stage_panel_asset samples 06 samples-beatfx-selected || return 1
    fi

    # A drive with no banks on it is the ordinary case, not a failure. A
    # prepare hook that returns non-zero stops the whole session and no
    # guarded word is written, so a module with nothing to do says so and
    # steps aside.
    [ -d "$SAMPLES_DIR" ] || {
        say "Sample pads: no samples directory on this drive, nothing to load"
        samples_disable || return 1
        return 0
    }

    count=0
    for candidate in "$SAMPLES_DIR"/banks/*; do
        [ -d "$candidate" ] || continue
        count=$((count+1))
    done

    # The core reads numbered WAV files directly from RX3_SAMPLES_DIR (deck 1)
    # and RX3_SAMPLES_DIR2 (deck 2, empty when both decks share the bank).
    # Bank selection belongs to the host and is resolved before launch.
    samples_read_active
    [ -n "$samples_active1" ] || {
        say "Sample pads: no valid active bank, nothing to load"
        samples_disable || return 1
        return 0
    }
    bank_dir="$SAMPLES_DIR/banks/$samples_active1"
    generation=$(samples_bank_generation "$bank_dir" "$samples_active1") || {
        say "Sample pads: bank contents could not be verified"
        return 1
    }
    bank_dir2=
    if [ "$samples_active2" != "$samples_active1" ]; then
        bank_dir2="$SAMPLES_DIR/banks/$samples_active2"
        generation2=$(samples_bank_generation "$bank_dir2" "$samples_active2") || {
            say "Sample pads: deck 2 bank contents could not be verified"
            return 1
        }
        # One value for both banks, so a change on either deck is a change.
        generation=$(printf 'deck1 %s\ndeck2 %s\n' "$generation" "$generation2" |
                     sha1sum 2>/dev/null | awk '{print $1}')
        [ "${#generation}" = 40 ] || return 1
    fi

    samples_publish "$SAMPLES_LINK" "$bank_dir" RX3_SAMPLES_DIR || return 1
    published=$samples_published
    published2=
    if [ -n "$bank_dir2" ]; then
        samples_publish "$SAMPLES_LINK2" "$bank_dir2" RX3_SAMPLES_DIR2 || return 1
        published2=$samples_published
    elif [ -L "$SAMPLES_LINK2" ]; then
        stage_runtime_removal "$SAMPLES_LINK2" || return 1
    fi

    export RX3_SAMPLES_CONFIG="$published/settings.ini"
    SAMPLES_READY=1
    module_export RX3_SAMPLES_DIR "$published" "Sample pads"
    module_export RX3_SAMPLES_DIR2 "$published2" "Sample pads" || :
    module_export RX3_SAMPLES_GENERATION "$generation" "Sample pads" || :
    say "Sample pads prepared: $count bank(s) at $SAMPLES_DIR, deck 1 $samples_active1, deck 2 $samples_active2"
}

samples_after_launch()
{
    [ "$SAMPLES_READY" = "1" ] || return 0
    say "SAMPLE: RELEASE FX (SHIFT + SLIP LOOP) is the sampler, 8 pads per deck; touch SAMPLES for its volume; playback hardware validation pending"
    # What this build of the pads can do, so the computer can offer exactly
    # that rather than offering a setting a deck would ignore. Read back from
    # the session file the player leaves on the drive.
    say "SAMPLE pad modes: once hold loop latch"
    say "SAMPLE shift.silence: SHIFT + a sampler pad stops that deck's pads"
}

register_prepare_hook samples_prepare
register_after_launch_hook samples_after_launch
