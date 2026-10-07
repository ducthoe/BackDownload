#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 ducttape3

[ "$BOOTMODE" = true ] || abort "Install from the root manager while Android is running."
[ "$ARCH" = arm64 ] || abort "This build supports arm64-v8a only."

case "$(getprop ro.product.manufacturer)" in
  samsung|Samsung|SAMSUNG) ;;
  *) abort "This module targets Samsung's DMC service." ;;
esac

ui_print "backdownload by ducttape3"
ui_print "Soft Reboot (if jailbroken), or either Reboot Android"

set_perm "$MODPATH/zygisk/arm64-v8a.so" 0 0 0755
set_perm "$MODPATH/post-fs-data.sh" 0 0 0755
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/action.sh" 0 0 0755
set_perm "$MODPATH/status.sh" 0 0 0755
set_perm "$MODPATH/update-status.sh" 0 0 0755

# Clear a previous release's success override until the new worker is loaded.
if [ -x /data/adb/ksu/bin/ksud ]; then
  KSU_MODULE=dmc_at_zygisk
  export KSU_MODULE
  /data/adb/ksu/bin/ksud module config set --temp override.description 'Unlocks Download Mode on Samsung devices. Tap Action to check the policy.' >/dev/null 2>&1
fi
