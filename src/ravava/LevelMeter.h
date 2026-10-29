#ifndef RAVAVA_LEVELMETER_H
#define RAVAVA_LEVELMETER_H

// Loudness meter, 0..100, for the audio thread: no allocation, no lock.
//
//   Feed()      : the ONE audio thread, with the interleaved s16 samples it just handled.
//   Level()     : any thread. Smooth value (instant attack, slow release) = "what the VU meter shows now".
//   TakePeak()  : any thread. Loudest value since the LAST TakePeak() call, so a slow GUI timer
//                 (e.g. once per second) still sees a short word instead of missing it.
//
// Scale: RMS loudness in dBFS, measured over a short sliding window (WindowMs, a few words' worth of
// "now", not a single 10 ms block): FloorDb (-60 dBFS, and below = silence) = 0, CeilingDb (-3 dBFS, the RMS of a
// FULL-SCALE SINE) and above = 100. So:
//   100  = as loud as digital audio can get (full-scale sine, or clipped / square-ish signals, e.g. a shouted or
//          knocked microphone, strong echo / feedback),
//   ~75..90 = normal loud music (mastered music has an RMS of about -8..-16 dBFS), ~60..80 = loud speech,
//   0    = silence.
// Why not the PEAK of each block: music (and speech) has a peak near 0 dBFS in almost every second, so a peak
// meter sits on 100 all the time and says nothing about the real loudness. Why not the RMS of a 10 ms block:
// it jumps around with every wave; the short window makes it steady.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace ravava {

class LevelMeter
{
public:
    static constexpr float FloorDb = -60.f;
    static constexpr float CeilingDb = -3.f;
    static constexpr float WindowMs = 100.f;    // time constant of the loudness window

    // 0 .. 100 for a given mean square of s16 samples (sum of x*x / count)
    static int MeanSquareToLevel(double meanSquare)
    {
        if(!(meanSquare > 0))
            return 0;
        const double db = 10.0 * std::log10(meanSquare / (32768.0 * 32768.0));      // RMS in dBFS
        const double v = (db - FloorDb) / (CeilingDb - FloorDb) * 100.0;
        return int(std::lround(std::min(100.0, std::max(0.0, v))));
    }

    // 0 .. 100 for ONE block taken alone (all channels interleaved), no history
    static int BlockLevel(const int16_t *pcm, size_t samples)
    {
        if(!pcm || !samples)
            return 0;
        double sum = 0;
        for(size_t i = 0; i < samples; i++)
            sum += double(pcm[i]) * double(pcm[i]);
        return MeanSquareToLevel(sum / double(samples));
    }

    // audio thread. `rate` = frames per second, `channels` = interleaved channels
    void Feed(const int16_t *pcm, uint32_t frames, int rate, int channels)
    {
        if(!pcm || !frames || rate <= 0 || channels <= 0)
            return;
        const size_t n = size_t(frames) * size_t(channels);
        double sum = 0;
        for(size_t i = 0; i < n; i++)
            sum += double(pcm[i]) * double(pcm[i]);
        const double blockMs = sum / double(n);

        // mean square follows the signal with time constant WindowMs (exponential window)
        const double dt = double(frames) / double(rate);
        const double a = 1.0 - std::exp(-dt * 1000.0 / double(WindowMs));
        m_meanSquare += (blockMs - m_meanSquare) * a;
        const float now = float(MeanSquareToLevel(m_meanSquare));

        // instant attack, release 150 %/s (a full-scale burst falls to 0 in ~0.7 s)
        float cur = m_smooth;
        const float fall = 150.f * float(dt);
        cur = now >= cur ? now : std::max(now, cur - fall);
        m_smooth = cur;
        m_level.store(int(cur + 0.5f), std::memory_order_relaxed);

        int peak = int(now + 0.5f);
        int old = m_peak.load(std::memory_order_relaxed);
        while(peak > old && !m_peak.compare_exchange_weak(old, peak, std::memory_order_relaxed)) {}
    }

    int Level() const { return m_level.load(std::memory_order_relaxed); }
    int TakePeak() { return m_peak.exchange(0, std::memory_order_relaxed); }

private:
    double m_meanSquare = 0;                // audio thread only
    float m_smooth = 0;                     // audio thread only
    std::atomic<int> m_level{0};
    std::atomic<int> m_peak{0};
};

} // namespace ravava

#endif // RAVAVA_LEVELMETER_H
