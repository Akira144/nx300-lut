#!/bin/sh
# ---------------------------------------------------------------------------
# NX300 LUT mod - boot hook
#
# Put this file in the ROOT of the SD card as:  autoexec.sh
# IMPORTANT: it must have UNIX line endings (LF), not CRLF.
#
# The camera app runs it as root from the SD card at every power-on.
# It writes a log, runs a quick self test, and starts lutd.
# ---------------------------------------------------------------------------

LOG=/mnt/mmc/autoexec.log

{
    echo "=== $(date) autoexec.sh as $(id)"
    mount | grep -i mmc
    echo "--- doctor"
    uname -a
    free 2>/dev/null | head -3
    df -h /mnt/mmc 2>/dev/null | tail -1
    if command -v pidof >/dev/null 2>&1; then echo "pidof: yes"; else echo "pidof: no"; fi
    if command -v nohup >/dev/null 2>&1; then echo "nohup: yes"; else echo "nohup: no"; fi
    busybox 2>&1 | head -1
} >> "$LOG" 2>&1

[ -d /mnt/mmc ] || exit 0

BIN=/mnt/mmc/lut/lutd
CONF=/mnt/mmc/lut/lutd.conf

if [ -x "$BIN" ]; then
    # loud self test: wrong arch / bad LUT path shows up in autoexec.log
    echo "--- selftest" >> "$LOG"
    "$BIN" --selftest "$CONF" >> "$LOG" 2>&1

    if [ -f "$CONF" ]; then
        RUNNING=0
        if command -v pidof >/dev/null 2>&1; then
            pidof lutd >/dev/null 2>&1 && RUNNING=1
        fi
        if [ "$RUNNING" = "0" ]; then
            nohup "$BIN" "$CONF" >> /mnt/mmc/lutd.out 2>&1 &
            echo "started lutd, pid $!" >> "$LOG"
        else
            echo "lutd already running" >> "$LOG"
        fi
    else
        echo "no $CONF, not starting" >> "$LOG"
    fi
fi

# --- optional debug: uncomment to get a root shell over wifi -----------------
# mkdir -p /dev/pts
# mount -t devpts none /dev/pts
# telnetd -l /bin/bash -F >> /mnt/mmc/telnetd.log 2>&1 &

exit 0
