#!/system/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 ducttape3

MODDIR=${0%/*}
sh "$MODDIR/update-status.sh" 0 >/dev/null 2>&1
