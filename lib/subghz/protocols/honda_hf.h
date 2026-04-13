#pragma once

#include <lib/subghz/protocols/base.h>
#include <lib/subghz/blocks/const.h>
#include <lib/subghz/blocks/decoder.h>
#include <lib/subghz/blocks/encoder.h>
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/blocks/math.h>

#define SUBGHZ_PROTOCOL_HONDA_HF_NAME "Honda V1"

/* ============================================================================
 * Honda_HF — Honda/Acura OOK PWM on 307.8 MHz (or 433 MHz for some models)
 *
 * Pandora case 0x17 (Honda) → Brand_Auto_Honda_TX @ firmware
 *
 * Modulation: OOK
 * Encoding: PWM (pulse-width modulation)
 *   bit 0: HIGH ~250µs + LOW ~250µs  (short period ~500µs)
 *   bit 1: HIGH ~480µs + LOW ~480µs  (long period ~960µs)
 *
 * Bit order: LSB-first per byte (firmware uses mask starting at bit 0)
 *
 * Preamble: 312 cycles × (HIGH 250µs + LOW 250µs)
 * Guard: HIGH 100µs → LOW 100µs → LOW 740µs before preamble
 *
 * Frame: 11 bytes (88 bits) from buf[0..10]
 *   buf[0]  = (button << 4) | serial_hi_nibble
 *   buf[1]  = serial[23:16]
 *   buf[2]  = serial[15:8]
 *   buf[3]  = serial[7:0]
 *   buf[4]  = counter[23:16]
 *   buf[5]  = counter[15:8]
 *   buf[6]  = counter[7:0]
 *   buf[7]  = (mode << 4) | counter_lsn
 *   buf[8]  = checksum (table-substituted)
 *   buf[9]  = extra byte 1
 *   buf[10] = extra byte 2
 * ==========================================================================*/

/* PWM timing constants */
#define HONDA_HF_TE_SHORT       250u    /* bit-0 half-period */
#define HONDA_HF_TE_LONG        480u    /* bit-1 half-period */
#define HONDA_HF_TE_DELTA       100u    /* tolerance */
#define HONDA_HF_GUARD_US       740u    /* pre-preamble LOW gap */
#define HONDA_HF_PREAMBLE_CYCLES 312u   /* number of preamble cycles */

/* Frame parameters */
#define HONDA_HF_FRAME_BYTES    11u     /* 11 bytes per firmware CMP R5, #0xB */
#define HONDA_HF_FRAME_BITS     88u     /* 11 × 8 */
#define HONDA_HF_MIN_BITS       64u     /* minimum for partial decode */

/* Button codes (same as Honda) */
#define HONDA_HF_BTN_LOCK       0x01u
#define HONDA_HF_BTN_UNLOCK     0x02u
#define HONDA_HF_BTN_TRUNK      0x04u
#define HONDA_HF_BTN_PANIC      0x08u
#define HONDA_HF_BTN_RSTART     0x05u
#define HONDA_HF_BTN_LOCK2PRESS 0x09u
#define HONDA_HF_CUSTOM_BTN_MAX 5

/* CC1101 preset — OOK for 307.8 MHz */
#define HONDA_HF_CC1101_PRESET_DATA \
    0x02, 0x0D, \
    0x0B, 0x06, \
    0x08, 0x32, \
    0x07, 0x04, \
    0x14, 0x00, \
    0x13, 0x02, \
    0x12, 0x04, \
    0x11, 0x36, \
    0x10, 0x69, \
    0x15, 0x32, \
    0x18, 0x18, \
    0x19, 0x16, \
    0x1D, 0x91, \
    0x1C, 0x00, \
    0x1B, 0x07, \
    0x20, 0xFB, \
    0x22, 0x10, \
    0x21, 0x56, \
    0x00, 0x00, \
    0xC0, 0x00

extern const SubGhzProtocolDecoder subghz_protocol_honda_hf_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_honda_hf_encoder;
extern const SubGhzProtocol        subghz_protocol_honda_hf;

void* subghz_protocol_decoder_honda_hf_alloc(SubGhzEnvironment* environment);
void  subghz_protocol_decoder_honda_hf_free(void* context);
void  subghz_protocol_decoder_honda_hf_reset(void* context);
void  subghz_protocol_decoder_honda_hf_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_honda_hf_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_honda_hf_serialize(
    void* context, FlipperFormat* flipper_format, SubGhzRadioPreset* preset);
SubGhzProtocolStatus subghz_protocol_decoder_honda_hf_deserialize(
    void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_honda_hf_get_string(void* context, FuriString* output);

void* subghz_protocol_encoder_honda_hf_alloc(SubGhzEnvironment* environment);
void  subghz_protocol_encoder_honda_hf_free(void* context);
void  subghz_protocol_encoder_honda_hf_stop(void* context);
LevelDuration subghz_protocol_encoder_honda_hf_yield(void* context);
SubGhzProtocolStatus subghz_protocol_encoder_honda_hf_deserialize(
    void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_honda_hf_set_button(void* context, uint8_t btn);

uint8_t subghz_protocol_honda_hf_btn_to_custom(uint8_t btn);
uint8_t subghz_protocol_honda_hf_custom_to_btn(uint8_t custom);
