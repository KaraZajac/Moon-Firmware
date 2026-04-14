#pragma once

/*
 * honda_keeloq.h
 * Honda KeeLoq SubGHz Protocol — Flipper Zero
 * RE: Pandora Code Grabber firmware (ARM Cortex-M / EFM32)
 */

#include <lib/subghz/protocols/base.h>
#include <lib/subghz/blocks/const.h>
#include <lib/subghz/blocks/decoder.h>
#include <lib/subghz/blocks/encoder.h>
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/blocks/math.h>

/* ─────────────────────────────────────────────────────────────────
 * IDENTIDAD
 * ───────────────────────────────────────────────────────────────── */
#define SUBGHZ_PROTOCOL_HONDA_KEELOQ_NAME  "Honda_KeeLoq"

/* ─────────────────────────────────────────────────────────────────
 * KEELOQ
 * NLF: 0x3A5C742E  verified @ firmware 0x9EB8
 * Rounds: 528
 * ───────────────────────────────────────────────────────────────── */
#define HONDA_KL_KEELOQ_NLF        0x3A5C742EUL
#define HONDA_KL_KEELOQ_ROUNDS     528

/* ─────────────────────────────────────────────────────────────────
 * FRAME — 66 bits LSB-first en el aire
 *  [0..31]  = encrypted (KeeLoq ciphertext)
 *  [32..59] = serial (28 bits)
 *  [60..63] = button (4 bits)
 *  [64..65] = status (2 bits)
 *
 * Plaintext:
 *  [3:0]   = function  (debe == button para validar)
 *  [11:4]  = discriminant = (serial >> 20) & 0xFF
 *  [31:16] = rolling code (16 bits)
 * ───────────────────────────────────────────────────────────────── */
#define HONDA_KL_FRAME_BITS        66

#define HONDA_KL_DEC_FUNC_MASK     0x0000000FUL
#define HONDA_KL_DEC_DISC_MASK     0x00000FF0UL
#define HONDA_KL_DEC_DISC_SHIFT    4
#define HONDA_KL_DEC_RC_MASK       0xFFFF0000UL
#define HONDA_KL_DEC_RC_SHIFT      16

/* ─────────────────────────────────────────────────────────────────
 * BOTONES — prefijo KL_ para evitar colisión con honda.h
 *
 * Valores del campo function en el plaintext KeeLoq.
 * NOTA: difieren de los botones físicos en honda.h (Mode 1/3).
 *   Verificados contra capturas reales:
 *   Lock_honda.sub   → field=0x4 (REMOTE_START mapeado)
 *   Unlock_honda.sub → field=0x0 (PANIC mapeado)
 *   Hondaaa.sub      → field=0xA (desconocido)
 * ───────────────────────────────────────────────────────────────── */
#define HONDA_KL_BTN_PANIC         0x0u   /* 0b0000 */
#define HONDA_KL_BTN_LOCK          0x1u   /* 0b0001 */
#define HONDA_KL_BTN_TRUNK         0x2u   /* 0b0010 */
#define HONDA_KL_BTN_REMOTE_START  0x4u   /* 0b0100 */
#define HONDA_KL_BTN_UNLOCK        0x8u   /* 0b1000 */
#define HONDA_KL_BTN_UNLOCK2       0x9u   /* 0b1001 — doble press */
#define HONDA_KL_BTN_MAX_CUSTOM    5u

/* ─────────────────────────────────────────────────────────────────
 * TIMINGS — Modo 2 (KeeLoq 66-bit, PWM invertido)
 * firmware: Crypto_Scramble_Finalize_Keeloq @ 0x9C00
 *
 * Preamble: 23 ciclos × HIGH 400µs + LOW 400µs
 * Sync gap: LOW 4000µs
 * Bit 0:    HIGH 400µs + LOW 400µs
 * Bit 1:    HIGH 400µs + LOW ~0µs  (PWM invertido)
 * Trail:    LOW 15600µs
 * Reps:     5
 * ───────────────────────────────────────────────────────────────── */
#define HONDA_KL_TE_BASE           400u
#define HONDA_KL_TE_DELTA          150u
#define HONDA_KL_TE_BIT1_LOW       50u    /* LOW corto para bit 1 */
#define HONDA_KL_PREAMBLE_COUNT    23u
#define HONDA_KL_SYNC_GAP_US       4000u
#define HONDA_KL_TRAIL_US          15600u
#define HONDA_KL_REPEAT            5u
#define HONDA_KL_SYNC_MIN_US       2500u

/* ─────────────────────────────────────────────────────────────────
 * ROLLING CODE WINDOW
 * ───────────────────────────────────────────────────────────────── */
#define HONDA_KL_ROLLING_WINDOW    128u

/* ─────────────────────────────────────────────────────────────────
 * ENCODER BUFFER
 * Calculo:
 *   23×2 + 1 + 66×2 + 1 = 180 símbolos/rep × 5 reps = 900 + margen
 * ───────────────────────────────────────────────────────────────── */
#define HONDA_KL_ENC_BUF_SIZE      1200u

/* ─────────────────────────────────────────────────────────────────
 * CLAVES DE FABRICANTE
 * Fuente: firmware Pandora @ 0x14D60
 * Formato: {low32, high32} → key = (high32 << 32) | low32
 * ───────────────────────────────────────────────────────────────── */
typedef struct {
    uint32_t low;
    uint32_t high;
} HondaKLMfrKey;

#define HONDA_MFR_KEY_COUNT 21

extern const HondaKLMfrKey honda_kl_mfr_keys[HONDA_MFR_KEY_COUNT];

/* ─────────────────────────────────────────────────────────────────
 * DECODER STATE
 * IMPORTANTE: base DEBE ser el primer campo (casting del framework)
 * ───────────────────────────────────────────────────────────────── */
typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder        decoder;
    SubGhzBlockGeneric        generic;

    uint32_t encrypted;
    uint32_t serial;
    uint8_t  btn;
    uint8_t  status;

    uint32_t decrypted;
    uint16_t rolling_code;
    uint8_t  discriminant;

    int8_t   key_idx;         /* -1 = no encontrado */

    uint16_t last_rolling;
    bool     has_last_rolling;
} SubGhzProtocolDecoderHondaKeeloq;

/* ─────────────────────────────────────────────────────────────────
 * ENCODER STATE
 * IMPORTANTE: base DEBE ser el primer campo (casting del framework)
 * ───────────────────────────────────────────────────────────────── */
typedef struct {
    SubGhzProtocolEncoderBase   base;
    SubGhzProtocolBlockEncoder  encoder;
    SubGhzBlockGeneric          generic;

    uint64_t key;
    uint32_t serial;
    uint16_t rolling_code;
    uint8_t  btn;
    uint8_t  status;
    uint8_t  active_button;

    bool     frame_bits[HONDA_KL_FRAME_BITS];
    uint32_t sym_pos;
    uint32_t repeat_count;
} SubGhzProtocolEncoderHondaKeeloq;

/* ─────────────────────────────────────────────────────────────────
 * VTABLES
 * ───────────────────────────────────────────────────────────────── */
extern const SubGhzProtocolDecoder subghz_protocol_honda_keeloq_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_honda_keeloq_encoder;
extern const SubGhzProtocol        subghz_protocol_honda_keeloq;

/* ─────────────────────────────────────────────────────────────────
 * API PÚBLICA
 * ───────────────────────────────────────────────────────────────── */

/* Decoder */
void* subghz_protocol_decoder_honda_keeloq_alloc(SubGhzEnvironment* environment);
void  subghz_protocol_decoder_honda_keeloq_free(void* context);
void  subghz_protocol_decoder_honda_keeloq_reset(void* context);
void  subghz_protocol_decoder_honda_keeloq_feed(
    void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_honda_keeloq_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_honda_keeloq_serialize(
    void* context, FlipperFormat* flipper_format, SubGhzRadioPreset* preset);
SubGhzProtocolStatus subghz_protocol_decoder_honda_keeloq_deserialize(
    void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_honda_keeloq_get_string(
    void* context, FuriString* output);

/* Encoder */
void* subghz_protocol_encoder_honda_keeloq_alloc(SubGhzEnvironment* environment);
void  subghz_protocol_encoder_honda_keeloq_free(void* context);
SubGhzProtocolStatus subghz_protocol_encoder_honda_keeloq_deserialize(
    void* context, FlipperFormat* flipper_format);
void  subghz_protocol_encoder_honda_keeloq_stop(void* context);
LevelDuration subghz_protocol_encoder_honda_keeloq_yield(void* context);
