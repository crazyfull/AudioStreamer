// minimal stand-in for opus.h (real libopus.so.0 is linked). Values match libopus.
#pragma once
#include <stdint.h>
typedef int32_t opus_int32; typedef int16_t opus_int16;
typedef struct OpusEncoder OpusEncoder; typedef struct OpusDecoder OpusDecoder;
#define OPUS_OK 0
#define OPUS_APPLICATION_VOIP 2048
#define OPUS_SIGNAL_VOICE 3001
#define OPUS_SET_BITRATE(x) 4002, (opus_int32)(x)
#define OPUS_SET_MAX_BANDWIDTH(x) 4004, (opus_int32)(x)
#define OPUS_SET_VBR(x) 4006, (opus_int32)(x)
#define OPUS_SET_COMPLEXITY(x) 4010, (opus_int32)(x)
#define OPUS_SET_INBAND_FEC(x) 4012, (opus_int32)(x)
#define OPUS_SET_PACKET_LOSS_PERC(x) 4014, (opus_int32)(x)
#define OPUS_SET_DTX(x) 4016, (opus_int32)(x)
#define OPUS_SET_VBR_CONSTRAINT(x) 4020, (opus_int32)(x)
#define OPUS_SET_SIGNAL(x) 4024, (opus_int32)(x)
#define OPUS_SET_LSB_DEPTH(x) 4036, (opus_int32)(x)
extern "C" {
OpusEncoder *opus_encoder_create(opus_int32 Fs, int channels, int application, int *error);
void opus_encoder_destroy(OpusEncoder *st);
opus_int32 opus_encode(OpusEncoder *st, const opus_int16 *pcm, int frame_size, unsigned char *data, opus_int32 max_data_bytes);
int opus_encoder_ctl(OpusEncoder *st, int request, ...);
OpusDecoder *opus_decoder_create(opus_int32 Fs, int channels, int *error);
void opus_decoder_destroy(OpusDecoder *st);
int opus_decode(OpusDecoder *st, const unsigned char *data, opus_int32 len, opus_int16 *pcm, int frame_size, int decode_fec);
const char *opus_strerror(int error);
}
