#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 ducttape3

MODDIR=${0%/*}
rm -f "$MODDIR/.status-description"
sh "$MODDIR/update-status.sh" reset >/dev/null 2>&1
