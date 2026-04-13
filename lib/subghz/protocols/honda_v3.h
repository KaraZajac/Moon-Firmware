#pragma once

/*
 * honda_v3.h — Honda KeeLoq Unified Protocol for Flipper Zero
 *
 * Unifies 3 modes extracted by RE from the Pandora firmware (ARM Cortex-M/EFM32):
 *
 * MODE 1 — Honda_V3_OOK (Brand_Auto_Honda_TX @ 0x13F24)
 * OOK PWM, 11 bytes, 88 bits, LSB-first, 315/433 MHz
 *
 * MODE 2 — Honda_V3_KL (Crypto_Scramble_Finalize_Keeloq @ 0x9C00)
 * OOK PWM inverted, 66-bit KeeLoq, 5 repeats, 433 MHz
 *
 * MODE 3 — Honda_V3_FSK (Protocol_TX_Honda_Extended @ 0xEFDC)
 * FSK Manchester, 14 bytes, 112 bits, MSB-first, 433 MHz
 * Captured as a compressed OOK (~8x) by CC1101
 */

#include <lib/subghz/protocols/base.h>
#include <lib/subghz/blocks/const.h>
#include <lib/subghz/blocks/decoder.h>
#include <lib/subghz/blocks/encoder.h>
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/blocks/math.h>

/* ═══════════════════════════════════════════════════════════════════
 * Protocol Name
 * ═══════════════════════════════════════════════════════════════════ */
#define SUBGHZ_PROTOCOL_HONDA_V3_OOK_NAME  "Honda_V3_OOK"
#define SUBGHZ_PROTOCOL_HONDA_V3_KL_NAME   "Honda_V3_KL"
#define SUBGHZ_PROTOCOL_HONDA_V3_FSK_NAME  "Honda_V3_FSK"

/* ═════════════════════════════════════════════════════════════════
 * MODE 1 — OOK PWM (Brand_Auto_Honda_TX)
 * Exact Firmware Timings:
 * Preamble: 312 × HIGH 250µs + LOW 250µs 
 * Bit 0: HIGH 250µs + LOW 250µs 
 * Bit 1: HIGH 480µs + LOW 480µs 
 * Guard: HIGH 100µs + LOW 740µs before preamble 
 * Frame: 11 bytes, 88 bits, LSB-first per byte 
 * ══════════════════════════════════════════════════════════════════════════ */

#define HV3_OOK_TE_SHORT          250u
#define HV3_OOK_TE_LONG           480u
#define HV3_OOK_TE_DELTA          100u
#define HV3_OOK_GUARD_HIGH_US     100u
#define HV3_OOK_GUARD_LOW_US      740u
#define HV3_OOK_PREAMBLE_CYCLES   312u
#define HV3_OOK_FRAME_BYTES       11u
#define HV3_OOK_FRAME_BITS        88u
#define HV3_OOK_MIN_BITS          64u
#define HV3_OOK_MIN_PREAMBLE      20u
/* Buffer encoder:
 *   2 (guard) + 312×2 (preamble) + 88×2 (data) + 1 (tail) = 803 → 900 */
#define HV3_OOK_ENC_BUF_SIZE      900u

/* ═════════════════════════════════════════════════════════════════
 * MODE 2 — Inverted KeeLoq PWM (Crypto_Scramble_Finalize_Keeloq)
 * Exact timings of the firmware: 
 * Preamble: 23 × HIGH 400µs + LOW 400µs 
 * Sync gap: LOW 4000µs 
 * Bit 0: HIGH 400µs + LOW 400µs 
 * Bit 1: HIGH 400µs + LOW ~50µs (PWM INVERTED) 
 * Trail: LOW 15600µs 
 * Reps: 5 (with new RC in each one) 
 * Frame: 66-bit LSB-first over the air 
 * ══════════════════════════════════════════════════════════════════════════ */

#define HV3_KL_TE_BASE            400u
#define HV3_KL_TE_DELTA           150u
#define HV3_KL_TE_BIT1_LOW        50u
#define HV3_KL_PREAMBLE_COUNT     23u
#define HV3_KL_SYNC_GAP_US        4000u
#define HV3_KL_TRAIL_US           15600u
#define HV3_KL_SYNC_MIN_US        2500u
#define HV3_KL_REPEAT             5u
#define HV3_KL_FRAME_BITS         66u
#define HV3_KL_ROLLING_WINDOW     128u
/* Buffer encoder:
 *   Per rep: 23×2 + 1 + 66×2 + 1 = 180 symbols
 *   5 reps: 900 + margin = 1200 */
#define HV3_KL_ENC_BUF_SIZE       1200u

/* KeeLoq engine */
#define HV3_KL_NLF                0x3A5C742EUL
#define HV3_KL_ROUNDS             528

#define HV3_KL_DEC_FUNC_MASK      0x0000000FUL
#define HV3_KL_DEC_DISC_MASK      0x00000FF0UL
#define HV3_KL_DEC_DISC_SHIFT     4
#define HV3_KL_DEC_RC_MASK        0xFFFF0000UL
#define HV3_KL_DEC_RC_SHIFT       16

/* ═══════════════════════════════════════════════════════════════════ 
 * MODE 3 — FSK Manchester (Protocol_TX_Honda_Extended) 
 * Firmware timings (500µs FSK → ~63µs OOK capture, ratio ~8x):
 * Short half-bit: ~63µs during OOK capture
 * Long half-bit: ~126µs during OOK capture
 * Frame: 14 bytes, 112 bits, MSB-first
 * Preamble: 55 alternating cycles
 * ══════════════════════════════════════════════════════════════════════════ */

#define HV3_FSK_TE_SHORT          63u
#define HV3_FSK_TE_LONG           126u
#define HV3_FSK_TE_DELTA          25u
#define HV3_FSK_GUARD_TIME_US     700u
#define HV3_FSK_PREAMBLE_CYCLES   55u
#define HV3_FSK_MIN_PREAMBLE      20u
#define HV3_FSK_FRAME_BYTES       14u
#define HV3_FSK_FRAME_BITS        112u
#define HV3_FSK_MIN_BITS          64u
/* Buffer encoder:
 *   2 (guard) + 55×2 (preamble) + 112×2 (data) + 1 = 339 → 400 */
#define HV3_FSK_ENC_BUF_SIZE      400u
#define HV3_FSK_HALF_BIT_BUF      512u

#define HV3_BTN_PANIC         0x0u
#define HV3_BTN_LOCK          0x1u
#define HV3_BTN_TRUNK         0x2u
#define HV3_BTN_REMOTE_START  0x4u
#define HV3_BTN_UNLOCK        0x8u
#define HV3_BTN_UNLOCK2       0x9u
#define HV3_BTN_CUSTOM_MAX    5u

/* ═════════════════════════════════════════════════════════════════
 * MANUFACTURER KEY TABLE — 21 entries
 * Source: Pandora firmware @ 0x14D60
 * key_64 = (high32 << 32) | low32 
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t low;
    uint32_t high;
} HondaV3MfrKey;

#define HV3_MFR_KEY_COUNT 21

extern const HondaV3MfrKey honda_v3_mfr_keys[HV3_MFR_KEY_COUNT];

/* ═══════════════════════════════════════════════════════════════════
 * Mode 3 — Protocol_TX_Honda_Extended
 * ═══════════════════════════════════════════════════════════════════ */

/* table_1492C — primary lookup (16×16) */
#define HV3_TABLE_PRIMARY \
    {0x01,0x0B,0x04,0x0E,0x07,0x0D,0x02,0x08,0x0A,0x00,0x0F,0x05,0x0C,0x06,0x09,0x03}, \
    {0x00,0x0A,0x05,0x0F,0x06,0x0C,0x03,0x09,0x0B,0x01,0x0E,0x04,0x0D,0x07,0x08,0x02}, \
    {0x05,0x0F,0x00,0x0A,0x03,0x09,0x06,0x0C,0x0E,0x04,0x0B,0x01,0x08,0x02,0x0D,0x07}, \
    {0x04,0x0E,0x01,0x0B,0x02,0x08,0x07,0x0D,0x0F,0x05,0x0A,0x00,0x09,0x03,0x0C,0x06}, \
    {0x0B,0x01,0x0E,0x04,0x0D,0x07,0x08,0x02,0x00,0x0A,0x05,0x0F,0x06,0x0C,0x03,0x09}, \
    {0x0A,0x00,0x0F,0x05,0x0C,0x06,0x09,0x03,0x01,0x0B,0x04,0x0E,0x07,0x0D,0x02,0x08}, \
    {0x0F,0x05,0x0A,0x00,0x09,0x03,0x0C,0x06,0x04,0x0E,0x01,0x0B,0x02,0x08,0x07,0x0D}, \
    {0x0E,0x04,0x0B,0x01,0x08,0x02,0x0D,0x07,0x05,0x0F,0x00,0x0A,0x03,0x09,0x06,0x0C}, \
    {0x09,0x03,0x06,0x0C,0x0F,0x05,0x0A,0x00,0x02,0x08,0x0D,0x07,0x04,0x0E,0x0B,0x01}, \
    {0x08,0x02,0x07,0x0D,0x0E,0x04,0x0B,0x01,0x03,0x09,0x0C,0x06,0x05,0x0F,0x0A,0x00}, \
    {0x0D,0x07,0x02,0x08,0x0B,0x01,0x0E,0x04,0x06,0x0C,0x09,0x03,0x00,0x0A,0x0F,0x05}, \
    {0x0C,0x06,0x03,0x09,0x0A,0x00,0x0F,0x05,0x07,0x0D,0x08,0x02,0x01,0x0B,0x0E,0x04}, \
    {0x03,0x09,0x0C,0x06,0x05,0x0F,0x00,0x0A,0x08,0x02,0x07,0x0D,0x0E,0x04,0x01,0x0B}, \
    {0x02,0x08,0x0D,0x07,0x04,0x0E,0x01,0x0B,0x09,0x03,0x06,0x0C,0x0F,0x05,0x00,0x0A}, \
    {0x07,0x0D,0x08,0x02,0x01,0x0B,0x04,0x0E,0x0C,0x06,0x03,0x09,0x0A,0x00,0x05,0x0F}, \
    {0x06,0x0C,0x09,0x03,0x00,0x0A,0x05,0x0F,0x0D,0x07,0x02,0x08,0x0B,0x01,0x04,0x0E}

/* table_14A2C — secondary lookup (16×16) */
#define HV3_TABLE_SECONDARY \
    {0x06,0x0C,0x03,0x09,0x00,0x0A,0x05,0x0F,0x0D,0x07,0x08,0x02,0x0B,0x01,0x0E,0x04}, \
    {0x07,0x0D,0x02,0x08,0x01,0x0B,0x04,0x0E,0x0C,0x06,0x09,0x03,0x0A,0x00,0x0F,0x05}, \
    {0x02,0x08,0x07,0x0D,0x04,0x0E,0x01,0x0B,0x09,0x03,0x0C,0x06,0x0F,0x05,0x0A,0x00}, \
    {0x03,0x09,0x06,0x0C,0x05,0x0F,0x00,0x0A,0x08,0x02,0x0D,0x07,0x0E,0x04,0x0B,0x01}, \
    {0x0C,0x06,0x09,0x03,0x0A,0x00,0x0F,0x05,0x07,0x0D,0x02,0x08,0x01,0x0B,0x04,0x0E}, \
    {0x0D,0x07,0x08,0x02,0x0B,0x01,0x0E,0x04,0x06,0x0C,0x03,0x09,0x00,0x0A,0x05,0x0F}, \
    {0x08,0x02,0x0D,0x07,0x0E,0x04,0x0B,0x01,0x03,0x09,0x06,0x0C,0x05,0x0F,0x00,0x0A}, \
    {0x09,0x03,0x0C,0x06,0x0F,0x05,0x0A,0x00,0x02,0x08,0x07,0x0D,0x04,0x0E,0x01,0x0B}, \
    {0x03,0x09,0x06,0x0C,0x05,0x0F,0x00,0x0A,0x08,0x02,0x0D,0x07,0x0E,0x04,0x0B,0x01}, \
    {0x02,0x08,0x07,0x0D,0x04,0x0E,0x01,0x0B,0x09,0x03,0x0C,0x06,0x0F,0x05,0x0A,0x00}, \
    {0x07,0x0D,0x02,0x08,0x01,0x0B,0x04,0x0E,0x0C,0x06,0x09,0x03,0x0A,0x00,0x0F,0x05}, \
    {0x06,0x0C,0x03,0x09,0x00,0x0A,0x05,0x0F,0x0D,0x07,0x08,0x02,0x0B,0x01,0x0E,0x04}, \
    {0x09,0x03,0x0C,0x06,0x0F,0x05,0x0A,0x00,0x02,0x08,0x07,0x0D,0x04,0x0E,0x01,0x0B}, \
    {0x08,0x02,0x0D,0x07,0x0E,0x04,0x0B,0x01,0x03,0x09,0x06,0x0C,0x05,0x0F,0x00,0x0A}, \
    {0x0D,0x07,0x08,0x02,0x0B,0x01,0x0E,0x04,0x06,0x0C,0x03,0x09,0x00,0x0A,0x05,0x0F}, \
    {0x0C,0x06,0x09,0x03,0x0A,0x00,0x0F,0x05,0x07,0x0D,0x02,0x08,0x01,0x0B,0x04,0x0E}

/* table_14B2C — substitution output (16×16) */
#define HV3_TABLE_SUBST \
    {0x01,0x00,0x05,0x04,0x0B,0x0A,0x0F,0x0E,0x04,0x05,0x00,0x01,0x0E,0x0F,0x0A,0x0B}, \
    {0x0F,0x0E,0x0B,0x0A,0x05,0x04,0x01,0x00,0x0A,0x0B,0x0E,0x0F,0x00,0x01,0x04,0x05}, \
    {0x0E,0x0F,0x0A,0x0B,0x04,0x05,0x00,0x01,0x0B,0x0A,0x0F,0x0E,0x01,0x00,0x05,0x04}, \
    {0x00,0x01,0x04,0x05,0x0A,0x0B,0x0E,0x0F,0x05,0x04,0x01,0x00,0x0F,0x0E,0x0B,0x0A}, \
    {0x02,0x03,0x06,0x07,0x08,0x09,0x0C,0x0D,0x07,0x06,0x03,0x02,0x0D,0x0C,0x09,0x08}, \
    {0x0C,0x0D,0x08,0x09,0x06,0x07,0x02,0x03,0x09,0x08,0x0D,0x0C,0x03,0x02,0x07,0x06}, \
    {0x0D,0x0C,0x09,0x08,0x07,0x06,0x03,0x02,0x08,0x09,0x0C,0x0D,0x02,0x03,0x06,0x07}, \
    {0x03,0x02,0x07,0x06,0x09,0x08,0x0D,0x0C,0x06,0x07,0x02,0x03,0x0C,0x0D,0x08,0x09}, \
    {0x04,0x05,0x00,0x01,0x0E,0x0F,0x0A,0x0B,0x01,0x00,0x05,0x04,0x0B,0x0A,0x0F,0x0E}, \
    {0x0A,0x0B,0x0E,0x0F,0x00,0x01,0x04,0x05,0x0F,0x0E,0x0B,0x0A,0x05,0x04,0x01,0x00}, \
    {0x0B,0x0A,0x0F,0x0E,0x01,0x00,0x05,0x04,0x0E,0x0F,0x0A,0x0B,0x04,0x05,0x00,0x01}, \
    {0x05,0x04,0x01,0x00,0x0F,0x0E,0x0B,0x0A,0x00,0x01,0x04,0x05,0x0A,0x0B,0x0E,0x0F}, \
    {0x07,0x06,0x03,0x02,0x0D,0x0C,0x09,0x08,0x02,0x03,0x06,0x07,0x08,0x09,0x0C,0x0D}, \
    {0x09,0x08,0x0D,0x0C,0x03,0x02,0x07,0x06,0x0C,0x0D,0x08,0x09,0x06,0x07,0x02,0x03}, \
    {0x08,0x09,0x0C,0x0D,0x02,0x03,0x06,0x07,0x0D,0x0C,0x09,0x08,0x07,0x06,0x03,0x02}, \
    {0x06,0x07,0x02,0x03,0x0C,0x0D,0x08,0x09,0x03,0x02,0x07,0x06,0x09,0x08,0x0D,0x0C}

/* ══════════════════════════════════════════════════════════════════
 * GENERIC.DATA LAYOUT — 64 bits, shared by all 3 modes
 *
 * [63:36] 28-bit serial
 * [35:32] button 4 bits 
 * [31:16] rolling 16 bits (RC for KL; counter[15:0] for OOK/FSK) 
 * [15:8] discr/mode 8 bits (discriminant KL / mode OOK-FSK) 
 * [7:4] key_idx 4 bits (KL: table index; OOK/FSK: 0xF) 
 * [3:2] proto_mode 2 bits (0=OOK, 1=KL, 2=FSK) 
 * [1:0] status/flags 2 bits 
 * ══════════════════════════════════════════════════════════════════════════ */

#define HV3_PROTO_MODE_OOK   0u
#define HV3_PROTO_MODE_KL    1u
#define HV3_PROTO_MODE_FSK   2u

/* ═══════════════════════════════════════════════════════════════════
 * DECODER STATE — specific frame type
 * ═══════════════════════════════════════════════════════════════════ */

/* Frame OOK (Mode 1) — 11 bytes */
typedef struct {
    uint8_t  button;
    uint32_t serial;
    uint32_t counter;
    uint8_t  mode;
    uint8_t  checksum;
    uint8_t  extra[2];
} HondaV3OOKFrame;

/* Frame KeeLoq (Mode 2) */
typedef struct {
    uint32_t encrypted;
    uint32_t serial;
    uint8_t  button;
    uint8_t  status;
    uint32_t decrypted;
    uint16_t rolling_code;
    uint8_t  discriminant;
    int8_t   key_idx;
} HondaV3KLFrame;

/* Frame FSK (Mode 3) — 14 bytes */
typedef struct {
    bool     type_b;
    uint8_t  type_b_header;
    uint8_t  button;
    uint32_t serial;
    uint32_t counter;
    uint8_t  mode;
    uint8_t  checksum;
    uint8_t  extra[5];
} HondaV3FSKFrame;

/* ═══════════════════════════════════════════════════════════════════
 * DECODER STATE — Honda V3 OOK
 * ═══════════════════════════════════════════════════════════════════ */
typedef enum {
    HV3OOKStep_Reset = 0,
    HV3OOKStep_Preamble,
    HV3OOKStep_Data,
} HV3OOKDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder        decoder;
    SubGhzBlockGeneric        generic;

    HV3OOKDecoderStep step;
    uint16_t          preamble_count;
    uint8_t           data_buf[16];
    uint8_t           bit_count;
    uint32_t          last_high_dur;
    bool              frame_valid;
    HondaV3OOKFrame   frame;
} SubGhzProtocolDecoderHondaV3OOK;

/* ═══════════════════════════════════════════════════════════════════
 * ENCODER STATE — Honda V3 OOK
 * ═══════════════════════════════════════════════════════════════════ */
typedef struct {
    SubGhzProtocolEncoderBase  base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric         generic;

    HondaV3OOKFrame frame;
    uint8_t         active_button;
} SubGhzProtocolEncoderHondaV3OOK;

/* ═══════════════════════════════════════════════════════════════════
 * DECODER STATE — Honda V3 KL
 * ═══════════════════════════════════════════════════════════════════ */
typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder        decoder;
    SubGhzBlockGeneric        generic;

    HondaV3KLFrame frame;
    bool           frame_valid;
    uint16_t       last_rolling;
    bool           has_last_rolling;
    FuriString*    result_str;
} SubGhzProtocolDecoderHondaV3KL;

/* ═══════════════════════════════════════════════════════════════════
 * ENCODER STATE — Honda V3 KL
 * ═══════════════════════════════════════════════════════════════════ */
typedef struct {
    SubGhzProtocolEncoderBase  base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric         generic;

    uint64_t key;
    uint32_t serial;
    uint16_t rolling_code;
    uint8_t  btn;
    uint8_t  status;
    uint8_t  active_button;
    bool     frame_bits[HV3_KL_FRAME_BITS];
} SubGhzProtocolEncoderHondaV3KL;

/* ═══════════════════════════════════════════════════════════════════
 * DECODER STATE — Honda V3 FSK
 * ═══════════════════════════════════════════════════════════════════ */
typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder        decoder;
    SubGhzBlockGeneric        generic;

    uint8_t  half_bits[HV3_FSK_HALF_BIT_BUF];
    uint16_t hb_count;
    bool     frame_valid;
    HondaV3FSKFrame frame;
} SubGhzProtocolDecoderHondaV3FSK;

/* ═══════════════════════════════════════════════════════════════════
 * ENCODER STATE — Honda V3 FSK
 * ═══════════════════════════════════════════════════════════════════ */
typedef struct {
    SubGhzProtocolEncoderBase  base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric         generic;

    HondaV3FSKFrame frame;
    uint8_t         active_button;
} SubGhzProtocolEncoderHondaV3FSK;

/* ═══════════════════════════════════════════════════════════════════
 * VTABLES — 3 independent protocols
 * ═══════════════════════════════════════════════════════════════════ */
extern const SubGhzProtocolDecoder subghz_protocol_honda_v3_ook_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_honda_v3_ook_encoder;
extern const SubGhzProtocol        subghz_protocol_honda_v3_ook;

extern const SubGhzProtocolDecoder subghz_protocol_honda_v3_kl_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_honda_v3_kl_encoder;
extern const SubGhzProtocol        subghz_protocol_honda_v3_kl;

extern const SubGhzProtocolDecoder subghz_protocol_honda_v3_fsk_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_honda_v3_fsk_encoder;
extern const SubGhzProtocol        subghz_protocol_honda_v3_fsk;

/* ═══════════════════════════════════════════════════════════════════
 * PUBLIC API — OOK
 * ═══════════════════════════════════════════════════════════════════ */
void*  subghz_protocol_decoder_honda_v3_ook_alloc(SubGhzEnvironment* env);
void   subghz_protocol_decoder_honda_v3_ook_free(void* ctx);
void   subghz_protocol_decoder_honda_v3_ook_reset(void* ctx);
void   subghz_protocol_decoder_honda_v3_ook_feed(void* ctx, bool level, uint32_t dur);
uint8_t subghz_protocol_decoder_honda_v3_ook_get_hash_data(void* ctx);
SubGhzProtocolStatus subghz_protocol_decoder_honda_v3_ook_serialize(
    void* ctx, FlipperFormat* ff, SubGhzRadioPreset* preset);
SubGhzProtocolStatus subghz_protocol_decoder_honda_v3_ook_deserialize(
    void* ctx, FlipperFormat* ff);
void   subghz_protocol_decoder_honda_v3_ook_get_string(void* ctx, FuriString* out);

void*  subghz_protocol_encoder_honda_v3_ook_alloc(SubGhzEnvironment* env);
void   subghz_protocol_encoder_honda_v3_ook_free(void* ctx);
SubGhzProtocolStatus subghz_protocol_encoder_honda_v3_ook_deserialize(
    void* ctx, FlipperFormat* ff);
void   subghz_protocol_encoder_honda_v3_ook_stop(void* ctx);
LevelDuration subghz_protocol_encoder_honda_v3_ook_yield(void* ctx);

/* ═══════════════════════════════════════════════════════════════════
 * PUBLIC API — KL
 * ═══════════════════════════════════════════════════════════════════ */
void*  subghz_protocol_decoder_honda_v3_kl_alloc(SubGhzEnvironment* env);
void   subghz_protocol_decoder_honda_v3_kl_free(void* ctx);
void   subghz_protocol_decoder_honda_v3_kl_reset(void* ctx);
void   subghz_protocol_decoder_honda_v3_kl_feed(void* ctx, bool level, uint32_t dur);
uint8_t subghz_protocol_decoder_honda_v3_kl_get_hash_data(void* ctx);
SubGhzProtocolStatus subghz_protocol_decoder_honda_v3_kl_serialize(
    void* ctx, FlipperFormat* ff, SubGhzRadioPreset* preset);
SubGhzProtocolStatus subghz_protocol_decoder_honda_v3_kl_deserialize(
    void* ctx, FlipperFormat* ff);
void   subghz_protocol_decoder_honda_v3_kl_get_string(void* ctx, FuriString* out);

void*  subghz_protocol_encoder_honda_v3_kl_alloc(SubGhzEnvironment* env);
void   subghz_protocol_encoder_honda_v3_kl_free(void* ctx);
SubGhzProtocolStatus subghz_protocol_encoder_honda_v3_kl_deserialize(
    void* ctx, FlipperFormat* ff);
void   subghz_protocol_encoder_honda_v3_kl_stop(void* ctx);
LevelDuration subghz_protocol_encoder_honda_v3_kl_yield(void* ctx);

/* ═══════════════════════════════════════════════════════════════════
 * PUBLIC API — FSK
 * ═══════════════════════════════════════════════════════════════════ */
void*  subghz_protocol_decoder_honda_v3_fsk_alloc(SubGhzEnvironment* env);
void   subghz_protocol_decoder_honda_v3_fsk_free(void* ctx);
void   subghz_protocol_decoder_honda_v3_fsk_reset(void* ctx);
void   subghz_protocol_decoder_honda_v3_fsk_feed(void* ctx, bool level, uint32_t dur);
uint8_t subghz_protocol_decoder_honda_v3_fsk_get_hash_data(void* ctx);
SubGhzProtocolStatus subghz_protocol_decoder_honda_v3_fsk_serialize(
    void* ctx, FlipperFormat* ff, SubGhzRadioPreset* preset);
SubGhzProtocolStatus subghz_protocol_decoder_honda_v3_fsk_deserialize(
    void* ctx, FlipperFormat* ff);
void   subghz_protocol_decoder_honda_v3_fsk_get_string(void* ctx, FuriString* out);

void*  subghz_protocol_encoder_honda_v3_fsk_alloc(SubGhzEnvironment* env);
void   subghz_protocol_encoder_honda_v3_fsk_free(void* ctx);
SubGhzProtocolStatus subghz_protocol_encoder_honda_v3_fsk_deserialize(
    void* ctx, FlipperFormat* ff);
void   subghz_protocol_encoder_honda_v3_fsk_stop(void* ctx);
LevelDuration subghz_protocol_encoder_honda_v3_fsk_yield(void* ctx);
