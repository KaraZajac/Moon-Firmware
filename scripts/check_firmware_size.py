#!/usr/bin/env python3
"""CI gate on runtime free-flash headroom.

Wraps arm-none-eabi-size to read .free_flash from a firmware ELF, then
converts the linker-visible value to the runtime value the XIP loader
actually sees (subtracting the BLE radio stack's reserved region at the
top of flash, set by the SFSA register). Compares against configurable
thresholds.

The linker's .free_flash section spans from the end of firmware code to
the end of flash (0x08100000). The BLE Full stack reserves the top N KB
via SFSA, so the runtime free region is smaller:

    runtime_free = linker_free - BLE_STACK_RESERVED

Exit codes:
  0  free_flash >= --warn-below
  0 with warning  --hard-floor <= free_flash < --warn-below
  1  free_flash < --hard-floor

Example:
    python3 scripts/check_firmware_size.py build/f7-C/firmware.elf \\
        --hard-floor 270336 --warn-below 286720
"""

import argparse
import subprocess
import sys

# Bytes reserved at the top of flash by the BLE Full stack firmware
# (stm32wb5x_BLE_Stack_full_fw.bin v1.20.0). Observed empirically from a
# booted device: linker `.free_flash` 489328 B vs. runtime free 282624 B.
# Update this if the radio stack version changes.
BLE_STACK_RESERVED = 205752

SECTIONS_TO_REPORT = (".text", ".rodata", ".data", ".bss", ".free_flash")


def read_elf_sections(elf_path: str) -> dict:
    """Run arm-none-eabi-size -A and return {section: size_bytes}."""
    try:
        raw = subprocess.check_output(
            ["arm-none-eabi-size", "-A", elf_path], shell=False
        )
    except FileNotFoundError:
        print("error: arm-none-eabi-size not in PATH", file=sys.stderr)
        sys.exit(2)
    except subprocess.CalledProcessError as e:
        print(f"error: arm-none-eabi-size failed: {e}", file=sys.stderr)
        sys.exit(2)

    sections = {}
    for line in raw.decode("utf-8").splitlines():
        parts = line.split()
        if len(parts) != 3:
            continue
        name, size, _addr = parts
        if name in SECTIONS_TO_REPORT:
            sections[name] = int(size)
    return sections


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("elf", help="path to firmware.elf")
    ap.add_argument(
        "--hard-floor",
        type=int,
        default=270336,  # 264 KB; SubGHz needs 258 KB + 6 KB cushion
        help="Fail the build if runtime free flash drops below this (bytes)",
    )
    ap.add_argument(
        "--warn-below",
        type=int,
        default=286720,  # 280 KB; soft target
        help="Warn if runtime free flash is below this (bytes)",
    )
    args = ap.parse_args()

    sections = read_elf_sections(args.elf)

    print("Firmware sections:")
    for name in SECTIONS_TO_REPORT:
        if name in sections:
            kb = sections[name] / 1024
            print(f"  {name:<12} {sections[name]:>8} ({kb:6.2f} KB)")

    linker_free = sections.get(".free_flash")
    if linker_free is None:
        print("error: .free_flash section missing from ELF", file=sys.stderr)
        return 2

    runtime_free = linker_free - BLE_STACK_RESERVED
    print(
        f"\nRuntime free flash (after BLE stack reserves {BLE_STACK_RESERVED} B):"
        f" {runtime_free} B ({runtime_free / 1024:.2f} KB)"
    )
    print(f"  hard floor:  {args.hard_floor} B ({args.hard_floor / 1024:.2f} KB)")
    print(f"  soft target: {args.warn_below} B ({args.warn_below / 1024:.2f} KB)")

    if runtime_free < args.hard_floor:
        shortfall = args.hard_floor - runtime_free
        print(
            f"\nFAIL: runtime free flash {runtime_free / 1024:.1f} KB is below "
            f"hard floor {args.hard_floor / 1024:.1f} KB (shortfall {shortfall} B). "
            f"Large FAPs (SubGHz, NFC) will fail to load.",
            file=sys.stderr,
        )
        return 1

    if runtime_free < args.warn_below:
        cushion = runtime_free - args.hard_floor
        print(
            f"\nWARN: runtime free flash {runtime_free / 1024:.1f} KB is below "
            f"soft target {args.warn_below / 1024:.1f} KB. "
            f"{cushion} B of cushion above the hard floor.",
            file=sys.stderr,
        )
        # Still exit 0 — warnings do not fail the build.
        return 0

    print("\nOK: runtime free flash within budget.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
