#ifndef RAVAVA_GAINRAMP_H
#define RAVAVA_GAINRAMP_H

// Volume / gain control in percent for the audio thread: no allocation, no lock, no clicks.
//
//   Set()     : any thread (the GUI slider).
//   Process() : the ONE audio thread, in place on interleaved s16 samples.
//
// 100 % = unchanged. Below 100 attenuates, above 100 amplifies (only if maxPercent > 100) and the result is
// saturated to the s16 range, so it clips instead of wrapping around. Curve: gain = (percent / 100)^2, which
// feels like an even slider (50 % = -12 dB, 200 % = +12 dB). The gain moves linearly from the previous block's
// value to the new one across a block, so moving the slider never clicks.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ravava {

class GainRamp
{
public:
    explicit GainRamp(int maxPercent = 100) : m_max(std::max(1, maxPercent)) {}

    void Set(int percent) { m_percent.store(std::min(std::max(percent, 0), m_max), std::memory_order_relaxed); }
    int  Get() const { return m_percent.load(std::memory_order_relaxed); }
    int  Max() const { return m_max; }

    static float PercentToGain(int percent)
    {
        const float v = float(percent) / 100.f;
        return v * v;
    }

    // audio thread: true if Process() would change the samples (lets the caller skip a copy at 100 %)
    bool Active() const { return PercentToGain(Get()) != 1.f || m_gain != 1.f; }

    // audio thread
    void Process(int16_t *pcm, uint32_t frames, int channels)
    {
        const float target = PercentToGain(Get());
        const float start = m_gain;
        m_gain = target;
        if(start == 1.f && target == 1.f)
            return;
        if(!pcm || !frames || channels <= 0)
            return;

        const float step = (target - start) / float(frames);
        float g = start;
        for(uint32_t f = 0; f < frames; f++){
            g += step;
            for(int c = 0; c < channels; c++){
                int16_t &x = pcm[size_t(f) * size_t(channels) + size_t(c)];
                const float v = float(x) * g;
                x = v >= 32767.f ? int16_t(32767) : (v <= -32768.f ? int16_t(-32768) : int16_t(v));
            }
        }
    }

private:
    int m_max;
    std::atomic<int> m_percent{100};
    float m_gain = 1.f;                 // gain at the end of the previous block (audio thread only)
};

} // namespace ravava

#endif // RAVAVA_GAINRAMP_H
