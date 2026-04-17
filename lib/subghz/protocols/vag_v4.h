#pragma once

#include "base.h"
#include "../blocks/math.h"

#define VAG_V4_PROTOCOL_NAME "VAG V4"

extern const SubGhzProtocol subghz_protocol_vag_v4;

void* subghz_protocol_decoder_vag_v4_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_vag_v4_free(void* context);
void subghz_protocol_decoder_vag_v4_reset(void* context);
void subghz_protocol_decoder_vag_v4_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_vag_v4_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_vag_v4_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_vag_v4_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_vag_v4_get_string(void* context, FuriString* output);

void* subghz_protocol_encoder_vag_v4_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_vag_v4_free(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_vag_v4_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_vag_v4_stop(void* context);
LevelDuration subghz_protocol_encoder_vag_v4_yield(void* context);
