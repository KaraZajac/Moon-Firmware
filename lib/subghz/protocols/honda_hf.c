#include "honda_hf.h"

/* Reuse rolling-code tables from honda.h */
#include "honda.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include "../blocks/custom_btn_i.h"

static inline uint8_t popcount8(uint8_t x) {
    x = x - ((x >> 1) & 0x55);
    x = (x & 0x33) + ((x >> 2) & 0x33);
    return (x + (x >> 4)) & 0x0F;
}

#define TAG "SubGhzProtocolHondaHF"

static const SubGhzBlockConst subghz_protocol_honda_hf_const = {
    .te_short            = HONDA_HF_TE_SHORT,
    .te_long             = HONDA_HF_TE_LONG,
    .te_delta            = HONDA_HF_TE_DELTA,
    .min_count_bit_for_found = HONDA_HF_MIN_BITS,
};

/* ============================================================================
 * Frame data — 11-byte OOK PWM Honda/Acura frame
 *
 * Firmware Brand_Auto_Honda_TX layout:
 *   buf[0]  = (button << 4) | serial_hi_nibble
 *   buf[1]  = serial[23:16]
 *   buf[2]  = serial[15:8]
 *   buf[3]  = serial[7:0]
 *   buf[4]  = counter[23:16]
 *   buf[5]  = counter[15:8]
 *   buf[6]  = counter[7:0]
 *   buf[7]  = (mode << 4) | counter_lsn
 *   buf[8]  = checksum
 *   buf[9]  = extra1
 *   buf[10] = extra2
 * ==========================================================================*/
typedef struct {
    uint8_t  button;
    uint32_t serial;        /* 28-bit: 4 from buf[0] + 24 from buf[1..3] */
    uint32_t counter;       /* 24-bit */
    uint8_t  mode;          /* high nibble of buf[7] */
    uint8_t  checksum;
    uint8_t  extra[2];      /* buf[9..10] */
} HondaHFFrameData;

/* ============================================================================
 * Build / parse 11-byte wire buffer
 *
 * CRITICAL: Bit order is LSB-first per byte on the wire.
 * The buf[] array stores bytes in their natural (MSB) order;
 * the LSB-first reversal happens during encode/decode of the signal.
 * ==========================================================================*/
static void _hf_to_buf(const HondaHFFrameData* f, uint8_t buf[11]) {
    memset(buf, 0, 11);
    buf[0] = (uint8_t)((f->button << 4) | ((f->serial >> 24) & 0x0Fu));
    buf[1] = (uint8_t)((f->serial >> 16) & 0xFFu);
    buf[2] = (uint8_t)((f->serial >> 8)  & 0xFFu);
    buf[3] = (uint8_t)( f->serial        & 0xFFu);
    buf[4] = (uint8_t)((f->counter >> 16) & 0xFFu);
    buf[5] = (uint8_t)((f->counter >> 8)  & 0xFFu);
    buf[6] = (uint8_t)( f->counter        & 0xFFu);
    buf[7] = (uint8_t)(((f->mode & 0x0Fu) << 4) | (f->counter & 0x0Fu));
    buf[8] = f->checksum;
    buf[9]  = f->extra[0];
    buf[10] = f->extra[1];
}

static void _hf_from_buf(const uint8_t buf[11], HondaHFFrameData* f) {
    f->button   = (buf[0] >> 4) & 0x0Fu;
    f->serial   = ((uint32_t)(buf[0] & 0x0Fu) << 24) |
                  ((uint32_t)buf[1] << 16) |
                  ((uint32_t)buf[2] << 8)  |
                   (uint32_t)buf[3];
    f->counter  = ((uint32_t)buf[4] << 16) |
                  ((uint32_t)buf[5] << 8)  |
                   (uint32_t)buf[6];
    f->mode     = (buf[7] >> 4) & 0x0Fu;
    f->checksum = buf[8];
    f->extra[0] = buf[9];
    f->extra[1] = buf[10];
}

/* Bit-reverse a byte (LSB↔MSB) — needed for LSB-first wire encoding */
static inline uint8_t _bit_rev8_hf(uint8_t v) {
    v = (uint8_t)(((v & 0xF0u) >> 4) | ((v & 0x0Fu) << 4));
    v = (uint8_t)(((v & 0xCCu) >> 2) | ((v & 0x33u) << 2));
    v = (uint8_t)(((v & 0xAAu) >> 1) | ((v & 0x55u) << 1));
    return v;
}

/* XOR checksum of first 8 bytes */
static uint8_t _hf_xor_checksum(const uint8_t buf[11]) {
    uint8_t c = 0;
    for(uint8_t i = 0; i < 8; i++) c ^= buf[i];
    return c;
}

/* ============================================================================
 * Pack / unpack to 64-bit generic.data
 * ==========================================================================*/
static uint64_t _hf_pack(const HondaHFFrameData* f) {
    return ((uint64_t)(f->button & 0x0Fu) << 60) |
           ((uint64_t)(f->serial & 0x0FFFFFFFu) << 32) |
           ((uint64_t)(f->counter & 0x00FFFFFFu) << 8) |
           ((uint64_t)(f->mode & 0x0Fu) << 4) |
           ((uint64_t)(f->checksum >> 4) & 0x0Fu);
}

static void _hf_unpack(uint64_t raw, HondaHFFrameData* f) {
    f->button   = (uint8_t)((raw >> 60) & 0x0Fu);
    f->serial   = (uint32_t)((raw >> 32) & 0x0FFFFFFFu);
    f->counter  = (uint32_t)((raw >> 8)  & 0x00FFFFFFu);
    f->mode     = (uint8_t)((raw >> 4) & 0x0Fu);
    f->checksum = (uint8_t)((raw & 0x0Fu) << 4);
    memset(f->extra, 0, sizeof(f->extra));
}

/* ============================================================================
 * Counter increment — bit-reversed nibble arithmetic (same as Honda)
 * ==========================================================================*/
static inline uint8_t _bit_rev4_hf(uint8_t v) {
    v &= 0x0Fu;
    return (uint8_t)(
        ((v & 0x1u) << 3) | ((v & 0x2u) << 1) |
        ((v & 0x4u) >> 1) | ((v & 0x8u) >> 3));
}

static void _hf_counter_increment(HondaHFFrameData* f) {
    f->counter = (f->counter + 1u) & 0x00FFFFFFu;
    f->mode = (f->mode == 0xCu) ? 0x2u : 0xCu;
}

/* ============================================================================
 * Decoder state
 * ==========================================================================*/
#define HONDA_HF_DATA_BUF_BITS 128u

typedef enum {
    HondaHFStepReset = 0,
    HondaHFStepPreamble,
    HondaHFStepData,
} HondaHFDecoderStep;

typedef struct SubGhzProtocolDecoderHondaHF {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder        decoder;
    SubGhzBlockGeneric        generic;

    HondaHFDecoderStep step;
    uint16_t           preamble_count;   /* counts preamble short cycles */
    uint8_t            data_buf[16];     /* decoded data bytes */
    uint8_t            bit_count;        /* bits decoded so far */
    bool               last_level;       /* level of previous duration */
    uint32_t           last_duration;    /* previous duration */

    HondaHFFrameData frame;
    bool             frame_valid;
} SubGhzProtocolDecoderHondaHF;

/* ============================================================================
 * Encoder state
 * ==========================================================================*/
#define HONDA_HF_ENC_BUF_SIZE 1024u

typedef struct SubGhzProtocolEncoderHondaHF {
    SubGhzProtocolEncoderBase   base;
    SubGhzProtocolBlockEncoder  encoder;
    SubGhzBlockGeneric          generic;

    HondaHFFrameData frame;
    uint8_t          active_button;
} SubGhzProtocolEncoderHondaHF;

const SubGhzProtocolDecoder subghz_protocol_honda_hf_decoder;
const SubGhzProtocolEncoder subghz_protocol_honda_hf_encoder;
const SubGhzProtocol        subghz_protocol_honda_hf;

/* ============================================================================
 * PWM duration classifier
 *
 * PWM encoding:
 *   bit 0: period ~500µs  (HIGH ~250µs + LOW ~250µs)
 *   bit 1: period ~960µs  (HIGH ~480µs + LOW ~480µs)
 *
 * We classify each HIGH or LOW duration:
 *   SHORT (~250µs) → part of bit-0
 *   LONG  (~480µs) → part of bit-1
 * ==========================================================================*/
typedef enum {
    PwmDurInvalid = 0,
    PwmDurShort   = 1,   /* ~250µs → bit 0 half */
    PwmDurLong    = 2,   /* ~480µs → bit 1 half */
} PwmDurClass;

static PwmDurClass _hf_classify(uint32_t dur) {
    if(dur + HONDA_HF_TE_DELTA >= HONDA_HF_TE_SHORT &&
       dur <= HONDA_HF_TE_SHORT + HONDA_HF_TE_DELTA) return PwmDurShort;
    if(dur + HONDA_HF_TE_DELTA >= HONDA_HF_TE_LONG &&
       dur <= HONDA_HF_TE_LONG  + HONDA_HF_TE_DELTA) return PwmDurLong;
    return PwmDurInvalid;
}

/* ============================================================================
 * PWM decoder
 *
 * PWM decoding strategy:
 * We look at pairs of (HIGH duration, LOW duration).
 * - If HIGH is SHORT and LOW is SHORT → bit 0
 * - If HIGH is LONG  and LOW is LONG  → bit 1
 * - Mismatched pairs indicate preamble or error
 *
 * Bit order: LSB-first per byte (firmware: mask starts at bit 0, shifts left)
 * We accumulate bits LSB-first into each byte.
 *
 * The preamble is 312 cycles of SHORT-SHORT, which we skip by counting
 * consecutive bit-0s until we see a bit-1 (or use a minimum preamble count).
 * ==========================================================================*/

static void _hf_decoder_reset_state(SubGhzProtocolDecoderHondaHF* inst) {
    inst->step = HondaHFStepReset;
    inst->preamble_count = 0;
    inst->bit_count = 0;
    inst->last_duration = 0;
    memset(inst->data_buf, 0, sizeof(inst->data_buf));
}

static bool _hf_validate_frame(SubGhzProtocolDecoderHondaHF* inst) {
    if(inst->bit_count < HONDA_HF_MIN_BITS) return false;

    /* The data_buf contains LSB-first bytes — reverse each byte to get
     * the natural byte value for field extraction */
    uint8_t natural[16];
    uint8_t num_bytes = (inst->bit_count + 7u) / 8u;
    for(uint8_t i = 0; i < num_bytes && i < 16; i++) {
        natural[i] = _bit_rev8_hf(inst->data_buf[i]);
    }

    FURI_LOG_D(TAG, "PWM bits=%u: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
               inst->bit_count,
               natural[0], natural[1], natural[2], natural[3],
               natural[4], natural[5], natural[6], natural[7],
               natural[8], natural[9], natural[10]);

    /* Extract fields from natural-order bytes */
    HondaHFFrameData f;
    _hf_from_buf(natural, &f);

    /* Validate: button valid, serial non-trivial */
    if(f.button == 0 || f.button > HONDA_HF_BTN_LOCK2PRESS) return false;
    if(f.serial == 0 || f.serial == 0x0FFFFFFFu) return false;

    /* Checksum: XOR of first 8 bytes must equal byte 8 — exact match */
    uint8_t xor_val = _hf_xor_checksum(natural);
    if(xor_val != natural[8]) {
        /* Allow 1-bit tolerance for noisy captures */
        if(popcount8(xor_val ^ natural[8]) > 1) return false;
    }

    inst->frame = f;
    inst->frame_valid = true;

    FURI_LOG_I(TAG, "DECODED btn=%u ser=%07lX cnt=%06lX mode=%X csum=%02X",
               f.button, (unsigned long)f.serial, (unsigned long)f.counter,
               f.mode, f.checksum);
    return true;
}

/* ============================================================================
 * Custom button helpers
 * ==========================================================================*/
uint8_t subghz_protocol_honda_hf_btn_to_custom(uint8_t btn) {
    switch(btn) {
    case HONDA_HF_BTN_LOCK:       return 1;
    case HONDA_HF_BTN_UNLOCK:     return 2;
    case HONDA_HF_BTN_TRUNK:      return 3;
    case HONDA_HF_BTN_PANIC:      return 4;
    case HONDA_HF_BTN_RSTART:     return 5;
    default:                      return 1;
    }
}

uint8_t subghz_protocol_honda_hf_custom_to_btn(uint8_t custom) {
    switch(custom) {
    case 1: return HONDA_HF_BTN_LOCK;
    case 2: return HONDA_HF_BTN_UNLOCK;
    case 3: return HONDA_HF_BTN_TRUNK;
    case 4: return HONDA_HF_BTN_PANIC;
    case 5: return HONDA_HF_BTN_RSTART;
    default: return HONDA_HF_BTN_LOCK;
    }
}

/* ============================================================================
 * Protocol vtables
 * ==========================================================================*/
const SubGhzProtocolDecoder subghz_protocol_honda_hf_decoder = {
    .alloc         = subghz_protocol_decoder_honda_hf_alloc,
    .free          = subghz_protocol_decoder_honda_hf_free,
    .feed          = subghz_protocol_decoder_honda_hf_feed,
    .reset         = subghz_protocol_decoder_honda_hf_reset,
    .get_hash_data = subghz_protocol_decoder_honda_hf_get_hash_data,
    .serialize     = subghz_protocol_decoder_honda_hf_serialize,
    .deserialize   = subghz_protocol_decoder_honda_hf_deserialize,
    .get_string    = subghz_protocol_decoder_honda_hf_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_honda_hf_encoder = {
    .alloc       = subghz_protocol_encoder_honda_hf_alloc,
    .free        = subghz_protocol_encoder_honda_hf_free,
    .deserialize = subghz_protocol_encoder_honda_hf_deserialize,
    .stop        = subghz_protocol_encoder_honda_hf_stop,
    .yield       = subghz_protocol_encoder_honda_hf_yield,
};

const SubGhzProtocol subghz_protocol_honda_hf = {
    .name    = SUBGHZ_PROTOCOL_HONDA_HF_NAME,
    .type    = SubGhzProtocolTypeDynamic,
    .flag    = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_AM |
               SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load |
               SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_honda_hf_decoder,
    .encoder = &subghz_protocol_honda_hf_encoder,
};

/* ============================================================================
 * Decoder — alloc / free / reset
 * ==========================================================================*/
void* subghz_protocol_decoder_honda_hf_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderHondaHF* inst = malloc(sizeof(SubGhzProtocolDecoderHondaHF));
    furi_check(inst);
    memset(inst, 0, sizeof(SubGhzProtocolDecoderHondaHF));
    inst->base.protocol         = &subghz_protocol_honda_hf;
    inst->generic.protocol_name = inst->base.protocol->name;
    inst->frame_valid           = false;
    return inst;
}

void subghz_protocol_decoder_honda_hf_free(void* context) {
    furi_assert(context);
    free(context);
}

void subghz_protocol_decoder_honda_hf_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderHondaHF* inst = context;
    _hf_decoder_reset_state(inst);
    /* Preserve frame/frame_valid for get_string */
}

/* ============================================================================
 * Decoder — feed
 *
 * PWM decoder: processes HIGH/LOW duration pairs.
 *
 * State machine:
 *   HondaHFStepReset    — waiting for preamble (SHORT HIGH + SHORT LOW pairs)
 *   HondaHFStepPreamble — counting preamble cycles
 *   HondaHFStepData     — decoding data bits
 *
 * For each HIGH→LOW pair:
 *   Both SHORT → preamble cycle or bit 0
 *   Both LONG  → bit 1
 *   Mismatch   → error, reset
 *
 * We use the previous (HIGH) duration paired with current (LOW) duration.
 * ==========================================================================*/
void subghz_protocol_decoder_honda_hf_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderHondaHF* inst = context;
    PwmDurClass c = _hf_classify(duration);

    switch(inst->step) {
    case HondaHFStepReset:
        /* Waiting for a valid HIGH duration to start */
        if(level && c == PwmDurShort) {
            inst->last_level = true;
            inst->last_duration = duration;
            inst->step = HondaHFStepPreamble;
            inst->preamble_count = 0;
        }
        break;

    case HondaHFStepPreamble:
        if(!level) {
            /* LOW duration — pair with previous HIGH */
            PwmDurClass prev_c = _hf_classify(inst->last_duration);
            if(prev_c == PwmDurShort && c == PwmDurShort) {
                /* Preamble cycle: SHORT HIGH + SHORT LOW */
                inst->preamble_count++;
            } else if(prev_c == PwmDurLong && c == PwmDurLong) {
                /* First data bit (bit 1) after preamble */
                if(inst->preamble_count >= 20u) {
                    inst->step = HondaHFStepData;
                    inst->bit_count = 0;
                    memset(inst->data_buf, 0, sizeof(inst->data_buf));
                    /* Store bit 1 LSB-first: byte 0, bit 0 */
                    inst->data_buf[0] |= 0x01u;
                    inst->bit_count = 1;
                } else {
                    _hf_decoder_reset_state(inst);
                }
            } else if(c == PwmDurInvalid) {
                /* Gap — if we had enough preamble, check for data */
                if(inst->preamble_count >= 20u && inst->bit_count >= HONDA_HF_MIN_BITS) {
                    if(_hf_validate_frame(inst)) {
                        inst->generic.data = _hf_pack(&inst->frame);
                        inst->generic.data_count_bit = HONDA_HF_FRAME_BITS;
                        inst->generic.serial = inst->frame.serial;
                        inst->generic.btn    = inst->frame.button;
                        inst->generic.cnt    = inst->frame.counter;

                        uint8_t custom = subghz_protocol_honda_hf_btn_to_custom(inst->frame.button);
                        if(subghz_custom_btn_get_original() == 0)
                            subghz_custom_btn_set_original(custom);
                        subghz_custom_btn_set_max(HONDA_HF_CUSTOM_BTN_MAX);

                        if(inst->base.callback)
                            inst->base.callback(&inst->base, inst->base.context);
                    }
                }
                _hf_decoder_reset_state(inst);
            } else {
                _hf_decoder_reset_state(inst);
            }
        } else {
            /* HIGH duration — store for pairing */
            inst->last_duration = duration;
            inst->last_level = true;
        }
        break;

    case HondaHFStepData:
        if(level) {
            /* HIGH — store duration for pairing with next LOW */
            inst->last_duration = duration;
            inst->last_level = true;
        } else {
            /* LOW — pair with previous HIGH to decode a bit */
            PwmDurClass prev_c = _hf_classify(inst->last_duration);

            if(prev_c == PwmDurShort && c == PwmDurShort) {
                /* bit 0 — LSB-first: bit stays 0 (already cleared) */
                inst->bit_count++;
            } else if(prev_c == PwmDurLong && c == PwmDurLong) {
                /* bit 1 — LSB-first */
                uint8_t byte_idx = inst->bit_count / 8u;
                uint8_t bit_idx  = inst->bit_count % 8u;  /* LSB-first: bit 0 first */
                if(byte_idx < 16u) {
                    inst->data_buf[byte_idx] |= (uint8_t)(1u << bit_idx);
                }
                inst->bit_count++;
            } else if(c == PwmDurInvalid || prev_c == PwmDurInvalid) {
                /* Gap or invalid — attempt decode */
                if(inst->bit_count >= HONDA_HF_MIN_BITS) {
                    if(_hf_validate_frame(inst)) {
                        inst->generic.data = _hf_pack(&inst->frame);
                        inst->generic.data_count_bit = HONDA_HF_FRAME_BITS;
                        inst->generic.serial = inst->frame.serial;
                        inst->generic.btn    = inst->frame.button;
                        inst->generic.cnt    = inst->frame.counter;

                        uint8_t custom = subghz_protocol_honda_hf_btn_to_custom(inst->frame.button);
                        if(subghz_custom_btn_get_original() == 0)
                            subghz_custom_btn_set_original(custom);
                        subghz_custom_btn_set_max(HONDA_HF_CUSTOM_BTN_MAX);

                        if(inst->base.callback)
                            inst->base.callback(&inst->base, inst->base.context);
                    }
                }
                _hf_decoder_reset_state(inst);
            } else {
                /* Mismatch (SHORT+LONG or LONG+SHORT) — error */
                _hf_decoder_reset_state(inst);
            }

            /* Check if we have a full frame */
            if(inst->step == HondaHFStepData && inst->bit_count >= HONDA_HF_FRAME_BITS) {
                if(_hf_validate_frame(inst)) {
                    inst->generic.data = _hf_pack(&inst->frame);
                    inst->generic.data_count_bit = HONDA_HF_FRAME_BITS;
                    inst->generic.serial = inst->frame.serial;
                    inst->generic.btn    = inst->frame.button;
                    inst->generic.cnt    = inst->frame.counter;

                    uint8_t custom = subghz_protocol_honda_hf_btn_to_custom(inst->frame.button);
                    if(subghz_custom_btn_get_original() == 0)
                        subghz_custom_btn_set_original(custom);
                    subghz_custom_btn_set_max(HONDA_HF_CUSTOM_BTN_MAX);

                    if(inst->base.callback)
                        inst->base.callback(&inst->base, inst->base.context);
                }
                _hf_decoder_reset_state(inst);
            }
        }
        break;
    }

    inst->decoder.te_last = duration;
}

uint8_t subghz_protocol_decoder_honda_hf_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderHondaHF* inst = context;
    return (uint8_t)(inst->generic.data        ^
                    (inst->generic.data >> 8)   ^
                    (inst->generic.data >> 16)  ^
                    (inst->generic.data >> 24)  ^
                    (inst->generic.data >> 32));
}

SubGhzProtocolStatus subghz_protocol_decoder_honda_hf_serialize(
    void* context, FlipperFormat* flipper_format, SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderHondaHF* inst = context;
    return subghz_block_generic_serialize(&inst->generic, flipper_format, preset);
}

SubGhzProtocolStatus subghz_protocol_decoder_honda_hf_deserialize(
    void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderHondaHF* inst = context;
    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &inst->generic, flipper_format,
        subghz_protocol_honda_hf_const.min_count_bit_for_found);
    if(ret == SubGhzProtocolStatusOk) {
        _hf_unpack(inst->generic.data, &inst->frame);
        inst->frame_valid    = true;
        inst->generic.serial = inst->frame.serial;
        inst->generic.btn    = inst->frame.button;
        inst->generic.cnt    = inst->frame.counter;

        uint8_t custom = subghz_protocol_honda_hf_btn_to_custom(inst->frame.button);
        if(subghz_custom_btn_get_original() == 0)
            subghz_custom_btn_set_original(custom);
        subghz_custom_btn_set_max(HONDA_HF_CUSTOM_BTN_MAX);
    }
    return ret;
}

void subghz_protocol_decoder_honda_hf_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderHondaHF* inst = context;

    if(!inst->frame_valid && inst->generic.data != 0) {
        _hf_unpack(inst->generic.data, &inst->frame);
        inst->frame_valid = true;
    }

    const char* btn_name;
    switch(inst->frame.button) {
    case HONDA_HF_BTN_LOCK:       btn_name = "Lock";         break;
    case HONDA_HF_BTN_UNLOCK:     btn_name = "Unlock";       break;
    case HONDA_HF_BTN_TRUNK:      btn_name = "Trunk/Hatch";  break;
    case HONDA_HF_BTN_PANIC:      btn_name = "Panic";        break;
    case HONDA_HF_BTN_RSTART:     btn_name = "Remote Start"; break;
    case HONDA_HF_BTN_LOCK2PRESS: btn_name = "Lock x2";      break;
    default:                      btn_name = "Unknown";      break;
    }

    furi_string_cat_printf(
        output,
        "%s %ubit\r\n"
        "Btn:%s (0x%X)\r\n"
        "Ser:%07lX\r\n"
        "Cnt:%06lX Mode:%X Chk:%02X\r\n",
        inst->generic.protocol_name,
        inst->generic.data_count_bit,
        btn_name,
        inst->frame.button,
        (unsigned long)inst->frame.serial,
        (unsigned long)inst->frame.counter,
        inst->frame.mode,
        inst->frame.checksum);
}

/* ============================================================================
 * Encoder — build OOK PWM upload buffer
 *
 * Wire format (LSB-first per byte):
 *   Guard: HIGH 100µs → LOW 840µs
 *   Preamble: 312 × (HIGH 250µs + LOW 250µs)
 *   Data: 88 bits, each bit:
 *     bit 0: HIGH 250µs + LOW 250µs
 *     bit 1: HIGH 480µs + LOW 480µs
 *   Tail: LOW 4000µs
 * ==========================================================================*/
static void _hf_build_upload(SubGhzProtocolEncoderHondaHF* inst) {
    LevelDuration* buf = inst->encoder.upload;
    size_t idx = 0;

    /* Guard */
    buf[idx++] = level_duration_make(true,  100u);
    buf[idx++] = level_duration_make(false, HONDA_HF_GUARD_US);

    /* Preamble: 312 short cycles */
    for(uint16_t p = 0; p < HONDA_HF_PREAMBLE_CYCLES; p++) {
        buf[idx++] = level_duration_make(true,  HONDA_HF_TE_SHORT);
        buf[idx++] = level_duration_make(false, HONDA_HF_TE_SHORT);
        if(idx >= HONDA_HF_ENC_BUF_SIZE - 20) break;
    }

    /* Build 11-byte frame */
    uint8_t frame_buf[11];
    _hf_to_buf(&inst->frame, frame_buf);

    /* Encode 88 bits, LSB-first per byte */
    for(uint8_t byte_i = 0; byte_i < HONDA_HF_FRAME_BYTES && idx < HONDA_HF_ENC_BUF_SIZE - 4; byte_i++) {
        uint8_t bval = frame_buf[byte_i];
        for(uint8_t bit_i = 0; bit_i < 8u; bit_i++) {
            /* LSB-first: bit 0 first */
            bool bit = (bval >> bit_i) & 1u;
            if(bit) {
                buf[idx++] = level_duration_make(true,  HONDA_HF_TE_LONG);
                buf[idx++] = level_duration_make(false, HONDA_HF_TE_LONG);
            } else {
                buf[idx++] = level_duration_make(true,  HONDA_HF_TE_SHORT);
                buf[idx++] = level_duration_make(false, HONDA_HF_TE_SHORT);
            }
        }
    }

    /* Tail */
    buf[idx++] = level_duration_make(false, 4000u);

    inst->encoder.size_upload = idx;
    inst->encoder.front       = 0;
}

/* ============================================================================
 * Encoder — alloc / free / stop / yield
 * ==========================================================================*/
void* subghz_protocol_encoder_honda_hf_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderHondaHF* inst = malloc(sizeof(SubGhzProtocolEncoderHondaHF));
    furi_check(inst);
    memset(inst, 0, sizeof(SubGhzProtocolEncoderHondaHF));
    inst->base.protocol         = &subghz_protocol_honda_hf;
    inst->generic.protocol_name = inst->base.protocol->name;
    inst->encoder.repeat        = 3;
    inst->encoder.size_upload   = 0;
    inst->encoder.upload        = malloc(HONDA_HF_ENC_BUF_SIZE * sizeof(LevelDuration));
    furi_check(inst->encoder.upload);
    inst->encoder.is_running = false;
    inst->encoder.front      = 0;
    return inst;
}

void subghz_protocol_encoder_honda_hf_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderHondaHF* inst = context;
    free(inst->encoder.upload);
    free(inst);
}

void subghz_protocol_encoder_honda_hf_stop(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderHondaHF* inst = context;
    inst->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_honda_hf_yield(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderHondaHF* inst = context;
    if(inst->encoder.repeat == 0 || !inst->encoder.is_running) {
        inst->encoder.is_running = false;
        return level_duration_reset();
    }
    LevelDuration ret = inst->encoder.upload[inst->encoder.front];
    if(++inst->encoder.front >= inst->encoder.size_upload) {
        inst->encoder.repeat--;
        inst->encoder.front = 0;
    }
    return ret;
}

SubGhzProtocolStatus subghz_protocol_encoder_honda_hf_deserialize(
    void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderHondaHF* inst = context;
    SubGhzProtocolStatus ret = subghz_block_generic_deserialize(&inst->generic, flipper_format);
    if(ret != SubGhzProtocolStatusOk) return ret;

    _hf_unpack(inst->generic.data, &inst->frame);

    uint8_t custom = subghz_protocol_honda_hf_btn_to_custom(inst->frame.button);
    if(subghz_custom_btn_get_original() == 0)
        subghz_custom_btn_set_original(custom);
    subghz_custom_btn_set_max(HONDA_HF_CUSTOM_BTN_MAX);

    uint8_t active_custom = subghz_custom_btn_get();
    inst->active_button = (active_custom == SUBGHZ_CUSTOM_BTN_OK)
        ? subghz_protocol_honda_hf_custom_to_btn(subghz_custom_btn_get_original())
        : subghz_protocol_honda_hf_custom_to_btn(active_custom);

    /* Advance counter */
    inst->frame.counter = (inst->frame.counter +
        furi_hal_subghz_get_rolling_counter_mult()) & 0x00FFFFFFu;
    _hf_counter_increment(&inst->frame);

    inst->frame.button = inst->active_button;

    /* Recompute checksum for new counter */
    uint8_t tmp_buf[11];
    _hf_to_buf(&inst->frame, tmp_buf);
    inst->frame.checksum = _hf_xor_checksum(tmp_buf);

    inst->generic.data = _hf_pack(&inst->frame);
    inst->generic.cnt  = inst->frame.counter;
    inst->generic.btn  = inst->active_button;

    /* Update Key in flipper_format */
    flipper_format_rewind(flipper_format);
    uint8_t key_data[8];
    for(int i = 0; i < 8; i++)
        key_data[i] = (uint8_t)(inst->generic.data >> (56 - i * 8));
    flipper_format_update_hex(flipper_format, "Key", key_data, 8);

    _hf_build_upload(inst);
    inst->encoder.is_running = true;
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_encoder_honda_hf_set_button(void* context, uint8_t btn) {
    furi_assert(context);
    SubGhzProtocolEncoderHondaHF* inst = context;
    inst->active_button      = btn & 0x0Fu;
    inst->encoder.is_running = false;
    _hf_counter_increment(&inst->frame);
    inst->frame.button = inst->active_button;

    uint8_t tmp_buf[11];
    _hf_to_buf(&inst->frame, tmp_buf);
    inst->frame.checksum = _hf_xor_checksum(tmp_buf);

    inst->generic.data = _hf_pack(&inst->frame);
    inst->generic.cnt  = inst->frame.counter;
    _hf_build_upload(inst);
    inst->encoder.repeat     = 3;
    inst->encoder.is_running = true;
}
