#ifndef RAVAVA_LEVELMETER_H
#define RAVAVA_LEVELMETER_H

// Loudness meter, 0..100, for the audio thread: no allocation, no lock.
//
//   Feed()      : the ONE audio thread, with the interleaved s16 samples it just handled.
//   Level()     : any thread. Smooth value (instant attack, slow release) = "what the VU meter shows now".
//   TakePeak()  : any thread. Loudest block since the LAST TakePeak() call, so a slow GUI timer
//                 (e.g. once per second) still sees a short word instead of missing it.
//
// Scale: RMS of each block in dBFS, -60 dBFS (and below, silence) = 0, 0 dBFS (full scale) = 100.
// So normal speech lands around 40..80 and silence is 0.

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

    // 0 .. 100 for one block of samples (all channels interleaved)
    static int BlockLevel(const int16_t *pcm, size_t samples)
    {
        if(!pcm || !samples)
            return 0;
        double sum = 0;
        for(size_t i = 0; i < samples; i++)
            sum += double(pcm[i]) * double(pcm[i]);
        const double rms = std::sqrt(sum / double(samples)) / 32768.0;
        if(rms <= 0)
            return 0;
        const double db = 20.0 * std::log10(rms);
        const double v = (db - FloorDb) / (0.0 - FloorDb) * 100.0;
        return int(std::lround(std::min(100.0, std::max(0.0, v))));
    }

    // audio thread. `rate` = frames per second, `channels` = interleaved channels
    void Feed(const int16_t *pcm, uint32_t frames, int rate, int channels)
    {
        if(!pcm || !frames || rate <= 0 || channels <= 0)
            return;
        const float now = float(BlockLevel(pcm, size_t(frames) * size_t(channels)));

        // instant attack, release 150 %/s (a full-scale burst falls to 0 in ~0.7 s)
        float cur = m_smooth;
        const float fall = 150.f * float(frames) / float(rate);
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
    float m_smooth = 0;                     // audio thread only
    std::atomic<int> m_level{0};
    std::atomic<int> m_peak{0};
};

} // namespace ravava

#endif // RAVAVA_LEVELMETER_H
