#!/bin/bash
# Host-side test of the OpenBootloader USB transport, build step.
#
# It cross-compiles the REAL sources under test
#   Middlewares/ST/OpenBootloader/Modules/USB/openbl_usb_cmd.c
#   Middlewares/ST/OpenBootloader/Util/openbl_util.c
# for an ARMv8-M core, together with a simulated NOR flash (stubs.c) and a driver that
# plays STM32CubeProgrammer (test_driver.c). run.py then executes the result in the
# Unicorn CPU emulator and prints the outcome.
#
# Reference run (OPENBL_REFERENCE=1, see test.sh --reference): the same tests, built with
# the UNTOUCHED sources of a STM32Cube_FW_N6 package instead:
#   OBL_USB_SRC=<package>/Middlewares/ST/OpenBootloader/Modules/USB/openbl_usb_cmd.c
#   OBL_UTIL_SRC=<package>/Middlewares/ST/OpenBootloader/Util/openbl_util.c
# default: the STM32Cube_FW_N6 package this project sits in. The driver then runs T0 only,
# the one case the ST OpenBootloader can pass: it proves that what the tests expect from
# the device is what the ST OpenBootloader does.
#
# Requirements: arm-none-eabi-gcc on the PATH, Python 3 with the "unicorn" package.
set -euo pipefail
cd "$(dirname "$0")"

PRJ="$(cd ../.. && pwd)"
MW="$PRJ/Middlewares/ST/OpenBootloader"
CUBE_FW="$(cd "$PRJ/../../../.." && pwd)"

OUT="build"
TEST_DEFS="-DOPENBL_USB_TEST_HOOKS"
USB_SRC="$MW/Modules/USB/openbl_usb_cmd.c"
UTIL_SRC="$MW/Util/openbl_util.c"
if [ "${OPENBL_REFERENCE:-0}" = "1" ]; then
  OUT="build_reference"
  TEST_DEFS="-DOPENBL_REFERENCE"
  USB_SRC="${OBL_USB_SRC:-$CUBE_FW/Middlewares/ST/OpenBootloader/Modules/USB/openbl_usb_cmd.c}"
  UTIL_SRC="${OBL_UTIL_SRC:-$CUBE_FW/Middlewares/ST/OpenBootloader/Util/openbl_util.c}"
  [ -f "$USB_SRC" ] || { echo "reference source not found: $USB_SRC" >&2; exit 1; }
  [ -f "$UTIL_SRC" ] || { echo "reference source not found: $UTIL_SRC" >&2; exit 1; }
fi

CC="arm-none-eabi-gcc"
SIZE="arm-none-eabi-size"

# The emulator has no Cortex-M55 model. The code under test is plain integer C, so it is
# built for the Cortex-M33 (same ARMv8-M mainline instruction set) for this test only.
CPU="-mcpu=cortex-m33 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard"
FLAGS="$CPU -Os -g3 -Wall -Werror -ffunction-sections -fdata-sections -std=gnu11 \
  --specs=nano.specs $TEST_DEFS \
  -I$MW/Core -I$MW/Modules/Mem -I$MW/Modules/USART -I$MW/Modules/USB -I$MW/Util \
  -I$MW/STM32_USB_Device_Library/Class/DFU/Inc -I$MW/STM32_USB_Device_Library/Core/Inc"

# The headers of the real modules pull in the HAL and the target configuration. Those are
# replaced by stubs.h: it is forced in front of every translation unit, and the empty
# headers of stubinc/ shadow the target ones (openbootloader_conf.h, openbl_mem.h, ...),
# whose content now comes from stubs.h.
STUB_HEADERS="openbootloader_conf.h openbl_mem.h otp_interface.h common_interface.h \
  usb_interface.h stm32n6xx_hal.h openbl_core.h"
rm -rf "$OUT" stubinc
mkdir -p "$OUT" stubinc
for h in $STUB_HEADERS; do printf '#pragma once\n' > "stubinc/$h"; done

INCL="-Istubinc -include stubs.h"
$CC $FLAGS $INCL -c crt0.c        -o "$OUT/crt0.o"
$CC $FLAGS $INCL -c stubs.c       -o "$OUT/stubs.o"
# the CHECK macro passes values to snprintf that the compiler cannot prove initialised
$CC $FLAGS $INCL -Wno-maybe-uninitialized -Wno-uninitialized -c test_driver.c -o "$OUT/test_driver.o"
$CC $FLAGS $INCL -c "$USB_SRC"  -o "$OUT/openbl_usb_cmd.o"
$CC $FLAGS $INCL -c "$UTIL_SRC" -o "$OUT/openbl_util.o"
# The descriptor test needs the real headers of the USB device library: no forced stubs
# for these (test_dfu_desc.c doubles the few USB core services the class calls). usbd_conf.h
# is the one of the target, with main.h replaced by dfu_inc/main.h so that no HAL is
# needed: the descriptors are therefore built with the production configuration.
DFU_INCL="-I. -Idfu_inc -I$PRJ/USB_Device/Target"
$CC $FLAGS $DFU_INCL -c "$MW/STM32_USB_Device_Library/Class/DFU/Src/usbd_dfu.c" -o "$OUT/usbd_dfu.o"
$CC $FLAGS $DFU_INCL -c "$MW/STM32_USB_Device_Library/Core/Src/usbd_ctlreq.c" -o "$OUT/usbd_ctlreq.o"
$CC $FLAGS $DFU_INCL -Wno-maybe-uninitialized -Wno-uninitialized -c test_dfu_desc.c -o "$OUT/test_dfu_desc.o"

$CC $CPU --specs=nano.specs --specs=nosys.specs -T test.ld -Wl,--gc-sections \
  -Wl,-Map="$OUT/test.map",--cref -Wl,--entry=Reset_Handler \
  "$OUT/crt0.o" "$OUT/stubs.o" "$OUT/test_driver.o" "$OUT/openbl_usb_cmd.o" "$OUT/openbl_util.o" \
  "$OUT/usbd_dfu.o" "$OUT/usbd_ctlreq.o" "$OUT/test_dfu_desc.o" \
  -Wl,--start-group -lc -lm -Wl,--end-group -o "$OUT/test.elf"

$SIZE "$OUT/test.elf"
