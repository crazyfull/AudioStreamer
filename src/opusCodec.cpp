#include "opusCodec.h"

#include <QDebug>
#include <cstring>

/*
    NB (narrowband)             4 kHz 	8 kHz
    MB (medium-band)[note 1] 	6 kHz 	12 kHz
    WB (wideband)           	8 kHz 	16 kHz
    SWB (super-wideband)     	12 kHz 	24 kHz
    FB (fullband)[note 2]    	20 kHz 	48 kHz
*/

opusCodec::opusCodec()
{
    m_pEncoder = nullptr;
    m_pDecoder = nullptr;
    m_encChannels = 1;
    m_decChannels = 1;
    m_frameBytes = 0;
}

opusCodec::~opusCodec()
{
    if(m_pEncoder){
        opus_encoder_destroy(m_pEncoder);
        m_pEncoder = nullptr;
    }

    if(m_pDecoder){
        opus_decoder_destroy(m_pDecoder);
        m_pDecoder = nullptr;
    }
}

bool opusCodec::InitEncoder(const AudioConfig &cfg)
{
    if(m_pEncoder)
        return true;

    int err = OPUS_OK;
    m_pEncoder = opus_encoder_create(cfg.inputRate, cfg.channels, OPUS_APPLICATION_VOIP, &err);
    if(err != OPUS_OK || !m_pEncoder){
        qWarning() << "opus_encoder_create failed:" << opus_strerror(err);
        m_pEncoder = nullptr;
        return false;
    }

    m_encChannels = cfg.channels;
    m_frameBytes = cfg.frameBytes();

    // ---- most compressed voice profile ----
    opus_encoder_ctl(m_pEncoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(m_pEncoder, OPUS_SET_BITRATE(cfg.opusBitrate));
    opus_encoder_ctl(m_pEncoder, OPUS_SET_VBR(1));
    opus_encoder_ctl(m_pEncoder, OPUS_SET_VBR_CONSTRAINT(0));
    opus_encoder_ctl(m_pEncoder, OPUS_SET_MAX_BANDWIDTH(cfg.MaxBandwith)); // 8kHz is enough for speech
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
        qWarning() << "opus_decoder_create failed:" << opus_strerror(err);
        m_pDecoder = nullptr;
        return false;
    }

    m_decChannels = channels;
    return true;
}

QByteArray opusCodec::Encode(const QByteArray &pcmFrame)
{
    if(!m_pEncoder)
        return QByteArray();

    if(pcmFrame.size() != m_frameBytes){
        qWarning() << "opus Encode: wrong frame size" << pcmFrame.size() << "expected" << m_frameBytes;
        return QByteArray();
    }

    const int frameSamples = m_frameBytes / (2 * m_encChannels);

    // copy to an aligned int16 buffer
    QByteArray pcm = pcmFrame;
    opus_int16 *in = reinterpret_cast<opus_int16*>(pcm.data());

    unsigned char out[1500];
    opus_int32 n = opus_encode(m_pEncoder, in, frameSamples, out, sizeof(out));
    if(n < 0){
        qWarning() << "opus_encode failed:" << opus_strerror(n);
        return QByteArray();
    }

    return QByteArray(reinterpret_cast<const char*>(out), n);
}

QByteArray opusCodec::Decode(const QByteArray &packet)
{
    if(!m_pDecoder || packet.isEmpty())
        return QByteArray();

    // 5760 = 120ms @ 48kHz, the biggest possible Opus packet
    static const int MAX_SAMPLES = 5760;
    opus_int16 pcm[MAX_SAMPLES * 2];

    int samples = opus_decode(m_pDecoder,
                              reinterpret_cast<const unsigned char*>(packet.constData()),
                              packet.size(), pcm, MAX_SAMPLES, 0);
    if(samples < 0){
        qWarning() << "opus_decode failed:" << opus_strerror(samples);
        return QByteArray();
    }

    return QByteArray(reinterpret_cast<const char*>(pcm), samples * m_decChannels * int(sizeof(opus_int16)));
}

QByteArray opusCodec::DecodeFec(const QByteArray &nextPacket, int frameSamples)
{
    if(!m_pDecoder || nextPacket.isEmpty())
        return QByteArray();

    opus_int16 pcm[5760 * 2];
    int samples = opus_decode(m_pDecoder,
                              reinterpret_cast<const unsigned char*>(nextPacket.constData()),
                              nextPacket.size(), pcm, frameSamples, 1);
    if(samples < 0){
        qWarning() << "opus_decode(fec) failed:" << opus_strerror(samples);
        return DecodePlc(frameSamples);
    }
    return QByteArray(reinterpret_cast<const char*>(pcm), samples * m_decChannels * int(sizeof(opus_int16)));
}

QByteArray opusCodec::DecodePlc(int frameSamples)
{
    if(!m_pDecoder)
        return QByteArray();

    opus_int16 pcm[5760 * 2];
    int samples = opus_decode(m_pDecoder, nullptr, 0, pcm, frameSamples, 0);
    if(samples < 0){
        qWarning() << "opus_decode(plc) failed:" << opus_strerror(samples);
        return QByteArray();
    }
    return QByteArray(reinterpret_cast<const char*>(pcm), samples * m_decChannels * int(sizeof(opus_int16)));
}
