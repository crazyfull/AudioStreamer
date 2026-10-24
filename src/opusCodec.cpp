#include "opusCodec.h"

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

    int err = OPUS_OK;
    m_pEncoder = opus_encoder_create(cfg.inputRate, cfg.channels, OPUS_APPLICATION_VOIP, &err);
    if(err != OPUS_OK || !m_pEncoder){
        AUDIO_WARN("opus_encoder_create failed: %s", opus_strerror(err));
        m_pEncoder = nullptr;
        return false;
    }

    m_encChannels = cfg.channels;
    m_frameSamples = cfg.frameSamples();

    // ---- most compressed voice profile ----
    opus_encoder_ctl(m_pEncoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(m_pEncoder, OPUS_SET_BITRATE(cfg.opusBitrate));
    opus_encoder_ctl(m_pEncoder, OPUS_SET_VBR(1));
    opus_encoder_ctl(m_pEncoder, OPUS_SET_VBR_CONSTRAINT(0));
    opus_encoder_ctl(m_pEncoder, OPUS_SET_MAX_BANDWIDTH(cfg.MaxBandwith));
    opus_encoder_ctl(m_pEncoder, OPUS_SET_COMPLEXITY(10));
    opus_encoder_ctl(m_pEncoder, OPUS_SET_LSB_DEPTH(16));
    opus_encoder_ctl(m_pEncoder, OPUS_SET_DTX(cfg.useDtx ? 1 : 0));
    opus_encoder_ctl(m_pEncoder, OPUS_SET_INBAND_FEC(cfg.useFec ? 1 : 0));
    opus_encoder_ctl(m_pEncoder, OPUS_SET_PACKET_LOSS_PERC(cfg.useFec ? cfg.expectedLossPerc : 0));

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
    return true;
}

Bytes opusCodec::Encode(const int16_t *pcm, int frameSamples)
{
    if(!m_pEncoder || !pcm)
        return Bytes();

    if(frameSamples != m_frameSamples){
        AUDIO_WARN("opus Encode: wrong frame size %d expected %d", frameSamples, m_frameSamples);
        return Bytes();
    }

    unsigned char out[1500];
    opus_int32 n = opus_encode(m_pEncoder, pcm, frameSamples, out, sizeof(out));
    if(n < 0){
        AUDIO_WARN("opus_encode failed: %s", opus_strerror(n));
        return Bytes();
    }

    return Bytes(out, out + n);
}

Pcm opusCodec::Decode(const Bytes &packet)
{
    if(!m_pDecoder || packet.empty())
        return Pcm();

    // 5760 = 120ms @ 48kHz, the biggest possible Opus packet
    static const int MAX_SAMPLES = 5760;
    Pcm pcm(size_t(MAX_SAMPLES) * m_decChannels);

    int samples = opus_decode(m_pDecoder, packet.data(), int(packet.size()), pcm.data(), MAX_SAMPLES, 0);
    if(samples < 0){
        AUDIO_WARN("opus_decode failed: %s", opus_strerror(samples));
        return Pcm();
    }

    pcm.resize(size_t(samples) * m_decChannels);
    return pcm;
}

Pcm opusCodec::DecodeFec(const Bytes &nextPacket, int frameSamples)
{
    if(!m_pDecoder || nextPacket.empty())
        return Pcm();

    Pcm pcm(size_t(frameSamples) * m_decChannels);
    int samples = opus_decode(m_pDecoder, nextPacket.data(), int(nextPacket.size()), pcm.data(), frameSamples, 1);
    if(samples < 0){
        AUDIO_WARN("opus_decode(fec) failed: %s", opus_strerror(samples));
        return DecodePlc(frameSamples);
    }

    pcm.resize(size_t(samples) * m_decChannels);
    return pcm;
}

Pcm opusCodec::DecodePlc(int frameSamples)
{
    if(!m_pDecoder)
        return Pcm();

    Pcm pcm(size_t(frameSamples) * m_decChannels);
    int samples = opus_decode(m_pDecoder, nullptr, 0, pcm.data(), frameSamples, 0);
    if(samples < 0){
        AUDIO_WARN("opus_decode(plc) failed: %s", opus_strerror(samples));
        return Pcm();
    }

    pcm.resize(size_t(samples) * m_decChannels);
    return pcm;
}
