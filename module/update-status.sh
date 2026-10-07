#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 ducttape3

MODDIR=${0%/*}
. "$MODDIR/status.sh"
case "$1" in
  reset) BD_DESCRIPTION=$BD_DEFAULT ;;
  unavailable) BD_DESCRIPTION='⚠️ No live policy response. Tap Action to retry.' ;;
  inactive) BD_DESCRIPTION='Module inactive. Policy not verified.' ;;
  '') bd_description ;;
  *) exit 1 ;;
esac

BD_CACHE="$MODDIR/.status-description"
[ "$(cat "$BD_CACHE" 2>/dev/null)" = "$BD_DESCRIPTION" ] && exit 0

if [ -x /data/adb/ksu/bin/ksud ]; then
  # Keep the on-disk description neutral. Runtime overrides disappear on boot.
  KSU_MODULE=${MODDIR##*/}
  export KSU_MODULE
  /data/adb/ksu/bin/ksud module config set --temp override.description "$BD_DESCRIPTION" >/dev/null 2>&1 || exit 1
else
  # Other managers read module.prop. Positive snapshots are explicitly labelled
  # as the last policy check rather than claiming a permanent successful patch.
  TMPPROP="$MODDIR/.module.prop.status.$$"
  trap 'rm -f "$TMPPROP"' EXIT
  sed "s/^description=.*/description=$BD_DESCRIPTION/" "$MODDIR/module.prop" > "$TMPPROP" || exit 1
  chmod 0644 "$TMPPROP" && mv -f "$TMPPROP" "$MODDIR/module.prop" || exit 1
fi
printf '%s\n' "$BD_DESCRIPTION" > "$BD_CACHE"
