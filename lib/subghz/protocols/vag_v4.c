#include "vag_v4.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include <lib/toolbox/manchester_decoder.h>
#include <string.h>

#define TAG "VAG_V4"

/*
 * VAG V4 (VW-4) Remote Keyless Entry, per:
 *   Oswald et al., "Breaking automotive remote keyless entry systems",
 *   CARDIS 2016 keynote. https://cardis.org/cardis2016/assets/data/161108-cardis-keynote.pdf
 *
 * Used in most VW Group vehicles from ~2010 onwards (Audi, VW, Seat, Skoda)
 * that pre-date the MQB (Golf 7) platform.
 *
 * PHY:   434.4 MHz, OOK/AM, Manchester encoded, 500/1000 us bit cells.
 * Frame: preamble + sync  (= the "start" field in the paper)
 *        UID     (32 bits)
 *        ctr     (24 bits)  - rolling counter
 *        btn'    ( 8 bits)  - button inside XTEA ciphertext
 *        btn     ( 8 bits)  - cleartext button
 * The 64 bits UID|ctr|btn' are encrypted on-air with XTEA under a
 * single manufacturer-wide key K4. The paper intentionally does not
 * publish K4, so this decoder exposes the ciphertext fields raw.
 */

static const SubGhzBlockConst subghz_protocol_vag_v4_const = {
    .te_short = 500,
    .te_long = 1000,
    .te_delta = 100,
    .min_count_bit_for_found = 80,
};

#define VAG_V4_TE_SHORT     500u
#define VAG_V4_TE_LONG      1000u
#define VAG_V4_TE_DELTA     100u
#define VAG_V4_LONG_DELTA   200u
#define VAG_V4_SYNC         750u
#define VAG_V4_SYNC_DELTA   150u
#define VAG_V4_PREAMBLE_MIN 31u
#define VAG_V4_SYNC_PAIRS   3u
#define VAG_V4_PAYLOAD_BITS 80u

typedef struct SubGhzProtocolDecoderVAG_V4 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint64_t payload_hi; // upper bits shifted in
    uint64_t payload_lo;
    uint16_t bit_count;
    uint16_t preamble_count;
    uint8_t sync_pairs;
    ManchesterState manchester_state;

    uint32_t uid;
    uint32_t ctr;
    uint8_t btn_enc;
    uint8_t btn;
} SubGhzProtocolDecoderVAG_V4;

typedef enum {
    VAGV4StepReset = 0,
    VAGV4StepPreamble,
    // After preamble terminator long-high, expect a short low (500 us)
    VAGV4StepSyncGap,
    // Expect a 750 us high (start of a sync pair)
    VAGV4StepSyncHigh,
    // Expect a 750 us low (end of a sync pair)
    VAGV4StepSyncLow,
    VAGV4StepData,
} VAGV4DecoderStep;

typedef struct SubGhzProtocolEncoderVAG_V4 {
    SubGhzProtocolEncoderBase base;
    SubGhzBlockGeneric generic;

    uint64_t payload_hi;
    uint64_t payload_lo;

    size_t repeat;
    size_t front;
    size_t size_upload;
    LevelDuration* upload;
    bool is_running;
} SubGhzProtocolEncoderVAG_V4;

const SubGhzProtocolDecoder subghz_protocol_vag_v4_decoder = {
    .alloc = subghz_protocol_decoder_vag_v4_alloc,
    .free = subghz_protocol_decoder_vag_v4_free,
    .feed = subghz_protocol_decoder_vag_v4_feed,
    .reset = subghz_protocol_decoder_vag_v4_reset,
    .get_hash_data = subghz_protocol_decoder_vag_v4_get_hash_data,
    .serialize = subghz_protocol_decoder_vag_v4_serialize,
    .deserialize = subghz_protocol_decoder_vag_v4_deserialize,
    .get_string = subghz_protocol_decoder_vag_v4_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_vag_v4_encoder = {
    .alloc = subghz_protocol_encoder_vag_v4_alloc,
    .free = subghz_protocol_encoder_vag_v4_free,
    .deserialize = subghz_protocol_encoder_vag_v4_deserialize,
    .stop = subghz_protocol_encoder_vag_v4_stop,
    .yield = subghz_protocol_encoder_vag_v4_yield,
};

const SubGhzProtocol subghz_protocol_vag_v4 = {
    .name = VAG_V4_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_vag_v4_decoder,
    .encoder = &subghz_protocol_vag_v4_encoder,
};

static void vag_v4_split_payload(SubGhzProtocolDecoderVAG_V4* instance) {
    // payload_hi holds the upper 16 bits (bits 64..79 of the 80-bit frame),
    // payload_lo holds the lower 64 bits (bits 0..63).
    uint64_t hi = instance->payload_hi & 0xFFFF;
    uint64_t lo = instance->payload_lo;

    instance->uid = (uint32_t)((hi << 16) | ((lo >> 48) & 0xFFFF));
    instance->ctr = (uint32_t)((lo >> 24) & 0xFFFFFFu);
    instance->btn_enc = (uint8_t)((lo >> 16) & 0xFF);
    instance->btn = (uint8_t)(lo & 0xFF);
    // bits [8..15] of lo are unused spacer/flag in some variants;
    // we don't claim anything about them here.
}

void* subghz_protocol_decoder_vag_v4_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderVAG_V4* instance = malloc(sizeof(SubGhzProtocolDecoderVAG_V4));
    furi_check(instance);
    memset(instance, 0, sizeof(*instance));
    instance->base.protocol = &subghz_protocol_vag_v4;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_vag_v4_free(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderVAG_V4* instance = context;
    free(instance);
}

void subghz_protocol_decoder_vag_v4_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderVAG_V4* instance = context;
    instance->decoder.parser_step = VAGV4StepReset;
    instance->payload_hi = 0;
    instance->payload_lo = 0;
    instance->bit_count = 0;
    instance->preamble_count = 0;
    instance->sync_pairs = 0;
    instance->uid = 0;
    instance->ctr = 0;
    instance->btn = 0;
    instance->btn_enc = 0;
}

static void vag_v4_shift_bit(SubGhzProtocolDecoderVAG_V4* instance, bool bit_value) {
    uint64_t carry = (instance->payload_lo >> 63) & 1ULL;
    instance->payload_lo = (instance->payload_lo << 1) | (bit_value ? 1ULL : 0ULL);
    instance->payload_hi = (instance->payload_hi << 1) | carry;
    instance->bit_count++;
}

void subghz_protocol_decoder_vag_v4_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderVAG_V4* instance = context;

    switch(instance->decoder.parser_step) {
    case VAGV4StepReset:
        if(!level) break;
        if(DURATION_DIFF(duration, VAG_V4_TE_SHORT) < VAG_V4_TE_DELTA) {
            instance->decoder.te_last = duration;
            instance->preamble_count = 0;
            instance->sync_pairs = 0;
            instance->payload_hi = 0;
            instance->payload_lo = 0;
            instance->bit_count = 0;
            manchester_advance(
                instance->manchester_state,
                ManchesterEventReset,
                &instance->manchester_state,
                NULL);
            instance->decoder.parser_step = VAGV4StepPreamble;
        }
        break;

    case VAGV4StepPreamble:
        if(!level) {
            if(DURATION_DIFF(duration, VAG_V4_TE_SHORT) < VAG_V4_TE_DELTA &&
               DURATION_DIFF(instance->decoder.te_last, VAG_V4_TE_SHORT) < VAG_V4_TE_DELTA) {
                instance->decoder.te_last = duration;
                instance->preamble_count++;
            } else {
                instance->decoder.parser_step = VAGV4StepReset;
            }
            break;
        }
        // high pulse: either another preamble pulse, or the long pulse that
        // terminates preamble and leads into the 3 sync pairs of 750 us.
        if(instance->preamble_count < VAG_V4_PREAMBLE_MIN) {
            if(DURATION_DIFF(duration, VAG_V4_TE_SHORT) < VAG_V4_TE_DELTA) {
                instance->decoder.te_last = duration;
            } else {
                instance->decoder.parser_step = VAGV4StepReset;
            }
            break;
        }
        if(DURATION_DIFF(duration, VAG_V4_TE_LONG) < VAG_V4_LONG_DELTA) {
            instance->decoder.te_last = duration;
            instance->decoder.parser_step = VAGV4StepSyncGap;
        } else {
            instance->decoder.parser_step = VAGV4StepReset;
        }
        break;

    case VAGV4StepSyncGap:
        // Expect the short 500 us low that follows the preamble-terminator
        // long high, before the first 750/750 sync pair.
        if(!level && DURATION_DIFF(duration, VAG_V4_TE_SHORT) < VAG_V4_TE_DELTA) {
            instance->decoder.te_last = duration;
            instance->decoder.parser_step = VAGV4StepSyncHigh;
        } else {
            instance->decoder.parser_step = VAGV4StepReset;
        }
        break;

    case VAGV4StepSyncHigh:
        if(level && DURATION_DIFF(duration, VAG_V4_SYNC) < VAG_V4_SYNC_DELTA) {
            instance->decoder.te_last = duration;
            instance->decoder.parser_step = VAGV4StepSyncLow;
        } else {
            instance->decoder.parser_step = VAGV4StepReset;
        }
        break;

    case VAGV4StepSyncLow:
        if(!level && DURATION_DIFF(duration, VAG_V4_SYNC) < VAG_V4_SYNC_DELTA &&
           DURATION_DIFF(instance->decoder.te_last, VAG_V4_SYNC) < VAG_V4_SYNC_DELTA) {
            instance->sync_pairs++;
            instance->decoder.te_last = duration;
            if(instance->sync_pairs >= VAG_V4_SYNC_PAIRS) {
                // Prime the payload with a leading 1 bit, mirroring the
                // proven VAG T34 decoder (the long-high preceding the sync
                // pairs accounts for the first Manchester-1 bit cell).
                instance->payload_hi = 0;
                instance->payload_lo = 1;
                instance->bit_count = 1;
                manchester_advance(
                    instance->manchester_state,
                    ManchesterEventReset,
                    &instance->manchester_state,
                    NULL);
                instance->decoder.parser_step = VAGV4StepData;
            } else {
                instance->decoder.parser_step = VAGV4StepSyncHigh;
            }
        } else {
            instance->decoder.parser_step = VAGV4StepReset;
        }
        break;

    case VAGV4StepData: {
        ManchesterEvent event = ManchesterEventReset;
        bool got_pulse = false;

        if(DURATION_DIFF(duration, VAG_V4_TE_SHORT) < VAG_V4_TE_DELTA) {
            event = level ? ManchesterEventShortLow : ManchesterEventShortHigh;
            got_pulse = true;
        } else if(DURATION_DIFF(duration, VAG_V4_TE_LONG) < VAG_V4_LONG_DELTA) {
            event = level ? ManchesterEventLongLow : ManchesterEventLongHigh;
            got_pulse = true;
        }

        if(got_pulse) {
            bool bit_value = false;
            if(manchester_advance(
                   instance->manchester_state,
                   event,
                   &instance->manchester_state,
                   &bit_value)) {
                vag_v4_shift_bit(instance, bit_value);
            }
        }

        if(instance->bit_count < VAG_V4_PAYLOAD_BITS) break;

        // Frame complete.
        vag_v4_split_payload(instance);
        instance->generic.data = instance->payload_lo;
        instance->generic.data_2 = instance->payload_hi & 0xFFFF;
        instance->generic.data_count_bit = VAG_V4_PAYLOAD_BITS;
        instance->generic.serial = instance->uid;
        instance->generic.cnt = instance->ctr;
        instance->generic.btn = instance->btn;

        if(instance->base.callback) {
            instance->base.callback(&instance->base, instance->base.context);
        }

        instance->decoder.parser_step = VAGV4StepReset;
        break;
    }

    default:
        instance->decoder.parser_step = VAGV4StepReset;
        break;
    }
}

uint8_t subghz_protocol_decoder_vag_v4_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderVAG_V4* instance = context;
    uint8_t hash = 0;
    const uint8_t* p = (const uint8_t*)&instance->payload_lo;
    for(size_t i = 0; i < sizeof(instance->payload_lo); i++) hash ^= p[i];
    p = (const uint8_t*)&instance->payload_hi;
    for(size_t i = 0; i < sizeof(uint16_t); i++) hash ^= p[i];
    return hash;
}

SubGhzProtocolStatus subghz_protocol_decoder_vag_v4_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderVAG_V4* instance = context;

    instance->generic.data = instance->payload_lo;
    instance->generic.data_2 = instance->payload_hi & 0xFFFF;
    instance->generic.data_count_bit = VAG_V4_PAYLOAD_BITS;

    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);

    if(ret == SubGhzProtocolStatusOk) {
        uint32_t key_hi = (uint32_t)(instance->payload_hi & 0xFFFF);
        uint32_t uid = instance->uid;
        uint32_t ctr = instance->ctr;
        uint32_t btn_enc = instance->btn_enc;
        uint32_t btn = instance->btn;
        flipper_format_write_uint32(flipper_format, "KeyHi", &key_hi, 1);
        flipper_format_write_uint32(flipper_format, "UID", &uid, 1);
        flipper_format_write_uint32(flipper_format, "Cnt", &ctr, 1);
        flipper_format_write_uint32(flipper_format, "BtnEnc", &btn_enc, 1);
        flipper_format_write_uint32(flipper_format, "Btn", &btn, 1);
    }

    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_vag_v4_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderVAG_V4* instance = context;

    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        subghz_protocol_vag_v4_const.min_count_bit_for_found);
    if(ret != SubGhzProtocolStatusOk) return ret;

    instance->payload_lo = instance->generic.data;
    instance->payload_hi = instance->generic.data_2 & 0xFFFF;

    uint32_t key_hi = 0;
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "KeyHi", &key_hi, 1)) {
        instance->payload_hi = key_hi & 0xFFFF;
        instance->generic.data_2 = instance->payload_hi;
    }

    vag_v4_split_payload(instance);
    return ret;
}

static const char* vag_v4_button_name(uint8_t btn) {
    switch(btn) {
    case 0x01:
    case 0x10:
        return "Unlock";
    case 0x02:
    case 0x20:
        return "Lock";
    case 0x04:
    case 0x40:
        return "Trunk";
    case 0x08:
    case 0x80:
        return "Panic";
    default:
        return "??";
    }
}

void subghz_protocol_decoder_vag_v4_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderVAG_V4* instance = context;

    uint16_t hi16 = (uint16_t)(instance->payload_hi & 0xFFFF);
    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "UID:    %08lX\r\n"
        "Ctr:    %06lX\r\n"
        "BtnEnc: %02X\r\n"
        "Btn:    %02X [%s]\r\n"
        "Raw:    %04X%016llX",
        VAG_V4_PROTOCOL_NAME,
        instance->generic.data_count_bit,
        (unsigned long)instance->uid,
        (unsigned long)instance->ctr,
        instance->btn_enc,
        instance->btn,
        vag_v4_button_name(instance->btn),
        hi16,
        (unsigned long long)instance->payload_lo);
}

#define VAG_V4_ENCODER_UPLOAD_MAX_SIZE 640

void* subghz_protocol_encoder_vag_v4_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderVAG_V4* instance = malloc(sizeof(SubGhzProtocolEncoderVAG_V4));
    furi_check(instance);
    memset(instance, 0, sizeof(*instance));
    instance->base.protocol = &subghz_protocol_vag_v4;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->upload = malloc(VAG_V4_ENCODER_UPLOAD_MAX_SIZE * sizeof(LevelDuration));
    furi_check(instance->upload);
    instance->repeat = 1;
    return instance;
}

void subghz_protocol_encoder_vag_v4_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderVAG_V4* instance = context;
    free(instance->upload);
    free(instance);
}

void subghz_protocol_encoder_vag_v4_stop(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderVAG_V4* instance = context;
    instance->is_running = false;
}

static void vag_v4_encoder_build_upload(SubGhzProtocolEncoderVAG_V4* instance) {
    size_t index = 0;
    LevelDuration* upload = instance->upload;

    // Preamble: ~45 short high/low pairs
    for(int i = 0; i < 45 && index + 2 <= VAG_V4_ENCODER_UPLOAD_MAX_SIZE; i++) {
        upload[index++] = level_duration_make(true, VAG_V4_TE_SHORT);
        upload[index++] = level_duration_make(false, VAG_V4_TE_SHORT);
    }
    // Long high kicks off sync
    upload[index++] = level_duration_make(true, VAG_V4_TE_LONG);
    upload[index++] = level_duration_make(false, VAG_V4_TE_SHORT);

    // 3 sync pairs of 750 us
    for(int i = 0; i < (int)VAG_V4_SYNC_PAIRS; i++) {
        upload[index++] = level_duration_make(true, VAG_V4_SYNC);
        upload[index++] = level_duration_make(false, VAG_V4_SYNC);
    }

    // Payload: 80 Manchester bits (bit=1 -> high-then-low, bit=0 -> low-then-high).
    // MSB is bit 79 in (payload_hi<<64 | payload_lo).
    uint64_t hi = instance->payload_hi & 0xFFFF;
    uint64_t lo = instance->payload_lo;
    for(int i = (int)VAG_V4_PAYLOAD_BITS - 1; i >= 0; i--) {
        bool bit;
        if(i >= 64) {
            bit = (hi >> (i - 64)) & 1ULL;
        } else {
            bit = (lo >> i) & 1ULL;
        }
        if(bit) {
            upload[index++] = level_duration_make(true, VAG_V4_TE_SHORT);
            upload[index++] = level_duration_make(false, VAG_V4_TE_SHORT);
        } else {
            upload[index++] = level_duration_make(false, VAG_V4_TE_SHORT);
            upload[index++] = level_duration_make(true, VAG_V4_TE_SHORT);
        }
    }

    // Inter-frame gap
    upload[index++] = level_duration_make(false, 10000);
    instance->size_upload = index;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_vag_v4_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderVAG_V4* instance = context;

    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        subghz_protocol_vag_v4_const.min_count_bit_for_found);
    if(ret != SubGhzProtocolStatusOk) return ret;

    instance->payload_lo = instance->generic.data;
    instance->payload_hi = instance->generic.data_2 & 0xFFFF;

    uint32_t key_hi = 0;
    flipper_format_rewind(flipper_format);
    if(flipper_format_read_uint32(flipper_format, "KeyHi", &key_hi, 1)) {
        instance->payload_hi = key_hi & 0xFFFF;
    }

    vag_v4_encoder_build_upload(instance);
    if(instance->size_upload == 0) return SubGhzProtocolStatusErrorEncoderGetUpload;

    instance->repeat = 3;
    instance->front = 0;
    instance->is_running = true;
    return SubGhzProtocolStatusOk;
}

LevelDuration subghz_protocol_encoder_vag_v4_yield(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderVAG_V4* instance = context;

    if(!instance->is_running || instance->repeat == 0) {
        instance->is_running = false;
        return level_duration_reset();
    }

    LevelDuration ret = instance->upload[instance->front++];
    if(instance->front >= instance->size_upload) {
        instance->front = 0;
        instance->repeat--;
    }
    return ret;
}
