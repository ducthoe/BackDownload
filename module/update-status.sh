#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 ducttape3

MODDIR=${0%/*}
case "$1" in
  1) DESCRIPTION='✅ Patch successful' ;;
  0) DESCRIPTION='Unlocks Download Mode on Samsung devices.' ;;
  *) exit 1 ;;
esac

TMPPROP="$MODDIR/.module.prop.status.$$"
trap 'rm -f "$TMPPROP"' EXIT
if sed "s/^description=.*/description=$DESCRIPTION/" "$MODDIR/module.prop" > "$TMPPROP"; then
  chmod 0644 "$TMPPROP" && mv -f "$TMPPROP" "$MODDIR/module.prop"
fi

if [ -x /data/adb/ksu/bin/ksud ]; then
  KSU_MODULE=${MODDIR##*/}
  export KSU_MODULE
  /data/adb/ksu/bin/ksud module config set --temp override.description "$DESCRIPTION" >/dev/null 2>&1
fi
