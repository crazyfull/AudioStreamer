/*
 * RavAva - real-time voice library (Opus over any datagram transport)
 * Copyright (c) 2026 Mojtaba Kahvand. All rights reserved.
 * Author: Mojtaba Kahvand
 *
 * Proprietary software, NOT free / open source. Use, copying, modification and distribution are forbidden without a
 * written license from the author. See the LICENSE file in the repository root.
 * SPDX-License-Identifier: LicenseRef-RavAva-Proprietary
 */
#include "OpusCodec.h"

namespace ravava {

namespace {
// 5760 = 120ms @ 48kHz, the biggest possible Opus packet (samples per channel)
const int MAX_SAMPLES = 5760;

bool ctlOk(int ret, const char *what)
{
    if(ret == OPUS_OK)
        return true;
    RAVAVA_WARN("opus_encoder_ctl(%s) failed: %s", what, opus_strerror(ret));
    return false;
}
}

bool OpusCodec::Inspect(const uint8_t *opusPacket, size_t size, PacketInfo &info)
{
    if(!opusPacket || size < 1 || size > 1500)
        return false;
    const int frames = opus_packet_get_nb_frames(opusPacket, opus_int32(size));
    const int perFrame = opus_packet_get_samples_per_frame(opusPacket, 48000);      // at 48 kHz: 1 ms = 48 samples
    if(frames < 1 || perFrame < 1)
        return false;
    const int samples = frames * perFrame;
    if(samples % 48 != 0 || samples / 48 < 5 || samples > MAX_SAMPLES)
        return false;
    info.frameMs = samples / 48;
    info.channels = opus_packet_get_nb_channels(opusPacket);
    info.bandwidth = opus_packet_get_bandwidth(opusPacket);
    return info.channels >= 1;
}

OpusCodec::OpusCodec()
{
    m_pEncoder = nullptr;
    m_pDecoder = nullptr;
    m_encChannels = 1;
    m_decChannels = 1;
    m_frameSamples = 0;
}

OpusCodec::~OpusCodec()
{
    if(m_pEncoder)
        opus_encoder_destroy(m_pEncoder);
    if(m_pDecoder)
        opus_decoder_destroy(m_pDecoder);
#ifdef RAVAVA_ENABLE_DRED
    if(m_pDred)
        opus_dred_free(m_pDred);
    if(m_pDredDec)
        opus_dred_decoder_destroy(m_pDredDec);
#endif
}

bool OpusCodec::InitEncoder(const AudioConfig &cfg)
{
    if(m_pEncoder)
        return true;

    std::string why;
    if(!cfg.Validate(&why)){
        RAVAVA_WARN("%s", why.c_str());
        return false;
    }

    int err = OPUS_OK;
    OpusEncoder *enc = opus_encoder_create(cfg.inputRate, cfg.channels, cfg.musicMode ? OPUS_APPLICATION_AUDIO : OPUS_APPLICATION_VOIP, &err);
    if(err != OPUS_OK || !enc){
        RAVAVA_WARN("opus_encoder_create failed: %s", opus_strerror(err));
        return false;
    }

    // ---- most compressed profile (speech, or music with cfg.musicMode) ---- every setting is checked: a rejected one means a wrong config
    bool ok = true;
    ok = ctlOk(opus_encoder_ctl(enc, OPUS_SET_SIGNAL(cfg.musicMode ? OPUS_SIGNAL_MUSIC : OPUS_SIGNAL_VOICE)), "SIGNAL") && ok;
    ok = ctlOk(opus_encoder_ctl(enc, OPUS_SET_BITRATE(cfg.Bitrate)), "BITRATE") && ok;
    ok = ctlOk(opus_encoder_ctl(enc, OPUS_SET_VBR(1)), "VBR") && ok;
    ok = ctlOk(opus_encoder_ctl(enc, OPUS_SET_VBR_CONSTRAINT(0)), "VBR_CONSTRAINT") && ok;
    ok = ctlOk(opus_encoder_ctl(enc, OPUS_SET_MAX_BANDWIDTH(cfg.maxBandwidth)), "MAX_BANDWIDTH") && ok;
    ok = ctlOk(opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(10)), "COMPLEXITY") && ok;
    ok = ctlOk(opus_encoder_ctl(enc, OPUS_SET_LSB_DEPTH(16)), "LSB_DEPTH") && ok;
    ok = ctlOk(opus_encoder_ctl(enc, OPUS_SET_DTX(cfg.useDtx ? 1 : 0)), "DTX") && ok;
    ok = ctlOk(opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(cfg.useFec ? 1 : 0)), "INBAND_FEC") && ok;
    ok = ctlOk(opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(cfg.useFec ? cfg.expectedLossPerc : 0)), "PACKET_LOSS_PERC") && ok;
    if(!ok){
        opus_encoder_destroy(enc);
        return false;
    }
#ifdef RAVAVA_ENABLE_DRED
    // not fatal: a libopus built without DRED answers OPUS_UNIMPLEMENTED and the stream is simply sent without it
    if(cfg.dredMs > 0)
        ctlOk(opus_encoder_ctl(enc, OPUS_SET_DRED_DURATION(cfg.dredMs / 10)), "DRED_DURATION");
#endif

    m_pEncoder = enc;
    m_lsbDepth = 16;
    m_encChannels = cfg.channels;
    m_frameSamples = cfg.FrameSamples();

    RAVAVA_INFO("opus_version: [%s]", opus_get_version_string());
    return true;
}

bool OpusCodec::InitDecoder(int sampleRate, int channels)
{
    if(m_pDecoder)
        return true;

    int err = OPUS_OK;
    m_pDecoder = opus_decoder_create(sampleRate, channels, &err);
    if(err != OPUS_OK || !m_pDecoder){
        RAVAVA_WARN("opus_decoder_create failed: %s", opus_strerror(err));
        m_pDecoder = nullptr;
        return false;
    }

    m_decChannels = channels;
    m_decRate = sampleRate;
    m_decScratch.assign(size_t(MAX_SAMPLES) * channels, 0);     // allocated once, here
    m_decScratchF.assign(size_t(MAX_SAMPLES) * channels, 0.0f);
    return true;
}

bool OpusCodec::GetEncoderMode(int &application, int &signal) const
{
    if(!m_pEncoder)
        return false;
    opus_int32 app = 0, sig = 0;
    if(opus_encoder_ctl(m_pEncoder, OPUS_GET_APPLICATION(&app)) != OPUS_OK ||
       opus_encoder_ctl(m_pEncoder, OPUS_GET_SIGNAL(&sig)) != OPUS_OK)
        return false;
    application = int(app);
    signal = int(sig);
    return true;
}

// the encoder's idea of the input precision (it only matters for DTX / noise shaping decisions): follows the input type
static void setLsbDepth(OpusEncoder *enc, int &current, int depth)
{
    if(current == depth)
        return;
    if(opus_encoder_ctl(enc, OPUS_SET_LSB_DEPTH(depth)) == OPUS_OK)
        current = depth;
}

bool OpusCodec::Encode(const int16_t *pcm, int frameSamples, Bytes &out)
{
    out.clear();
    if(!m_pEncoder || !pcm)
        return false;

    if(frameSamples != m_frameSamples){
        RAVAVA_WARN("opus Encode: wrong frame size %d expected %d", frameSamples, m_frameSamples);
        return false;
    }

    setLsbDepth(m_pEncoder, m_lsbDepth, 16);
    opus_int32 n = opus_encode(m_pEncoder, pcm, frameSamples, m_encBuf, sizeof(m_encBuf));
    if(n < 0){
        RAVAVA_WARN("opus_encode failed: %s", opus_strerror(n));
        return false;
    }

    out.assign(m_encBuf, m_encBuf + n);
    return true;
}

bool OpusCodec::EncodeFloat(const float *pcm, int frameSamples, Bytes &out)
{
    out.clear();
    if(!m_pEncoder || !pcm)
        return false;
    if(frameSamples != m_frameSamples){
        RAVAVA_WARN("opus EncodeFloat: wrong frame size %d expected %d", frameSamples, m_frameSamples);
        return false;
    }
    setLsbDepth(m_pEncoder, m_lsbDepth, 24);
    const opus_int32 n = opus_encode_float(m_pEncoder, pcm, frameSamples, m_encBuf, sizeof(m_encBuf));
    if(n < 0){
        RAVAVA_WARN("opus_encode_float failed: %s", opus_strerror(n));
        return false;
    }
    out.assign(m_encBuf, m_encBuf + n);
    return true;
}

bool OpusCodec::DecodeFloat(const uint8_t *packet, size_t size, PcmF &out)
{
    out.clear();
    if(!m_pDecoder || !packet || !size)
        return false;
    const int samples = opus_decode_float(m_pDecoder, packet, int(size), m_decScratchF.data(), MAX_SAMPLES, 0);
    if(samples < 0){
        RAVAVA_WARN("opus_decode_float failed: %s", opus_strerror(samples));
        return false;
    }
    out.assign(m_decScratchF.begin(), m_decScratchF.begin() + size_t(samples) * m_decChannels);
    return true;
}

Bytes OpusCodec::Encode(const int16_t *pcm, int frameSamples)
{
    Bytes out;
    Encode(pcm, frameSamples, out);
    return out;
}

bool OpusCodec::Decode(const Bytes &packet, Pcm &out)
{
    return Decode(packet.data(), packet.size(), out);
}

bool OpusCodec::Decode(const uint8_t *packet, size_t size, Pcm &out)
{
    out.clear();
    if(!m_pDecoder || !packet || !size)
        return false;

    int samples = opus_decode(m_pDecoder, packet, int(size), m_decScratch.data(), MAX_SAMPLES, 0);
    if(samples < 0){
        RAVAVA_WARN("opus_decode failed: %s", opus_strerror(samples));
        return false;
    }

    out.assign(m_decScratch.begin(), m_decScratch.begin() + size_t(samples) * m_decChannels);
    return true;
}

Pcm OpusCodec::Decode(const Bytes &packet)
{
    Pcm out;
    Decode(packet, out);
    return out;
}

bool OpusCodec::DecodeFec(const Bytes &nextPacket, int frameSamples, Pcm &out)
{
    return DecodeFec(nextPacket.data(), nextPacket.size(), frameSamples, out);
}

bool OpusCodec::DecodeFec(const uint8_t *nextPacket, size_t size, int frameSamples, Pcm &out)
{
    out.clear();
    if(!m_pDecoder || !nextPacket || !size || frameSamples <= 0 || frameSamples > MAX_SAMPLES)
        return false;

    int samples = opus_decode(m_pDecoder, nextPacket, int(size), m_decScratch.data(), frameSamples, 1);
    if(samples < 0){
        RAVAVA_WARN("opus_decode(fec) failed: %s", opus_strerror(samples));
        return DecodePlc(frameSamples, out);
    }

    out.assign(m_decScratch.begin(), m_decScratch.begin() + size_t(samples) * m_decChannels);
    return true;
}

Pcm OpusCodec::DecodeFec(const Bytes &nextPacket, int frameSamples)
{
    Pcm out;
    DecodeFec(nextPacket, frameSamples, out);
    return out;
}

bool OpusCodec::DecodePlc(int frameSamples, Pcm &out)
{
    out.clear();
    if(!m_pDecoder || frameSamples <= 0 || frameSamples > MAX_SAMPLES)
        return false;

    int samples = opus_decode(m_pDecoder, nullptr, 0, m_decScratch.data(), frameSamples, 0);
    if(samples < 0){
        RAVAVA_WARN("opus_decode(plc) failed: %s", opus_strerror(samples));
        return false;
    }

    out.assign(m_decScratch.begin(), m_decScratch.begin() + size_t(samples) * m_decChannels);
    return true;
}

bool OpusCodec::DredCompiled()
{
#ifdef RAVAVA_ENABLE_DRED
    return true;
#else
    return false;
#endif
}

#ifdef RAVAVA_ENABLE_DRED
bool OpusCodec::InitDredDecoder()
{
    if(m_pDredDec)
        return true;
    int err = OPUS_OK;
    OpusDREDDecoder *d = opus_dred_decoder_create(&err);
    OpusDRED *s = err == OPUS_OK && d ? opus_dred_alloc(&err) : nullptr;
    if(err != OPUS_OK || !d || !s){
        RAVAVA_WARN("DRED decoder not available (libopus built without --enable-dred?): %s", opus_strerror(err));
        if(s) opus_dred_free(s);
        if(d) opus_dred_decoder_destroy(d);
        return false;
    }
    m_pDredDec = d;
    m_pDred = s;
    return true;
}

bool OpusCodec::DredReady() const
{
    return m_pDredDec && m_pDecoder;
}

int OpusCodec::DredParse(const uint8_t *packet, size_t size, int maxSamples)
{
    if(!DredReady() || !packet || !size || maxSamples <= 0)
        return 0;
    int dredEnd = 0;
    const int avail = opus_dred_parse(m_pDredDec, m_pDred, packet, opus_int32(size), maxSamples, m_decRate, &dredEnd, 0);
    return avail > 0 ? avail : 0;
}

bool OpusCodec::DecodeDred(int offsetSamples, int frameSamples, Pcm &out)
{
    out.clear();
    if(!DredReady() || frameSamples <= 0 || frameSamples > MAX_SAMPLES)
        return false;
    const int samples = opus_decoder_dred_decode(m_pDecoder, m_pDred, offsetSamples, m_decScratch.data(), frameSamples);
    if(samples <= 0)
        return false;
    out.assign(m_decScratch.begin(), m_decScratch.begin() + size_t(samples) * m_decChannels);
    return true;
}
#else
bool OpusCodec::InitDredDecoder() { return false; }
bool OpusCodec::DredReady() const { return false; }
int OpusCodec::DredParse(const uint8_t *, size_t, int) { return 0; }
bool OpusCodec::DecodeDred(int, int, Pcm &out) { out.clear(); return false; }
#endif

Pcm OpusCodec::DecodePlc(int frameSamples)
{
    Pcm out;
    DecodePlc(frameSamples, out);
    return out;
}

} // namespace ravava
