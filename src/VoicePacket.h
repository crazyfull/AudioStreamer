#ifndef VOICEPACKET_H
#define VOICEPACKET_H

#include <QByteArray>
#include <QtGlobal>

// Wire format (3 byte header + one Opus packet):
//   byte 0   : bit0    = marker (first packet after silence / DTX gap)
//              bit1-7  = tx counter: +1 (mod 128) for every packet actually TRANSMITTED
//   byte 1-2 : sequence number, uint16 little endian, +1 for EVERY frame (also DTX-skipped ones)
// The sequence is used to reorder and to place packets in time (-> FEC / PLC).
// The tx counter is used for the packet loss statistics: a hole in the tx counter is a
// packet that was really sent and never arrived, whereas a hole in the sequence can also be
// plain DTX silence (even when the marker packet itself got lost).
namespace VoicePacket
{
    const int HeaderSize = 3;
    const quint8 FlagMarker = 0x01;
    const quint8 TxMask = 0x7F;

    inline QByteArray wrap(quint16 seq, bool marker, quint8 tx, const QByteArray &opusPayload)
    {
        QByteArray p;
        p.reserve(HeaderSize + opusPayload.size());
        p.append(char((marker ? FlagMarker : 0) | ((tx & TxMask) << 1)));
        p.append(char(seq & 0xFF));
        p.append(char((seq >> 8) & 0xFF));
        p.append(opusPayload);
        return p;
    }

    inline bool parse(const QByteArray &datagram, quint16 &seq, bool &marker, quint8 &tx, QByteArray &opusPayload)
    {
        if(datagram.size() <= HeaderSize)
            return false;
        const uchar *d = reinterpret_cast<const uchar*>(datagram.constData());
        marker = (d[0] & FlagMarker) != 0;
        tx = quint8(d[0] >> 1);
        seq = quint16(d[1] | (d[2] << 8));
        opusPayload = datagram.mid(HeaderSize);
        return true;
    }
}

#endif // VOICEPACKET_H
