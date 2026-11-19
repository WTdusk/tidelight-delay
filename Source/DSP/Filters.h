#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

#if defined (__SSE2__) || defined (_M_X64) || defined (_M_AMD64)
 #include <emmintrin.h>
 #define GUITAR_DELAY_SSE2 1
#elif defined (__aarch64__) && (defined (__ARM_NEON) || defined (__ARM_NEON__))
 #include <arm_neon.h>
 #define GUITAR_DELAY_NEON 1
#endif

namespace guitar_delay
{

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kTwoPi = 6.28318530717958647692f;

// Gentle highpass on the signal entering a delay loop. The dry guitar
// stays full-range; the repeats shed low end so the pick and the amp's
// low strings don't pile up.
inline constexpr float kRepeatLowCutHz = 120.0f;

inline float lerp (float a, float b, float t) noexcept
{
    return a + (b - a) * t;
}

inline float flushDenormal (float x) noexcept
{
    return std::abs (x) < 1.0e-15f ? 0.0f : x;
}

namespace detail
{
constexpr int tanhStepsPerUnit = 64;
constexpr float tanhLimit = 10.0f;

struct TanhSegment
{
    float value, slope, quadratic, cubic;
};

inline constexpr auto tanhSegments = []
{
    constexpr double step = 1.0 / tanhStepsPerUnit;
    constexpr double stepTanh = 0.015623728558408866; // tanh(1 / 64)
    std::array<TanhSegment, static_cast<int> (tanhLimit) * tanhStepsPerUnit> segments {};

    // The tanh addition formula supplies the endpoints at compile time.
    // Cubic Hermite interpolation also matches their analytic slopes.
    double value = 0.0;
    for (auto& segment : segments)
    {
        const double next = (value + stepTanh) / (1.0 + value * stepTanh);
        const double slope = (1.0 - value * value) * step;
        const double nextSlope = (1.0 - next * next) * step;
        const double delta = next - value;
        segment = { static_cast<float> (value), static_cast<float> (slope),
                    static_cast<float> (3.0 * delta - 2.0 * slope - nextSlope),
                    static_cast<float> (slope + nextSlope - 2.0 * delta) };
        value = next;
    }
    return segments;
}();
} // namespace detail

inline float saturationTanh (float x) noexcept
{
    const float magnitude = std::abs (x);
    if (! (magnitude < detail::tanhLimit))
        return magnitude >= detail::tanhLimit ? std::copysign (1.0f, x) : x;

    const float position = magnitude * detail::tanhStepsPerUnit;
    const auto index = static_cast<int> (position);
    const float t = position - static_cast<float> (index);
    const auto& p = detail::tanhSegments[static_cast<size_t> (index)];
    return std::copysign (p.value + t * (p.slope + t * (p.quadratic + t * p.cubic)), x);
}

// Bit compare, so a settled parameter skips a recompute without a float-equality warning.
inline bool bitsEqual (float a, float b) noexcept
{
    return std::memcmp (&a, &b, sizeof (float)) == 0;
}

inline bool bitsEqual (double a, double b) noexcept
{
    return std::memcmp (&a, &b, sizeof (double)) == 0;
}

// One-pole lowpass. Cheap tape-loss and gentle tone control.
struct OnePoleLowpass
{
    float coeff = 1.0f;
    float state = 0.0f;
    float lastHz = 0.0f;
    float lastSr = 0.0f;
    bool coeffValid = false;

    void setCutoff (float hz, double sampleRate) noexcept
    {
        const float sr = static_cast<float> (sampleRate);
        if (coeffValid && bitsEqual (hz, lastHz) && bitsEqual (sr, lastSr))
            return;

        lastHz = hz;
        lastSr = sr;
        coeffValid = true;
        const float fc = std::clamp (hz, 10.0f, sr * 0.49f);
        coeff = 1.0f - std::exp (-kTwoPi * fc / sr);
    }

    float process (float x) noexcept
    {
        state += coeff * (x - state);
        state = flushDenormal (state);
        return state;
    }

    void reset() noexcept { state = 0.0f; }
};

struct OnePoleHighpass
{
    OnePoleLowpass lowpass;

    void setCutoff (float hz, double sampleRate) noexcept { lowpass.setCutoff (hz, sampleRate); }

    float process (float x) noexcept { return x - lowpass.process (x); }

    void reset() noexcept { lowpass.reset(); }
};

struct DcBlocker
{
    float x1 = 0.0f;
    float y1 = 0.0f;
    float r = 0.995f;

    void prepare (double sampleRate) noexcept
    {
        // ~30 Hz at 48 kHz, scaled so the corner stays put at other rates.
        const float sr = static_cast<float> (sampleRate);
        r = std::exp (-kTwoPi * 30.0f / sr);
    }

    float process (float x) noexcept
    {
        const float y = x - x1 + r * y1;
        x1 = x;
        y1 = flushDenormal (y);
        return y1;
    }

    void reset() noexcept
    {
        x1 = 0.0f;
        y1 = 0.0f;
    }
};

// RBJ biquad, transposed direct form II.
struct Biquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;
    float lastHz = 0.0f;
    float lastQ = 0.0f;
    double lastSr = 0.0;
    bool coeffValid = false;

    void setLowpass (float hz, float q, double sampleRate) noexcept
    {
        if (coeffValid && bitsEqual (hz, lastHz) && bitsEqual (q, lastQ) && bitsEqual (sampleRate, lastSr))
            return;

        lastHz = hz;
        lastQ = q;
        lastSr = sampleRate;
        coeffValid = true;

        const float sr = static_cast<float> (sampleRate);
        const float fc = std::clamp (hz, 20.0f, sr * 0.45f);
        const float w0 = kTwoPi * fc / sr;
        const float cosw = std::cos (w0);
        const float sinw = std::sin (w0);
        const float alpha = sinw / (2.0f * std::max (q, 0.05f));
        const float a0 = 1.0f + alpha;
        const float inv = 1.0f / a0;
        b0 = ((1.0f - cosw) * 0.5f) * inv;
        b1 = (1.0f - cosw) * inv;
        b2 = b0;
        a1 = (-2.0f * cosw) * inv;
        a2 = (1.0f - alpha) * inv;
    }

    float process (float x) noexcept
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        z1 = flushDenormal (z1);
        z2 = flushDenormal (z2);
        return y;
    }

    void reset() noexcept
    {
        z1 = 0.0f;
        z2 = 0.0f;
    }
};

// 4th-order Butterworth via two Q-staggered biquads.
// Used as the anti-alias and reconstruction pair around the BBD.
struct Lowpass4
{
    Biquad stage1;
    Biquad stage2;

    void setCutoff (float hz, double sampleRate) noexcept
    {
        stage1.setLowpass (hz, 0.54119610f, sampleRate);
        stage2.setLowpass (hz, 1.30656296f, sampleRate);
    }

    float process (float x) noexcept { return stage2.process (stage1.process (x)); }

    void reset() noexcept
    {
        stage1.reset();
        stage2.reset();
    }
};

// 8x oversampler for memoryless nonlinearities (tape curve, BBD clip).
// Three cascaded 2x halfband stages. Each lowpass is a 95-tap Kaiser
// windowed sinc with cutoff 0.22 cycles at that stage's rate, so images
// fall before the next decimator while guitar harmonics pass.
// Group delay is about 82 samples at the base rate (~1.7 ms at 48 kHz),
// which only shifts the wet path.
inline constexpr int kHalfbandTaps = 95;
inline constexpr float kHalfband[kHalfbandTaps] = {
    2.0308331073e-06f, 3.9771000862e-06f, -6.5751265785e-06f, -1.7377142452e-05f, 7.6290667063e-06f,
    4.6478609360e-05f, 8.5259025611e-06f, -9.1655105986e-05f, -6.4141111436e-05f, 1.3875690992e-04f,
    1.8429457650e-04f, -1.5063117086e-04f, -3.8331327052e-04f, 6.4301987633e-05f, 6.4447433169e-04f,
    2.0006856183e-04f, -8.9843649478e-04f, -7.1420986177e-04f, 1.0099034687e-03f, 1.5014414669e-03f,
    -7.8380069196e-04f, -2.4887250185e-03f, 0.0f, 3.4656299839e-03f, 1.5212257697e-03f,
    -4.0685612881e-03f, -3.8310374287e-03f, 3.8066731769e-03f, 6.7595745234e-03f, -2.1371883907e-03f,
    -9.8438693110e-03f, -1.4163146581e-03f, 1.2298413241e-02f, 7.1249211392e-03f, -1.3035914203e-02f,
    -1.4929237515e-02f, 1.0714037644e-02f, 2.4366274633e-02f, -3.7194342883e-03f, -3.4578740524e-02f,
    -1.0173641207e-02f, 4.4418574572e-02f, 3.5458784014e-02f, -5.2631758826e-02f, -8.7869738974e-02f,
    5.8087560775e-02f, 3.1200087212e-01f, 4.3999975439e-01f, 3.1200087212e-01f, 5.8087560775e-02f,
    -8.7869738974e-02f, -5.2631758826e-02f, 3.5458784014e-02f, 4.4418574572e-02f, -1.0173641207e-02f,
    -3.4578740524e-02f, -3.7194342883e-03f, 2.4366274633e-02f, 1.0714037644e-02f, -1.4929237515e-02f,
    -1.3035914203e-02f, 7.1249211392e-03f, 1.2298413241e-02f, -1.4163146581e-03f, -9.8438693110e-03f,
    -2.1371883907e-03f, 6.7595745234e-03f, 3.8066731769e-03f, -3.8310374287e-03f, -4.0685612881e-03f,
    1.5212257697e-03f, 3.4656299839e-03f, 0.0f, -2.4887250185e-03f, -7.8380069196e-04f,
    1.5014414669e-03f, 1.0099034687e-03f, -7.1420986177e-04f, -8.9843649478e-04f, 2.0006856183e-04f,
    6.4447433169e-04f, 6.4301987633e-05f, -3.8331327052e-04f, -1.5063117086e-04f, 1.8429457650e-04f,
    1.3875690992e-04f, -6.4141111436e-05f, -9.1655105986e-05f, 8.5259025611e-06f, 4.6478609360e-05f,
    7.6290667063e-06f, -1.7377142452e-05f, -6.5751265785e-06f, 3.9771000862e-06f, 2.0308331073e-06f
};

// Polyphase views of kHalfband. The upsampler inserts a zero between input
// samples, so the even and odd outputs only touch even and odd taps. Stored
// oldest-first so each dot product is a contiguous multiply. The 96th
// downsample coefficient is padding so the width is a multiple of 4.
struct alignas (16) HalfbandTables
{
    float evenOldest[48] {};
    float oddOldest[48] {};
    float downOldest[96] {};

    static constexpr HalfbandTables make() noexcept
    {
        HalfbandTables tables {};
        for (int j = 0; j < 48; ++j)
            tables.evenOldest[j] = kHalfband[2 * (47 - j)];
        for (int j = 1; j < 48; ++j)
            tables.oddOldest[j] = kHalfband[2 * (47 - j) + 1];
        for (int j = 0; j < kHalfbandTaps; ++j)
            tables.downOldest[j] = kHalfband[kHalfbandTaps - 1 - j];
        return tables;
    }
};

inline constexpr HalfbandTables kHalfbandTables = HalfbandTables::make();

template <int N>
inline float dotProduct (const float* samples, const float* coeffs) noexcept
{
#if defined (GUITAR_DELAY_SSE2)
    __m128 acc = _mm_setzero_ps();
    int i = 0;
    for (; i + 4 <= N; i += 4)
        acc = _mm_add_ps (acc, _mm_mul_ps (_mm_loadu_ps (samples + i), _mm_loadu_ps (coeffs + i)));

    const __m128 swapped = _mm_shuffle_ps (acc, acc, 0xB1);
    acc = _mm_add_ps (acc, swapped);
    const __m128 high = _mm_movehl_ps (acc, acc);
    acc = _mm_add_ss (acc, high);
    float sum = _mm_cvtss_f32 (acc);
    for (; i < N; ++i)
        sum += samples[i] * coeffs[i];
    return sum;
#elif defined (GUITAR_DELAY_NEON) && defined (__aarch64__)
    float32x4_t acc = vdupq_n_f32 (0.0f);
    int i = 0;
    for (; i + 4 <= N; i += 4)
        acc = vmlaq_f32 (acc, vld1q_f32 (samples + i), vld1q_f32 (coeffs + i));

    float lanes[4];
    vst1q_f32 (lanes, acc);
    float sum = (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]);
    for (; i < N; ++i)
        sum += samples[i] * coeffs[i];
    return sum;
#else
    float sum = 0.0f;
    for (int i = 0; i < N; ++i)
        sum += samples[i] * coeffs[i];
    return sum;
#endif
}

inline void interpolate2x (const float* samples, float& even, float& odd) noexcept
{
#if defined (GUITAR_DELAY_SSE2)
    __m128 evenAcc = _mm_setzero_ps();
    __m128 oddAcc = _mm_setzero_ps();
    for (int i = 0; i < 48; i += 4)
    {
        const auto input = _mm_loadu_ps (samples + i);
        evenAcc = _mm_add_ps (evenAcc, _mm_mul_ps (input, _mm_loadu_ps (kHalfbandTables.evenOldest + i)));
        oddAcc = _mm_add_ps (oddAcc, _mm_mul_ps (input, _mm_loadu_ps (kHalfbandTables.oddOldest + i)));
    }
    const auto sumLanes = [] (__m128 acc)
    {
        acc = _mm_add_ps (acc, _mm_shuffle_ps (acc, acc, 0xB1));
        return _mm_cvtss_f32 (_mm_add_ss (acc, _mm_movehl_ps (acc, acc)));
    };
    even = sumLanes (evenAcc) * 2.0f;
    odd = sumLanes (oddAcc) * 2.0f;
#else
    even = dotProduct<48> (samples, kHalfbandTables.evenOldest) * 2.0f;
    odd = dotProduct<48> (samples, kHalfbandTables.oddOldest) * 2.0f;
#endif
}

struct HalfbandStage
{
    // Duplicated rings keep the FIR windows contiguous at the wrap. The
    // decimator writes a pair per step, so both histories share a cursor.
    static constexpr int kUpHist = 48;
    static constexpr int kDownHist = kHalfbandTaps + 1;
    static_assert (kDownHist == 2 * kUpHist);

    alignas (16) float upHist[2 * kUpHist] {};
    alignas (16) float downHist[2 * kDownHist] {};
    int pos = 0;

    void reset() noexcept
    {
        std::fill_n (upHist, 2 * kUpHist, 0.0f);
        std::fill_n (downHist, 2 * kDownHist, 0.0f);
        pos = 0;
    }

    // One sample in and out. The nonlinear function runs on both phases,
    // but only the even decimator output is kept.
    template <typename Fn>
    float process (float x, Fn&& fn) noexcept
    {
        upHist[pos] = x;
        upHist[pos + kUpHist] = x;
        const float* upWindow = upHist + pos + 1;
        float even = 0.0f;
        float odd = 0.0f;
        interpolate2x (upWindow, even, odd);

        const int downIndex = 2 * pos;
        const float yEven = fn (even);
        downHist[downIndex] = yEven;
        downHist[downIndex + kDownHist] = yEven;
        // The 95 taps end at the even sample's second copy. The following
        // slot is read by SIMD but multiplied by the zero padding coefficient.
        const float kept = dotProduct<96> (downHist + downIndex + 2, kHalfbandTables.downOldest);

        const float yOdd = fn (odd);
        downHist[downIndex + 1] = yOdd;
        downHist[downIndex + kDownHist + 1] = yOdd;

        if (++pos == kUpHist)
            pos = 0;

        return flushDenormal (kept);
    }
};

struct Oversampler8x
{
    // Longer than the cascaded halfband tails. Past this, a digital-zero input
    // has already left the filter, so the stages can sleep until audio returns.
    static constexpr int kQuietSamples = 512;
    static constexpr float kQuietInput = 1.0e-12f;
    static constexpr float kQuietOutput = 1.0e-10f;

    HalfbandStage outer;
    HalfbandStage mid;
    HalfbandStage inner;
    int quietSamples = 0;
    bool quietBypass = false;

    void resetFilters() noexcept
    {
        outer.reset();
        mid.reset();
        inner.reset();
    }

    void reset() noexcept
    {
        resetFilters();
        quietSamples = 0;
        quietBypass = false;
    }

    template <typename Fn>
    float process (float x, Fn&& fn) noexcept
    {
        if (quietBypass)
        {
            if (std::abs (x) < kQuietInput)
                return 0.0f;

            quietBypass = false;
            quietSamples = 0;
        }

        const float y = outer.process (x, [&] (float midSample)
        {
            return mid.process (midSample, [&] (float innerSample)
            {
                return inner.process (innerSample, fn);
            });
        });

        if (std::abs (x) < kQuietInput && std::abs (y) < kQuietOutput)
        {
            if (++quietSamples >= kQuietSamples)
            {
                resetFilters();
                quietBypass = true;
            }
        }
        else
        {
            quietSamples = 0;
        }

        return y;
    }
};

struct FastRng
{
    uint32_t state = 0xA341316Cu;

    float nextBipolar() noexcept
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<float> (state & 0x00FFFFFFu) * (1.0f / 8388608.0f) - 1.0f;
    }

    void reset (uint32_t seed) noexcept { state = seed == 0u ? 1u : seed; }
};

} // namespace guitar_delay
