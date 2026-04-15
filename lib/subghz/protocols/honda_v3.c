/*
* honda_v3.c — Honda KeeLoq Unified Protocol
* Implements the 3 modes of the Honda protocol extracted from the Pandora firmware:

* MODE 1 (Honda_V3_OOK): OOK PWM, 11 bytes, 88 bits, LSB-first

* Brand_Auto_Honda_TX @ 0x13F24

* MODE 2 (Honda_V3_KL): Inverted OOK PWM, 66-bit KeeLoq

* Crypto_Scramble_Finalize_Keeloq @ 0x9C00

* Verified: Lock/Unlock/Hondaaa.sub

* MODE 3 (Honda_V3_FSK): Compressed Manchester FSK as OOK, 14 bytes

* Protocol_TX_Honda_Extended @ 0xEFDC

* Critical Notes:

* - KeeLoq decryption: key_bit = (15 - round) & 0x3F (NO round & 0x3F)
* - Mode 2: bit 1 produces a short HIGH (inverted PWM)
* - OOK: LSB-first per byte on the air
* - FSK: MSB-first, captured as OOK with ~8x compressed timings
* - Wide timing tolerance (±100-150µs) for variability between controls
*/

#include "honda_v3.h"

#include <furi.h>

static inline uint8_t popcount8(uint8_t x) {
    x = x - ((x >> 1) & 0x55);
    x = (x & 0x33) + ((x >> 2) & 0x33);
    return (x + (x >> 4)) & 0x0F;
}
#include <furi_hal.h>
#include <lib/subghz/blocks/const.h>
#include <lib/subghz/blocks/decoder.h>
#include <lib/subghz/blocks/encoder.h>
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/blocks/math.h>
#include <lib/subghz/blocks/custom_btn_i.h>

#define TAG "HondaV3"

/* ═════════════════════════════════════════════════════════════════
* MANUFACTURER KEY TABLE
* Source: Pandora firmware @ 0x14D60 (20×8 bytes)
* Test Order Optimized: [14,1,0] are the most common
* ═════════════════════════════════════════════════════════════════════════════════ */

const HondaV3MfrKey honda_v3_mfr_keys[HV3_MFR_KEY_COUNT] = {
    {0x8DAA5CDB, 0xA8F5DFFC},  /*  0 main firmware @ 0x9EB4               */
    {0x10C1F940, 0xABBA94D4},  /*  1 ROM[0]  Hondaaa.sub ✓                */
    {0x460114B7, 0xDCF40FCD},  /*  2 ROM[1]                               */
    {0xE9B977B4, 0x56EB02D8},  /*  3 ROM[2]                               */
    {0x3F26FA21, 0xA8507BA5},  /*  4 ROM[3]                               */
    {0xA4C5C542, 0x6857C1C5},  /*  5 ROM[4]                               */
    {0xEBBB753E, 0x54E900DA},  /*  6 ROM[5]                               */
    {0x6A67C523, 0x5683FC9B},  /*  7 ROM[6]                               */
    {0xECBC7263, 0x53EE07DD},  /*  8 ROM[7]                               */
    {0xE1CA6024, 0x5D973077},  /*  9 ROM[8]                               */
    {0xEDBD737C, 0x52EF06DC},  /* 10 ROM[9]                               */
    {0x6011AC25, 0x36714027},  /* 11 ROM[10]                              */
    {0xEEBE7059, 0x51EC05DF},  /* 12 ROM[11]                              */
    {0x9BB87A26, 0x1F4DAA96},  /* 13 ROM[12]                              */
    {0xEFBF713C, 0x50ED04DE},  /* 14 ROM[13] Lock/Unlock ✓                */
    {0xF39FD627, 0xDC8DC8EA},  /* 15 ROM[14]                              */
    {0x610A14EB, 0x30850A82},  /* 16 ROM[15]                              */
    {0x1719FF41, 0xD5204919},  /* 17 ROM[16]                              */
    {0x779F14DA, 0x558EA3B0},  /* 18 ROM[17]                              */
    {0x47454DE2, 0x54454441},  /* 19 ROM[18]                              */
    {0x0514F348, 0x40802014},  /* 20 ROM[19]                              */
};


static const uint8_t kl_key_probe_order[HV3_MFR_KEY_COUNT] = {
    14, 1, 0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15, 16, 17, 18, 19, 20
};

/* ═══════════════════════════════════════════════════════════════════
 * LOOKUP TABLES (Mode 3 — FSK extended)
 * ═══════════════════════════════════════════════════════════════════ */
static const uint8_t hv3_tbl_primary[16][16]   = { HV3_TABLE_PRIMARY };
static const uint8_t hv3_tbl_secondary[16][16] = { HV3_TABLE_SECONDARY };
static const uint8_t hv3_tbl_subst[16][16]     = { HV3_TABLE_SUBST };

/* ═══════════════════════════════════════════════════════════════════
 * HELPERS
 * ═══════════════════════════════════════════════════════════════════ */

/* Bit-reverse */
static inline uint8_t hv3_bit_rev8(uint8_t v) {
    v = (uint8_t)(((v & 0xF0u) >> 4) | ((v & 0x0Fu) << 4));
    v = (uint8_t)(((v & 0xCCu) >> 2) | ((v & 0x33u) << 2));
    v = (uint8_t)(((v & 0xAAu) >> 1) | ((v & 0x55u) << 1));
    return v;
}

/* Bit-reverse nibble */
static inline uint8_t hv3_bit_rev4(uint8_t v) {
    return (uint8_t)(hv3_bit_rev8(v & 0x0Fu) >> 4);
}

static const char* hv3_btn_name(uint8_t btn) {
    switch(btn) {
    case HV3_BTN_PANIC:        return "Panic";
    case HV3_BTN_LOCK:         return "Lock";
    case HV3_BTN_TRUNK:        return "Trunk/Hatch";
    case HV3_BTN_REMOTE_START: return "Remote Start";
    case HV3_BTN_UNLOCK:       return "Unlock";
    case HV3_BTN_UNLOCK2:      return "Unlock x2";
    default:                   return "Unknown";
    }
}

/* Custom button ↔ btn code */
static uint8_t hv3_btn_to_custom(uint8_t btn) {
    switch(btn) {
    case HV3_BTN_LOCK:         return 1;
    case HV3_BTN_UNLOCK:       return 2;
    case HV3_BTN_TRUNK:        return 3;
    case HV3_BTN_PANIC:        return 4;
    case HV3_BTN_REMOTE_START: return 5;
    default:                   return 1;
    }
}

static uint8_t hv3_custom_to_btn(uint8_t custom) {
    switch(custom) {
    case 1: return HV3_BTN_LOCK;
    case 2: return HV3_BTN_UNLOCK;
    case 3: return HV3_BTN_TRUNK;
    case 4: return HV3_BTN_PANIC;
    case 5: return HV3_BTN_REMOTE_START;
    default: return HV3_BTN_LOCK;
    }
}

static void hv3_register_btn(uint8_t btn) {
    uint8_t custom = hv3_btn_to_custom(btn);
    if(subghz_custom_btn_get_original() == 0)
        subghz_custom_btn_set_original(custom);
    subghz_custom_btn_set_max(HV3_BTN_CUSTOM_MAX);
}

static uint8_t hv3_get_active_btn(void) {
    uint8_t ac = subghz_custom_btn_get();
    if(ac == SUBGHZ_CUSTOM_BTN_OK)
        return hv3_custom_to_btn(subghz_custom_btn_get_original());
    return hv3_custom_to_btn(ac);
}

static uint64_t hv3_pack(
    uint32_t serial, uint8_t btn,
    uint16_t rolling, uint8_t discr_mode,
    uint8_t key_idx,  uint8_t proto_mode, uint8_t flags)
{
    return ((uint64_t)(serial      & 0x0FFFFFFFu) << 36) |
           ((uint64_t)(btn         & 0x0Fu)        << 32) |
           ((uint64_t)rolling                      << 16) |
           ((uint64_t)discr_mode                   <<  8) |
           ((uint64_t)(key_idx     & 0x0Fu)        <<  4) |
           ((uint64_t)(proto_mode  & 0x03u)        <<  2) |
           ((uint64_t)(flags       & 0x03u));
}

static void hv3_unpack(
    uint64_t raw,
    uint32_t* serial, uint8_t* btn,
    uint16_t* rolling, uint8_t* discr_mode,
    uint8_t* key_idx,  uint8_t* proto_mode, uint8_t* flags)
{
    *serial     = (uint32_t)((raw >> 36) & 0x0FFFFFFFu);
    *btn        = (uint8_t) ((raw >> 32) & 0x0Fu);
    *rolling    = (uint16_t)((raw >> 16) & 0xFFFFu);
    *discr_mode = (uint8_t) ((raw >>  8) & 0xFFu);
    *key_idx    = (uint8_t) ((raw >>  4) & 0x0Fu);
    *proto_mode = (uint8_t) ((raw >>  2) & 0x03u);
    *flags      = (uint8_t) ((raw      ) & 0x03u);
}

/* ═══════════════════════════════════════════════════════════════════
* KEELOQ ENGINE
* Source: Crypto.LFSR.Rolling_Code @0x9EB8
* NLF: 0x3A5C742E
* Rounds: 528
* Taps: bits 1, 9, 20, 26, 31
*
* CRITICAL NOTE decrypt: kb = bit(key, (15-i) & 0x3F)
* Firmware uses RSB R0, R5, #0xF → NOT i & 0x3F
* ══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t kl_nlf(uint32_t x) {
    uint32_t idx =
        (((x >>  1) & 1u)      ) |
        (((x >>  9) & 1u) << 1u) |
        (((x >> 20) & 1u) << 2u) |
        (((x >> 26) & 1u) << 3u) |
        (((x >> 31) & 1u) << 4u);
    return (HV3_KL_NLF >> idx) & 1u;
}

static uint32_t kl_decrypt(uint32_t ciphertext, uint64_t key) {
    uint32_t x = ciphertext;
    for(int i = 0; i < HV3_KL_ROUNDS; i++) {
        uint32_t kb  = (uint32_t)((key >> ((15 - i) & 0x3F)) & 1u);
        uint32_t nlf = kl_nlf(x);
        uint32_t msb = x & 1u;
        uint32_t b15 = (x >> 15) & 1u;
        uint32_t fb  = msb ^ nlf ^ b15 ^ kb;
        x = (x >> 1) | (fb << 31);
    }
    return x;
}

static uint32_t kl_encrypt(uint32_t plaintext, uint64_t key) {
    uint32_t x = plaintext;
    for(int i = 0; i < HV3_KL_ROUNDS; i++) {
        uint32_t kb  = (uint32_t)((key >> (i & 0x3F)) & 1u);
        uint32_t nlf = kl_nlf(x);
        uint32_t b0  = x & 1u;
        uint32_t b16 = (x >> 16) & 1u;
        uint32_t fb  = nlf ^ b16 ^ b0 ^ kb;
        x = ((x << 1) & 0xFFFFFFFFu) | fb;
    }
    return x;
}

static int8_t kl_find_key(
    uint32_t  enc,
    uint8_t   btn,
    uint32_t* out_dec,
    uint16_t* out_rc)
{
    for(int probe = 0; probe < HV3_MFR_KEY_COUNT; probe++) {
        uint8_t i = kl_key_probe_order[probe];
        uint64_t k = ((uint64_t)honda_v3_mfr_keys[i].high << 32) |
                      (uint64_t)honda_v3_mfr_keys[i].low;
        uint32_t dec = kl_decrypt(enc, k);

        if((dec & HV3_KL_DEC_FUNC_MASK) != (uint32_t)btn) continue;

        uint16_t rc   = (uint16_t)((dec & HV3_KL_DEC_RC_MASK)
                                        >> HV3_KL_DEC_RC_SHIFT);
        uint8_t  disc = (uint8_t) ((dec & HV3_KL_DEC_DISC_MASK)
                                        >> HV3_KL_DEC_DISC_SHIFT);
        if(rc == 0 && disc == 0) continue;

        *out_dec = dec;
        *out_rc  = rc;
        return (int8_t)i;
    }
    return -1;
}

static void kl_build_frame(
    bool     bits[HV3_KL_FRAME_BITS],
    uint64_t key,
    uint32_t serial,
    uint8_t  btn,
    uint8_t  status,
    uint16_t rc)
{
    uint8_t  discr = (uint8_t)((serial >> 20) & 0xFFu);
    uint32_t plain = ((uint32_t)(btn & 0xFu))    |
                     ((uint32_t)discr      << 4)  |
                     ((uint32_t)rc         << 16);
    uint32_t enc = kl_encrypt(plain, key);

    for(int i = 0; i < 32; i++) bits[i]    = (bool)((enc    >> i) & 1u);
    for(int i = 0; i < 28; i++) bits[32+i] = (bool)((serial >> i) & 1u);
    for(int i = 0; i < 4;  i++) bits[60+i] = (bool)((btn    >> i) & 1u);
    for(int i = 0; i < 2;  i++) bits[64+i] = (bool)((status >> i) & 1u);
}

/* ═══════════════════════════════════════════════════════════════════
 * FORWARD DECLARATIONS — vtables
 * ═══════════════════════════════════════════════════════════════════ */
const SubGhzProtocolDecoder subghz_protocol_honda_v3_ook_decoder;
const SubGhzProtocolEncoder subghz_protocol_honda_v3_ook_encoder;
const SubGhzProtocol        subghz_protocol_honda_v3_ook;

const SubGhzProtocolDecoder subghz_protocol_honda_v3_kl_decoder;
const SubGhzProtocolEncoder subghz_protocol_honda_v3_kl_encoder;
const SubGhzProtocol        subghz_protocol_honda_v3_kl;

const SubGhzProtocolDecoder subghz_protocol_honda_v3_fsk_decoder;
const SubGhzProtocolEncoder subghz_protocol_honda_v3_fsk_encoder;
const SubGhzProtocol        subghz_protocol_honda_v3_fsk;

/* ═══════════════════════════════════════════════════════════════════
 * ███████████████████ MODE 1 — OOK PWM ███████████████████
 * Brand_Auto_Honda_TX @ 0x13F24
 *
 * Bit 0: HIGH 250µs + LOW 250µs
 * Bit 1: HIGH 480µs + LOW 480µs
 * Preamble: 312 SHORT cycles
 * LSB-first per byte on the air
 * ═══════════════════════════════════════════════════════════════════ */

/* ── OOK: buffer ↔ frame ── */
static void ook_to_buf(const HondaV3OOKFrame* f, uint8_t buf[HV3_OOK_FRAME_BYTES]) {
    memset(buf, 0, HV3_OOK_FRAME_BYTES);
    buf[0] = (uint8_t)((f->button << 4) | ((f->serial >> 24) & 0x0Fu));
    buf[1] = (uint8_t)((f->serial >> 16) & 0xFFu);
    buf[2] = (uint8_t)((f->serial >>  8) & 0xFFu);
    buf[3] = (uint8_t)( f->serial        & 0xFFu);
    buf[4] = (uint8_t)((f->counter >> 16) & 0xFFu);
    buf[5] = (uint8_t)((f->counter >>  8) & 0xFFu);
    buf[6] = (uint8_t)( f->counter        & 0xFFu);
    buf[7] = (uint8_t)(((f->mode & 0x0Fu) << 4) | (f->counter & 0x0Fu));
    buf[8] = f->checksum;
    buf[9]  = f->extra[0];
    buf[10] = f->extra[1];
}

static void ook_from_buf(const uint8_t buf[HV3_OOK_FRAME_BYTES], HondaV3OOKFrame* f) {
    f->button  = (buf[0] >> 4) & 0x0Fu;
    f->serial  = ((uint32_t)(buf[0] & 0x0Fu) << 24) |
                 ((uint32_t)buf[1] << 16) |
                 ((uint32_t)buf[2] <<  8) |
                  (uint32_t)buf[3];
    f->counter = ((uint32_t)buf[4] << 16) |
                 ((uint32_t)buf[5] <<  8) |
                  (uint32_t)buf[6];
    f->mode     = (buf[7] >> 4) & 0x0Fu;
    f->checksum = buf[8];
    f->extra[0] = buf[9];
    f->extra[1] = buf[10];
}

/* XOR checksum first 8 bytes */
static uint8_t ook_checksum(const uint8_t buf[HV3_OOK_FRAME_BYTES]) {
    uint8_t c = 0;
    for(uint8_t i = 0; i < 8u; i++) c ^= buf[i];
    return c;
}

/*
 * OOK counter — exact firmware logic @ 0xEFF8-0xF08E
 * Operates with bit_rev8 in the cascade byte (buf[3]).
 * Flip mode: 0x2 ↔ 0xC
 */

static void ook_counter_increment(HondaV3OOKFrame* f) {
    uint8_t buf[HV3_OOK_FRAME_BYTES];
    ook_to_buf(f, buf);

    uint8_t src3 = buf[3];
    uint8_t src2 = buf[2];
    uint8_t tmp = hv3_bit_rev8((uint8_t)(hv3_bit_rev8(src3) + 1u));
    buf[3] = (tmp & 0xF0u) | (src3 & 0x0Fu);
    if((src3 >> 4) == 0x0Fu) {
        tmp = hv3_bit_rev8((uint8_t)(hv3_bit_rev8(src3) + 1u));
        buf[3] = (buf[3] & 0xF0u) | (tmp & 0x0Fu);
    }

    if(src3 == 0xFFu) {
        uint8_t hi_new = hv3_bit_rev8(
            (uint8_t)((hv3_bit_rev8(src2) >> 4) + 1u));
        buf[2] = (buf[2] & 0x0Fu) | (uint8_t)((hi_new & 0x0Fu) << 4);

        if((src2 >> 4) == 0x0Fu) {
            tmp = hv3_bit_rev8((uint8_t)(hv3_bit_rev8(src2) + 1u));
            buf[2] = (buf[2] & 0xF0u) | (tmp & 0x0Fu);
        }
    }

    /* Mode flip */
    f->mode = (f->mode == 0xCu) ? 0x2u : 0xCu;
    buf[7]  = (uint8_t)(((f->mode & 0x0Fu) << 4) | (buf[7] & 0x0Fu));

    f->counter = ((uint32_t)buf[4] << 16) |
                 ((uint32_t)buf[5] <<  8) |
                  (uint32_t)buf[6];

    /* Recompute checksum */
    f->checksum = ook_checksum(buf);
}

static uint8_t ook_classify(uint32_t dur) {
    if(dur + HV3_OOK_TE_DELTA >= HV3_OOK_TE_SHORT &&
       dur <= HV3_OOK_TE_SHORT + HV3_OOK_TE_DELTA) return 1u;
    if(dur + HV3_OOK_TE_DELTA >= HV3_OOK_TE_LONG &&
       dur <= HV3_OOK_TE_LONG  + HV3_OOK_TE_DELTA) return 2u;
    return 0u;
}

static void ook_decoder_reset_state(SubGhzProtocolDecoderHondaV3OOK* inst) {
    inst->step           = HV3OOKStep_Reset;
    inst->preamble_count = 0;
    inst->bit_count      = 0;
    inst->last_high_dur  = 0;
    memset(inst->data_buf, 0, sizeof(inst->data_buf));
}

static bool ook_validate_frame(SubGhzProtocolDecoderHondaV3OOK* inst) {
    if(inst->bit_count < HV3_OOK_MIN_BITS) return false;

    uint8_t natural[HV3_OOK_FRAME_BYTES];
    uint8_t num_bytes = (inst->bit_count + 7u) / 8u;
    if(num_bytes > HV3_OOK_FRAME_BYTES) num_bytes = HV3_OOK_FRAME_BYTES;

    for(uint8_t i = 0; i < num_bytes; i++)
        natural[i] = hv3_bit_rev8(inst->data_buf[i]);

    FURI_LOG_D(TAG, "[OOK] bits=%u %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
               inst->bit_count,
               natural[0], natural[1], natural[2], natural[3], natural[4],
               natural[5], natural[6], natural[7], natural[8], natural[9], natural[10]);

    HondaV3OOKFrame f;
    ook_from_buf(natural, &f);

    if(f.button == 0 || f.button > HV3_BTN_UNLOCK2)    return false;
    if(f.serial == 0 || f.serial == 0x0FFFFFFFu)        return false;

    uint8_t xchk = ook_checksum(natural);
    if(xchk != natural[8]) {
        if(popcount8(xchk ^ natural[8]) > 1) return false;
    }

    inst->frame       = f;
    inst->frame_valid = true;

    FURI_LOG_I(TAG, "[OOK] btn=%u ser=%07lX cnt=%06lX mode=%X",
               f.button,
               (unsigned long)f.serial,
               (unsigned long)f.counter,
               f.mode);
    return true;
}

static void ook_notify_frame(SubGhzProtocolDecoderHondaV3OOK* inst) {
    inst->generic.data = hv3_pack(
        inst->frame.serial,
        inst->frame.button,
        (uint16_t)(inst->frame.counter & 0xFFFFu),
        inst->frame.mode,
        0xFu,                    /* key_idx = 0xF → OOK */
        HV3_PROTO_MODE_OOK,
        0u);
    inst->generic.data_count_bit = HV3_OOK_FRAME_BITS;
    inst->generic.serial = inst->frame.serial;
    inst->generic.btn    = inst->frame.button;
    inst->generic.cnt    = inst->frame.counter;
    hv3_register_btn(inst->frame.button);
    if(inst->base.callback)
        inst->base.callback(&inst->base, inst->base.context);
}

/* ── OOK Decoder: alloc / free / reset ── */
void* subghz_protocol_decoder_honda_v3_ook_alloc(SubGhzEnvironment* env) {
    UNUSED(env);
    SubGhzProtocolDecoderHondaV3OOK* inst =
        malloc(sizeof(SubGhzProtocolDecoderHondaV3OOK));
    furi_check(inst);
    memset(inst, 0, sizeof(SubGhzProtocolDecoderHondaV3OOK));
    inst->base.protocol         = &subghz_protocol_honda_v3_ook;
    inst->generic.protocol_name = inst->base.protocol->name;
    inst->frame_valid           = false;
    FURI_LOG_I(TAG, "[OOK] decoder alloc");
    return inst;
}

void subghz_protocol_decoder_honda_v3_ook_free(void* ctx) {
    furi_assert(ctx);
    free(ctx);
}

void subghz_protocol_decoder_honda_v3_ook_reset(void* ctx) {
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV3OOK* inst = ctx;
    ook_decoder_reset_state(inst);
}

/* ── OOK Decoder: feed ── 
 *
 * State machine: 
 * Reset → wait HIGH SHORT (preamble start) 
 * Preamble → count SHORT-SHORT cycles 
 * Data → decode HIGH+LOW pairs 
 *
 * Bit decode (LSB-first, accumulate in data_buf): 
 * HIGH SHORT + LOW SHORT → bit 0 
 * HIGH LONG + LOW LONG → bit 1 
 * Gap/invalid → try to decode frame 
 */
void subghz_protocol_decoder_honda_v3_ook_feed(
    void* ctx, bool level, uint32_t duration)
{
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV3OOK* inst = ctx;
    uint8_t c = ook_classify(duration);

    switch(inst->step) {

    case HV3OOKStep_Reset:
        if(level && c == 1u) {
            inst->last_high_dur  = duration;
            inst->preamble_count = 0;
            inst->step           = HV3OOKStep_Preamble;
        }
        break;

    case HV3OOKStep_Preamble:
        if(!level) {
            if(ook_classify(inst->last_high_dur) == 1u && c == 1u) {
                inst->preamble_count++;
            } else if(c == 0u) {
                if(inst->preamble_count >= HV3_OOK_MIN_PREAMBLE) {
                    inst->bit_count = 0;
                    memset(inst->data_buf, 0, sizeof(inst->data_buf));
                    inst->step = HV3OOKStep_Data;
                } else {
                    ook_decoder_reset_state(inst);
                }
            } else {

                if(inst->preamble_count >= HV3_OOK_MIN_PREAMBLE &&
                   ook_classify(inst->last_high_dur) == 2u && c == 2u) {
                    inst->bit_count = 0;
                    memset(inst->data_buf, 0, sizeof(inst->data_buf));
                    inst->data_buf[0] |= 0x01u;
                    inst->bit_count   = 1u;
                    inst->step        = HV3OOKStep_Data;
                } else {
                    ook_decoder_reset_state(inst);
                }
            }
        } else {
            inst->last_high_dur = duration;
        }
        break;

    case HV3OOKStep_Data:
        if(level) {
            inst->last_high_dur = duration;
        } else {
            uint8_t hc = ook_classify(inst->last_high_dur);

            if(hc == 1u && c == 1u) {
                if(inst->bit_count < (uint8_t)(HV3_OOK_FRAME_BITS))
                    inst->bit_count++;
            } else if(hc == 2u && c == 2u) {
                if(inst->bit_count < (uint8_t)(HV3_OOK_FRAME_BITS)) {
                    uint8_t byte_i = inst->bit_count / 8u;
                    uint8_t bit_i  = inst->bit_count % 8u;
                    if(byte_i < 16u)
                        inst->data_buf[byte_i] |= (uint8_t)(1u << bit_i);
                    inst->bit_count++;
                }
            } else {
                if(inst->bit_count >= HV3_OOK_MIN_BITS) {
                    if(ook_validate_frame(inst))
                        ook_notify_frame(inst);
                }
                ook_decoder_reset_state(inst);
                break;
            }

            if(inst->bit_count >= HV3_OOK_FRAME_BITS) {
                if(ook_validate_frame(inst))
                    ook_notify_frame(inst);
                ook_decoder_reset_state(inst);
            }
        }
        break;
    }

    inst->decoder.te_last = duration;
}

uint8_t subghz_protocol_decoder_honda_v3_ook_get_hash_data(void* ctx) {
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV3OOK* inst = ctx;
    return (uint8_t)(inst->generic.data ^
                    (inst->generic.data >>  8) ^
                    (inst->generic.data >> 16) ^
                    (inst->generic.data >> 24) ^
                    (inst->generic.data >> 32));
}

SubGhzProtocolStatus subghz_protocol_decoder_honda_v3_ook_serialize(
    void* ctx, FlipperFormat* ff, SubGhzRadioPreset* preset)
{
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV3OOK* inst = ctx;
    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&inst->generic, ff, preset);
    if(ret != SubGhzProtocolStatusOk) return ret;

    uint32_t v = inst->frame.serial;
    if(!flipper_format_write_uint32(ff, "Serial", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    v = inst->frame.counter;
    if(!flipper_format_write_uint32(ff, "Counter", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    v = inst->frame.button;
    if(!flipper_format_write_uint32(ff, "Button", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus subghz_protocol_decoder_honda_v3_ook_deserialize(
    void* ctx, FlipperFormat* ff)
{
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV3OOK* inst = ctx;
    SubGhzProtocolStatus ret =
        subghz_block_generic_deserialize_check_count_bit(
            &inst->generic, ff, HV3_OOK_FRAME_BITS);
    if(ret != SubGhzProtocolStatusOk) return ret;

    uint32_t v = 0;
    if(!flipper_format_read_uint32(ff, "Serial", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->frame.serial = v;

    if(!flipper_format_read_uint32(ff, "Counter", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->frame.counter = v;

    if(!flipper_format_read_uint32(ff, "Button", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->frame.button = (uint8_t)v;

    inst->frame.mode      = 0x2u;
    inst->frame.checksum  = 0u;
    inst->frame_valid     = true;
    inst->generic.serial  = inst->frame.serial;
    inst->generic.btn     = inst->frame.button;
    inst->generic.cnt     = inst->frame.counter;
    hv3_register_btn(inst->frame.button);
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_decoder_honda_v3_ook_get_string(
    void* ctx, FuriString* out)
{
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV3OOK* inst = ctx;

    if(!inst->frame_valid && inst->generic.data != 0) {
        uint8_t pm, ki, fl, dm;
        uint16_t roll;
        hv3_unpack(inst->generic.data,
                   &inst->frame.serial, &inst->frame.button,
                   &roll, &dm, &ki, &pm, &fl);
        inst->frame.counter   = roll;
        inst->frame.mode      = dm;
        inst->frame_valid     = true;
    }

    furi_string_cat_printf(
        out,
        "%s %ubit\r\n"
        "Btn:%s (0x%X)\r\n"
        "Ser:%07lX\r\n"
        "Cnt:%06lX Mode:%X\r\n",
        SUBGHZ_PROTOCOL_HONDA_V3_OOK_NAME,
        inst->generic.data_count_bit,
        hv3_btn_name(inst->frame.button),
        inst->frame.button,
        (unsigned long)inst->frame.serial,
        (unsigned long)inst->frame.counter,
        inst->frame.mode);
}

/* ── OOK Encoder ── */
static void ook_build_upload(SubGhzProtocolEncoderHondaV3OOK* inst) {
    LevelDuration* buf = inst->encoder.upload;
    size_t idx = 0;

    /* Guard: HIGH 100µs + LOW 740µs */
    buf[idx++] = level_duration_make(true,  HV3_OOK_GUARD_HIGH_US);
    buf[idx++] = level_duration_make(false, HV3_OOK_GUARD_LOW_US);

    for(uint16_t p = 0;
        p < HV3_OOK_PREAMBLE_CYCLES && idx < HV3_OOK_ENC_BUF_SIZE - 10u;
        p++) {
        buf[idx++] = level_duration_make(true,  HV3_OOK_TE_SHORT);
        buf[idx++] = level_duration_make(false, HV3_OOK_TE_SHORT);
    }

    uint8_t frame_buf[HV3_OOK_FRAME_BYTES];
    inst->frame.button = inst->active_button;
    ook_to_buf(&inst->frame, frame_buf);

    for(uint8_t byte_i = 0;
        byte_i < HV3_OOK_FRAME_BYTES && idx < HV3_OOK_ENC_BUF_SIZE - 4u;
        byte_i++) {
        uint8_t bval = frame_buf[byte_i];
        for(uint8_t bit_i = 0; bit_i < 8u; bit_i++) {
            bool bit = (bval >> bit_i) & 1u;
            if(bit) {
                buf[idx++] = level_duration_make(true,  HV3_OOK_TE_LONG);
                buf[idx++] = level_duration_make(false, HV3_OOK_TE_LONG);
            } else {
                buf[idx++] = level_duration_make(true,  HV3_OOK_TE_SHORT);
                buf[idx++] = level_duration_make(false, HV3_OOK_TE_SHORT);
            }
        }
    }

    buf[idx++] = level_duration_make(false, 4000u);

    inst->encoder.size_upload = idx;
    inst->encoder.front       = 0;
    FURI_LOG_D(TAG, "[OOK] upload %u symbols", (unsigned)idx);
}

void* subghz_protocol_encoder_honda_v3_ook_alloc(SubGhzEnvironment* env) {
    UNUSED(env);
    SubGhzProtocolEncoderHondaV3OOK* inst =
        malloc(sizeof(SubGhzProtocolEncoderHondaV3OOK));
    furi_check(inst);
    memset(inst, 0, sizeof(SubGhzProtocolEncoderHondaV3OOK));
    inst->base.protocol         = &subghz_protocol_honda_v3_ook;
    inst->generic.protocol_name = inst->base.protocol->name;
    inst->encoder.repeat        = 3;
    inst->encoder.upload        =
        malloc(HV3_OOK_ENC_BUF_SIZE * sizeof(LevelDuration));
    furi_check(inst->encoder.upload);
    inst->encoder.is_running    = false;
    return inst;
}

void subghz_protocol_encoder_honda_v3_ook_free(void* ctx) {
    furi_assert(ctx);
    SubGhzProtocolEncoderHondaV3OOK* inst = ctx;
    free(inst->encoder.upload);
    free(inst);
}

void subghz_protocol_encoder_honda_v3_ook_stop(void* ctx) {
    furi_assert(ctx);
    ((SubGhzProtocolEncoderHondaV3OOK*)ctx)->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_honda_v3_ook_yield(void* ctx) {
    furi_assert(ctx);
    SubGhzProtocolEncoderHondaV3OOK* inst = ctx;
    if(!inst->encoder.is_running || inst->encoder.repeat == 0)
        return level_duration_reset();
    LevelDuration ret = inst->encoder.upload[inst->encoder.front];
    if(++inst->encoder.front >= inst->encoder.size_upload) {
        inst->encoder.repeat--;
        inst->encoder.front = 0;
    }
    return ret;
}

SubGhzProtocolStatus subghz_protocol_encoder_honda_v3_ook_deserialize(
    void* ctx, FlipperFormat* ff)
{
    furi_assert(ctx);
    SubGhzProtocolEncoderHondaV3OOK* inst = ctx;
    SubGhzProtocolStatus ret =
        subghz_block_generic_deserialize(&inst->generic, ff);
    if(ret != SubGhzProtocolStatusOk) return ret;

    uint32_t v = 0;
    if(!flipper_format_read_uint32(ff, "Serial", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->frame.serial = v;

    if(!flipper_format_read_uint32(ff, "Counter", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->frame.counter = v;

    if(!flipper_format_read_uint32(ff, "Button", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->frame.button = (uint8_t)v;
    inst->frame.mode   = 0x2u;

    hv3_register_btn(inst->frame.button);
    inst->active_button = hv3_get_active_btn();

    inst->frame.counter = (inst->frame.counter +
        (uint32_t)furi_hal_subghz_get_rolling_counter_mult()) & 0x00FFFFFFu;
    ook_counter_increment(&inst->frame);
    inst->frame.button  = inst->active_button;

    inst->generic.cnt = inst->frame.counter;
    inst->generic.btn = inst->active_button;
    inst->generic.data = hv3_pack(
        inst->frame.serial, inst->active_button,
        (uint16_t)(inst->frame.counter & 0xFFFFu),
        inst->frame.mode, 0xFu, HV3_PROTO_MODE_OOK, 0u);

    flipper_format_rewind(ff);
    uint8_t kd[8];
    for(int i = 0; i < 8; i++)
        kd[i] = (uint8_t)(inst->generic.data >> (56 - i * 8));
    flipper_format_update_hex(ff, "Key", kd, 8);

    ook_build_upload(inst);
    inst->encoder.is_running = true;

    FURI_LOG_I(TAG, "[OOK] encoder ready ser=%07lX btn=%u cnt=%06lX",
               (unsigned long)inst->frame.serial,
               inst->active_button,
               (unsigned long)inst->frame.counter);
    return SubGhzProtocolStatusOk;
}

/* ═══════════════════════════════════════════════════════════════════
 * ██████████████████ MODE 2 — KEELOQ INVERTED PWM ██████████████████ 
 * Crypto_Scramble_Finalize_Keeloq @ 0x9C00 
 * 
 * Bit 0: HIGH 400µs + LOW 400µs 
 * Bit 1: HIGH 400µs + LOW ~50µs ← PWM INVERTED 
 * Preamble: 23 cycles 
 * Sync gap: LOW 4000µs 
 * Trail: LOW 15600µs
 * 5 repetitions, new RC in each
 * ══════════════════════════════════════════════════════════════════════════ */


static void kl_process_frame(SubGhzProtocolDecoderHondaV3KL* inst) {
    uint64_t raw = 0;
    uint64_t tmp = inst->decoder.decode_data;
    for(int i = 0; i < 64; i++) {
        raw = (raw << 1) | (tmp & 1u);
        tmp >>= 1;
    }

    uint32_t enc    = (uint32_t)(raw & 0xFFFFFFFFu);
    uint32_t serial = (uint32_t)((raw >> 32) & 0x0FFFFFFFu);
    uint8_t  btn    = (uint8_t) ((raw >> 60) & 0x0Fu);

    uint32_t dec = 0;
    uint16_t rc  = 0;
    int8_t   ki  = kl_find_key(enc, btn, &dec, &rc);

    if(ki < 0) {
        FURI_LOG_D(TAG, "[KL] no key enc=%08lX btn=%u",
                   (unsigned long)enc, btn);
        return;
    }

    if(inst->has_last_rolling) {
        uint16_t diff = (uint16_t)(rc - inst->last_rolling);
        if(diff == 0 || diff > HV3_KL_ROLLING_WINDOW) {
            FURI_LOG_D(TAG, "[KL] RC out window rc=%u last=%u",
                       rc, inst->last_rolling);
            return;
        }
    }

    uint8_t discr = (uint8_t)((dec & HV3_KL_DEC_DISC_MASK)
                                   >> HV3_KL_DEC_DISC_SHIFT);

    inst->frame.encrypted    = enc;
    inst->frame.serial       = serial;
    inst->frame.button       = btn;
    inst->frame.status       = 0u;
    inst->frame.decrypted    = dec;
    inst->frame.rolling_code = rc;
    inst->frame.discriminant = discr;
    inst->frame.key_idx      = ki;
    inst->frame_valid        = true;
    inst->last_rolling       = rc;
    inst->has_last_rolling   = true;

    inst->generic.data = hv3_pack(
        serial, btn, rc, discr,
        (uint8_t)ki, HV3_PROTO_MODE_KL, 0u);
    inst->generic.data_count_bit = HV3_KL_FRAME_BITS;
    inst->generic.serial = serial;
    inst->generic.btn    = btn;
    inst->generic.cnt    = rc;

    hv3_register_btn(btn);

    FURI_LOG_I(TAG,
        "[KL] VALID key[%d] ser=%07lX btn=%u rc=%04X enc=%08lX dec=%08lX",
        ki, (unsigned long)serial, btn, rc,
        (unsigned long)enc, (unsigned long)dec);

    if(inst->base.callback)
        inst->base.callback(&inst->base, inst->base.context);
}

/* ── KL Decoder: alloc / free / reset ── */
void* subghz_protocol_decoder_honda_v3_kl_alloc(SubGhzEnvironment* env) {
    UNUSED(env);
    SubGhzProtocolDecoderHondaV3KL* inst =
        malloc(sizeof(SubGhzProtocolDecoderHondaV3KL));
    furi_check(inst);
    memset(inst, 0, sizeof(SubGhzProtocolDecoderHondaV3KL));
    inst->base.protocol         = &subghz_protocol_honda_v3_kl;
    inst->generic.protocol_name = inst->base.protocol->name;
    inst->result_str            = furi_string_alloc();
    inst->frame.key_idx         = -1;
    inst->has_last_rolling      = false;
    inst->frame_valid           = false;
    FURI_LOG_I(TAG, "[KL] decoder alloc");
    return inst;
}

void subghz_protocol_decoder_honda_v3_kl_free(void* ctx) {
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV3KL* inst = ctx;
    furi_string_free(inst->result_str);
    free(inst);
}

void subghz_protocol_decoder_honda_v3_kl_reset(void* ctx) {
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV3KL* inst = ctx;
    inst->decoder.parser_step      = 0;
    inst->decoder.decode_data      = 0;
    inst->decoder.decode_count_bit = 0;
}

/* ── KL Decoder: feed ──
 *
 * State 0 (RESET): wait for LOW >= HV3_KL_SYNC_MIN_US
 * State 1 (DATA):
 * LOW >= SYNC_MIN → inter-repeat sync gap
 * LOW in [250..550] → bit 0 (LOW 400µs ± delta)
 * LOW < 250 or inverted → bit 1 (PWM inverted, LOW cut off)
 * Bit 64 reached → process frame
 *
 * The shift register accumulates MSB-first.
 * kl_process_frame() reverts to LSB-first to extract fields.
 */
void subghz_protocol_decoder_honda_v3_kl_feed(
    void* ctx, bool level, uint32_t duration)
{
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV3KL* inst = ctx;

    switch(inst->decoder.parser_step) {

    case 0:
        if(!level && duration >= HV3_KL_SYNC_MIN_US) {
            inst->decoder.decode_data      = 0;
            inst->decoder.decode_count_bit = 0;
            inst->decoder.parser_step      = 1;
            FURI_LOG_D(TAG, "[KL] sync dur=%lu", (unsigned long)duration);
        }
        break;

    case 1: /* DATA */
        if(!level) {
            if(duration >= HV3_KL_SYNC_MIN_US) {
                if(inst->decoder.decode_count_bit == HV3_KL_FRAME_BITS) {
                    kl_process_frame(inst);
                } else if(inst->decoder.decode_count_bit > 60u) {
                    kl_process_frame(inst);
                }
                inst->decoder.decode_data      = 0;
                inst->decoder.decode_count_bit = 0;
                break;
            }

            uint8_t bit_val;
            if(duration + HV3_KL_TE_DELTA >= HV3_KL_TE_BASE &&
               duration <= HV3_KL_TE_BASE + HV3_KL_TE_DELTA) {
                bit_val = 0u;
            } else {
                bit_val = 1u;
            }

            if(inst->decoder.decode_count_bit < 64u) {
                inst->decoder.decode_data >>= 1;
                if(bit_val)
                    inst->decoder.decode_data |= (1ULL << 63);
            }
            inst->decoder.decode_count_bit++;

            if(inst->decoder.decode_count_bit >= HV3_KL_FRAME_BITS) {
                kl_process_frame(inst);
                inst->decoder.decode_data      = 0;
                inst->decoder.decode_count_bit = 0;
                inst->decoder.parser_step      = 0;
            }
        }

        break;

    default:
        inst->decoder.parser_step = 0;
        break;
    }

    inst->decoder.te_last = duration;
}

/* ── KL Decoder: hash / serialize / deserialize / get_string ── */
uint8_t subghz_protocol_decoder_honda_v3_kl_get_hash_data(void* ctx) {
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV3KL* inst = ctx;
    return (uint8_t)(
         inst->frame.serial ^
        (inst->frame.serial >>  8) ^
        (inst->frame.serial >> 16) ^
         inst->frame.button ^
        (uint8_t)(inst->frame.key_idx >= 0 ? inst->frame.key_idx : 0));
}

SubGhzProtocolStatus subghz_protocol_decoder_honda_v3_kl_serialize(
    void* ctx, FlipperFormat* ff, SubGhzRadioPreset* preset)
{
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV3KL* inst = ctx;
    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&inst->generic, ff, preset);
    if(ret != SubGhzProtocolStatusOk) return ret;

    uint32_t v = inst->frame.serial;
    if(!flipper_format_write_uint32(ff, "Serial", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    v = inst->frame.rolling_code;
    if(!flipper_format_write_uint32(ff, "RollingCode", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    v = inst->frame.button;
    if(!flipper_format_write_uint32(ff, "Button", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    v = (inst->frame.key_idx >= 0) ?
        (uint32_t)(uint8_t)inst->frame.key_idx : 0u;
    if(!flipper_format_write_uint32(ff, "KeyIdx", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus subghz_protocol_decoder_honda_v3_kl_deserialize(
    void* ctx, FlipperFormat* ff)
{
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV3KL* inst = ctx;
    SubGhzProtocolStatus ret =
        subghz_block_generic_deserialize_check_count_bit(
            &inst->generic, ff, HV3_KL_FRAME_BITS);
    if(ret != SubGhzProtocolStatusOk) return ret;

    uint32_t v = 0;
    if(!flipper_format_read_uint32(ff, "Serial", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->frame.serial = v;

    if(!flipper_format_read_uint32(ff, "RollingCode", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->frame.rolling_code = (uint16_t)v;

    if(!flipper_format_read_uint32(ff, "Button", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->frame.button = (uint8_t)v;

    if(!flipper_format_read_uint32(ff, "KeyIdx", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->frame.key_idx = (int8_t)v;

    inst->frame.discriminant  = (uint8_t)((inst->frame.serial >> 20) & 0xFFu);
    inst->frame_valid         = true;
    inst->has_last_rolling    = true;
    inst->last_rolling        = inst->frame.rolling_code;
    inst->generic.serial      = inst->frame.serial;
    inst->generic.btn         = inst->frame.button;
    inst->generic.cnt         = inst->frame.rolling_code;
    hv3_register_btn(inst->frame.button);
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_decoder_honda_v3_kl_get_string(
    void* ctx, FuriString* out)
{
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV3KL* inst = ctx;

    if(!inst->frame_valid && inst->generic.data != 0) {
        uint8_t pm, ki, fl, dm;
        uint16_t roll;
        hv3_unpack(inst->generic.data,
                   &inst->frame.serial,
                   &inst->frame.button,
                   &roll, &dm, &ki, &pm, &fl);
        inst->frame.rolling_code  = roll;
        inst->frame.discriminant  = dm;
        inst->frame.key_idx       = (int8_t)ki;
        inst->frame_valid         = true;
    }

    furi_string_cat_printf(
        out,
        "%s %ubit\r\n"
        "Btn:%s (0x%X)\r\n"
        "Ser:%07lX\r\n"
        "RC:%04X Key[%d]\r\n"
        "Dis:%02X Enc:%08lX\r\n",
        SUBGHZ_PROTOCOL_HONDA_V3_KL_NAME,
        inst->generic.data_count_bit,
        hv3_btn_name(inst->frame.button),
        inst->frame.button,
        (unsigned long)inst->frame.serial,
        inst->frame.rolling_code,
        inst->frame.key_idx,
        inst->frame.discriminant,
        (unsigned long)inst->frame.encrypted);
}

/* ── KL Encoder ── */
static void kl_build_upload(SubGhzProtocolEncoderHondaV3KL* inst) {
    LevelDuration* buf = inst->encoder.upload;
    size_t idx = 0;

    for(uint8_t rep = 0;
        rep < HV3_KL_REPEAT && idx < HV3_KL_ENC_BUF_SIZE - 200u;
        rep++)
    {

        for(uint8_t p = 0; p < HV3_KL_PREAMBLE_COUNT; p++) {
            buf[idx++] = level_duration_make(true,  HV3_KL_TE_BASE);
            buf[idx++] = level_duration_make(false, HV3_KL_TE_BASE);
        }

        /* Sync gap */
        buf[idx++] = level_duration_make(false, HV3_KL_SYNC_GAP_US);

        for(uint8_t b = 0;
            b < HV3_KL_FRAME_BITS && idx < HV3_KL_ENC_BUF_SIZE - 4u;
            b++) {
            bool bit = inst->frame_bits[b];
            buf[idx++] = level_duration_make(true, HV3_KL_TE_BASE);
            if(bit) {
                buf[idx++] = level_duration_make(false, HV3_KL_TE_BIT1_LOW);
                buf[idx++] = level_duration_make(false,
                    HV3_KL_TE_BASE - HV3_KL_TE_BIT1_LOW);
            } else {
                buf[idx++] = level_duration_make(false, HV3_KL_TE_BASE);
            }
        }

        /* Trail */
        buf[idx++] = level_duration_make(false, HV3_KL_TRAIL_US);

        if(rep + 1u < HV3_KL_REPEAT) {
            inst->rolling_code++;
            kl_build_frame(
                inst->frame_bits,
                inst->key,
                inst->serial,
                inst->active_button,
                inst->status,
                inst->rolling_code);
        }
    }

    inst->encoder.size_upload = idx;
    inst->encoder.front       = 0;
    FURI_LOG_D(TAG, "[KL] upload %u symbols", (unsigned)idx);
}

void* subghz_protocol_encoder_honda_v3_kl_alloc(SubGhzEnvironment* env) {
    UNUSED(env);
    SubGhzProtocolEncoderHondaV3KL* inst =
        malloc(sizeof(SubGhzProtocolEncoderHondaV3KL));
    furi_check(inst);
    memset(inst, 0, sizeof(SubGhzProtocolEncoderHondaV3KL));
    inst->base.protocol         = &subghz_protocol_honda_v3_kl;
    inst->generic.protocol_name = inst->base.protocol->name;
    inst->encoder.repeat        = (int32_t)1;
    inst->encoder.upload        =
        malloc(HV3_KL_ENC_BUF_SIZE * sizeof(LevelDuration));
    furi_check(inst->encoder.upload);
    inst->encoder.is_running    = false;
    return inst;
}

void subghz_protocol_encoder_honda_v3_kl_free(void* ctx) {
    furi_assert(ctx);
    SubGhzProtocolEncoderHondaV3KL* inst = ctx;
    free(inst->encoder.upload);
    free(inst);
}

void subghz_protocol_encoder_honda_v3_kl_stop(void* ctx) {
    furi_assert(ctx);
    ((SubGhzProtocolEncoderHondaV3KL*)ctx)->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_honda_v3_kl_yield(void* ctx) {
    furi_assert(ctx);
    SubGhzProtocolEncoderHondaV3KL* inst = ctx;
    if(!inst->encoder.is_running || inst->encoder.repeat == 0)
        return level_duration_reset();
    LevelDuration ret = inst->encoder.upload[inst->encoder.front];
    if(++inst->encoder.front >= inst->encoder.size_upload) {
        inst->encoder.repeat--;
        inst->encoder.front = 0;
    }
    return ret;
}

SubGhzProtocolStatus subghz_protocol_encoder_honda_v3_kl_deserialize(
    void* ctx, FlipperFormat* ff)
{
    furi_assert(ctx);
    SubGhzProtocolEncoderHondaV3KL* inst = ctx;
    SubGhzProtocolStatus ret =
        subghz_block_generic_deserialize(&inst->generic, ff);
    if(ret != SubGhzProtocolStatusOk) return ret;

    uint32_t v = 0;
    if(!flipper_format_read_uint32(ff, "Serial", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->serial = v;

    if(!flipper_format_read_uint32(ff, "RollingCode", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->rolling_code = (uint16_t)v;

    if(!flipper_format_read_uint32(ff, "Button", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->btn = (uint8_t)v;

    if(!flipper_format_read_uint32(ff, "KeyIdx", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    int8_t ki = (int8_t)v;
    if(ki < 0 || ki >= HV3_MFR_KEY_COUNT)
        return SubGhzProtocolStatusErrorParserOthers;

    inst->key = ((uint64_t)honda_v3_mfr_keys[ki].high << 32) |
                 (uint64_t)honda_v3_mfr_keys[ki].low;

    hv3_register_btn(inst->btn);
    inst->active_button = hv3_get_active_btn();
    inst->status        = 0u;

    inst->rolling_code = (uint16_t)(
        inst->rolling_code +
        (uint16_t)furi_hal_subghz_get_rolling_counter_mult());

    kl_build_frame(
        inst->frame_bits,
        inst->key,
        inst->serial,
        inst->active_button,
        inst->status,
        inst->rolling_code);

    inst->generic.cnt = inst->rolling_code;
    inst->generic.btn = inst->active_button;

    flipper_format_rewind(ff);
    uint8_t kd[8];
    uint64_t packed = hv3_pack(
        inst->serial, inst->active_button,
        inst->rolling_code,
        (uint8_t)((inst->serial >> 20) & 0xFFu),
        (uint8_t)ki, HV3_PROTO_MODE_KL, 0u);
    for(int i = 0; i < 8; i++)
        kd[i] = (uint8_t)(packed >> (56 - i * 8));
    flipper_format_update_hex(ff, "Key", kd, 8);

    kl_build_upload(inst);
    inst->encoder.is_running = true;

    FURI_LOG_I(TAG, "[KL] encoder ready ser=%07lX btn=%u rc=%04X key=%016llX",
               (unsigned long)inst->serial,
               inst->active_button,
               inst->rolling_code,
               (unsigned long long)inst->key);
    return SubGhzProtocolStatusOk;
}

/* ═══════════════════════════════════════════════════════════════════
 * ████████████████████ MODE 3 — FSK MANCHESTER ████████████████████
 * Protocol_TX_Honda_Extended @ 0xEFDC
 *
 * FSK Manchester captured as an OOK (~8x compressed):
 * Half-bit SHORT: ~63µs in OOK capture
 * Half-bit LONG: ~126µs (two merged half-bits at the same level)
 * Manchester IEEE 802.3:
 * bit 1: LOW→HIGH (rising edge, half-bits 0.1) 
 * bit 0: HIGH→LOW (falling edge, half-bits 1.0) 
 * MSB-first, 112 bits (14 bytes) 
 *
 * Checksum rolling (table @ 0xF0CE) for Mode 3 extended. 
 * ═════════════════════════════════════════════════════════════════════════ */


static inline uint8_t fsk_classify(uint32_t dur) {
    if(dur + HV3_FSK_TE_DELTA >= HV3_FSK_TE_SHORT &&
       dur <= HV3_FSK_TE_SHORT + HV3_FSK_TE_DELTA) return 1u;
    if(dur + HV3_FSK_TE_DELTA >= HV3_FSK_TE_LONG &&
       dur <= HV3_FSK_TE_LONG  + HV3_FSK_TE_DELTA) return 2u;
    return 0u;
}

static uint8_t fsk_rolling_checksum(const uint8_t buf[HV3_FSK_FRAME_BYTES]) {
    uint8_t prev_lo = buf[8] & 0x0Fu;
    uint8_t prev_hi = (buf[8] >> 4) & 0x0Fu;
    uint8_t new_lo  = prev_lo;
    uint8_t new_hi  = prev_hi;

    uint8_t col_lo = hv3_bit_rev4(buf[3] & 0x0Fu);
    uint8_t col_hi = hv3_bit_rev4((buf[3] >> 4) & 0x0Fu);

    bool found = false;
    for(uint8_t row = 0; row < 16u && !found; row++) {
        if(hv3_tbl_primary[row][15] == prev_lo) {
            uint8_t target = hv3_tbl_primary[row][0];
            for(uint8_t r2 = 0; r2 < 16u; r2++) {
                if(hv3_tbl_subst[r2][col_lo] == target) {
                    uint8_t nc = (col_lo + 1u) & 0x0Fu;
                    new_lo = hv3_tbl_subst[r2][nc];
                    found = true;
                    break;
                }
            }
            if(!found) {
                new_lo = hv3_tbl_subst[row][col_lo];
                found = true;
            }
        }
    }
    if(!found) {
        for(uint8_t row = 0; row < 16u; row++) {
            if(hv3_tbl_secondary[row][15] == prev_lo) {
                uint8_t target = hv3_tbl_secondary[row][0];
                for(uint8_t r2 = 0; r2 < 16u; r2++) {
                    if(hv3_tbl_subst[r2][col_lo] == target) {
                        uint8_t nc = (col_lo + 1u) & 0x0Fu;
                        new_lo = hv3_tbl_subst[r2][nc];
                        break;
                    }
                }
                break;
            }
        }
    }

    /* High nibble */
    found = false;
    for(uint8_t row = 0; row < 16u && !found; row++) {
        if(hv3_tbl_primary[row][15] == prev_hi) {
            uint8_t target = hv3_tbl_primary[row][0];
            for(uint8_t r2 = 0; r2 < 16u; r2++) {
                if(hv3_tbl_subst[r2][col_hi] == target) {
                    uint8_t nc = (col_hi + 1u) & 0x0Fu;
                    new_hi = hv3_tbl_subst[r2][nc];
                    found = true;
                    break;
                }
            }
            if(!found) {
                new_hi = hv3_tbl_subst[row][col_hi];
                found = true;
            }
        }
    }
    if(!found) {
        for(uint8_t row = 0; row < 16u; row++) {
            if(hv3_tbl_secondary[row][15] == prev_hi) {
                uint8_t target = hv3_tbl_secondary[row][0];
                for(uint8_t r2 = 0; r2 < 16u; r2++) {
                    if(hv3_tbl_subst[r2][col_hi] == target) {
                        uint8_t nc = (col_hi + 1u) & 0x0Fu;
                        new_hi = hv3_tbl_subst[r2][nc];
                        break;
                    }
                }
                break;
            }
        }
    }

    return (uint8_t)((new_hi << 4) | (new_lo & 0x0Fu));
}

static uint8_t fsk_xor_checksum(const uint8_t* data, uint8_t len) {
    uint8_t c = 0;
    for(uint8_t i = 0; i < len; i++) c ^= data[i];
    return c;
}

/* build/parse buf ↔ frame for Type-A & Type-B */
static void fsk_to_buf(
    const HondaV3FSKFrame* f,
    uint8_t buf[HV3_FSK_FRAME_BYTES])
{
    memset(buf, 0, HV3_FSK_FRAME_BYTES);
    if(!f->type_b) {
        buf[0] = (uint8_t)((f->button << 4) | ((f->serial >> 24) & 0x0Fu));
        buf[1] = (uint8_t)((f->serial >> 16) & 0xFFu);
        buf[2] = (uint8_t)((f->serial >>  8) & 0xFFu);
        buf[3] = (uint8_t)( f->serial        & 0xFFu);
        buf[4] = (uint8_t)((f->counter >> 16) & 0xFFu);
        buf[5] = (uint8_t)((f->counter >>  8) & 0xFFu);
        buf[6] = (uint8_t)( f->counter        & 0xFFu);
        buf[7] = (uint8_t)(((f->mode & 0x0Fu) << 4) | (f->counter & 0x0Fu));
    } else {
        buf[0] = (uint8_t)((f->type_b_header << 4) | (f->button & 0x0Fu));
        buf[1] = (uint8_t)((f->serial >> 20) & 0xFFu);
        buf[2] = (uint8_t)((f->serial >> 12) & 0xFFu);
        buf[3] = (uint8_t)((f->serial >>  4) & 0xFFu);
        buf[4] = (uint8_t)(((f->serial & 0x0Fu) << 4) |
                            ((f->counter >> 20) & 0x0Fu));
        buf[5] = (uint8_t)((f->counter >> 12) & 0xFFu);
        buf[6] = (uint8_t)((f->counter >>  4) & 0xFFu);
        buf[7] = (uint8_t)(((f->mode & 0x0Fu) << 4) | (f->counter & 0x0Fu));
    }
    buf[8] = f->checksum;
    memcpy(&buf[9], f->extra, 5u);
}

static void fsk_from_buf(
    const uint8_t buf[HV3_FSK_FRAME_BYTES],
    HondaV3FSKFrame* f,
    bool type_b)
{
    f->type_b = type_b;
    if(!type_b) {
        f->type_b_header = 0u;
        f->button  = (buf[0] >> 4) & 0x0Fu;
        f->serial  = ((uint32_t)(buf[0] & 0x0Fu) << 24) |
                     ((uint32_t)buf[1] << 16) |
                     ((uint32_t)buf[2] <<  8) |
                      (uint32_t)buf[3];
        f->counter = ((uint32_t)buf[4] << 16) |
                     ((uint32_t)buf[5] <<  8) |
                      (uint32_t)buf[6];
        f->mode    = (buf[7] >> 4) & 0x0Fu;
    } else {
        f->type_b_header = (buf[0] >> 4) & 0x0Fu;
        f->button  = buf[0] & 0x0Fu;
        f->serial  = ((uint32_t)buf[1] << 20) |
                     ((uint32_t)buf[2] << 12) |
                     ((uint32_t)buf[3] <<  4) |
                     ((uint32_t)(buf[4] >> 4) & 0x0Fu);
        f->counter = ((uint32_t)(buf[4] & 0x0Fu) << 20) |
                     ((uint32_t)buf[5] << 12) |
                     ((uint32_t)buf[6] <<  4) |
                      (uint32_t)(buf[7] & 0x0Fu);
        f->mode    = (buf[7] >> 4) & 0x0Fu;
    }
    f->checksum = buf[8];
    memcpy(f->extra, &buf[9], 5u);
}

static void fsk_counter_increment(HondaV3FSKFrame* f) {
    f->counter = (f->counter + 1u) & 0x00FFFFFFu;
    f->mode    = (f->mode == 0xCu) ? 0x2u : 0xCu;
}

/*
 * Manchester decoder for accumulated half-bits.
 *
 * Receives an array of half-bits (0=LOW, 1=HIGH).
 * Finds the end of the preamble (run alternating ≥ MIN_PREAMBLE).
 * Decodes pairs of half-bits:
 * (0,1) → bit 1 (LOW→HIGH, rising)
 * (1,0) → bit 0 (HIGH→LOW, falling)
 * MSB-first in decoded[].
 */
static bool fsk_manchester_decode(
    const uint8_t* hb, uint16_t hb_count,
    uint8_t* decoded, uint8_t* out_bits,
    uint8_t max_bits, bool invert)
{
    uint8_t bit_count = 0;
    memset(decoded, 0, (max_bits + 7u) / 8u);

    uint16_t alt_run = 0;
    uint16_t preamble_end = 0;
    for(uint16_t j = 1; j < hb_count; j++) {
        if(hb[j] != hb[j-1]) {
            alt_run++;
        } else {
            if(alt_run >= HV3_FSK_MIN_PREAMBLE) {
                preamble_end = j;
                break;
            }
            alt_run = 0;
        }
    }

    if(preamble_end == 0 && alt_run >= HV3_FSK_MIN_PREAMBLE)
        preamble_end = hb_count;

    if(preamble_end == 0) {
        *out_bits = 0;
        return false;
    }

    uint16_t i = preamble_end;

    while(i + 1 < hb_count && hb[i] == hb[i+1]) i++;

    /* Clamp max_bits to buffer capacity (decoded is HV3_FSK_FRAME_BYTES + 2 = 16 bytes) */
    if(max_bits > (HV3_FSK_FRAME_BYTES + 2) * 8u) {
        max_bits = (HV3_FSK_FRAME_BYTES + 2) * 8u;
    }

    while(i + 1 < hb_count && bit_count < max_bits) {
        uint8_t h0 = hb[i];
        uint8_t h1 = hb[i+1];

        if(h0 == h1) break;

        uint8_t bit_val;
        if(!invert) {
            bit_val = (h0 == 0u && h1 == 1u) ? 1u : 0u;
        } else {
            bit_val = (h0 == 1u && h1 == 0u) ? 1u : 0u;
        }

        /* MSB-first — byte_idx bounded by max_bits clamp above */
        uint8_t byte_idx = bit_count / 8u;
        uint8_t bit_idx  = 7u - (bit_count % 8u);
        if(bit_val)
            decoded[byte_idx] |= (uint8_t)(1u << bit_idx);

        bit_count++;
        i += 2;
    }

    *out_bits = bit_count;
    return (bit_count >= HV3_FSK_MIN_BITS);
}

static bool fsk_try_decode_polarity(
    SubGhzProtocolDecoderHondaV3FSK* inst, bool invert)
{
    uint8_t decoded[HV3_FSK_FRAME_BYTES + 2] = {0};
    uint8_t bit_count = 0;

    if(!fsk_manchester_decode(
        inst->half_bits, inst->hb_count,
        decoded, &bit_count,
        HV3_FSK_FRAME_BITS, invert)) {
        return false;
    }

    FURI_LOG_D(TAG,
        "[FSK] pol=%s bits=%u: %02X %02X %02X %02X %02X %02X %02X %02X",
        invert ? "INV" : "NOR", bit_count,
        decoded[0], decoded[1], decoded[2], decoded[3],
        decoded[4], decoded[5], decoded[6], decoded[7]);

    /* Try Type-A */
    if(bit_count >= 72u) {
        HondaV3FSKFrame f;
        fsk_from_buf(decoded, &f, false);

        if(f.button <= HV3_BTN_UNLOCK2 && f.serial != 0 &&
           f.serial != 0x0FFFFFFFu) {
            uint8_t xchk = fsk_xor_checksum(decoded, 8u);
            if(xchk == decoded[8] ||
               popcount8(xchk ^ decoded[8]) <= 1) {
                inst->frame       = f;
                inst->frame_valid = true;
                FURI_LOG_I(TAG,
                    "[FSK] TypeA pol=%s btn=%u ser=%07lX cnt=%06lX mode=%X",
                    invert?"INV":"NOR",
                    f.button, (unsigned long)f.serial,
                    (unsigned long)f.counter, f.mode);
                return true;
            }
        }
    }

    /* Try Type-B */
    if(bit_count >= 72u) {
        HondaV3FSKFrame f;
        fsk_from_buf(decoded, &f, true);

        if(f.button <= HV3_BTN_UNLOCK2 && f.serial != 0 &&
           f.serial != 0x0FFFFFFFu) {
            uint8_t xchk = fsk_xor_checksum(decoded, 8u);
            if(xchk == decoded[8] ||
               popcount8(xchk ^ decoded[8]) <= 1) {
                inst->frame       = f;
                inst->frame_valid = true;
                FURI_LOG_I(TAG,
                    "[FSK] TypeB pol=%s hdr=%u btn=%u ser=%07lX cnt=%06lX",
                    invert?"INV":"NOR",
                    f.type_b_header, f.button,
                    (unsigned long)f.serial, (unsigned long)f.counter);
                return true;
            }
        }
    }

    /* Fallback 64 bits */
    if(bit_count >= HV3_FSK_MIN_BITS && bit_count < 72u) {
        HondaV3FSKFrame f;
        fsk_from_buf(decoded, &f, false);
        if(f.button > 0 && f.button <= HV3_BTN_UNLOCK2 &&
           f.serial != 0 && f.serial != 0x0FFFFFFFu) {
            inst->frame       = f;
            inst->frame_valid = true;
            FURI_LOG_I(TAG,
                "[FSK] 64bit pol=%s btn=%u ser=%07lX",
                invert?"INV":"NOR", f.button,
                (unsigned long)f.serial);
            return true;
        }
    }

    return false;
}

static bool fsk_try_decode(SubGhzProtocolDecoderHondaV3FSK* inst) {
    if(inst->hb_count < (HV3_FSK_MIN_PREAMBLE + 16u)) return false;
    if(fsk_try_decode_polarity(inst, false)) return true;
    if(fsk_try_decode_polarity(inst, true))  return true;
    return false;
}

/* ── FSK Decoder: alloc / free / reset ── */
void* subghz_protocol_decoder_honda_v3_fsk_alloc(SubGhzEnvironment* env) {
    UNUSED(env);
    SubGhzProtocolDecoderHondaV3FSK* inst =
        malloc(sizeof(SubGhzProtocolDecoderHondaV3FSK));
    furi_check(inst);
    memset(inst, 0, sizeof(SubGhzProtocolDecoderHondaV3FSK));
    inst->base.protocol         = &subghz_protocol_honda_v3_fsk;
    inst->generic.protocol_name = inst->base.protocol->name;
    inst->frame_valid           = false;
    FURI_LOG_I(TAG, "[FSK] decoder alloc");
    return inst;
}

void subghz_protocol_decoder_honda_v3_fsk_free(void* ctx) {
    furi_assert(ctx);
    free(ctx);
}

void subghz_protocol_decoder_honda_v3_fsk_reset(void* ctx) {
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV3FSK* inst = ctx;
    inst->decoder.parser_step = 0;
    inst->decoder.te_last     = 0;
    inst->hb_count            = 0;
}

void subghz_protocol_decoder_honda_v3_fsk_feed(
    void* ctx, bool level, uint32_t duration)
{
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV3FSK* inst = ctx;
    uint8_t lvl = level ? 1u : 0u;
    uint8_t dc  = fsk_classify(duration);

    if(dc == 1u) {
        if(inst->hb_count < HV3_FSK_HALF_BIT_BUF)
            inst->half_bits[inst->hb_count++] = lvl;
    } else if(dc == 2u) {
        if(inst->hb_count + 2u <= HV3_FSK_HALF_BIT_BUF) {
            inst->half_bits[inst->hb_count++] = lvl;
            inst->half_bits[inst->hb_count++] = lvl;
        }
    } else {
        if(inst->hb_count >= (HV3_FSK_MIN_PREAMBLE + 16u)) {
            if(fsk_try_decode(inst)) {
                inst->generic.data = hv3_pack(
                    inst->frame.serial,
                    inst->frame.button,
                    (uint16_t)(inst->frame.counter & 0xFFFFu),
                    inst->frame.mode,
                    0xFu,
                    HV3_PROTO_MODE_FSK,
                    (uint8_t)(inst->frame.type_b ? 1u : 0u));
                inst->generic.data_count_bit = HV3_FSK_FRAME_BITS;
                inst->generic.serial = inst->frame.serial;
                inst->generic.btn    = inst->frame.button;
                inst->generic.cnt    = inst->frame.counter;
                hv3_register_btn(inst->frame.button);
                if(inst->base.callback)
                    inst->base.callback(&inst->base, inst->base.context);
            }
        }
        inst->hb_count = 0;
    }

    inst->decoder.te_last = duration;
}

/* ── FSK Decoder: hash / serialize / deserialize / get_string ── */
uint8_t subghz_protocol_decoder_honda_v3_fsk_get_hash_data(void* ctx) {
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV3FSK* inst = ctx;
    return (uint8_t)(inst->generic.data ^
                    (inst->generic.data >>  8) ^
                    (inst->generic.data >> 16) ^
                    (inst->generic.data >> 24) ^
                    (inst->generic.data >> 32));
}

SubGhzProtocolStatus subghz_protocol_decoder_honda_v3_fsk_serialize(
    void* ctx, FlipperFormat* ff, SubGhzRadioPreset* preset)
{
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV3FSK* inst = ctx;
    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&inst->generic, ff, preset);
    if(ret != SubGhzProtocolStatusOk) return ret;

    uint32_t v = inst->frame.serial;
    if(!flipper_format_write_uint32(ff, "Serial", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    v = inst->frame.counter;
    if(!flipper_format_write_uint32(ff, "Counter", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    v = inst->frame.button;
    if(!flipper_format_write_uint32(ff, "Button", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    v = (uint32_t)(inst->frame.type_b ? 1u : 0u);
    if(!flipper_format_write_uint32(ff, "TypeB", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus subghz_protocol_decoder_honda_v3_fsk_deserialize(
    void* ctx, FlipperFormat* ff)
{
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV3FSK* inst = ctx;
    SubGhzProtocolStatus ret =
        subghz_block_generic_deserialize_check_count_bit(
            &inst->generic, ff, HV3_FSK_FRAME_BITS);
    if(ret != SubGhzProtocolStatusOk) return ret;

    uint32_t v = 0;
    if(!flipper_format_read_uint32(ff, "Serial", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->frame.serial = v;

    if(!flipper_format_read_uint32(ff, "Counter", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->frame.counter = v;

    if(!flipper_format_read_uint32(ff, "Button", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->frame.button = (uint8_t)v;

    if(!flipper_format_read_uint32(ff, "TypeB", &v, 1))
        inst->frame.type_b = false;
    else
        inst->frame.type_b = (v != 0u);

    inst->frame.mode        = 0x2u;
    inst->frame.checksum    = 0u;
    inst->frame_valid       = true;
    inst->generic.serial    = inst->frame.serial;
    inst->generic.btn       = inst->frame.button;
    inst->generic.cnt       = inst->frame.counter;
    hv3_register_btn(inst->frame.button);
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_decoder_honda_v3_fsk_get_string(
    void* ctx, FuriString* out)
{
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV3FSK* inst = ctx;

    if(!inst->frame_valid && inst->generic.data != 0) {
        uint8_t pm, ki, fl, dm;
        uint16_t roll;
        hv3_unpack(inst->generic.data,
                   &inst->frame.serial, &inst->frame.button,
                   &roll, &dm, &ki, &pm, &fl);
        inst->frame.counter  = roll;
        inst->frame.mode     = dm;
        inst->frame.type_b   = (bool)(fl & 1u);
        inst->frame_valid    = true;
    }

    furi_string_cat_printf(
        out,
        "%s %s %ubit\r\n"
        "Btn:%s (0x%X)\r\n"
        "Ser:%07lX\r\n"
        "Cnt:%06lX Mode:%X\r\n",
        SUBGHZ_PROTOCOL_HONDA_V3_FSK_NAME,
        inst->frame.type_b ? "TB" : "TA",
        inst->generic.data_count_bit,
        hv3_btn_name(inst->frame.button),
        inst->frame.button,
        (unsigned long)inst->frame.serial,
        (unsigned long)inst->frame.counter,
        inst->frame.mode);
}

/* ── FSK Encoder ── */
static void fsk_build_upload(SubGhzProtocolEncoderHondaV3FSK* inst) {
    LevelDuration* buf = inst->encoder.upload;
    size_t idx = 0;

    buf[idx++] = level_duration_make(false, HV3_FSK_GUARD_TIME_US);

    for(uint16_t p = 0;
        p < (uint16_t)(HV3_FSK_PREAMBLE_CYCLES * 2u) &&
        idx < HV3_FSK_ENC_BUF_SIZE - 10u;
        p++) {
        buf[idx++] = level_duration_make((p & 1u) == 0u, HV3_FSK_TE_SHORT);
    }

    /* Frame: 14 bytes, MSB-first, Manchester */
    uint8_t frame_buf[HV3_FSK_FRAME_BYTES];
    inst->frame.button = inst->active_button;
    fsk_to_buf(&inst->frame, frame_buf);

    for(uint8_t b = 0;
        b < HV3_FSK_FRAME_BITS && idx < HV3_FSK_ENC_BUF_SIZE - 4u;
        b++) {
        uint8_t byte_idx = b / 8u;
        uint8_t bit_idx  = 7u - (b % 8u);  /* MSB-first */
        uint8_t bit      = (frame_buf[byte_idx] >> bit_idx) & 1u;

        if(bit) {
            /* bit 1: LOW→HIGH */
            buf[idx++] = level_duration_make(false, HV3_FSK_TE_SHORT);
            buf[idx++] = level_duration_make(true,  HV3_FSK_TE_SHORT);
        } else {
            /* bit 0: HIGH→LOW */
            buf[idx++] = level_duration_make(true,  HV3_FSK_TE_SHORT);
            buf[idx++] = level_duration_make(false, HV3_FSK_TE_SHORT);
        }
    }

    buf[idx++] = level_duration_make(false, HV3_FSK_GUARD_TIME_US);

    inst->encoder.size_upload = idx;
    inst->encoder.front       = 0;
    FURI_LOG_D(TAG, "[FSK] upload %u symbols", (unsigned)idx);
}

void* subghz_protocol_encoder_honda_v3_fsk_alloc(SubGhzEnvironment* env) {
    UNUSED(env);
    SubGhzProtocolEncoderHondaV3FSK* inst =
        malloc(sizeof(SubGhzProtocolEncoderHondaV3FSK));
    furi_check(inst);
    memset(inst, 0, sizeof(SubGhzProtocolEncoderHondaV3FSK));
    inst->base.protocol         = &subghz_protocol_honda_v3_fsk;
    inst->generic.protocol_name = inst->base.protocol->name;
    inst->encoder.repeat        = 3;
    inst->encoder.upload        =
        malloc(HV3_FSK_ENC_BUF_SIZE * sizeof(LevelDuration));
    furi_check(inst->encoder.upload);
    inst->encoder.is_running    = false;
    return inst;
}

void subghz_protocol_encoder_honda_v3_fsk_free(void* ctx) {
    furi_assert(ctx);
    SubGhzProtocolEncoderHondaV3FSK* inst = ctx;
    free(inst->encoder.upload);
    free(inst);
}

void subghz_protocol_encoder_honda_v3_fsk_stop(void* ctx) {
    furi_assert(ctx);
    ((SubGhzProtocolEncoderHondaV3FSK*)ctx)->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_honda_v3_fsk_yield(void* ctx) {
    furi_assert(ctx);
    SubGhzProtocolEncoderHondaV3FSK* inst = ctx;
    if(!inst->encoder.is_running || inst->encoder.repeat == 0)
        return level_duration_reset();
    LevelDuration ret = inst->encoder.upload[inst->encoder.front];
    if(++inst->encoder.front >= inst->encoder.size_upload) {
        inst->encoder.repeat--;
        inst->encoder.front = 0;
    }
    return ret;
}

SubGhzProtocolStatus subghz_protocol_encoder_honda_v3_fsk_deserialize(
    void* ctx, FlipperFormat* ff)
{
    furi_assert(ctx);
    SubGhzProtocolEncoderHondaV3FSK* inst = ctx;
    SubGhzProtocolStatus ret =
        subghz_block_generic_deserialize(&inst->generic, ff);
    if(ret != SubGhzProtocolStatusOk) return ret;

    uint32_t v = 0;
    if(!flipper_format_read_uint32(ff, "Serial", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->frame.serial = v;

    if(!flipper_format_read_uint32(ff, "Counter", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->frame.counter = v;

    if(!flipper_format_read_uint32(ff, "Button", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->frame.button = (uint8_t)v;

    if(!flipper_format_read_uint32(ff, "TypeB", &v, 1))
        inst->frame.type_b = false;
    else
        inst->frame.type_b = (v != 0u);

    inst->frame.mode = 0x2u;

    hv3_register_btn(inst->frame.button);
    inst->active_button = hv3_get_active_btn();

    inst->frame.counter = (inst->frame.counter +
        (uint32_t)furi_hal_subghz_get_rolling_counter_mult()) & 0x00FFFFFFu;
    fsk_counter_increment(&inst->frame);
    inst->frame.button = inst->active_button;

    uint8_t tmp[HV3_FSK_FRAME_BYTES];
    fsk_to_buf(&inst->frame, tmp);
    inst->frame.checksum = fsk_rolling_checksum(tmp);

    inst->generic.cnt  = inst->frame.counter;
    inst->generic.btn  = inst->active_button;
    inst->generic.data = hv3_pack(
        inst->frame.serial, inst->active_button,
        (uint16_t)(inst->frame.counter & 0xFFFFu),
        inst->frame.mode, 0xFu, HV3_PROTO_MODE_FSK,
        (uint8_t)(inst->frame.type_b ? 1u : 0u));

    flipper_format_rewind(ff);
    uint8_t kd[8];
    for(int i = 0; i < 8; i++)
        kd[i] = (uint8_t)(inst->generic.data >> (56 - i * 8));
    flipper_format_update_hex(ff, "Key", kd, 8);

    fsk_build_upload(inst);
    inst->encoder.is_running = true;

    FURI_LOG_I(TAG, "[FSK] encoder ready ser=%07lX btn=%u cnt=%06lX",
               (unsigned long)inst->frame.serial,
               inst->active_button,
               (unsigned long)inst->frame.counter);
    return SubGhzProtocolStatusOk;
}

/* ═══════════════════════════════════════════════════════════════════
 * PROTOCOL VTABLES
 * ═══════════════════════════════════════════════════════════════════ */

/* ── OOK ── */
const SubGhzProtocolDecoder subghz_protocol_honda_v3_ook_decoder = {
    .alloc         = subghz_protocol_decoder_honda_v3_ook_alloc,
    .free          = subghz_protocol_decoder_honda_v3_ook_free,
    .feed          = subghz_protocol_decoder_honda_v3_ook_feed,
    .reset         = subghz_protocol_decoder_honda_v3_ook_reset,
    .get_hash_data = subghz_protocol_decoder_honda_v3_ook_get_hash_data,
    .serialize     = subghz_protocol_decoder_honda_v3_ook_serialize,
    .deserialize   = subghz_protocol_decoder_honda_v3_ook_deserialize,
    .get_string    = subghz_protocol_decoder_honda_v3_ook_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_honda_v3_ook_encoder = {
    .alloc       = subghz_protocol_encoder_honda_v3_ook_alloc,
    .free        = subghz_protocol_encoder_honda_v3_ook_free,
    .deserialize = subghz_protocol_encoder_honda_v3_ook_deserialize,
    .stop        = subghz_protocol_encoder_honda_v3_ook_stop,
    .yield       = subghz_protocol_encoder_honda_v3_ook_yield,
};

const SubGhzProtocol subghz_protocol_honda_v3_ook = {
    .name    = SUBGHZ_PROTOCOL_HONDA_V3_OOK_NAME,
    .type    = SubGhzProtocolTypeDynamic,
    .flag    = SubGhzProtocolFlag_433 |
               SubGhzProtocolFlag_315 |
               SubGhzProtocolFlag_AM  |
               SubGhzProtocolFlag_Decodable |
               SubGhzProtocolFlag_Load |
               SubGhzProtocolFlag_Save |
               SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_honda_v3_ook_decoder,
    .encoder = &subghz_protocol_honda_v3_ook_encoder,
};

/* ── KL ── */
const SubGhzProtocolDecoder subghz_protocol_honda_v3_kl_decoder = {
    .alloc         = subghz_protocol_decoder_honda_v3_kl_alloc,
    .free          = subghz_protocol_decoder_honda_v3_kl_free,
    .feed          = subghz_protocol_decoder_honda_v3_kl_feed,
    .reset         = subghz_protocol_decoder_honda_v3_kl_reset,
    .get_hash_data = subghz_protocol_decoder_honda_v3_kl_get_hash_data,
    .serialize     = subghz_protocol_decoder_honda_v3_kl_serialize,
    .deserialize   = subghz_protocol_decoder_honda_v3_kl_deserialize,
    .get_string    = subghz_protocol_decoder_honda_v3_kl_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_honda_v3_kl_encoder = {
    .alloc       = subghz_protocol_encoder_honda_v3_kl_alloc,
    .free        = subghz_protocol_encoder_honda_v3_kl_free,
    .deserialize = subghz_protocol_encoder_honda_v3_kl_deserialize,
    .stop        = subghz_protocol_encoder_honda_v3_kl_stop,
    .yield       = subghz_protocol_encoder_honda_v3_kl_yield,
};

const SubGhzProtocol subghz_protocol_honda_v3_kl = {
    .name    = SUBGHZ_PROTOCOL_HONDA_V3_KL_NAME,
    .type    = SubGhzProtocolTypeDynamic,
    .flag    = SubGhzProtocolFlag_433 |
               SubGhzProtocolFlag_315 |
               SubGhzProtocolFlag_AM  |
               SubGhzProtocolFlag_Decodable |
               SubGhzProtocolFlag_Load |
               SubGhzProtocolFlag_Save |
               SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_honda_v3_kl_decoder,
    .encoder = &subghz_protocol_honda_v3_kl_encoder,
};

/* ── FSK ── */
const SubGhzProtocolDecoder subghz_protocol_honda_v3_fsk_decoder = {
    .alloc         = subghz_protocol_decoder_honda_v3_fsk_alloc,
    .free          = subghz_protocol_decoder_honda_v3_fsk_free,
    .feed          = subghz_protocol_decoder_honda_v3_fsk_feed,
    .reset         = subghz_protocol_decoder_honda_v3_fsk_reset,
    .get_hash_data = subghz_protocol_decoder_honda_v3_fsk_get_hash_data,
    .serialize     = subghz_protocol_decoder_honda_v3_fsk_serialize,
    .deserialize   = subghz_protocol_decoder_honda_v3_fsk_deserialize,
    .get_string    = subghz_protocol_decoder_honda_v3_fsk_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_honda_v3_fsk_encoder = {
    .alloc       = subghz_protocol_encoder_honda_v3_fsk_alloc,
    .free        = subghz_protocol_encoder_honda_v3_fsk_free,
    .deserialize = subghz_protocol_encoder_honda_v3_fsk_deserialize,
    .stop        = subghz_protocol_encoder_honda_v3_fsk_stop,
    .yield       = subghz_protocol_encoder_honda_v3_fsk_yield,
};

const SubGhzProtocol subghz_protocol_honda_v3_fsk = {
    .name    = SUBGHZ_PROTOCOL_HONDA_V3_FSK_NAME,
    .type    = SubGhzProtocolTypeDynamic,
    .flag    = SubGhzProtocolFlag_433 |
               SubGhzProtocolFlag_AM  |
               SubGhzProtocolFlag_Decodable |
               SubGhzProtocolFlag_Load |
               SubGhzProtocolFlag_Save |
               SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_honda_v3_fsk_decoder,
    .encoder = &subghz_protocol_honda_v3_fsk_encoder,
};
