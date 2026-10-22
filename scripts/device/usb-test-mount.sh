#!/bin/sh
# usb-test-mount.sh — Auto Mount & Management script for virtual Rekordbox USB test image
#
# Attaches, detaches, and monitors the virtual FAT32 Rekordbox test image (/opt/rblive4/usb1_test.img)
# using Linux loop block devices. When attached, usb-watch.sh automatically mounts it into
# /opt/rblive4/media/usb1 (or usb2), binds it to the chroot, and notifies rbp.
#
# Usage:
#   usb-test-mount.sh start|mount     - Attach loop device and wait for Rekordbox mount
#   usb-test-mount.sh stop|umount     - Safely notify Rekordbox, unmount and detach loop
#   usb-test-mount.sh restart         - Re-mount / toggle test USB image
#   usb-test-mount.sh status          - Show loop device, mount point, label, and Rekordbox status
#   usb-test-mount.sh enable          - Enable auto-mounting on system boot
#   usb-test-mount.sh disable         - Disable auto-mounting on system boot

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
if [ -f "$HERE/lib.sh" ]; then
    . "$HERE/lib.sh"
    rb_load_conf 2>/dev/null || true
fi

DEPLOY_ROOT="${RB_DEPLOY_ROOT:-/opt/rblive4}"
CHROOT_DIR="${RB_CHROOT:-$DEPLOY_ROOT/rbx3-run}"
TEST_IMG="${RB_USB_TEST_IMG:-$DEPLOY_ROOT/usb1_test.img}"
CONF_FILE="$DEPLOY_ROOT/rb.local.conf"
SERVICE_NAME="rblive4-usbtest.service"

MNT1="${RB_MEDIA_MOUNT:-$DEPLOY_ROOT/media/usb1}/sda1"
CH_MNT1="$CHROOT_DIR${RB_CHROOT_MEDIA:-/media/usb1/sda1}"
FIFO1="/tmp/udev_usb1"
LABEL1="/tmp/udev_usb1.label"
UUID1="/tmp/udev_usb1.uuid"

MNT2="${RB_MEDIA_MOUNT2:-$DEPLOY_ROOT/media/usb2}/sda1"
CH_MNT2="$CHROOT_DIR${RB_CHROOT_MEDIA2:-/media/usb2/sda1}"
FIFO2="/tmp/udev_usb2"
LABEL2="/tmp/udev_usb2.label"
UUID2="/tmp/udev_usb2.uuid"

# Ensure run as root for mount/losetup operations
require_root() {
    if [ "$(id -u)" -ne 0 ]; then
        echo "ERROR: This script must be run as root." >&2
        exit 1
    fi
}

# Return primary loop device path for $TEST_IMG, or empty
get_loop_dev() {
    losetup -j "$TEST_IMG" 2>/dev/null | cut -d: -f1 | head -1
}

# Return all loop devices associated with $TEST_IMG
get_all_loop_devs() {
    losetup -j "$TEST_IMG" 2>/dev/null | cut -d: -f1
}

# Determine which slot (1 or 2) a device or path is attached to
get_slot_for_dev() {
    dev="$1"
    m1=$(awk -v m="$MNT1" '$2 == m { print $1; exit }' /proc/mounts 2>/dev/null)
    m2=$(awk -v m="$MNT2" '$2 == m { print $1; exit }' /proc/mounts 2>/dev/null)
    if [ "$m1" = "$dev" ] || [ "$m1" = "${dev}p1" ]; then
        echo 1
    elif [ "$m2" = "$dev" ] || [ "$m2" = "${dev}p1" ]; then
        echo 2
    else
        echo 0
    fi
}

notify_rbp_umount() {
    slot="$1"
    if [ "$slot" = "1" ]; then
        if [ -p "$FIFO1" ]; then
            timeout 2 sh -c 'printf "%s" "umount /media/usb1/sda1" > "$1"' sh "$FIFO1" 2>/dev/null || true
        fi
        sleep 1
        umount -l "$CH_MNT1" 2>/dev/null || true
        umount -l "$MNT1" 2>/dev/null || true
        rm -f "$LABEL1" "$UUID1"
    elif [ "$slot" = "2" ]; then
        if [ -p "$FIFO2" ]; then
            timeout 2 sh -c 'printf "%s" "umount /media/usb2/sda1" > "$1"' sh "$FIFO2" 2>/dev/null || true
        fi
        sleep 1
        umount -l "$CH_MNT2" 2>/dev/null || true
        umount -l "$MNT2" 2>/dev/null || true
        rm -f "$LABEL2" "$UUID2"
    fi
}

do_status() {
    echo "================ USB TEST STATUS ================"
    echo "Image file:     $TEST_IMG $([ -f "$TEST_IMG" ] && echo '(exists)' || echo '(MISSING)')"
    if [ -f "$TEST_IMG" ]; then
        size_mb=$(du -m "$TEST_IMG" 2>/dev/null | cut -f1)
        echo "Image size:     ${size_mb} MB"
    fi

    ldev=$(get_loop_dev)
    if [ -n "$ldev" ]; then
        echo "Loop device:    $ldev (attached)"
        lbl=$(blkid -s LABEL -o value "$ldev" 2>/dev/null || echo "none")
        uuid=$(blkid -s UUID -o value "$ldev" 2>/dev/null || echo "none")
        fstype=$(blkid -s TYPE -o value "$ldev" 2>/dev/null || echo "unknown")
        echo "Filesystem:     $fstype"
        echo "Volume Label:   $lbl"
        echo "UUID:           $uuid"

        slot=$(get_slot_for_dev "$ldev")
        if [ "$slot" = "1" ]; then
            echo "Assigned Slot:  USB1 ($MNT1)"
            echo "Chroot Bind:    $CH_MNT1 $(mountpoint -q "$CH_MNT1" && echo '(mounted)' || echo '(not mounted)')"
        elif [ "$slot" = "2" ]; then
            echo "Assigned Slot:  USB2 ($MNT2)"
            echo "Chroot Bind:    $CH_MNT2 $(mountpoint -q "$CH_MNT2" && echo '(mounted)' || echo '(not mounted)')"
        else
            echo "Assigned Slot:  Pending (waiting for usb-watch.sh)"
        fi
    else
        echo "Loop device:    none (not attached)"
    fi

    # Rekordbox status
    rbp_pid=$(pgrep -f "rbp -a" 2>/dev/null | head -1)
    if [ -n "$rbp_pid" ]; then
        echo "rbp PID:        $rbp_pid (running)"
        if ls -l "/proc/$rbp_pid/fd" 2>/dev/null | grep -q "export.pdb"; then
            echo "DB Status:      Rekordbox library export.pdb is OPEN and LOADED"
        else
            echo "DB Status:      Rekordbox running, DB not open yet"
        fi
    else
        echo "rbp PID:        none (rbp not running)"
    fi

    # Auto-mount status
    auto_boot="disabled"
    if [ "${RB_USB_TEST_AUTO:-0}" = 1 ]; then
        auto_boot="enabled (via rb.local.conf)"
    elif systemctl is-enabled "$SERVICE_NAME" >/dev/null 2>&1; then
        auto_boot="enabled (via systemd service)"
    fi
    echo "Auto-Mount:     $auto_boot"
    echo "================================================="
}

do_start() {
    require_root
    if [ ! -f "$TEST_IMG" ]; then
        echo "ERROR: Test image $TEST_IMG does not exist!" >&2
        return 1
    fi

    ldev=$(get_loop_dev)
    if [ -n "$ldev" ]; then
        echo "USB TEST image is already attached as $ldev."
    else
        echo "Attaching $TEST_IMG via loop device..."
        if ! losetup -fP "$TEST_IMG"; then
            echo "ERROR: losetup failed to attach $TEST_IMG" >&2
            return 1
        fi
        ldev=$(get_loop_dev)
        echo "Attached as $ldev"
    fi

    echo "Waiting for usb-watch to detect and mount media..."
    mounted=0
    slot=0
    for i in $(seq 1 12); do
        slot=$(get_slot_for_dev "$ldev")
        if [ "$slot" != "0" ]; then
            mounted=1
            break
        fi
        sleep 1
    done

    if [ "$mounted" = 1 ]; then
        lbl=$(blkid -s LABEL -o value "$ldev" 2>/dev/null || echo "UNX-TEST")
        echo "SUCCESS: USB TEST mounted on slot USB$slot (Label: '$lbl', Device: $ldev)"
        return 0
    else
        echo "WARNING: $ldev attached, but usb-watch has not mounted it yet."
        echo "Check 'sh $DEPLOY_ROOT/usb-watch.sh status' or usbwatch.log"
        return 0
    fi
}

do_stop() {
    require_root
    ldevs=$(get_all_loop_devs)
    if [ -z "$ldevs" ]; then
        echo "USB TEST is not currently attached."
        return 0
    fi

    for ld in $ldevs; do
        echo "Detaching $ld ($TEST_IMG)..."
        slot=$(get_slot_for_dev "$ld")
        if [ "$slot" = "0" ]; then
            [ "$(awk -v m="$MNT1" '$2 == m { print $1 }' /proc/mounts 2>/dev/null)" = "$ld" ] && slot=1
            [ "$(awk -v m="$MNT2" '$2 == m { print $1 }' /proc/mounts 2>/dev/null)" = "$ld" ] && slot=2
        fi

        if [ "$slot" != "0" ]; then
            echo "Notifying Rekordbox and unmounting slot USB$slot..."
            notify_rbp_umount "$slot"
        fi

        if awk -v d="$ld" '$1 == d { print $2 }' /proc/mounts 2>/dev/null | grep -q "probe-mnt"; then
            umount -l "$DEPLOY_ROOT/probe-mnt" 2>/dev/null || true
        fi

        losetup -d "$ld" 2>/dev/null || true
    done

    sleep 1
    udevadm settle --timeout=2 2>/dev/null || true

    remaining=$(get_loop_dev)
    if [ -z "$remaining" ]; then
        echo "SUCCESS: USB TEST cleanly detached and unmounted."
    else
        echo "WARNING: Loop device $remaining still present (autoclear pending)."
    fi
}

do_restart() {
    echo "Restarting USB TEST media..."
    do_stop
    sleep 2
    do_start
}

do_enable() {
    require_root
    echo "Enabling USB TEST auto-mount on system boot..."
    touch "$CONF_FILE" 2>/dev/null || true
    if grep -q "^RB_USB_TEST_AUTO=" "$CONF_FILE" 2>/dev/null; then
        sed -i 's/^RB_USB_TEST_AUTO=.*/RB_USB_TEST_AUTO=1/' "$CONF_FILE"
    else
        echo "RB_USB_TEST_AUTO=1" >> "$CONF_FILE"
    fi

    if [ -f "/etc/systemd/system/$SERVICE_NAME" ]; then
        systemctl daemon-reload 2>/dev/null || true
        systemctl enable "$SERVICE_NAME" 2>/dev/null || true
    fi
    echo "Auto-mount USB TEST enabled (RB_USB_TEST_AUTO=1 in $CONF_FILE)."
}

do_disable() {
    require_root
    echo "Disabling USB TEST auto-mount on system boot..."
    touch "$CONF_FILE" 2>/dev/null || true
    if grep -q "^RB_USB_TEST_AUTO=" "$CONF_FILE" 2>/dev/null; then
        sed -i 's/^RB_USB_TEST_AUTO=.*/RB_USB_TEST_AUTO=0/' "$CONF_FILE"
    fi

    if [ -f "/etc/systemd/system/$SERVICE_NAME" ]; then
        systemctl disable "$SERVICE_NAME" 2>/dev/null || true
    fi
    echo "Auto-mount USB TEST disabled (RB_USB_TEST_AUTO=0 in $CONF_FILE)."
}

case "${1:-}" in
  start|mount)     do_start ;;
  stop|umount)     do_stop ;;
  restart)         do_restart ;;
  status)          do_status ;;
  enable)          do_enable ;;
  disable)         do_disable ;;
  *)
    echo "Usage: $0 {start|stop|restart|status|enable|disable}" >&2
    exit 1
    ;;
esac
