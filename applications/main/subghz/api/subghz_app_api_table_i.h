#include <subghz/devices/cc1101_configs.h>

/*
 * SubGhz app private API table — exposes CC1101 preset register arrays
 * to the radio_device_cc1101_ext plugin when SubGhz runs as a FAP.
 */
static constexpr auto subghz_app_api_table = sort(create_array_t<sym_entry>(
    API_VARIABLE(subghz_device_cc1101_preset_ook_270khz_async_regs, const uint8_t[]),
    API_VARIABLE(subghz_device_cc1101_preset_ook_650khz_async_regs, const uint8_t[]),
    API_VARIABLE(subghz_device_cc1101_preset_2fsk_dev2_38khz_async_regs, const uint8_t[]),
    API_VARIABLE(subghz_device_cc1101_preset_2fsk_dev12khz_async_regs, const uint8_t[]),
    API_VARIABLE(subghz_device_cc1101_preset_2fsk_dev47_6khz_async_regs, const uint8_t[]),
    API_VARIABLE(subghz_device_cc1101_preset_msk_99_97kb_async_regs, const uint8_t[]),
    API_VARIABLE(subghz_device_cc1101_preset_gfsk_9_99kb_async_regs, const uint8_t[])));
