---
name: External App Migration Pattern
description: Technical pattern for moving Flipper apps from internal flash to SD card FAPs - symlinks, fap_private_libs, SDK header removal
type: project
---

Pattern for moving apps to external storage:

1. Change `apptype` from `FlipperAppType.APP` to `FlipperAppType.MENUEXTERNAL` in application.fam
2. Create symlinks in `app/lib/` pointing to `../../../../lib/<library>` for required libraries
3. Add `fap_private_libs` in the FAM to bundle those libraries into the FAP
4. Remove `sdk_headers` from the library's `SConscript` (decouples from firmware API)
5. Remove library from `targets/f7/target.json` firmware build
6. Update `targets/f7/api_symbols.csv` to remove exported symbols

**Apps migrated:** iButton, Infrared, LFRFID, SubGHz, JS Runner
**Libraries made private:** ibutton, infrared, lfrfid, mjs, one_wire

**Special case — SubGHz:** The old dual-app structure (subghz + subghz_fap proxy) was collapsed. Extended freq settings moved to a new `subghz_startup` STARTUP app so settings load even without the SubGHz FAP installed.

**Why:** Saves internal flash memory on the STM32WB55.
**How to apply:** Follow this same pattern when moving additional apps to external storage.
