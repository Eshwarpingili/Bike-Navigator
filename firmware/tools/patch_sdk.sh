#!/usr/bin/env bash
# Overlay Ai-Thinker's AiPi board support (ST7796 "Ai" LCD driver, CHSC6540 touch,
# LVGL port with rotation) onto the Ai-M6X SDK. Same steps as AiPi-Open-Kits'
# update_sdk.sh, minus its older utils/CMakeLists.txt, which would break this SDK.
#
# Usage: tools/patch_sdk.sh [SDK_DIR] [AIPI_OPEN_KITS_DIR]
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SDK="${1:-$HERE/../../third_party/aithinker_Ai-M6X_SDK}"
KITS="${2:-$HERE/../../third_party/AiPi-Open-Kits}"
BSP="$KITS/bl61x_SDK/AiPi_bsp"

[ -d "$SDK/bsp/common" ] || { echo "SDK not found at $SDK" >&2; exit 1; }
[ -d "$BSP/common" ] || { echo "AiPi_bsp not found at $BSP" >&2; exit 1; }

if [ -f "$SDK/.aipi_patched" ]; then
    echo "SDK already patched ($SDK)"
    exit 0
fi

cp -r "$SDK/bsp/common" "$SDK/bsp/common.orig"
cp -r "$BSP/common/lcd" "$SDK/bsp/common/"
cp -r "$BSP/common/touch" "$SDK/bsp/common/"
cp "$BSP/common/CMakeLists.txt" "$SDK/bsp/common/CMakeLists.txt"
cp "$BSP/lvgl_port/"*.c "$SDK/components/graphics/lvgl/port/"

# The port's LCD_ROTATED_* branches invert both touch axes, which is right for
# this panel: its touch origin is opposite its pixel origin, and LVGL applies
# the display rotation separately (lv_indev.c). Measured, not assumed - a tap on
# the control row reports panel x 194 where the row sits at 239 - 194 = 45.
# Left as the vendor ships it.

date > "$SDK/.aipi_patched"
echo "Patched $SDK"
