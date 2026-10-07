#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 ducttape3

MODDIR=${0%/*}
. "$MODDIR/status.sh"

BD_SERVICE_LOCK="$MODDIR/.status-service"
BD_THIS_BOOT=$(cat "$BD_PROC/sys/kernel/random/boot_id" 2>/dev/null)
if ! mkdir "$BD_SERVICE_LOCK" 2>/dev/null; then
  # Give a simultaneous starter time to finish writing its PID.
  [ -s "$BD_SERVICE_LOCK/pid" ] || sleep 0.1
  IFS=' ' read -r BD_SERVICE_PID BD_SERVICE_BOOT < "$BD_SERVICE_LOCK/pid" 2>/dev/null
  case "$BD_SERVICE_PID" in
    ''|*[!0-9]*) ;;
    *)
      if [ "$BD_SERVICE_BOOT" = "$BD_THIS_BOOT" ] && kill -0 "$BD_SERVICE_PID" 2>/dev/null; then
        exit 0
      fi
      ;;
  esac
  rm -f "$BD_SERVICE_LOCK/pid"
  rmdir "$BD_SERVICE_LOCK" 2>/dev/null
  mkdir "$BD_SERVICE_LOCK" 2>/dev/null || exit 0
fi
printf '%s %s\n' "$$" "$BD_THIS_BOOT" > "$BD_SERVICE_LOCK/pid"
trap 'rm -f "$BD_SERVICE_LOCK/pid"; rmdir "$BD_SERVICE_LOCK" 2>/dev/null' EXIT

while [ -f "$MODDIR/module.prop" ]; do
  sh "$MODDIR/update-status.sh" >/dev/null 2>&1
  if [ -e "$MODDIR/disable" ] || [ -e "$MODDIR/remove" ]; then
    sh "$MODDIR/update-status.sh" inactive >/dev/null 2>&1
    if [ -f "$BD_REQUEST_FILE" ] && [ ! -L "$BD_REQUEST_FILE" ]; then
      printf 'stop\n' > "$BD_REQUEST_FILE"
    fi
    break
  fi
  sleep 5
done
