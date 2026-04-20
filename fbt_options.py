from pathlib import Path
import posixpath
import os

# For more details on these options, run 'fbt -h'

FIRMWARE_ORIGIN = "Moon"

# Default hardware target
TARGET_HW = 7

# Optimization flags
## Optimize for size
COMPACT = 1
## Optimize for debugging experience
DEBUG = 0

# Suffix to add to files when building distribution
# If OS environment has DIST_SUFFIX set, it will be used instead

if not os.environ.get("DIST_SUFFIX"):
    # Check scripts/get_env.py to mirror CI naming
    def git(*args):
        import subprocess

        return (
            subprocess.check_output(["git", *args], stderr=subprocess.DEVNULL)
            .decode()
            .strip()
        )

    try:
        # For tags, dist name is just the tag name: moon-(ver)
        DIST_SUFFIX = git("describe", "--tags", "--abbrev=0", "--exact-match")
    except Exception:
        # If not a tag, dist name is: moon-(branch)-(commmit)
        branch_name = git("rev-parse", "--abbrev-ref", "HEAD").removeprefix("moon-")
        commit_sha = git("rev-parse", "HEAD")[:8]
        DIST_SUFFIX = f"moon-{branch_name}-{commit_sha}"
    # Dist name is only for naming of output files
    DIST_SUFFIX = DIST_SUFFIX.replace("/", "-")
    # Instead, FW version uses tag name (moon-xxx), or "moon-dev" if not a tag (see scripts/version.py)
    # You can get commit and branch info in firmware with appropriate version_get_*() calls

# Skip external apps by default
SKIP_EXTERNAL = False

# Appid's to include even when skipping externals
EXTRA_EXT_APPS = []

# Coprocessor firmware — match carter-0's proven approach:
# use ob_custradio.data which omits Core2 secure fields (SFSA, SBRSA,
# SNBRSA, SBRV, C2OPT) so FUS manages them during radio stack install.
COPRO_OB_DATA = "scripts/ob_custradio.data"

# Must match lib/stm32wb_copro version
COPRO_CUBE_VERSION = "1.20.0"

COPRO_CUBE_DIR = "lib/stm32wb_copro"

# BLE Full stack — central/scanning, GATT client, 8 connections.
# Note: ST's "full" stack is actually a modified Basic variant (labelled
# "BF = Basic Features" in their release notes) with PHY 2 Mb and
# additional-beacon re-added. It does NOT include L2CAP
# Connection-Oriented Channels or Extended Advertising — those only ship
# in "full_extended_fw.bin", which we avoid because extended advertising
# caused boot-loops for us in past testing.
COPRO_STACK_BIN = "stm32wb5x_BLE_Stack_full_fw.bin"
COPRO_STACK_TYPE = "ble_full"

# Leave 0 to let scripts automatically calculate it
COPRO_STACK_ADDR = "0x0"

# If you override COPRO_CUBE_DIR on commandline, override this as well
COPRO_STACK_BIN_DIR = posixpath.join(COPRO_CUBE_DIR, "firmware")

# Supported toolchain versions
# Also specify in scripts/ufbt/SConstruct
FBT_TOOLCHAIN_VERSIONS = (" 12.3.", " 13.2.")

OPENOCD_OPTS = [
    "-f",
    "interface/cmsis-dap.cfg",
    "-c",
    "transport select swd",
    "-f",
    "${FBT_DEBUG_DIR}/stm32wbx.cfg",
    "-c",
    "stm32wbx.cpu configure -rtos auto",
]

SVD_FILE = "${FBT_DEBUG_DIR}/STM32WB55_CM4.svd"

# Look for blackmagic probe on serial ports and local network
BLACKMAGIC = "auto"

# Application to start on boot
LOADER_AUTOSTART = ""

FIRMWARE_APPS = {
    "default": [
        # Svc
        "basic_services",
        # Apps
        "main_apps",
        "system_apps",
        # Settings
        "settings_apps",
    ],
    "unit_tests": [
        # Svc
        "basic_services",
        # Apps
        "main_apps",
        "system_apps",
        # Settings
        "settings_apps",
        # Tests
        "unit_tests",
    ],
    "unit_tests_min": [
        "basic_services",
        "updater_app",
        "radio_device_cc1101_ext",
        "unit_tests",
        "js_app",
        "infrared",
        "archive",
    ],
}

FIRMWARE_APP_SET = "default"

custom_options_fn = "fbt_options_local.py"

if Path(custom_options_fn).exists():
    exec(compile(Path(custom_options_fn).read_text(), custom_options_fn, "exec"))
