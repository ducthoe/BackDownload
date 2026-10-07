#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 ducttape3

BD_STATUS_FILE=/data/system/backdownload.status
BD_REQUEST_FILE=/data/system/backdownload.request
BD_PROC=/proc
BD_DEFAULT='Unlocks Download Mode on Samsung devices. Tap Action to check the policy.'

bd_snapshot() {
  BD_STATE=unavailable
  [ -f "$BD_STATUS_FILE" ] && [ ! -L "$BD_STATUS_FILE" ] || return 1
  IFS=' ' read -r BD_MAGIC BD_BOOT BD_PID BD_TOKEN BD_TIME BD_STATE BD_LOCK BD_MAINT BD_AT BD_EXTRA < "$BD_STATUS_FILE" || return 1
  [ "$BD_MAGIC" = BD1 ] && [ -z "$BD_EXTRA" ] || return 1
  [ -f "$BD_REQUEST_FILE" ] && [ ! -L "$BD_REQUEST_FILE" ] || return 1
  BD_EXPECTED=$(cat "$BD_REQUEST_FILE" 2>/dev/null)
  [ -n "$BD_EXPECTED" ] || BD_EXPECTED=0
  [ "$BD_TOKEN" = "$BD_EXPECTED" ] || return 1
  [ "$BD_BOOT" = "$(cat "$BD_PROC/sys/kernel/random/boot_id" 2>/dev/null)" ] || return 1
  case "$BD_PID:$BD_TIME" in *[!0-9:]*|:*|*:) return 1 ;; esac
  [ "$BD_PID" -gt 1 ] || return 1
  [ "$(cat "$BD_PROC/$BD_PID/comm" 2>/dev/null)" = system_server ] || return 1
  IFS=' ' read -r BD_NOW BD_IDLE < "$BD_PROC/uptime" || return 1
  BD_NOW=${BD_NOW%%.*}
  case "$BD_NOW" in ''|*[!0-9]*) return 1 ;; esac
  [ "$BD_TIME" -le "$BD_NOW" ] && [ "$((BD_NOW - BD_TIME))" -le 60 ] || return 1
  case "$BD_STATE" in
    valid)
      case "$BD_LOCK:$BD_MAINT:$BD_AT" in
        [01]:[01]:[01]) return 0 ;;
      esac
      ;;
    starting|read_failed|unsupported)
      [ "$BD_LOCK:$BD_MAINT:$BD_AT" = '-1:-1:-1' ] && return 0
      ;;
  esac
  return 1
}

bd_description() {
  BD_DESCRIPTION='⚠️ Policy not verified. Tap Action to check.'
  if [ -e "$MODDIR/disable" ] || [ -e "$MODDIR/remove" ]; then
    BD_DESCRIPTION='Module disabled. Policy not verified.'
    return
  fi
  bd_snapshot || return
  case "$BD_STATE" in
    valid)
      if [ "$BD_AT" = 1 ]; then
        BD_DESCRIPTION='✅ Last policy check: AT authorization enabled.'
      elif [ "$BD_LOCK" = 0 ] || [ "$BD_MAINT" = 1 ]; then
        BD_DESCRIPTION='⚠️ Last policy check: AT patch inactive; Download Mode allowed.'
      else
        BD_DESCRIPTION='❌ Last policy check: Download Mode blocked; AT patch inactive.'
      fi
      ;;
    starting) BD_DESCRIPTION='Checking policy after Android startup.' ;;
    read_failed) BD_DESCRIPTION='⚠️ Policy read failed. Tap Action to retry.' ;;
    unsupported) BD_DESCRIPTION='⚠️ Unsupported DMC policy. Patch not verified.' ;;
  esac
}
