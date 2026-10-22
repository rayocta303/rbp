#!/bin/bash
PROBE_MNT="/opt/rblive4/probe-mnt"
ppart="sda1"
mkdir -p "$PROBE_MNT"
pfstype=$(blkid -s TYPE -o value "/dev/$ppart" 2>/dev/null)
echo "ppart=$ppart pfstype=$pfstype"
mount -t "$pfstype" -o ro "/dev/$ppart" "$PROBE_MNT"
echo "mount_exit=$?"
ls -la "$PROBE_MNT/PIONEER/rekordbox/export.pdb"
echo "exists_exit=$?"
umount "$PROBE_MNT" 2>/dev/null
rmdir "$PROBE_MNT" 2>/dev/null
