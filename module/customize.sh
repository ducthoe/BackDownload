#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 ducttape3

[ "$BOOTMODE" = true ] || abort "Install from the root manager while Android is running."
[ "$ARCH" = arm64 ] || abort "This build supports arm64-v8a only."
[ "$API" -ge 35 ] || abort "This build targets Samsung Android 15 or newer."

case "$(getprop ro.product.manufacturer)" in
  samsung|Samsung|SAMSUNG) ;;
  *) abort "This module targets Samsung's DMC service." ;;
esac

ui_print "backdownload by ducttape3"
ui_print "Requires a Zygisk provider with system_server injection (API v4)."
ui_print "Reboot Android normally after installation."

set_perm "$MODPATH/zygisk/arm64-v8a.so" 0 0 0755
