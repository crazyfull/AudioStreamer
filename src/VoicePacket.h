#ifndef VOICEPACKET_H
#define VOICEPACKET_H

#include <QByteArray>
#include <QtGlobal>

// Wire format (3 byte header + one Opus packet):
//   byte 0   : flags   bit0 = marker (first packet after silence / DTX gap)
//   byte 1-2 : sequence number, uint16 little endian, +1 for EVERY 20ms frame
// The receiver needs the sequence to reorder, to detect loss (-> FEC / PLC)
// and to tell real loss apart from DTX silence (marker).
namespace VoicePacket
{
    const int HeaderSize = 3;
    const quint8 FlagMarker = 0x01;

    inline QByteArray wrap(quint16 seq, bool marker, const QByteArray &opusPayload)
    {
        QByteArray p;
        p.reserve(HeaderSize + opusPayload.size());
        p.append(char(marker ? FlagMarker : 0));
        p.append(char(seq & 0xFF));
        p.append(char((seq >> 8) & 0xFF));
        p.append(opusPayload);
        return p;
    }

    inline bool parse(const QByteArray &datagram, quint16 &seq, bool &marker, QByteArray &opusPayload)
    {
        if(datagram.size() <= HeaderSize)
            return false;
        const uchar *d = reinterpret_cast<const uchar*>(datagram.constData());
        marker = (d[0] & FlagMarker) != 0;
        seq = quint16(d[1] | (d[2] << 8));
        opusPayload = datagram.mid(HeaderSize);
        return true;
    }
}

#endif // VOICEPACKET_H
