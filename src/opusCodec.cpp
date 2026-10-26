#include "opusCodec.h"

namespace {
// 5760 = 120ms @ 48kHz, the biggest possible Opus packet (samples per channel)
const int MAX_SAMPLES = 5760;

bool ctlOk(int ret, const char *what)
{
    if(ret == OPUS_OK)
        return true;
    AUDIO_WARN("opus_encoder_ctl(%s) failed: %s", what, opus_strerror(ret));
    return false;
}
}

opusCodec::opusCodec()
{
    m_pEncoder = nullptr;
    m_pDecoder = nullptr;
    m_encChannels = 1;
    m_decChannels = 1;
    m_frameSamples = 0;
}

opusCodec::~opusCodec()
{
    if(m_pEncoder)
        opus_encoder_destroy(m_pEncoder);
    if(m_pDecoder)
        opus_decoder_destroy(m_pDecoder);
}

bool opusCodec::InitEncoder(const AudioConfig &cfg)
{
    if(m_pEncoder)
        return true;

    std::string why;
    if(!cfg.validate(&why)){
        AUDIO_WARN("%s", why.c_str());
        return false;
    }

    int err = OPUS_OK;
    OpusEncoder *enc = opus_encoder_create(cfg.inputRate, cfg.channels, OPUS_APPLICATION_VOIP, &err);
    if(err != OPUS_OK || !enc){
        AUDIO_WARN("opus_encoder_create failed: %s", opus_strerror(err));
        return false;
    }

    // ---- most compressed voice profile ---- every setting is checked: a rejected one means a wrong config
    bool ok = true;
    ok = ctlOk(opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE)), "SIGNAL") && ok;
    ok = ctlOk(opus_encoder_ctl(enc, OPUS_SET_BITRATE(cfg.opusBitrate)), "BITRATE") && ok;
    ok = ctlOk(opus_encoder_ctl(enc, OPUS_SET_VBR(1)), "VBR") && ok;
    ok = ctlOk(opus_encoder_ctl(enc, OPUS_SET_VBR_CONSTRAINT(0)), "VBR_CONSTRAINT") && ok;
    ok = ctlOk(opus_encoder_ctl(enc, OPUS_SET_MAX_BANDWIDTH(cfg.MaxBandwith)), "MAX_BANDWIDTH") && ok;
    ok = ctlOk(opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(10)), "COMPLEXITY") && ok;
    ok = ctlOk(opus_encoder_ctl(enc, OPUS_SET_LSB_DEPTH(16)), "LSB_DEPTH") && ok;
    ok = ctlOk(opus_encoder_ctl(enc, OPUS_SET_DTX(cfg.useDtx ? 1 : 0)), "DTX") && ok;
    ok = ctlOk(opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(cfg.useFec ? 1 : 0)), "INBAND_FEC") && ok;
    ok = ctlOk(opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(cfg.useFec ? cfg.expectedLossPerc : 0)), "PACKET_LOSS_PERC") && ok;
    if(!ok){
        opus_encoder_destroy(enc);
        return false;
    }

    m_pEncoder = enc;
    m_encChannels = cfg.channels;
    m_frameSamples = cfg.frameSamples();
    return true;
}

bool opusCodec::InitDecoder(int sampleRate, int channels)
{
    if(m_pDecoder)
        return true;

    int err = OPUS_OK;
    m_pDecoder = opus_decoder_create(sampleRate, channels, &err);
    if(err != OPUS_OK || !m_pDecoder){
        AUDIO_WARN("opus_decoder_create failed: %s", opus_strerror(err));
        m_pDecoder = nullptr;
        return false;
    }

    m_decChannels = channels;
    m_decScratch.assign(size_t(MAX_SAMPLES) * channels, 0);     // allocated once, here
    return true;
}

bool opusCodec::Encode(const int16_t *pcm, int frameSamples, Bytes &out)
{
    out.clear();
    if(!m_pEncoder || !pcm)
        return false;

    if(frameSamples != m_frameSamples){
        AUDIO_WARN("opus Encode: wrong frame size %d expected %d", frameSamples, m_frameSamples);
        return false;
    }

    opus_int32 n = opus_encode(m_pEncoder, pcm, frameSamples, m_encBuf, sizeof(m_encBuf));
    if(n < 0){
        AUDIO_WARN("opus_encode failed: %s", opus_strerror(n));
        return false;
    }

    out.assign(m_encBuf, m_encBuf + n);
    return true;
}

Bytes opusCodec::Encode(const int16_t *pcm, int frameSamples)
{
    Bytes out;
    Encode(pcm, frameSamples, out);
    return out;
}

bool opusCodec::Decode(const Bytes &packet, Pcm &out)
{
    out.clear();
    if(!m_pDecoder || packet.empty())
        return false;

    int samples = opus_decode(m_pDecoder, packet.data(), int(packet.size()), m_decScratch.data(), MAX_SAMPLES, 0);
    if(samples < 0){
        AUDIO_WARN("opus_decode failed: %s", opus_strerror(samples));
        return false;
    }

    out.assign(m_decScratch.begin(), m_decScratch.begin() + size_t(samples) * m_decChannels);
    return true;
}

Pcm opusCodec::Decode(const Bytes &packet)
{
    Pcm out;
    Decode(packet, out);
    return out;
}

bool opusCodec::DecodeFec(const Bytes &nextPacket, int frameSamples, Pcm &out)
{
    out.clear();
    if(!m_pDecoder || nextPacket.empty() || frameSamples <= 0 || frameSamples > MAX_SAMPLES)
        return false;

    int samples = opus_decode(m_pDecoder, nextPacket.data(), int(nextPacket.size()), m_decScratch.data(), frameSamples, 1);
    if(samples < 0){
        AUDIO_WARN("opus_decode(fec) failed: %s", opus_strerror(samples));
        return DecodePlc(frameSamples, out);
    }

    out.assign(m_decScratch.begin(), m_decScratch.begin() + size_t(samples) * m_decChannels);
    return true;
}

Pcm opusCodec::DecodeFec(const Bytes &nextPacket, int frameSamples)
{
    Pcm out;
    DecodeFec(nextPacket, frameSamples, out);
    return out;
}

bool opusCodec::DecodePlc(int frameSamples, Pcm &out)
{
    out.clear();
    if(!m_pDecoder || frameSamples <= 0 || frameSamples > MAX_SAMPLES)
        return false;

    int samples = opus_decode(m_pDecoder, nullptr, 0, m_decScratch.data(), frameSamples, 0);
    if(samples < 0){
        AUDIO_WARN("opus_decode(plc) failed: %s", opus_strerror(samples));
        return false;
    }

    out.assign(m_decScratch.begin(), m_decScratch.begin() + size_t(samples) * m_decChannels);
    return true;
}

Pcm opusCodec::DecodePlc(int frameSamples)
{
    Pcm out;
    DecodePlc(frameSamples, out);
    return out;
}
