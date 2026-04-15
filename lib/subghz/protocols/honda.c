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

#define TAG "SubGhzProtocolHonda"

static const SubGhzBlockConst subghz_protocol_honda_const = {
    .te_short = HONDA_TE_SHORT,
    .te_long  = HONDA_TE_LONG,
    .te_delta = HONDA_TE_DELTA,
    .min_count_bit_for_found = HONDA_MIN_BITS,
};

/* ============================================================================
 * Pandora rolling-code tables (extracted from firmware)
 *
 * Firmware table mapping:
 *   table_1492C = honda_tbl_primary   — primary lookup (match column 15)
 *   table_14A2C = honda_tbl_secondary — secondary lookup (match column 15)
 *   table_14B2C = honda_tbl_subst     — substitution output table
 * ==========================================================================*/
static const uint8_t honda_tbl_primary[16][16]   = { HONDA_TABLE_1492C };
static const uint8_t honda_tbl_secondary[16][16] = { HONDA_TABLE_14A2C };
static const uint8_t honda_tbl_subst[16][16]     = { HONDA_TABLE_14B2C };

/* ============================================================================
 * Bit-reverse helpers (mirrors Crypto.Util.Bit_Reverse_Byte @ 0x11AD4)
 * ==========================================================================*/
static inline uint8_t _bit_rev8(uint8_t v) {
    v = (uint8_t)(((v & 0xF0u) >> 4) | ((v & 0x0Fu) << 4));
    v = (uint8_t)(((v & 0xCCu) >> 2) | ((v & 0x33u) << 2));
    v = (uint8_t)(((v & 0xAAu) >> 1) | ((v & 0x55u) << 1));
    return v;
}

static inline uint8_t _bit_rev4(uint8_t v) {
    return (uint8_t)(_bit_rev8(v & 0x0Fu) >> 4);
}

/* ============================================================================
 * Frame data structure
 *
 * Full 14-byte frame from Protocol_TX_Honda_Extended:
 *   buf[0]  = (button << 4) | serial_hi_nibble   [Type-A]
 *           = (type_b_header << 4) | button       [Type-B]
 *   buf[1]  = serial[23:16]
 *   buf[2]  = serial[15:8]
 *   buf[3]  = serial[7:0] / counter cascade
 *   buf[4]  = counter[23:16]
 *   buf[5]  = counter[19:12]
 *   buf[6]  = counter[11:4]
 *   buf[7]  = (mode_nibble << 4) | counter[3:0]
 *   buf[8]  = checksum
 *   buf[9..13] = extra bytes (preserved for retransmission)
 * ==========================================================================*/
typedef struct {
    bool     type_b;
    uint8_t  type_b_header;
    uint8_t  button;
    uint32_t serial;
    uint32_t counter;
    uint8_t  checksum;
    uint8_t  mode;        /* high nibble of buf[7]: 0x2 or 0xC */
    uint8_t  extra[5];    /* buf[9..13] — preserved from capture */
} HondaFrameData;

/* ============================================================================
 * Build / parse the 14-byte Pandora buffer
 * ==========================================================================*/
static void _honda_to_buf(const HondaFrameData* f, uint8_t buf[14]) {
    memset(buf, 0, 14);
    if(!f->type_b) {
        buf[0] = (uint8_t)((f->button << 4) | ((f->serial >> 24) & 0x0Fu));
        buf[1] = (uint8_t)((f->serial >> 16) & 0xFFu);
        buf[2] = (uint8_t)((f->serial >> 8)  & 0xFFu);
        buf[3] = (uint8_t)( f->serial        & 0xFFu);
        buf[4] = (uint8_t)((f->counter >> 16) & 0xFFu);
        buf[5] = (uint8_t)((f->counter >> 8)  & 0xFFu);
        buf[6] = (uint8_t)( f->counter        & 0xFFu);
        buf[7] = (uint8_t)(((f->mode & 0x0Fu) << 4) | ((f->counter) & 0x0Fu));
    } else {
        buf[0] = (uint8_t)((f->type_b_header << 4) | (f->button & 0x0Fu));
        buf[1] = (uint8_t)((f->serial >> 20) & 0xFFu);
        buf[2] = (uint8_t)((f->serial >> 12) & 0xFFu);
        buf[3] = (uint8_t)((f->serial >> 4)  & 0xFFu);
        buf[4] = (uint8_t)(((f->serial & 0x0Fu) << 4) | ((f->counter >> 20) & 0x0Fu));
        buf[5] = (uint8_t)((f->counter >> 12) & 0xFFu);
        buf[6] = (uint8_t)((f->counter >> 4)  & 0xFFu);
        buf[7] = (uint8_t)(((f->mode & 0x0Fu) << 4) | (f->counter & 0x0Fu));
    }
    buf[8] = f->checksum;
    memcpy(&buf[9], f->extra, 5);
}

static void _honda_from_buf(const uint8_t buf[14], HondaFrameData* f, bool type_b) {
    f->type_b = type_b;
    if(!type_b) {
        f->type_b_header = 0;
        f->button = (buf[0] >> 4) & 0x0Fu;
        f->serial = ((uint32_t)(buf[0] & 0x0Fu) << 24) |
                    ((uint32_t)buf[1] << 16) |
                    ((uint32_t)buf[2] << 8)  |
                     (uint32_t)buf[3];
        f->counter = ((uint32_t)buf[4] << 16) |
                     ((uint32_t)buf[5] << 8)  |
                      (uint32_t)buf[6];
        f->mode = (buf[7] >> 4) & 0x0Fu;
    } else {
        f->type_b_header = (buf[0] >> 4) & 0x0Fu;
        f->button = buf[0] & 0x0Fu;
        f->serial = ((uint32_t)buf[1] << 20) |
                    ((uint32_t)buf[2] << 12) |
                    ((uint32_t)buf[3] << 4)  |
                    ((uint32_t)(buf[4] >> 4) & 0x0Fu);
        f->counter = ((uint32_t)(buf[4] & 0x0Fu) << 20) |
                     ((uint32_t)buf[5] << 12) |
                     ((uint32_t)buf[6] << 4)  |
                      (uint32_t)(buf[7] & 0x0Fu);
        f->mode = (buf[7] >> 4) & 0x0Fu;
    }
    f->checksum = buf[8];
    memcpy(f->extra, &buf[9], 5);
}

/* ============================================================================
 * Rolling code checksum — mirrors Pandora firmware @ 0xF0CE-0xF1A0
 * ==========================================================================*/
static uint8_t _honda_rolling_checksum(const uint8_t buf[14]) {
    uint8_t prev_lo = buf[8] & 0x0Fu;
    uint8_t prev_hi = (buf[8] >> 4) & 0x0Fu;
    uint8_t new_lo = prev_lo;
    uint8_t new_hi = prev_hi;

    uint8_t col_lo = _bit_rev4(buf[3] & 0x0Fu);
    uint8_t col_hi = _bit_rev4((buf[3] >> 4) & 0x0Fu);

    /* Low nibble lookup */
    bool found = false;
    for(uint8_t row = 0; row < 16 && !found; row++) {
        if(honda_tbl_primary[row][15] == prev_lo) {
            uint8_t target = honda_tbl_primary[row][0];
            for(uint8_t row2 = 0; row2 < 16; row2++) {
                if(honda_tbl_subst[row2][col_lo] == target) {
                    uint8_t next_col = (col_lo + 1u) & 0x0Fu;
                    new_lo = honda_tbl_subst[row2][next_col];
                    found = true;
                    break;
                }
            }
            if(!found) {
                new_lo = honda_tbl_subst[row][col_lo];
                found = true;
            }
        }
    }
    if(!found) {
        for(uint8_t row = 0; row < 16; row++) {
            if(honda_tbl_secondary[row][15] == prev_lo) {
                uint8_t target = honda_tbl_secondary[row][0];
                for(uint8_t row2 = 0; row2 < 16; row2++) {
                    if(honda_tbl_subst[row2][col_lo] == target) {
                        uint8_t next_col = (col_lo + 1u) & 0x0Fu;
                        new_lo = honda_tbl_subst[row2][next_col];
                        found = true;
                        break;
                    }
                }
                break;
            }
        }
    }

    /* High nibble lookup */
    found = false;
    for(uint8_t row = 0; row < 16 && !found; row++) {
        if(honda_tbl_primary[row][15] == prev_hi) {
            uint8_t target = honda_tbl_primary[row][0];
            for(uint8_t row2 = 0; row2 < 16; row2++) {
                if(honda_tbl_subst[row2][col_hi] == target) {
                    uint8_t next_col = (col_hi + 1u) & 0x0Fu;
                    new_hi = honda_tbl_subst[row2][next_col];
                    found = true;
                    break;
                }
            }
            if(!found) {
                new_hi = honda_tbl_subst[row][col_hi];
                found = true;
            }
        }
    }
    if(!found) {
        for(uint8_t row = 0; row < 16; row++) {
            if(honda_tbl_secondary[row][15] == prev_hi) {
                uint8_t target = honda_tbl_secondary[row][0];
                for(uint8_t row2 = 0; row2 < 16; row2++) {
                    if(honda_tbl_subst[row2][col_hi] == target) {
                        uint8_t next_col = (col_hi + 1u) & 0x0Fu;
                        new_hi = honda_tbl_subst[row2][next_col];
                        found = true;
                        break;
                    }
                }
                break;
            }
        }
    }

    return (uint8_t)((new_hi << 4) | (new_lo & 0x0Fu));
}

/* ============================================================================
 * Simple XOR checksum (for initial decode validation only)
 * ==========================================================================*/
static uint8_t _honda_xor_checksum(const uint8_t* data, uint8_t len) {
    uint8_t c = 0;
    for(uint8_t i = 0; i < len; i++) c ^= data[i];
    return c;
}

/* ============================================================================
 * Counter increment — exact Pandora firmware logic @ 0xEFF8-0xF08E
 *
 * The firmware operates on the WIRE BUFFER (R5=src, R4=dest temp copy).
 * Key insight: it uses bit_rev8 on the FULL BYTE, not nibble-by-nibble.
 *
 * Step 1 (0xEFF8-0xF010): high nibble of dest[3] =
 *     bit_rev8(bit_rev8(src[3]) + 1) & 0xF0
 *     low nibble of dest[3] = src[3] & 0x0F  (unchanged)
 *
 * Step 2 (0xF01A-0xF036): if (src[3] >> 4) == 0xF → carry:
 *     low nibble of dest[3] = bit_rev8(bit_rev8(src[3]) + 1) & 0x0F
 *
 * Step 3 (0xF038-0xF060): if src[3] == 0xFF AND high carry:
 *     dest[2] high nibble updated via bit_rev8((bit_rev8(src[2])>>4) + 1)
 *
 * Step 4 (0xF062-0xF08E): if (src[2] >> 4) == 0xF AND step-3 triggered:
 *     dest[2] low nibble updated via bit_rev8(bit_rev8(src[2]) + 1) & 0x0F
 *
 * After updating the buffer, extract the new counter back into f->counter,
 * then compute the rolling checksum on the updated buffer.
 * ==========================================================================*/
static void _honda_counter_increment(HondaFrameData* f) {
    uint8_t buf[14];
    _honda_to_buf(f, buf);   /* snapshot current state into wire buffer */

    uint8_t src3 = buf[3];   /* original buf[3] — used for carry checks */
    uint8_t src2 = buf[2];   /* original buf[2] — used for carry checks */

    /* Step 1: high nibble ← bit_rev8(bit_rev8(src[3]) + 1), low nibble unchanged */
    uint8_t tmp = _bit_rev8((uint8_t)(_bit_rev8(src3) + 1u));
    buf[3] = (tmp & 0xF0u) | (src3 & 0x0Fu);

    /* Step 2: carry — if original high nibble of src[3] == 0xF */
    if((src3 >> 4) == 0x0Fu) {
        tmp = _bit_rev8((uint8_t)(_bit_rev8(src3) + 1u));
        buf[3] = (buf[3] & 0xF0u) | (tmp & 0x0Fu);
    }

    /* Step 3: if src[3] == 0xFF (full byte overflow) → carry into buf[2] */
    if(src3 == 0xFFu && (src3 >> 4) == 0x0Fu) {
        /* BFI inserts into bits [4..7]: high nibble of buf[2] */
        uint8_t hi_new = _bit_rev8((uint8_t)((_bit_rev8(src2) >> 4) + 1u));
        buf[2] = (buf[2] & 0x0Fu) | (uint8_t)((hi_new & 0x0Fu) << 4);

        /* Step 4: if original high nibble of src[2] == 0xF → carry low nibble */
        if((src2 >> 4) == 0x0Fu) {
            tmp = _bit_rev8((uint8_t)(_bit_rev8(src2) + 1u));
            buf[2] = (buf[2] & 0xF0u) | (tmp & 0x0Fu);
        }
    }

    /* Mode flip 0x2 ↔ 0xC */
    f->mode = (f->mode == 0xCu) ? 0x2u : 0xCu;
    buf[7]  = (uint8_t)(((f->mode & 0x0Fu) << 4) | (buf[7] & 0x0Fu));

    /* Extract updated counter from buffer back into f->counter */
    if(!f->type_b) {
        f->counter = ((uint32_t)buf[4] << 16) |
                     ((uint32_t)buf[5] << 8)  |
                      (uint32_t)buf[6];
    } else {
        f->counter = ((uint32_t)(buf[4] & 0x0Fu) << 20) |
                     ((uint32_t)buf[5] << 12) |
                     ((uint32_t)buf[6] << 4)  |
                      (uint32_t)(buf[7] & 0x0Fu);
    }

    /* Compute rolling checksum on the correctly-updated buffer */
    f->checksum = _honda_rolling_checksum(buf);
}

/* ============================================================================
 * Bit helpers — used by fallback 64-bit decoder path
 * ==========================================================================*/
static uint32_t _bits_get(const uint8_t* data, uint8_t start, uint8_t len) {
    uint32_t val = 0;
    for(uint8_t i = 0; i < len; i++) {
        uint8_t byte_idx = (uint8_t)((start + i) / 8u);
        uint8_t bit_idx  = (uint8_t)(7u - ((start + i) % 8u));
        val = (val << 1) | ((data[byte_idx] >> bit_idx) & 1u);
    }
    return val;
}

/* ============================================================================
 * Pack / unpack — 64-bit generic.data for core fields
 * Extra bytes stored in HondaFrameData.extra[]
 * ==========================================================================*/
static uint64_t _honda_pack(const HondaFrameData* f) {
    uint8_t key[8] = {0};
    key[0] = (uint8_t)(((f->type_b ? 1u : 0u) << 7) |
                       ((f->type_b_header & 0x07u) << 4) | (f->button & 0x0Fu));
    key[1] = (uint8_t)((f->serial >> 20) & 0xFFu);
    key[2] = (uint8_t)((f->serial >> 12) & 0xFFu);
    key[3] = (uint8_t)((f->serial >> 4)  & 0xFFu);
    key[4] = (uint8_t)(((f->serial & 0x0Fu) << 4) | ((f->mode & 0x0Fu)));
    key[5] = (uint8_t)((f->counter >> 16) & 0xFFu);
    key[6] = (uint8_t)((f->counter >> 8)  & 0xFFu);
    key[7] = (uint8_t)( f->counter        & 0xFFu);

    uint64_t out = 0;
    for(int i = 0; i < 8; i++) out = (out << 8) | key[i];
    return out;
}

static void _honda_unpack(uint64_t raw, HondaFrameData* f) {
    uint8_t key[8];
    for(int i = 7; i >= 0; i--) {
        key[i] = (uint8_t)(raw & 0xFFu);
        raw >>= 8;
    }

    f->type_b        = (key[0] >> 7) & 0x01u;
    f->type_b_header = (key[0] >> 4) & 0x07u;
    f->button        =  key[0]       & 0x0Fu;
    f->serial  = ((uint32_t)key[1] << 20) | ((uint32_t)key[2] << 12) |
                 ((uint32_t)key[3] << 4)  | ((uint32_t)(key[4] >> 4) & 0x0Fu);
    f->mode    = key[4] & 0x0Fu;
    f->counter = ((uint32_t)key[5] << 16) | ((uint32_t)key[6] << 8) | (uint32_t)key[7];
    f->checksum = 0;
    memset(f->extra, 0, sizeof(f->extra));
}

/* ============================================================================
 * Decoder state
 * ==========================================================================*/
#define HONDA_HALF_BIT_BUF 512u

typedef enum {
    HondaDecoderStepReset = 0,
    HondaDecoderStepAccumulate,
} HondaDecoderStep;

typedef struct SubGhzProtocolDecoderHonda {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder        decoder;
    SubGhzBlockGeneric        generic;

    uint8_t  half_bits[HONDA_HALF_BIT_BUF];
    uint16_t hb_count;

    HondaFrameData frame;
    bool           frame_valid;
} SubGhzProtocolDecoderHonda;

/* ============================================================================
 * Encoder state
 * ==========================================================================*/
#define HONDA_ENC_BUF_SIZE 600u

typedef struct SubGhzProtocolEncoderHonda {
    SubGhzProtocolEncoderBase   base;
    SubGhzProtocolBlockEncoder  encoder;
    SubGhzBlockGeneric          generic;

    HondaFrameData frame;
    uint8_t        active_button;
} SubGhzProtocolEncoderHonda;

const SubGhzProtocolDecoder subghz_protocol_honda_decoder;
const SubGhzProtocolEncoder subghz_protocol_honda_encoder;
const SubGhzProtocol        subghz_protocol_honda;

/* ============================================================================
 * Duration classifier for OOK-captured FSK Manchester
 * TE_SHORT = 63µs (one half-bit), TE_LONG = 126µs (two fused half-bits)
 * ==========================================================================*/
static uint8_t _classify_duration(uint32_t abs_dur) {
    /* Use addition on the lower bound to avoid unsigned underflow if
     * TE_DELTA >= TE_SHORT.  Equivalent to abs_dur in [TE-DELTA, TE+DELTA]. */
    if(abs_dur + HONDA_TE_DELTA >= HONDA_TE_SHORT &&
       abs_dur <= HONDA_TE_SHORT + HONDA_TE_DELTA) return 1;
    if(abs_dur + HONDA_TE_DELTA >= HONDA_TE_LONG &&
       abs_dur <= HONDA_TE_LONG  + HONDA_TE_DELTA) return 2;
    return 0;
}

/* ============================================================================
 * Manchester decoder — REWRITTEN to fix all bugs
 *
 * Fixes applied:
 *   1. No more i++ on violations — terminates the frame
 *   2. Gap detection between repeated frames within a transmission
 *   3. Frame size = 112 bits (14 bytes), not 64
 *   4. Checksum tolerance = 0 (exact match)
 *   5. MSB-first bit order (confirmed from firmware)
 *   6. Manchester polarity: bit 1 = LOW->HIGH (rising), bit 0 = HIGH->LOW (falling)
 *      This matches the firmware FSK_Mode where GPIO clear=LOW first for bit 1.
 * ==========================================================================*/
static bool _honda_manchester_decode(
    const uint8_t* hb, uint16_t hb_count,
    uint8_t* decoded, uint8_t* out_bit_count,
    uint8_t max_bits, bool invert) {

    uint8_t bit_count = 0;
    memset(decoded, 0, (max_bits + 7u) / 8u);

    uint16_t i = 0;

    /* Skip to data start: find end of alternating preamble */
    uint16_t alt_run = 0;
    uint16_t preamble_end = 0;
    for(uint16_t j = 1; j < hb_count; j++) {
        if(hb[j] != hb[j - 1]) {
            alt_run++;
        } else {
            if(alt_run >= HONDA_MIN_PREAMBLE_COUNT) {
                preamble_end = j;
                break;
            }
            alt_run = 0;
        }
    }

    if(preamble_end == 0 && alt_run >= HONDA_MIN_PREAMBLE_COUNT) {
        /* All preamble, no data */
        *out_bit_count = 0;
        return false;
    }

    i = preamble_end;

    /* Skip any same-level run at transition point (sync alignment) */
    while(i + 1 < hb_count && hb[i] == hb[i + 1]) i++;

    /* Manchester decode: each data bit is 2 half-bits that MUST differ */
    while(i + 1 < hb_count && bit_count < max_bits) {
        uint8_t h0 = hb[i];
        uint8_t h1 = hb[i + 1];

        if(h0 == h1) {
            /* Manchester violation — frame ends here. */
            break;
        }

        /* Decode the bit */
        uint8_t bit_val;
        if(!invert) {
            /* Normal: bit 1 = LOW->HIGH (0,1), bit 0 = HIGH->LOW (1,0) */
            bit_val = (h0 == 0 && h1 == 1) ? 1u : 0u;
        } else {
            /* Inverted: bit 1 = HIGH->LOW (1,0), bit 0 = LOW->HIGH (0,1) */
            bit_val = (h0 == 1 && h1 == 0) ? 1u : 0u;
        }

        /* Store MSB-first */
        uint8_t byte_idx = bit_count / 8u;
        uint8_t bit_idx  = 7u - (bit_count % 8u);
        if(bit_val)
            decoded[byte_idx] |= (uint8_t)(1u << bit_idx);

        bit_count++;
        i += 2;
    }

    *out_bit_count = bit_count;
    return (bit_count >= HONDA_MIN_BITS);
}

static bool _honda_try_decode_polarity(SubGhzProtocolDecoderHonda* inst, bool invert) {
    uint8_t decoded[16] = {0};
    uint8_t bit_count = 0;

    if(!_honda_manchester_decode(
        inst->half_bits, inst->hb_count,
        decoded, &bit_count, 112u, invert)) {
        return false;
    }

    FURI_LOG_D(
        TAG, "pol=%s bits=%u: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
        invert ? "INV" : "NOR", bit_count,
        decoded[0], decoded[1], decoded[2], decoded[3],
        decoded[4], decoded[5], decoded[6], decoded[7],
        decoded[8], decoded[9], decoded[10], decoded[11],
        decoded[12], decoded[13]);

    /* ---- Try Type-A: full 14-byte frame ---- */
    if(bit_count >= 72u) {
        HondaFrameData f;
        _honda_from_buf(decoded, &f, false);

        if(f.button > 0 && f.button <= HONDA_BTN_LOCK2PRESS &&
           f.serial != 0 && f.serial != 0x0FFFFFFFu) {

            uint8_t xor_check = _honda_xor_checksum(decoded, 8);
            if(xor_check == decoded[8]) {
                inst->frame = f;
                inst->frame_valid = true;
                FURI_LOG_I(
                    TAG, "DECODED TypeA pol=%s btn=%u ser=%07lX cnt=%06lX mode=%X csum=%02X",
                    invert ? "INV" : "NOR",
                    f.button, (unsigned long)f.serial, (unsigned long)f.counter,
                    f.mode, f.checksum);
                return true;
            }

            /* Accept rolling-code frames without strict XOR checksum */
            inst->frame = f;
            inst->frame_valid = true;
            FURI_LOG_I(
                TAG, "DECODED TypeA (rolling) pol=%s btn=%u ser=%07lX cnt=%06lX mode=%X",
                invert ? "INV" : "NOR",
                f.button, (unsigned long)f.serial, (unsigned long)f.counter,
                f.mode);
            return true;
        }
    }

    /* ---- Try Type-B ---- */
    if(bit_count >= 72u) {
        HondaFrameData f;
        _honda_from_buf(decoded, &f, true);

        if(f.button <= HONDA_BTN_LOCK2PRESS && f.button > 0 &&
           f.serial != 0 && f.serial != 0x0FFFFFFFu) {

            uint8_t xor_check = _honda_xor_checksum(decoded, 8);
            if(xor_check == decoded[8] ||
               popcount8(xor_check ^ decoded[8]) <= 1) {
                inst->frame = f;
                inst->frame_valid = true;
                FURI_LOG_I(
                    TAG, "DECODED TypeB pol=%s hdr=%u btn=%u ser=%07lX cnt=%06lX",
                    invert ? "INV" : "NOR",
                    f.type_b_header, f.button,
                    (unsigned long)f.serial, (unsigned long)f.counter);
                return true;
            }
        }
    }

    /* ---- Fallback: 64-bit minimal decode ---- */
    if(bit_count >= 64u && bit_count < 72u) {
        uint8_t  btn     = (uint8_t)_bits_get(decoded, 0, 4);
        uint32_t serial  = _bits_get(decoded, 4, 28);
        uint32_t counter = _bits_get(decoded, 32, 24);
        uint8_t  csum    = (uint8_t)_bits_get(decoded, 56, 8);

        if(btn > 0 && btn <= HONDA_BTN_LOCK2PRESS &&
           serial != 0 && serial != 0x0FFFFFFFu) {
            inst->frame.type_b        = false;
            inst->frame.type_b_header = 0;
            inst->frame.button        = btn;
            inst->frame.serial        = serial;
            inst->frame.counter       = counter;
            inst->frame.checksum      = csum;
            inst->frame.mode          = 0x2u;
            memset(inst->frame.extra, 0, sizeof(inst->frame.extra));
            inst->frame_valid         = true;
            FURI_LOG_I(
                TAG, "DECODED 64bit pol=%s btn=%u ser=%07lX cnt=%06lX",
                invert ? "INV" : "NOR",
                btn, (unsigned long)serial, (unsigned long)counter);
            return true;
        }
    }

    return false;
}

static bool _honda_try_decode(SubGhzProtocolDecoderHonda* inst) {
    if(inst->hb_count < 40u) return false;
    if(_honda_try_decode_polarity(inst, false)) return true;
    if(_honda_try_decode_polarity(inst, true))  return true;
    return false;
}

/* ============================================================================
 * Encoder — build Manchester upload buffer
 * ==========================================================================*/
static void _honda_build_upload(SubGhzProtocolEncoderHonda* inst) {
    LevelDuration* buf = inst->encoder.upload;
    size_t idx = 0;

    buf[idx++] = level_duration_make(false, HONDA_GUARD_TIME_US);

    for(uint16_t p = 0; p < (uint16_t)(HONDA_PREAMBLE_CYCLES * 2u); p++) {
        buf[idx++] = level_duration_make((p & 1u) != 0u, HONDA_TE_SHORT);
        if(idx >= HONDA_ENC_BUF_SIZE - 10) break;
    }

    uint8_t frame_buf[14];
    _honda_to_buf(&inst->frame, frame_buf);
    frame_buf[0] = (uint8_t)((inst->active_button << 4) |
                             (frame_buf[0] & 0x0Fu));

    for(uint8_t b = 0; b < HONDA_FRAME_BITS && idx < HONDA_ENC_BUF_SIZE - 4; b++) {
        uint8_t byte_idx = b / 8u;
        uint8_t bit_idx  = 7u - (b % 8u);
        uint8_t bit      = (frame_buf[byte_idx] >> bit_idx) & 1u;
        if(bit) {
            buf[idx++] = level_duration_make(false, HONDA_TE_SHORT);
            buf[idx++] = level_duration_make(true,  HONDA_TE_SHORT);
        } else {
            buf[idx++] = level_duration_make(true,  HONDA_TE_SHORT);
            buf[idx++] = level_duration_make(false, HONDA_TE_SHORT);
        }
    }

    buf[idx++] = level_duration_make(false, HONDA_GUARD_TIME_US);

    inst->encoder.size_upload = idx;
    inst->encoder.front       = 0;
}

/* ============================================================================
 * Protocol vtable definitions
 * ==========================================================================*/
const SubGhzProtocolDecoder subghz_protocol_honda_decoder = {
    .alloc         = subghz_protocol_decoder_honda_alloc,
    .free          = subghz_protocol_decoder_honda_free,
    .feed          = subghz_protocol_decoder_honda_feed,
    .reset         = subghz_protocol_decoder_honda_reset,
    .get_hash_data = subghz_protocol_decoder_honda_get_hash_data,
    .serialize     = subghz_protocol_decoder_honda_serialize,
    .deserialize   = subghz_protocol_decoder_honda_deserialize,
    .get_string    = subghz_protocol_decoder_honda_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_honda_encoder = {
    .alloc       = subghz_protocol_encoder_honda_alloc,
    .free        = subghz_protocol_encoder_honda_free,
    .deserialize = subghz_protocol_encoder_honda_deserialize,
    .stop        = subghz_protocol_encoder_honda_stop,
    .yield       = subghz_protocol_encoder_honda_yield,
};

const SubGhzProtocol subghz_protocol_honda = {
    .name    = SUBGHZ_PROTOCOL_HONDA_NAME,
    .type    = SubGhzProtocolTypeDynamic,
    .flag    = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 |
               SubGhzProtocolFlag_AM  | SubGhzProtocolFlag_Decodable |
               SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_honda_decoder,
    .encoder = &subghz_protocol_honda_encoder,
};

/* ============================================================================
 * Custom button helpers
 * ==========================================================================*/
uint8_t subghz_protocol_honda_btn_to_custom(uint8_t btn) {
    switch(btn) {
    case HONDA_BTN_LOCK:       return 1;
    case HONDA_BTN_UNLOCK:     return 2;
    case HONDA_BTN_TRUNK:      return 3;
    case HONDA_BTN_PANIC:      return 4;
    case HONDA_BTN_RSTART:     return 5;
    default:                   return 1;
    }
}

uint8_t subghz_protocol_honda_custom_to_btn(uint8_t custom) {
    switch(custom) {
    case 1: return HONDA_BTN_LOCK;
    case 2: return HONDA_BTN_UNLOCK;
    case 3: return HONDA_BTN_TRUNK;
    case 4: return HONDA_BTN_PANIC;
    case 5: return HONDA_BTN_RSTART;
    default: return HONDA_BTN_LOCK;
    }
}

/* ============================================================================
 * Decoder — alloc / free / reset
 * ==========================================================================*/
void* subghz_protocol_decoder_honda_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderHonda* inst = malloc(sizeof(SubGhzProtocolDecoderHonda));
    furi_check(inst);
    memset(inst, 0, sizeof(SubGhzProtocolDecoderHonda));
    inst->base.protocol         = &subghz_protocol_honda;
    inst->generic.protocol_name = inst->base.protocol->name;
    inst->frame_valid           = false;
    FURI_LOG_I(TAG, "decoder allocated");
    return inst;
}

void subghz_protocol_decoder_honda_free(void* context) {
    furi_assert(context);
    free(context);
}

void subghz_protocol_decoder_honda_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderHonda* inst = context;
    inst->decoder.parser_step = HondaDecoderStepReset;
    inst->decoder.te_last     = 0;
    inst->hb_count            = 0;
}

/* ============================================================================
 * Decoder — feed
 * ==========================================================================*/
void subghz_protocol_decoder_honda_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderHonda* inst = context;
    uint8_t lvl       = level ? 1u : 0u;
    uint8_t dur_class = _classify_duration(duration);

    if(dur_class > 0) {
        if(dur_class == 1) {
            if(inst->hb_count < HONDA_HALF_BIT_BUF)
                inst->half_bits[inst->hb_count++] = lvl;
        } else {
            if(inst->hb_count + 2u <= HONDA_HALF_BIT_BUF) {
                inst->half_bits[inst->hb_count++] = lvl;
                inst->half_bits[inst->hb_count++] = lvl;
            }
        }
    } else {
        if(inst->hb_count >= (HONDA_MIN_PREAMBLE_COUNT + 16u)) {
            if(_honda_try_decode(inst)) {
                inst->generic.data = _honda_pack(&inst->frame);
                inst->generic.data_count_bit = HONDA_FRAME_BITS_CORE;
                inst->generic.serial = inst->frame.serial;
                inst->generic.btn    = inst->frame.button;
                inst->generic.cnt    = inst->frame.counter;
                FURI_LOG_I(
                    TAG, "FRAME btn=%u ser=%07lX cnt=%06lX mode=%X",
                    inst->frame.button,
                    (unsigned long)inst->frame.serial,
                    (unsigned long)inst->frame.counter,
                    inst->frame.mode);

                uint8_t custom = subghz_protocol_honda_btn_to_custom(inst->frame.button);
                if(subghz_custom_btn_get_original() == 0)
                    subghz_custom_btn_set_original(custom);
                subghz_custom_btn_set_max(HONDA_CUSTOM_BTN_MAX);

                if(inst->base.callback)
                    inst->base.callback(&inst->base, inst->base.context);
            }
        }
        inst->hb_count = 0;
    }
    inst->decoder.te_last = duration;
}

uint8_t subghz_protocol_decoder_honda_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderHonda* inst = context;
    return (uint8_t)(inst->generic.data        ^
                    (inst->generic.data >> 8)   ^
                    (inst->generic.data >> 16)  ^
                    (inst->generic.data >> 24)  ^
                    (inst->generic.data >> 32));
}

SubGhzProtocolStatus subghz_protocol_decoder_honda_serialize(
    void* context, FlipperFormat* flipper_format, SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderHonda* inst = context;
    return subghz_block_generic_serialize(&inst->generic, flipper_format, preset);
}

SubGhzProtocolStatus subghz_protocol_decoder_honda_deserialize(
    void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderHonda* inst = context;
    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &inst->generic, flipper_format,
        subghz_protocol_honda_const.min_count_bit_for_found);
    if(ret == SubGhzProtocolStatusOk) {
        _honda_unpack(inst->generic.data, &inst->frame);
        inst->frame_valid      = true;
        inst->generic.serial   = inst->frame.serial;
        inst->generic.btn      = inst->frame.button;
        inst->generic.cnt      = inst->frame.counter;

        uint8_t custom = subghz_protocol_honda_btn_to_custom(inst->frame.button);
        if(subghz_custom_btn_get_original() == 0)
            subghz_custom_btn_set_original(custom);
        subghz_custom_btn_set_max(HONDA_CUSTOM_BTN_MAX);

        FURI_LOG_I(
            TAG, "deserialize: btn=%u ser=%07lX cnt=%06lX",
            inst->frame.button,
            (unsigned long)inst->frame.serial,
            (unsigned long)inst->frame.counter);
    }
    return ret;
}

void subghz_protocol_decoder_honda_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderHonda* inst = context;

    if(!inst->frame_valid && inst->generic.data != 0) {
        _honda_unpack(inst->generic.data, &inst->frame);
        inst->frame_valid = true;
    }

    const char* btn_name;
    switch(inst->frame.button) {
    case HONDA_BTN_LOCK:       btn_name = "Lock";         break;
    case HONDA_BTN_UNLOCK:     btn_name = "Unlock";       break;
    case HONDA_BTN_TRUNK:      btn_name = "Trunk/Hatch";  break;
    case HONDA_BTN_PANIC:      btn_name = "Panic";        break;
    case HONDA_BTN_RSTART:     btn_name = "Remote Start"; break;
    case HONDA_BTN_LOCK2PRESS: btn_name = "Lock x2";      break;
    default:                   btn_name = "Unknown";      break;
    }

    furi_string_cat_printf(
        output,
        "%s %s %ubit\r\n"
        "Btn:%s (0x%X)\r\n"
        "Ser:%07lX\r\n"
        "Cnt:%06lX Chk:%02X Mode:%X\r\n",
        inst->generic.protocol_name,
        inst->frame.type_b ? "TB" : "TA",
        inst->generic.data_count_bit,
        btn_name,
        inst->frame.button,
        (unsigned long)inst->frame.serial,
        (unsigned long)inst->frame.counter,
        inst->frame.checksum,
        inst->frame.mode);
}

/* ============================================================================
 * Encoder — alloc / free / stop / yield
 * ==========================================================================*/
void* subghz_protocol_encoder_honda_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderHonda* inst = malloc(sizeof(SubGhzProtocolEncoderHonda));
    furi_check(inst);
    memset(inst, 0, sizeof(SubGhzProtocolEncoderHonda));
    inst->base.protocol         = &subghz_protocol_honda;
    inst->generic.protocol_name = inst->base.protocol->name;
    inst->encoder.repeat        = 3;
    inst->encoder.size_upload   = 0;
    inst->encoder.upload        = malloc(HONDA_ENC_BUF_SIZE * sizeof(LevelDuration));
    furi_check(inst->encoder.upload);
    inst->encoder.is_running = false;
    inst->encoder.front      = 0;
    return inst;
}

void subghz_protocol_encoder_honda_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderHonda* inst = context;
    free(inst->encoder.upload);
    free(inst);
}

void subghz_protocol_encoder_honda_stop(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderHonda* inst = context;
    inst->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_honda_yield(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderHonda* inst = context;
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

SubGhzProtocolStatus subghz_protocol_encoder_honda_deserialize(
    void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderHonda* inst = context;
    SubGhzProtocolStatus ret = subghz_block_generic_deserialize(&inst->generic, flipper_format);
    if(ret != SubGhzProtocolStatusOk) return ret;

    _honda_unpack(inst->generic.data, &inst->frame);

    uint8_t custom = subghz_protocol_honda_btn_to_custom(inst->frame.button);
    if(subghz_custom_btn_get_original() == 0)
        subghz_custom_btn_set_original(custom);
    subghz_custom_btn_set_max(HONDA_CUSTOM_BTN_MAX);

    uint8_t active_custom = subghz_custom_btn_get();
    inst->active_button = (active_custom == SUBGHZ_CUSTOM_BTN_OK)
        ? subghz_protocol_honda_custom_to_btn(subghz_custom_btn_get_original())
        : subghz_protocol_honda_custom_to_btn(active_custom);

    inst->frame.counter = (inst->frame.counter +
        furi_hal_subghz_get_rolling_counter_mult()) & 0x00FFFFFFu;
    _honda_counter_increment(&inst->frame);

    inst->frame.button = inst->active_button;

    inst->generic.data = _honda_pack(&inst->frame);
    inst->generic.cnt  = inst->frame.counter;
    inst->generic.btn  = inst->active_button;

    flipper_format_rewind(flipper_format);
    uint8_t key_data[8];
    for(int i = 0; i < 8; i++)
        key_data[i] = (uint8_t)(inst->generic.data >> (56 - i * 8));
    flipper_format_update_hex(flipper_format, "Key", key_data, 8);

    _honda_build_upload(inst);
    inst->encoder.is_running = true;
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_encoder_honda_set_button(void* context, uint8_t btn) {
    furi_assert(context);
    SubGhzProtocolEncoderHonda* inst = context;
    inst->active_button      = btn & 0x0Fu;
    inst->encoder.is_running = false;
    _honda_counter_increment(&inst->frame);
    inst->generic.data    = _honda_pack(&inst->frame);
    inst->generic.cnt     = inst->frame.counter;
    _honda_build_upload(inst);
    inst->encoder.repeat     = 3;
    inst->encoder.is_running = true;
}
