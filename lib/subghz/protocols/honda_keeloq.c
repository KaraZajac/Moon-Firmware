/*
 * honda_keeloq.c
 * Honda KeeLoq SubGHz Protocol — Flipper Zero
 * RE: Pandora Code Grabber firmware (ARM Cortex-M / EFM32)
 *
 * Verificado contra capturas:
 *   Lock_honda.sub    key[14]=0x50ED04DEEFBF713C ser=0x1000000  rc=62555
 *   Unlock_honda.sub  key[14]=0x50ED04DEEFBF713C ser=0x4000002  rc=64780
 *   Hondaaa.sub       key[1] =0xABBA94D410C1F940 ser=0x46442C1  rc=27723
 */

#include "honda_keeloq.h"

#include <furi.h>
#include <furi_hal.h>
#include <lib/subghz/blocks/const.h>
#include <lib/subghz/blocks/decoder.h>
#include <lib/subghz/blocks/encoder.h>
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/blocks/math.h>
#include <lib/subghz/blocks/custom_btn_i.h>

#define TAG "HondaKeeLoq"

/* ═══════════════════════════════════════════════════════════════════
 * TABLA DE CLAVES DE FABRICANTE
 * Fuente: firmware Pandora @ 0x14D60
 * ═══════════════════════════════════════════════════════════════════ */
const HondaKLMfrKey honda_kl_mfr_keys[HONDA_MFR_KEY_COUNT] = {
    {0x8DAA5CDB, 0xA8F5DFFC},  /*  0: firmware principal @ 0x9EB4     */
    {0x10C1F940, 0xABBA94D4},  /*  1: ROM[0]  Hondaaa.sub verified    */
    {0x460114B7, 0xDCF40FCD},  /*  2: ROM[1]                          */
    {0xE9B977B4, 0x56EB02D8},  /*  3: ROM[2]                          */
    {0x3F26FA21, 0xA8507BA5},  /*  4: ROM[3]                          */
    {0xA4C5C542, 0x6857C1C5},  /*  5: ROM[4]                          */
    {0xEBBB753E, 0x54E900DA},  /*  6: ROM[5]                          */
    {0x6A67C523, 0x5683FC9B},  /*  7: ROM[6]                          */
    {0xECBC7263, 0x53EE07DD},  /*  8: ROM[7]                          */
    {0xE1CA6024, 0x5D973077},  /*  9: ROM[8]                          */
    {0xEDBD737C, 0x52EF06DC},  /* 10: ROM[9]                          */
    {0x6011AC25, 0x36714027},  /* 11: ROM[10]                         */
    {0xEEBE7059, 0x51EC05DF},  /* 12: ROM[11]                         */
    {0x9BB87A26, 0x1F4DAA96},  /* 13: ROM[12]                         */
    {0xEFBF713C, 0x50ED04DE},  /* 14: ROM[13] Lock/Unlock verified    */
    {0xF39FD627, 0xDC8DC8EA},  /* 15: ROM[14]                         */
    {0x610A14EB, 0x30850A82},  /* 16: ROM[15]                         */
    {0x1719FF41, 0xD5204919},  /* 17: ROM[16]                         */
    {0x779F14DA, 0x558EA3B0},  /* 18: ROM[17]                         */
    {0x47454DE2, 0x54454441},  /* 19: ROM[18]                         */
    {0x0514F348, 0x40802014},  /* 20: ROM[19]                         */
};

/* ═══════════════════════════════════════════════════════════════════
 * KEELOQ ENGINE
 *
 * DECRYPT — firmware @ 0x9EB8
 * Key rotation: bit(key, (15 - round) & 0x3F)
 * NLF taps: bits 1,9,20,26,31 del estado
 *
 * ENCRYPT — inverso para TX
 * Key rotation: bit(key, round & 0x3F)
 * ═══════════════════════════════════════════════════════════════════ */
static uint32_t honda_kl_decrypt(uint32_t ciphertext, uint64_t key) {
    uint32_t x = ciphertext;
    for(int i = 0; i < HONDA_KL_KEELOQ_ROUNDS; i++) {
        uint32_t kb = (uint32_t)((key >> ((15 - i) & 0x3F)) & 1u);

        uint32_t nlf_idx =
            (((x >>  1) & 1u)        ) |
            (((x >>  9) & 1u) << 1u  ) |
            (((x >> 20) & 1u) << 2u  ) |
            (((x >> 26) & 1u) << 3u  ) |
            (((x >> 31) & 1u) << 4u  );

        uint32_t nlf = (HONDA_KL_KEELOQ_NLF >> nlf_idx) & 1u;
        uint32_t msb = x & 1u;
        uint32_t b16 = (x >> 15) & 1u;
        uint32_t fb  = msb ^ nlf ^ b16 ^ kb;

        x = (x >> 1) | (fb << 31);
    }
    return x;
}

static uint32_t honda_kl_encrypt(uint32_t plaintext, uint64_t key) {
    uint32_t x = plaintext;
    for(int i = 0; i < HONDA_KL_KEELOQ_ROUNDS; i++) {
        uint32_t kb = (uint32_t)((key >> (i & 0x3F)) & 1u);

        uint32_t nlf_idx =
            (((x >>  1) & 1u)        ) |
            (((x >>  9) & 1u) << 1u  ) |
            (((x >> 20) & 1u) << 2u  ) |
            (((x >> 26) & 1u) << 3u  ) |
            (((x >> 31) & 1u) << 4u  );

        uint32_t nlf  = (HONDA_KL_KEELOQ_NLF >> nlf_idx) & 1u;
        uint32_t bit0 = x & 1u;
        uint32_t b16  = (x >> 16) & 1u;
        uint32_t fb   = nlf ^ b16 ^ bit0 ^ kb;

        x = ((x << 1) & 0xFFFFFFFFu) | fb;
    }
    return x;
}

/* ═══════════════════════════════════════════════════════════════════
 * BÚSQUEDA DE CLAVE DE FABRICANTE
 *
 * Validación primaria:  dec[3:0] == button_field
 * Validación secundaria: rc!=0 || disc!=0  (evitar falsos positivos)
 * ═══════════════════════════════════════════════════════════════════ */
static int8_t honda_kl_find_key(
    uint32_t  enc,
    uint8_t   btn,
    uint32_t* out_dec,
    uint16_t* out_rc)
{
    for(int i = 0; i < HONDA_MFR_KEY_COUNT; i++) {
        uint64_t k = ((uint64_t)honda_kl_mfr_keys[i].high << 32) |
                      (uint64_t)honda_kl_mfr_keys[i].low;
        uint32_t dec = honda_kl_decrypt(enc, k);

        if((dec & HONDA_KL_DEC_FUNC_MASK) != (uint32_t)btn) continue;

        uint16_t rc   = (uint16_t)((dec & HONDA_KL_DEC_RC_MASK)
                                       >> HONDA_KL_DEC_RC_SHIFT);
        uint8_t  disc = (uint8_t) ((dec & HONDA_KL_DEC_DISC_MASK)
                                       >> HONDA_KL_DEC_DISC_SHIFT);
        if(rc == 0 && disc == 0) continue;

        *out_dec = dec;
        *out_rc  = rc;
        return (int8_t)i;
    }
    return -1;
}

/* ═══════════════════════════════════════════════════════════════════
 * BUILD FRAME PARA TX — 66 bits LSB-first
 *
 * Plaintext:
 *   [3:0]   = function = btn
 *   [11:4]  = discriminant = (serial >> 20) & 0xFF
 *   [31:16] = rolling code
 * ═══════════════════════════════════════════════════════════════════ */
static void honda_kl_build_frame(
    bool     bits[HONDA_KL_FRAME_BITS],
    uint64_t key,
    uint32_t serial,
    uint8_t  btn,
    uint8_t  status,
    uint16_t rc)
{
    uint8_t  discr = (uint8_t)((serial >> 20) & 0xFFu);
    uint32_t plain = ((uint32_t)(btn & 0xFu))      |
                     ((uint32_t)discr        << 4)  |
                     ((uint32_t)rc           << 16);
    uint32_t enc   = honda_kl_encrypt(plain, key);

    for(int i = 0; i < 32; i++) bits[i]    = (bool)((enc    >> i) & 1u);
    for(int i = 0; i < 28; i++) bits[32+i] = (bool)((serial >> i) & 1u);
    for(int i = 0; i < 4;  i++) bits[60+i] = (bool)((btn    >> i) & 1u);
    for(int i = 0; i < 2;  i++) bits[64+i] = (bool)((status >> i) & 1u);
}

/* ═══════════════════════════════════════════════════════════════════
 * PACK / UNPACK — generic.data (64 bits)
 *
 * [63:36] serial      (28b)
 * [35:32] button      (4b)
 * [31:16] rolling     (16b)
 * [15:8]  discriminant(8b)
 * [7:4]   key_idx     (4b, max 15)
 * [3:2]   status      (2b)
 * [1:0]   reservado
 * ═══════════════════════════════════════════════════════════════════ */
static uint64_t honda_kl_pack(
    uint32_t serial,
    uint8_t  btn,
    uint16_t rc,
    uint8_t  discr,
    int8_t   key_idx,
    uint8_t  status)
{
    /* Clamp invalid key_idx to 0 — callers should validate beforehand */
    uint8_t ki = (key_idx >= 0 && key_idx < HONDA_MFR_KEY_COUNT) ?
        (uint8_t)key_idx : 0u;

    return ((uint64_t)(serial & 0x0FFFFFFFu) << 36) |
           ((uint64_t)(btn    & 0x0Fu)       << 32) |
           ((uint64_t)rc                     << 16) |
           ((uint64_t)discr                  <<  8) |
           ((uint64_t)(ki     & 0x0Fu)       <<  4) |
           ((uint64_t)(status & 0x03u)       <<  2);
}

static void honda_kl_unpack(
    uint64_t  raw,
    uint32_t* serial,
    uint8_t*  btn,
    uint16_t* rc,
    uint8_t*  discr,
    int8_t*   key_idx,
    uint8_t*  status)
{
    *serial  = (uint32_t)((raw >> 36) & 0x0FFFFFFFu);
    *btn     = (uint8_t) ((raw >> 32) & 0x0Fu);
    *rc      = (uint16_t)((raw >> 16) & 0xFFFFu);
    *discr   = (uint8_t) ((raw >>  8) & 0xFFu);
    *key_idx = (int8_t)  ((raw >>  4) & 0x0Fu);
    *status  = (uint8_t) ((raw >>  2) & 0x03u);
}

/* ═══════════════════════════════════════════════════════════════════
 * FORWARD DECLARATIONS — necesarias porque las vtables se definen
 * después de las funciones que referencian
 * ═══════════════════════════════════════════════════════════════════ */
const SubGhzProtocolDecoder subghz_protocol_honda_keeloq_decoder;
const SubGhzProtocolEncoder subghz_protocol_honda_keeloq_encoder;
const SubGhzProtocol        subghz_protocol_honda_keeloq;

/* ═══════════════════════════════════════════════════════════════════
 * PROCESO DE FRAME RECIBIDO
 * ═══════════════════════════════════════════════════════════════════ */
static void honda_kl_process_frame(SubGhzProtocolDecoderHondaKeeloq* inst) {
    /*
     * decode_data acumulado MSB-first en el shift register.
     * Necesitamos LSB-first → revertir bit order.
     */
    uint64_t raw = 0;
    uint64_t tmp = inst->decoder.decode_data;
    for(int i = 0; i < 64; i++) {
        raw = (raw << 1) | (tmp & 1u);
        tmp >>= 1;
    }

    uint32_t enc    = (uint32_t)(raw & 0xFFFFFFFFu);
    uint32_t serial = (uint32_t)((raw >> 32) & 0x0FFFFFFFu);
    uint8_t  btn    = (uint8_t) ((raw >> 60) & 0x0Fu);
    uint8_t  status = 0u;

    uint32_t dec = 0;
    uint16_t rc  = 0;
    int8_t   ki  = honda_kl_find_key(enc, btn, &dec, &rc);

    if(ki < 0) {
        FURI_LOG_D(TAG, "no key match enc=%08lX btn=%u",
                   (unsigned long)enc, btn);
        return;
    }

    /* Validar ventana de rolling code */
    if(inst->has_last_rolling) {
        uint16_t diff = (uint16_t)(rc - inst->last_rolling);
        if(diff == 0 || diff > HONDA_KL_ROLLING_WINDOW) {
            FURI_LOG_D(TAG, "RC out window rc=%u last=%u diff=%u",
                       rc, inst->last_rolling, diff);
            return;
        }
    }

    uint8_t discr = (uint8_t)(
        (dec & HONDA_KL_DEC_DISC_MASK) >> HONDA_KL_DEC_DISC_SHIFT);

    inst->encrypted        = enc;
    inst->serial           = serial;
    inst->btn              = btn;
    inst->status           = status;
    inst->decrypted        = dec;
    inst->rolling_code     = rc;
    inst->discriminant     = discr;
    inst->key_idx          = ki;
    inst->last_rolling     = rc;
    inst->has_last_rolling = true;

    inst->generic.data           = honda_kl_pack(serial, btn, rc,
                                                  discr, ki, status);
    inst->generic.data_count_bit = HONDA_KL_FRAME_BITS;
    inst->generic.serial         = serial;
    inst->generic.btn            = btn;
    inst->generic.cnt            = rc;

    FURI_LOG_I(TAG,
        "VALID key[%d] ser=%07lX btn=%u rc=%04X enc=%08lX dec=%08lX",
        ki,
        (unsigned long)serial,
        btn, rc,
        (unsigned long)enc,
        (unsigned long)dec);

    /* Notificar al framework — callback en base, NO en generic */
    if(inst->base.callback)
        inst->base.callback(&inst->base, inst->base.context);
}

/* ═══════════════════════════════════════════════════════════════════
 * DECODER — alloc / free / reset
 * ═══════════════════════════════════════════════════════════════════ */
void* subghz_protocol_decoder_honda_keeloq_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderHondaKeeloq* inst =
        malloc(sizeof(SubGhzProtocolDecoderHondaKeeloq));
    furi_check(inst);
    memset(inst, 0, sizeof(SubGhzProtocolDecoderHondaKeeloq));

    inst->base.protocol         = &subghz_protocol_honda_keeloq;
    inst->generic.protocol_name = inst->base.protocol->name;
    inst->key_idx               = -1;
    inst->has_last_rolling      = false;

    FURI_LOG_I(TAG, "decoder allocated");
    return inst;
}

void subghz_protocol_decoder_honda_keeloq_free(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderHondaKeeloq* inst = context;
    free(inst);
}

void subghz_protocol_decoder_honda_keeloq_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderHondaKeeloq* inst = context;
    inst->decoder.parser_step      = 0;
    inst->decoder.decode_data      = 0;
    inst->decoder.decode_count_bit = 0;
    /* Preservar serial/btn/rc para get_string */
}

/* ═══════════════════════════════════════════════════════════════════
 * DECODER — feed
 *
 * State machine de 2 estados:
 *   0 = RESET  — esperar sync gap (LOW >= HONDA_KL_SYNC_MIN_US)
 *   1 = DATA   — decodificar bits
 *
 * Bit decode (PWM invertido, base 400µs):
 *   LOW en [250µs .. 550µs] → bit 0  (período completo)
 *   LOW < 250µs o inválido  → bit 1  (PWM invertido: LOW cortado)
 *   LOW >= 2500µs           → sync gap → procesar frame si completo
 * ═══════════════════════════════════════════════════════════════════ */
void subghz_protocol_decoder_honda_keeloq_feed(
    void* context, bool level, uint32_t duration)
{
    furi_assert(context);
    SubGhzProtocolDecoderHondaKeeloq* inst = context;

    switch(inst->decoder.parser_step) {

    /* ── Estado 0: RESET — esperar sync ── */
    case 0:
        if(!level && duration >= HONDA_KL_SYNC_MIN_US) {
            inst->decoder.decode_data      = 0;
            inst->decoder.decode_count_bit = 0;
            inst->decoder.parser_step      = 1;
            FURI_LOG_D(TAG, "sync dur=%lu", (unsigned long)duration);
        }
        break;

    /* ── Estado 1: DATA — acumular bits ── */
    case 1:
        if(!level) {
            /* Sync gap durante data → procesar frame si completo */
            if(duration >= HONDA_KL_SYNC_MIN_US) {
                if(inst->decoder.decode_count_bit == HONDA_KL_FRAME_BITS) {
                    honda_kl_process_frame(inst);
                }
                /* Reset para siguiente frame — puede venir inmediatamente */
                inst->decoder.decode_data      = 0;
                inst->decoder.decode_count_bit = 0;
                /* Permanecer en estado 1 — el sync ya es el inicio */
                break;
            }

            /* Clasificar LOW duration → bit value */
            uint8_t bit_val;
            if(duration + HONDA_KL_TE_DELTA >= HONDA_KL_TE_BASE &&
               duration <= HONDA_KL_TE_BASE + HONDA_KL_TE_DELTA) {
                /* LOW ~400µs → bit 0 (período completo) */
                bit_val = 0;
            } else {
                /* LOW corto o fuera de rango → bit 1 (PWM invertido) */
                bit_val = 1;
            }

            /* Acumular en shift register MSB-first (máximo 64 bits útiles) */
            if(inst->decoder.decode_count_bit < 64) {
                inst->decoder.decode_data >>= 1;
                if(bit_val) {
                    inst->decoder.decode_data |= (1ULL << 63);
                }
            }
            inst->decoder.decode_count_bit++;

            /* Frame completo (66 bits — bits 64-65 son status=0) */
            if(inst->decoder.decode_count_bit >= HONDA_KL_FRAME_BITS) {
                honda_kl_process_frame(inst);
                inst->decoder.decode_data      = 0;
                inst->decoder.decode_count_bit = 0;
                inst->decoder.parser_step      = 0;
            }
        }
        /* HIGH: no acción — esperamos el LOW siguiente para medir */
        break;

    default:
        inst->decoder.parser_step = 0;
        break;
    }

    inst->decoder.te_last = duration;
}

/* ═══════════════════════════════════════════════════════════════════
 * DECODER — hash
 * ═══════════════════════════════════════════════════════════════════ */
uint8_t subghz_protocol_decoder_honda_keeloq_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderHondaKeeloq* inst = context;
    return (uint8_t)(
         inst->serial          ^
        (inst->serial >>  8)   ^
        (inst->serial >> 16)   ^
         inst->btn             ^
        (uint8_t)inst->key_idx);
}

/* ═══════════════════════════════════════════════════════════════════
 * DECODER — serialize
 * ═══════════════════════════════════════════════════════════════════ */
SubGhzProtocolStatus subghz_protocol_decoder_honda_keeloq_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset)
{
    furi_assert(context);
    SubGhzProtocolDecoderHondaKeeloq* inst = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&inst->generic, flipper_format, preset);
    if(ret != SubGhzProtocolStatusOk) return ret;

    uint32_t v = inst->serial;
    if(!flipper_format_write_uint32(flipper_format, "Serial", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;

    v = inst->rolling_code;
    if(!flipper_format_write_uint32(flipper_format, "RollingCode", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;

    v = inst->btn;
    if(!flipper_format_write_uint32(flipper_format, "Button", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;

    /* FIX: cast explícito para evitar mezcla int8_t / unsigned */
    v = (inst->key_idx >= 0) ? (uint32_t)(uint8_t)inst->key_idx : 0u;
    if(!flipper_format_write_uint32(flipper_format, "KeyIdx", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;

    return SubGhzProtocolStatusOk;
}

/* ═══════════════════════════════════════════════════════════════════
 * DECODER — deserialize
 * ═══════════════════════════════════════════════════════════════════ */
SubGhzProtocolStatus subghz_protocol_decoder_honda_keeloq_deserialize(
    void* context,
    FlipperFormat* flipper_format)
{
    furi_assert(context);
    SubGhzProtocolDecoderHondaKeeloq* inst = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_deserialize_check_count_bit(
            &inst->generic, flipper_format, HONDA_KL_FRAME_BITS);
    if(ret != SubGhzProtocolStatusOk) return ret;

    uint32_t v = 0;

    if(!flipper_format_read_uint32(flipper_format, "Serial", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->serial = v;

    if(!flipper_format_read_uint32(flipper_format, "RollingCode", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->rolling_code = (uint16_t)v;

    if(!flipper_format_read_uint32(flipper_format, "Button", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->btn = (uint8_t)v;

    if(!flipper_format_read_uint32(flipper_format, "KeyIdx", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->key_idx = (int8_t)v;

    inst->discriminant     = (uint8_t)((inst->serial >> 20) & 0xFFu);
    inst->has_last_rolling = true;
    inst->last_rolling     = inst->rolling_code;
    inst->generic.serial   = inst->serial;
    inst->generic.btn      = inst->btn;
    inst->generic.cnt      = inst->rolling_code;

    return SubGhzProtocolStatusOk;
}

/* ═══════════════════════════════════════════════════════════════════
 * DECODER — get_string
 * ═══════════════════════════════════════════════════════════════════ */
void subghz_protocol_decoder_honda_keeloq_get_string(
    void* context, FuriString* output)
{
    furi_assert(context);
    SubGhzProtocolDecoderHondaKeeloq* inst = context;

    /* Reconstruir desde generic.data si los campos están vacíos */
    if(inst->serial == 0 && inst->generic.data != 0) {
        uint8_t st = 0;
        honda_kl_unpack(
            inst->generic.data,
            &inst->serial,
            &inst->btn,
            &inst->rolling_code,
            &inst->discriminant,
            &inst->key_idx,
            &st);
        inst->status = st;
    }

    const char* btn_name;
    switch(inst->btn) {
    case HONDA_KL_BTN_PANIC:        btn_name = "Panic";       break;
    case HONDA_KL_BTN_LOCK:         btn_name = "Lock";        break;
    case HONDA_KL_BTN_TRUNK:        btn_name = "Trunk/Hatch"; break;
    case HONDA_KL_BTN_REMOTE_START: btn_name = "RemoteStart"; break;
    case HONDA_KL_BTN_UNLOCK:       btn_name = "Unlock";      break;
    case HONDA_KL_BTN_UNLOCK2:      btn_name = "Unlock x2";   break;
    default:                        btn_name = "Unknown";     break;
    }

    furi_string_cat_printf(
        output,
        "%s %ubit\r\n"
        "Btn: %s (0x%X)\r\n"
        "Ser: %07lX\r\n"
        "RC:  %04X  Key[%d]\r\n"
        "Dis: %02X  Sts:%u\r\n",
        SUBGHZ_PROTOCOL_HONDA_KEELOQ_NAME,
        inst->generic.data_count_bit,
        btn_name,
        inst->btn,
        (unsigned long)inst->serial,
        inst->rolling_code,
        inst->key_idx,
        inst->discriminant,
        inst->status);
}

/* ═══════════════════════════════════════════════════════════════════
 * ENCODER — build upload buffer
 *
 * Por cada repetición:
 *   23 × (HIGH 400µs + LOW 400µs)    preámbulo
 *   LOW 4000µs                        sync gap
 *   66 × bit:
 *     bit 0 → HIGH 400µs + LOW 400µs
 *     bit 1 → HIGH 400µs + LOW 50µs  (PWM invertido)
 *   LOW 15600µs                       trail
 * ═══════════════════════════════════════════════════════════════════ */
static void honda_kl_build_upload(SubGhzProtocolEncoderHondaKeeloq* inst) {
    LevelDuration* buf = inst->encoder.upload;
    size_t idx = 0;

    for(uint8_t rep = 0;
        rep < HONDA_KL_REPEAT && idx < HONDA_KL_ENC_BUF_SIZE - 200u;
        rep++)
    {
        /* Preámbulo */
        for(uint8_t p = 0; p < HONDA_KL_PREAMBLE_COUNT; p++) {
            buf[idx++] = level_duration_make(true,  HONDA_KL_TE_BASE);
            buf[idx++] = level_duration_make(false, HONDA_KL_TE_BASE);
        }

        /* Sync gap */
        buf[idx++] = level_duration_make(false, HONDA_KL_SYNC_GAP_US);

        /* Data: 66 bits LSB-first, PWM invertido */
        for(uint8_t b = 0; b < HONDA_KL_FRAME_BITS; b++) {
            bool bit = inst->frame_bits[b];
            buf[idx++] = level_duration_make(true, HONDA_KL_TE_BASE);
            if(bit) {
                /* bit 1: LOW muy corto */
                buf[idx++] = level_duration_make(false, HONDA_KL_TE_BIT1_LOW);
            } else {
                /* bit 0: LOW completo */
                buf[idx++] = level_duration_make(false, HONDA_KL_TE_BASE);
            }
        }

        /* Trail */
        buf[idx++] = level_duration_make(false, HONDA_KL_TRAIL_US);
    }

    inst->encoder.size_upload = idx;
    inst->encoder.front       = 0;

    FURI_LOG_D(TAG, "upload built %u symbols", (unsigned)idx);
}

/* ═══════════════════════════════════════════════════════════════════
 * ENCODER — alloc
 * ═══════════════════════════════════════════════════════════════════ */
void* subghz_protocol_encoder_honda_keeloq_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderHondaKeeloq* inst =
        malloc(sizeof(SubGhzProtocolEncoderHondaKeeloq));
    furi_check(inst);
    memset(inst, 0, sizeof(SubGhzProtocolEncoderHondaKeeloq));

    inst->base.protocol         = &subghz_protocol_honda_keeloq;
    inst->generic.protocol_name = inst->base.protocol->name;
    inst->encoder.repeat        = (int32_t)HONDA_KL_REPEAT;
    inst->encoder.upload        =
        malloc(HONDA_KL_ENC_BUF_SIZE * sizeof(LevelDuration));
    furi_check(inst->encoder.upload);
    inst->encoder.is_running    = false;
    inst->encoder.front         = 0;
    inst->encoder.size_upload   = 0;

    return inst;
}

/* ═══════════════════════════════════════════════════════════════════
 * ENCODER — free / stop / yield
 * ═══════════════════════════════════════════════════════════════════ */
void subghz_protocol_encoder_honda_keeloq_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderHondaKeeloq* inst = context;
    free(inst->encoder.upload);
    free(inst);
}

void subghz_protocol_encoder_honda_keeloq_stop(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderHondaKeeloq* inst = context;
    inst->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_honda_keeloq_yield(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderHondaKeeloq* inst = context;

    if(!inst->encoder.is_running || inst->encoder.repeat == 0)
        return level_duration_reset();

    LevelDuration ret = inst->encoder.upload[inst->encoder.front];
    if(++inst->encoder.front >= inst->encoder.size_upload) {
        inst->encoder.repeat--;
        inst->encoder.front = 0;
    }
    return ret;
}

/* ═══════════════════════════════════════════════════════════════════
 * ENCODER — deserialize
 * ═══════════════════════════════════════════════════════════════════ */
SubGhzProtocolStatus subghz_protocol_encoder_honda_keeloq_deserialize(
    void* context,
    FlipperFormat* flipper_format)
{
    furi_assert(context);
    SubGhzProtocolEncoderHondaKeeloq* inst = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_deserialize(&inst->generic, flipper_format);
    if(ret != SubGhzProtocolStatusOk) return ret;

    uint32_t v = 0;

    if(!flipper_format_read_uint32(flipper_format, "Serial", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->serial = v;

    if(!flipper_format_read_uint32(flipper_format, "RollingCode", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->rolling_code = (uint16_t)v;

    if(!flipper_format_read_uint32(flipper_format, "Button", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->btn = (uint8_t)v;

    if(!flipper_format_read_uint32(flipper_format, "KeyIdx", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;

    int8_t ki = (int8_t)v;
    if(ki < 0 || ki >= HONDA_MFR_KEY_COUNT)
        return SubGhzProtocolStatusErrorParserOthers;

    inst->key = ((uint64_t)honda_kl_mfr_keys[ki].high << 32) |
                 (uint64_t)honda_kl_mfr_keys[ki].low;

    inst->active_button = inst->btn;

    /* Incrementar RC para TX — avanzar al siguiente código válido */
    inst->rolling_code = (uint16_t)(
        inst->rolling_code +
        (uint16_t)furi_hal_subghz_get_rolling_counter_mult());

    honda_kl_build_frame(
        inst->frame_bits,
        inst->key,
        inst->serial,
        inst->active_button,
        inst->status,
        inst->rolling_code);

    inst->generic.cnt = inst->rolling_code;
    inst->generic.btn = inst->active_button;

    honda_kl_build_upload(inst);
    inst->encoder.is_running = true;

    FURI_LOG_I(TAG,
        "encoder ready ser=%07lX btn=%u rc=%04X",
        (unsigned long)inst->serial,
        inst->active_button,
        inst->rolling_code);

    return SubGhzProtocolStatusOk;
}

/* ═══════════════════════════════════════════════════════════════════
 * PROTOCOL VTABLES
 * ═══════════════════════════════════════════════════════════════════ */
const SubGhzProtocolDecoder subghz_protocol_honda_keeloq_decoder = {
    .alloc         = subghz_protocol_decoder_honda_keeloq_alloc,
    .free          = subghz_protocol_decoder_honda_keeloq_free,
    .feed          = subghz_protocol_decoder_honda_keeloq_feed,
    .reset         = subghz_protocol_decoder_honda_keeloq_reset,
    .get_hash_data = subghz_protocol_decoder_honda_keeloq_get_hash_data,
    .serialize     = subghz_protocol_decoder_honda_keeloq_serialize,
    .deserialize   = subghz_protocol_decoder_honda_keeloq_deserialize,
    .get_string    = subghz_protocol_decoder_honda_keeloq_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_honda_keeloq_encoder = {
    .alloc       = subghz_protocol_encoder_honda_keeloq_alloc,
    .free        = subghz_protocol_encoder_honda_keeloq_free,
    .deserialize = subghz_protocol_encoder_honda_keeloq_deserialize,
    .stop        = subghz_protocol_encoder_honda_keeloq_stop,
    .yield       = subghz_protocol_encoder_honda_keeloq_yield,
};

const SubGhzProtocol subghz_protocol_honda_keeloq = {
    .name    = SUBGHZ_PROTOCOL_HONDA_KEELOQ_NAME,
    .type    = SubGhzProtocolTypeDynamic,
    .flag    = SubGhzProtocolFlag_433 |
               SubGhzProtocolFlag_315 |
               SubGhzProtocolFlag_AM  |
               SubGhzProtocolFlag_Decodable |
               SubGhzProtocolFlag_Load |
               SubGhzProtocolFlag_Save |
               SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_honda_keeloq_decoder,
    .encoder = &subghz_protocol_honda_keeloq_encoder,
};
