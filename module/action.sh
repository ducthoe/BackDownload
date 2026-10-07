#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 ducttape3

MODDIR=${0%/*}
. "$MODDIR/status.sh"

printf 'BackDownload\n\n'
if [ "$(id -u)" != 0 ]; then
  printf 'Root is required to check the policy.\n'
  exit 1
fi
if [ -e "$MODDIR/disable" ] || [ -e "$MODDIR/remove" ]; then
  sh "$MODDIR/update-status.sh" >/dev/null 2>&1
  printf 'Module disabled. Current policy is not verified.\n'
  exit 1
fi

BD_QUERY=$(cat "$BD_PROC/sys/kernel/random/uuid" 2>/dev/null)
BD_READY=0
if [ ${#BD_QUERY} = 36 ] && [ -f "$BD_REQUEST_FILE" ] && [ ! -L "$BD_REQUEST_FILE" ]; then
  printf 'Checking the current DMC policy...\n\n'
  if printf '%s\n' "$BD_QUERY" > "$BD_REQUEST_FILE"; then
    BD_WAIT=0
    while [ "$BD_WAIT" -lt 100 ]; do
      if bd_snapshot && [ "$BD_TOKEN" = "$BD_QUERY" ]; then
        BD_READY=1
        break
      fi
      sleep 0.1
      BD_WAIT=$((BD_WAIT + 1))
    done
  fi
fi

if [ "$BD_READY" != 1 ]; then
  sh "$MODDIR/update-status.sh" unavailable >/dev/null 2>&1
  printf '⚠️ No live policy response.\nCheck that Root and Zygisk are running, then retry.\nCurrent policy is unknown.\n'
  exit 1
fi

sh "$MODDIR/update-status.sh" >/dev/null 2>&1
case "$BD_STATE" in
  valid)
    if [ "$BD_LOCK" = 1 ]; then BD_LOCK_TEXT=Enabled; else BD_LOCK_TEXT=Disabled; fi
    if [ "$BD_MAINT" = 1 ]; then BD_MAINT_TEXT=Enabled; else BD_MAINT_TEXT=Disabled; fi
    if [ "$BD_AT" = 1 ]; then BD_AT_TEXT=Enabled; else BD_AT_TEXT=Disabled; fi
    printf 'Screen lock: %s\nMaintenance Mode: %s\nAT authorization: %s\n\n' "$BD_LOCK_TEXT" "$BD_MAINT_TEXT" "$BD_AT_TEXT"
    if [ "$BD_LOCK:$BD_MAINT:$BD_AT" = 1:0:0 ]; then
      printf '❌ Download Mode policy: BLOCKED\n'
    else
      printf '✅ Download Mode policy: ALLOWED\n'
    fi
    if [ "$BD_AT" = 1 ]; then
      printf '✅ AT authorization is active.\n'
    else
      printf 'AT patch is inactive.\n'
    fi
    ;;
  starting) printf 'Module loaded. Waiting for Android startup to finish.\nPolicy is not verified yet.\n' ;;
  read_failed) printf '⚠️ VaultKeeper could not read the policy.\nPolicy state is unknown.\n'; exit 1 ;;
  unsupported) printf '⚠️ Unsupported DMC policy format.\nPolicy state is unknown.\n'; exit 1 ;;
esac
