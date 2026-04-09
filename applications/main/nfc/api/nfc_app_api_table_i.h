#include "gallagher/gallagher_util.h"
#include "mosgortrans/mosgortrans_util.h"
#include "../nfc_app_i.h"
#include "../helpers/protocol_support/nfc_protocol_support_gui_common.h"
#include "../helpers/protocol_support/nfc_protocol_support_unlock_helper.h"
#include <nfc/nfc_device.h>
#include <nfc/nfc_poller.h>
#include <nfc/nfc_listener.h>
#include <nfc/protocols/mf_classic/mf_classic.h>
#include <nfc/protocols/mf_classic/mf_classic_poller_sync.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight.h>
#include <nfc/protocols/mf_desfire/mf_desfire.h>
#include <nfc/protocols/mf_plus/mf_plus.h>
#include <nfc/protocols/iso14443_3a/iso14443_3a.h>
#include <nfc/protocols/iso14443_3b/iso14443_3b.h>
#include <nfc/protocols/iso14443_4a/iso14443_4a.h>
#include <nfc/protocols/iso14443_4b/iso14443_4b.h>
#include <nfc/protocols/iso15693_3/iso15693_3.h>
#include <nfc/protocols/slix/slix.h>
#include <nfc/protocols/felica/felica.h>

/*
 * A list of app's private functions and objects to expose for plugins.
 * It is used to generate a table of symbols for import resolver to use.
 *
 * Because NFC/SubGhz run as MENUEXTERNAL FAPs (XIP), the protocol libraries
 * are bundled in the FAP, not the firmware.  Protocol and supported-card
 * plugins loaded at runtime resolve these symbols through the
 * CompositeApiResolver.
 */
static constexpr auto nfc_app_api_table = sort(create_array_t<sym_entry>(
    /* ---- Original app helpers ---- */
    API_METHOD(
        gallagher_deobfuscate_and_parse_credential,
        void,
        (GallagherCredential * credential, const uint8_t* cardholder_data_obfuscated)),
    API_VARIABLE(GALLAGHER_CARDAX_ASCII, const uint8_t*),
    API_METHOD(
        mosgortrans_parse_transport_block,
        bool,
        (const MfClassicBlock* block, FuriString* result)),
    API_METHOD(
        render_section_header,
        void,
        (FuriString * str,
         const char* name,
         uint8_t prefix_separator_cnt,
         uint8_t suffix_separator_cnt)),
    API_METHOD(
        nfc_append_filename_string_when_present,
        void,
        (NfcApp * instance, FuriString* string)),
    API_METHOD(nfc_protocol_support_common_submenu_callback, void, (void* context, uint32_t index)),
    API_METHOD(
        nfc_protocol_support_common_widget_callback,
        void,
        (GuiButtonType result, InputType type, void* context)),
    API_METHOD(nfc_protocol_support_common_on_enter_empty, void, (NfcApp * instance)),
    API_METHOD(
        nfc_protocol_support_common_on_event_empty,
        bool,
        (NfcApp * instance, SceneManagerEvent event)),
    API_METHOD(nfc_unlock_helper_setup_from_state, void, (NfcApp * instance)),
    API_METHOD(nfc_unlock_helper_card_detected_handler, void, (NfcApp * instance)),

    /* ---- NFC Device ---- */
    API_METHOD(nfc_device_alloc, NfcDevice*, ()),
    API_METHOD(nfc_device_free, void, (NfcDevice * instance)),
    API_METHOD(nfc_device_clear, void, (NfcDevice * instance)),
    API_METHOD(nfc_device_reset, void, (NfcDevice * instance)),
    API_METHOD(nfc_device_get_protocol, NfcProtocol, (const NfcDevice* instance)),
    API_METHOD(nfc_device_get_protocol_name, const char*, (NfcProtocol protocol)),
    API_METHOD(nfc_device_get_data, const NfcDeviceData*, (const NfcDevice* instance, NfcProtocol protocol)),
    API_METHOD(nfc_device_set_data, void, (NfcDevice * instance, NfcProtocol protocol, const NfcDeviceData* data)),
    API_METHOD(nfc_device_copy_data, void, (const NfcDevice* instance, NfcProtocol protocol, NfcDeviceData* data)),
    API_METHOD(nfc_device_get_name, const char*, (const NfcDevice* instance, NfcDeviceNameType name_type)),
    API_METHOD(nfc_device_get_uid, const uint8_t*, (const NfcDevice* instance, size_t* uid_len)),
    API_METHOD(nfc_device_set_uid, bool, (NfcDevice * instance, const uint8_t* uid, size_t uid_len)),
    API_METHOD(nfc_device_is_equal, bool, (const NfcDevice* instance, const NfcDevice* other)),
    API_METHOD(nfc_device_is_equal_data, bool, (const NfcDevice* instance, NfcProtocol protocol, const NfcDeviceData* data)),
    API_METHOD(nfc_device_set_loading_callback, void, (NfcDevice * instance, NfcLoadingCallback callback, void* context)),
    API_METHOD(nfc_device_save, bool, (NfcDevice * instance, const char* path)),
    API_METHOD(nfc_device_load, bool, (NfcDevice * instance, const char* path)),

    /* ---- NFC Poller ---- */
    API_METHOD(nfc_poller_alloc, NfcPoller*, (Nfc * nfc, NfcProtocol protocol)),
    API_METHOD(nfc_poller_free, void, (NfcPoller * instance)),
    API_METHOD(nfc_poller_start, void, (NfcPoller * instance, NfcGenericCallback callback, void* context)),
    API_METHOD(nfc_poller_start_ex, void, (NfcPoller * instance, NfcGenericCallbackEx callback, void* context)),
    API_METHOD(nfc_poller_stop, void, (NfcPoller * instance)),
    API_METHOD(nfc_poller_detect, bool, (NfcPoller * instance)),
    API_METHOD(nfc_poller_get_protocol, NfcProtocol, (const NfcPoller* instance)),
    API_METHOD(nfc_poller_get_data, const NfcDeviceData*, (const NfcPoller* instance)),

    /* ---- NFC Listener ---- */
    API_METHOD(nfc_listener_alloc, NfcListener*, (Nfc * nfc, NfcProtocol protocol, const NfcDeviceData* data)),
    API_METHOD(nfc_listener_free, void, (NfcListener * instance)),
    API_METHOD(nfc_listener_start, void, (NfcListener * instance, NfcGenericCallback callback, void* context)),
    API_METHOD(nfc_listener_stop, void, (NfcListener * instance)),
    API_METHOD(nfc_listener_get_protocol, NfcProtocol, (const NfcListener* instance)),
    API_METHOD(nfc_listener_get_data, const NfcDeviceData*, (const NfcListener* instance, NfcProtocol protocol)),

    /* ---- MF Classic ---- */
    API_METHOD(mf_classic_alloc, MfClassicData*, ()),
    API_METHOD(mf_classic_free, void, (MfClassicData * data)),
    API_METHOD(mf_classic_reset, void, (MfClassicData * data)),
    API_METHOD(mf_classic_copy, void, (MfClassicData * data, const MfClassicData* other)),
    API_METHOD(mf_classic_verify, bool, (MfClassicData * data, const FuriString* device_type)),
    API_METHOD(mf_classic_load, bool, (MfClassicData * data, FlipperFormat* ff, uint32_t version)),
    API_METHOD(mf_classic_save, bool, (const MfClassicData* data, FlipperFormat* ff)),
    API_METHOD(mf_classic_is_equal, bool, (const MfClassicData* data, const MfClassicData* other)),
    API_METHOD(mf_classic_get_uid, const uint8_t*, (const MfClassicData* data, size_t* uid_len)),
    API_METHOD(mf_classic_set_uid, bool, (MfClassicData * data, const uint8_t* uid, size_t uid_len)),
    API_METHOD(mf_classic_get_base_data, Iso14443_3aData*, (const MfClassicData* data)),
    API_METHOD(mf_classic_get_device_name, const char*, (const MfClassicData* data, NfcDeviceNameType name_type)),
    API_METHOD(mf_classic_get_first_block_num_of_sector, uint8_t, (uint8_t sector)),
    API_METHOD(mf_classic_get_blocks_num_in_sector, uint8_t, (uint8_t sector)),
    API_METHOD(mf_classic_get_total_sectors_num, uint8_t, (MfClassicType type)),
    API_METHOD(mf_classic_get_total_block_num, uint16_t, (MfClassicType type)),
    API_METHOD(mf_classic_get_sector_trailer_num_by_sector, uint8_t, (uint8_t sector)),
    API_METHOD(mf_classic_get_sector_trailer_num_by_block, uint8_t, (uint8_t block)),
    API_METHOD(mf_classic_get_sector_trailer_by_sector, MfClassicSectorTrailer*, (const MfClassicData* data, uint8_t sector_num)),
    API_METHOD(mf_classic_get_sector_by_block, uint8_t, (uint8_t block)),
    API_METHOD(mf_classic_is_key_found, bool, (const MfClassicData* data, uint8_t sector_num, MfClassicKeyType key_type)),
    API_METHOD(mf_classic_set_key_found, void, (MfClassicData * data, uint8_t sector_num, MfClassicKeyType key_type, uint64_t key)),
    API_METHOD(mf_classic_set_key_not_found, void, (MfClassicData * data, uint8_t sector_num, MfClassicKeyType key_type)),
    API_METHOD(mf_classic_get_key, MfClassicKey, (const MfClassicData* data, uint8_t sector_num, MfClassicKeyType key_type)),
    API_METHOD(mf_classic_is_block_read, bool, (const MfClassicData* data, uint8_t block_num)),
    API_METHOD(mf_classic_set_block_read, void, (MfClassicData * data, uint8_t block_num, MfClassicBlock* block_data)),
    API_METHOD(mf_classic_is_sector_read, bool, (const MfClassicData* data, uint8_t sector_num)),
    API_METHOD(mf_classic_is_sector_trailer, bool, (uint8_t block)),
    API_METHOD(mf_classic_is_card_read, bool, (const MfClassicData* data)),
    API_METHOD(mf_classic_is_value_block, bool, (MfClassicSectorTrailer * sec_tr, uint8_t block_num)),
    API_METHOD(mf_classic_block_to_value, bool, (const MfClassicBlock* block, int32_t* value, uint8_t* addr)),
    API_METHOD(mf_classic_value_to_block, void, (int32_t value, uint8_t addr, MfClassicBlock* block)),
    API_METHOD(mf_classic_set_sector_trailer_read, void, (MfClassicData * data, uint8_t block_num, MfClassicSectorTrailer* sec_tr)),
    API_METHOD(mf_classic_is_allowed_access_data_block, bool, (MfClassicSectorTrailer * sec_tr, uint8_t block_num, MfClassicKeyType key_type, MfClassicAction action)),
    API_METHOD(mf_classic_is_allowed_access, bool, (MfClassicData * data, uint8_t block_num, MfClassicKeyType key_type, MfClassicAction action)),
    API_METHOD(mf_classic_get_read_sectors_and_keys, void, (const MfClassicData* data, uint8_t* sectors_read, uint8_t* keys_found)),

    /* ---- MF Classic Poller Sync ---- */
    API_METHOD(mf_classic_poller_sync_detect_type, MfClassicError, (Nfc * nfc, MfClassicType* type)),
    API_METHOD(mf_classic_poller_sync_read, MfClassicError, (Nfc * nfc, const MfClassicDeviceKeys* keys, MfClassicData* data)),
    API_METHOD(mf_classic_poller_sync_read_block, MfClassicError, (Nfc * nfc, uint8_t block_num, MfClassicKey* key, MfClassicKeyType key_type, MfClassicBlock* data)),
    API_METHOD(mf_classic_poller_sync_write_block, MfClassicError, (Nfc * nfc, uint8_t block_num, MfClassicKey* key, MfClassicKeyType key_type, MfClassicBlock* data)),
    API_METHOD(mf_classic_poller_sync_auth, MfClassicError, (Nfc * nfc, uint8_t block_num, MfClassicKey* key, MfClassicKeyType key_type, MfClassicAuthContext* data)),
    API_METHOD(mf_classic_poller_sync_collect_nt, MfClassicError, (Nfc * nfc, uint8_t block_num, MfClassicKeyType key_type, MfClassicNt* nt)),
    API_METHOD(mf_classic_poller_sync_read_value, MfClassicError, (Nfc * nfc, uint8_t block_num, MfClassicKey* key, MfClassicKeyType key_type, int32_t* value)),
    API_METHOD(mf_classic_poller_sync_change_value, MfClassicError, (Nfc * nfc, uint8_t block_num, MfClassicKey* key, MfClassicKeyType key_type, int32_t data, int32_t* new_value)),

    /* ---- MF Ultralight ---- */
    API_METHOD(mf_ultralight_alloc, MfUltralightData*, ()),
    API_METHOD(mf_ultralight_free, void, (MfUltralightData * data)),
    API_METHOD(mf_ultralight_reset, void, (MfUltralightData * data)),
    API_METHOD(mf_ultralight_copy, void, (MfUltralightData * data, const MfUltralightData* other)),
    API_METHOD(mf_ultralight_verify, bool, (MfUltralightData * data, const FuriString* device_type)),
    API_METHOD(mf_ultralight_load, bool, (MfUltralightData * data, FlipperFormat* ff, uint32_t version)),
    API_METHOD(mf_ultralight_save, bool, (const MfUltralightData* data, FlipperFormat* ff)),
    API_METHOD(mf_ultralight_is_equal, bool, (const MfUltralightData* data, const MfUltralightData* other)),
    API_METHOD(mf_ultralight_get_device_name, const char*, (const MfUltralightData* data, NfcDeviceNameType name_type)),
    API_METHOD(mf_ultralight_get_uid, const uint8_t*, (const MfUltralightData* data, size_t* uid_len)),
    API_METHOD(mf_ultralight_set_uid, bool, (MfUltralightData * data, const uint8_t* uid, size_t uid_len)),
    API_METHOD(mf_ultralight_get_base_data, Iso14443_3aData*, (const MfUltralightData* data)),
    API_METHOD(mf_ultralight_get_pages_total, uint16_t, (MfUltralightType type)),
    API_METHOD(mf_ultralight_get_type_by_version, MfUltralightType, (MfUltralightVersion * version)),
    API_METHOD(mf_ultralight_get_config_page_num, uint16_t, (MfUltralightType type)),
    API_METHOD(mf_ultralight_support_feature, bool, (const uint32_t feature_set, const uint32_t features_to_check)),
    API_METHOD(mf_ultralight_get_config_page, bool, (const MfUltralightData* data, MfUltralightConfigPages** config)),
    API_METHOD(mf_ultralight_is_all_data_read, bool, (const MfUltralightData* data)),
    API_METHOD(mf_ultralight_detect_protocol, bool, (const Iso14443_3aData* iso14443_3a_data)),
    API_METHOD(mf_ultralight_is_counter_configured, bool, (const MfUltralightData* data)),

    /* ---- MF DESFire ---- */
    API_METHOD(mf_desfire_alloc, MfDesfireData*, ()),
    API_METHOD(mf_desfire_free, void, (MfDesfireData * data)),
    API_METHOD(mf_desfire_reset, void, (MfDesfireData * data)),
    API_METHOD(mf_desfire_copy, void, (MfDesfireData * data, const MfDesfireData* other)),
    API_METHOD(mf_desfire_verify, bool, (MfDesfireData * data, const FuriString* device_type)),
    API_METHOD(mf_desfire_load, bool, (MfDesfireData * data, FlipperFormat* ff, uint32_t version)),
    API_METHOD(mf_desfire_save, bool, (const MfDesfireData* data, FlipperFormat* ff)),
    API_METHOD(mf_desfire_is_equal, bool, (const MfDesfireData* data, const MfDesfireData* other)),
    API_METHOD(mf_desfire_get_device_name, const char*, (const MfDesfireData* data, NfcDeviceNameType name_type)),
    API_METHOD(mf_desfire_get_uid, const uint8_t*, (const MfDesfireData* data, size_t* uid_len)),
    API_METHOD(mf_desfire_set_uid, bool, (MfDesfireData * data, const uint8_t* uid, size_t uid_len)),
    API_METHOD(mf_desfire_get_base_data, Iso14443_4aData*, (const MfDesfireData* data)),
    API_METHOD(mf_desfire_get_application, const MfDesfireApplication*, (const MfDesfireData* data, const MfDesfireApplicationId* app_id)),
    API_METHOD(mf_desfire_get_file_settings, const MfDesfireFileSettings*, (const MfDesfireApplication* data, const MfDesfireFileId* file_id)),
    API_METHOD(mf_desfire_get_file_data, const MfDesfireFileData*, (const MfDesfireApplication* data, const MfDesfireFileId* file_id)),

    /* ---- MF Plus ---- */
    API_METHOD(mf_plus_alloc, MfPlusData*, ()),
    API_METHOD(mf_plus_free, void, (MfPlusData * data)),
    API_METHOD(mf_plus_reset, void, (MfPlusData * data)),
    API_METHOD(mf_plus_copy, void, (MfPlusData * data, const MfPlusData* other)),
    API_METHOD(mf_plus_is_equal, bool, (const MfPlusData* data, const MfPlusData* other)),
    API_METHOD(mf_plus_get_device_name, const char*, (const MfPlusData* data, NfcDeviceNameType name_type)),
    API_METHOD(mf_plus_get_uid, const uint8_t*, (const MfPlusData* data, size_t* uid_len)),
    API_METHOD(mf_plus_set_uid, bool, (MfPlusData * data, const uint8_t* uid, size_t uid_len)),
    API_METHOD(mf_plus_get_base_data, Iso14443_4aData*, (const MfPlusData* data)),

    /* ---- ISO14443-3A ---- */
    API_METHOD(iso14443_3a_alloc, Iso14443_3aData*, ()),
    API_METHOD(iso14443_3a_free, void, (Iso14443_3aData * data)),
    API_METHOD(iso14443_3a_reset, void, (Iso14443_3aData * data)),
    API_METHOD(iso14443_3a_copy, void, (Iso14443_3aData * data, const Iso14443_3aData* other)),
    API_METHOD(iso14443_3a_is_equal, bool, (const Iso14443_3aData* data, const Iso14443_3aData* other)),
    API_METHOD(iso14443_3a_get_device_name, const char*, (const Iso14443_3aData* data, NfcDeviceNameType name_type)),
    API_METHOD(iso14443_3a_get_uid, const uint8_t*, (const Iso14443_3aData* data, size_t* uid_len)),
    API_METHOD(iso14443_3a_set_uid, bool, (Iso14443_3aData * data, const uint8_t* uid, size_t uid_len)),
    API_METHOD(iso14443_3a_supports_iso14443_4, bool, (const Iso14443_3aData* data)),
    API_METHOD(iso14443_3a_get_cuid, uint32_t, (const Iso14443_3aData* data)),
    API_METHOD(iso14443_3a_get_sak, uint8_t, (const Iso14443_3aData* data)),
    API_METHOD(iso14443_3a_set_sak, void, (Iso14443_3aData * data, uint8_t sak)),

    /* ---- ISO14443-3B ---- */
    API_METHOD(iso14443_3b_alloc, Iso14443_3bData*, ()),
    API_METHOD(iso14443_3b_free, void, (Iso14443_3bData * data)),
    API_METHOD(iso14443_3b_reset, void, (Iso14443_3bData * data)),
    API_METHOD(iso14443_3b_copy, void, (Iso14443_3bData * data, const Iso14443_3bData* other)),
    API_METHOD(iso14443_3b_is_equal, bool, (const Iso14443_3bData* data, const Iso14443_3bData* other)),
    API_METHOD(iso14443_3b_get_device_name, const char*, (const Iso14443_3bData* data, NfcDeviceNameType name_type)),
    API_METHOD(iso14443_3b_get_uid, const uint8_t*, (const Iso14443_3bData* data, size_t* uid_len)),
    API_METHOD(iso14443_3b_set_uid, bool, (Iso14443_3bData * data, const uint8_t* uid, size_t uid_len)),

    /* ---- ISO14443-4A ---- */
    API_METHOD(iso14443_4a_alloc, Iso14443_4aData*, ()),
    API_METHOD(iso14443_4a_free, void, (Iso14443_4aData * data)),
    API_METHOD(iso14443_4a_reset, void, (Iso14443_4aData * data)),
    API_METHOD(iso14443_4a_copy, void, (Iso14443_4aData * data, const Iso14443_4aData* other)),
    API_METHOD(iso14443_4a_is_equal, bool, (const Iso14443_4aData* data, const Iso14443_4aData* other)),
    API_METHOD(iso14443_4a_get_device_name, const char*, (const Iso14443_4aData* data, NfcDeviceNameType name_type)),
    API_METHOD(iso14443_4a_get_uid, const uint8_t*, (const Iso14443_4aData* data, size_t* uid_len)),
    API_METHOD(iso14443_4a_set_uid, bool, (Iso14443_4aData * data, const uint8_t* uid, size_t uid_len)),
    API_METHOD(iso14443_4a_get_base_data, Iso14443_3aData*, (const Iso14443_4aData* data)),

    /* ---- ISO14443-4B ---- */
    API_METHOD(iso14443_4b_alloc, Iso14443_4bData*, ()),
    API_METHOD(iso14443_4b_free, void, (Iso14443_4bData * data)),
    API_METHOD(iso14443_4b_reset, void, (Iso14443_4bData * data)),
    API_METHOD(iso14443_4b_copy, void, (Iso14443_4bData * data, const Iso14443_4bData* other)),
    API_METHOD(iso14443_4b_is_equal, bool, (const Iso14443_4bData* data, const Iso14443_4bData* other)),
    API_METHOD(iso14443_4b_get_device_name, const char*, (const Iso14443_4bData* data, NfcDeviceNameType name_type)),
    API_METHOD(iso14443_4b_get_uid, const uint8_t*, (const Iso14443_4bData* data, size_t* uid_len)),
    API_METHOD(iso14443_4b_set_uid, bool, (Iso14443_4bData * data, const uint8_t* uid, size_t uid_len)),
    API_METHOD(iso14443_4b_get_base_data, Iso14443_3bData*, (const Iso14443_4bData* data)),

    /* ---- ISO15693-3 ---- */
    API_METHOD(iso15693_3_alloc, Iso15693_3Data*, ()),
    API_METHOD(iso15693_3_free, void, (Iso15693_3Data * data)),
    API_METHOD(iso15693_3_reset, void, (Iso15693_3Data * data)),
    API_METHOD(iso15693_3_copy, void, (Iso15693_3Data * data, const Iso15693_3Data* other)),
    API_METHOD(iso15693_3_is_equal, bool, (const Iso15693_3Data* data, const Iso15693_3Data* other)),
    API_METHOD(iso15693_3_get_device_name, const char*, (const Iso15693_3Data* data, NfcDeviceNameType name_type)),
    API_METHOD(iso15693_3_get_uid, const uint8_t*, (const Iso15693_3Data* data, size_t* uid_len)),
    API_METHOD(iso15693_3_set_uid, bool, (Iso15693_3Data * data, const uint8_t* uid, size_t uid_len)),
    API_METHOD(iso15693_3_get_manufacturer_id, uint8_t, (const Iso15693_3Data* data)),
    API_METHOD(iso15693_3_get_block_count, uint16_t, (const Iso15693_3Data* data)),
    API_METHOD(iso15693_3_get_block_size, uint8_t, (const Iso15693_3Data* data)),
    API_METHOD(iso15693_3_get_block_data, const uint8_t*, (const Iso15693_3Data* data, uint8_t block_index)),
    API_METHOD(iso15693_3_is_block_locked, bool, (const Iso15693_3Data* data, uint8_t block_index)),

    /* ---- SLIX ---- */
    API_METHOD(slix_alloc, SlixData*, ()),
    API_METHOD(slix_free, void, (SlixData * data)),
    API_METHOD(slix_reset, void, (SlixData * data)),
    API_METHOD(slix_copy, void, (SlixData * data, const SlixData* other)),
    API_METHOD(slix_is_equal, bool, (const SlixData* data, const SlixData* other)),
    API_METHOD(slix_get_device_name, const char*, (const SlixData* data, NfcDeviceNameType name_type)),
    API_METHOD(slix_get_uid, const uint8_t*, (const SlixData* data, size_t* uid_len)),
    API_METHOD(slix_set_uid, bool, (SlixData * data, const uint8_t* uid, size_t uid_len)),
    API_METHOD(slix_get_base_data, const Iso15693_3Data*, (const SlixData* data)),
    API_METHOD(slix_get_type, SlixType, (const SlixData* data)),
    API_METHOD(slix_get_password, SlixPassword, (const SlixData* data, SlixPasswordType password_type)),
    API_METHOD(slix_get_counter, uint16_t, (const SlixData* data)),
    API_METHOD(slix_is_privacy_mode, bool, (const SlixData* data)),
    API_METHOD(slix_is_block_protected, bool, (const SlixData* data, SlixPasswordType password_type, uint8_t block_num)),
    API_METHOD(slix_is_counter_increment_protected, bool, (const SlixData* data)),
    API_METHOD(slix_type_has_features, bool, (SlixType slix_type, SlixTypeFeatures features)),
    API_METHOD(slix_type_supports_password, bool, (SlixType slix_type, SlixPasswordType password_type)),

    /* ---- FeliCa ---- */
    API_METHOD(felica_alloc, FelicaData*, ()),
    API_METHOD(felica_free, void, (FelicaData * data)),
    API_METHOD(felica_reset, void, (FelicaData * data)),
    API_METHOD(felica_copy, void, (FelicaData * data, const FelicaData* other)),
    API_METHOD(felica_is_equal, bool, (const FelicaData* data, const FelicaData* other)),
    API_METHOD(felica_get_device_name, const char*, (const FelicaData* data, NfcDeviceNameType name_type)),
    API_METHOD(felica_get_uid, const uint8_t*, (const FelicaData* data, size_t* uid_len)),
    API_METHOD(felica_set_uid, bool, (FelicaData * data, const uint8_t* uid, size_t uid_len))));
