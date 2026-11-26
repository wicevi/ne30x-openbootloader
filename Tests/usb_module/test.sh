#!/bin/bash
# Host-side test of the OpenBootloader USB transport: build, then run in the Unicorn emulator.
#
# The real openbl_usb_cmd.c and openbl_util.c are executed against a simulated NOR flash,
# driven the way STM32CubeProgrammer drives the device. See test_driver.c for the cases.
#
#   ./test.sh                build if needed, then run
#   ./test.sh -f             force a rebuild
#   ./test.sh --layout <dir> also play a flash layout built by a host tool (T11)
#   ./test.sh --reference    T0 only, against the UNTOUCHED ST sources of the STM32Cube_FW_N6
#                            package (see build.sh): the check that the tests expect of the
#                            device what the ST OpenBootloader does
set -euo pipefail
cd "$(dirname "$0")"

force=0
reference=0
run_args=()
for arg in "$@"; do
  case "$arg" in
    -f) force=1 ;;
    --reference) reference=1 ;;
    *) run_args+=("$arg") ;;
  esac
done

if [ "$reference" = 1 ]; then
  # always rebuilt: the reference sources are outside this tree, their changes are not watched
  OUT="build_reference"
  force=1
else
  OUT="build"
fi

if [ "$force" = 1 ] || [ ! -f "$OUT/test.elf" ] \
   || [ -n "$(find ../Middlewares/ST/OpenBootloader/Modules/USB ../Middlewares/ST/OpenBootloader/Util \
                    stubs.c stubs.h test_driver.c crt0.c test.ld build.sh -newer "$OUT/test.elf" -print -quit 2>/dev/null)" ]; then
  # the newlib stubs of nosys.specs warn about every system call: not shown
  log="$(mktemp)"
  if ! OPENBL_REFERENCE=$reference bash build.sh >"$log" 2>&1; then
    cat "$log"
    rm -f "$log"
    exit 1
  fi
  grep -v -e 'is not implemented and will always fail' -e 'does not take linker garbage collection' \
          -e '^(.text' -e ": in function \`_\(close\|lseek\|read\|write\)_r'" "$log" || true
  rm -f "$log"
fi

PYTHON="${PYTHON:-python}"
# the ELF is always given: a run must not pick up the build of the other flavour by default
exec "$PYTHON" run.py --elf "$OUT/test.elf" "${run_args[@]}"
