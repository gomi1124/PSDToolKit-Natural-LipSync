#include "japanese_reading.h"
#include "lipsync_analyzer.h"
#include "wav_audio_source.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{

constexpr double kPi = 3.14159265358979323846;

class MemoryAudioSource final : public aviutl1_lipsync::AudioSource
{
  public:
    MemoryAudioSource(int sample_rate, std::vector<float> left, std::vector<float> right = {})
        : sample_rate_(sample_rate), left_(std::move(left)), right_(std::move(right))
    {
    }

    aviutl1_lipsync::AudioInfo GetInfo() const override
    {
        return {
            static_cast<std::int64_t>(left_.size()),
            sample_rate_,
            right_.empty() ? 1 : 2,
        };
    }

    int Read(std::int64_t sample_index, int sample_count, float *left, float *right) const override
    {
        last_read_index_ = sample_index;
        if (sample_index < 0 || sample_index >= static_cast<std::int64_t>(left_.size()))
        {
            return 0;
        }
        const auto available = static_cast<int>(std::min<std::int64_t>(
            sample_count, static_cast<std::int64_t>(left_.size()) - sample_index));
        for (int i = 0; i < available; ++i)
        {
            const auto source_index = static_cast<std::size_t>(sample_index + i);
            left[i] = left_[source_index];
            right[i] = right_.empty() ? 0.0F : right_[source_index];
        }
        return available;
    }

    std::int64_t GetLastReadIndex() const
    {
        return last_read_index_;
    }

  private:
    int sample_rate_;
    std::vector<float> left_;
    std::vector<float> right_;
    mutable std::int64_t last_read_index_ = -1;
};

bool IsNear(double actual, double expected, double tolerance)
{
    return std::abs(actual - expected) <= tolerance;
}

void Require(bool condition, const std::string &message)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void WriteU16(std::ostream &stream, std::uint16_t value)
{
    const char bytes[] = {
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8U) & 0xffU),
    };
    stream.write(bytes, sizeof(bytes));
}

void WriteU32(std::ostream &stream, std::uint32_t value)
{
    const char bytes[] = {
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8U) & 0xffU),
        static_cast<char>((value >> 16U) & 0xffU),
        static_cast<char>((value >> 24U) & 0xffU),
    };
    stream.write(bytes, sizeof(bytes));
}

void TestWavAudioSourceReadsPcm16Stereo()
{
    const auto path =
        std::filesystem::temp_directory_path() / "LipSyncAviUtl1Tests_pcm16.wav";
    constexpr std::uint16_t channel_count = 2;
    constexpr std::uint32_t sample_rate = 44100;
    const std::vector<std::int16_t> interleaved = {
        0, 32767, 16384, -16384, -32768, 0,
    };
    const auto data_size =
        static_cast<std::uint32_t>(interleaved.size() * sizeof(std::int16_t));

    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        Require(static_cast<bool>(stream), "temporary WAV must be writable");
        stream.write("RIFF", 4);
        WriteU32(stream, 36U + data_size);
        stream.write("WAVE", 4);
        stream.write("fmt ", 4);
        WriteU32(stream, 16);
        WriteU16(stream, 1);
        WriteU16(stream, channel_count);
        WriteU32(stream, sample_rate);
        WriteU32(stream, sample_rate * channel_count * sizeof(std::int16_t));
        WriteU16(stream, channel_count * sizeof(std::int16_t));
        WriteU16(stream, 16);
        stream.write("data", 4);
        WriteU32(stream, data_size);
        stream.write(reinterpret_cast<const char *>(interleaved.data()), data_size);
    }

    const aviutl1_lipsync::WavAudioSource source(path);
    const auto info = source.GetInfo();
    Require(info.sample_count == 3, "WAV source must expose the PCM frame count");
    Require(info.sample_rate == static_cast<int>(sample_rate),
            "WAV source must expose the original sample rate");
    Require(info.channel_count == channel_count,
            "WAV source must expose the original channel count");

    float left[3]{};
    float right[3]{};
    Require(source.Read(0, 3, left, right) == 3,
            "WAV source must return every requested PCM frame");
    Require(IsNear(left[0], 0.0, 1e-8) && IsNear(right[0], 32767.0 / 32768.0, 1e-8),
            "WAV source must preserve the first stereo frame");
    Require(IsNear(left[1], 0.5, 1e-8) && IsNear(right[1], -0.5, 1e-8),
            "WAV source must preserve signed PCM values");
    Require(IsNear(left[2], -1.0, 1e-8) && IsNear(right[2], 0.0, 1e-8),
            "WAV source must preserve the minimum int16 sample");

    std::error_code error;
    std::filesystem::remove(path, error);
}

std::vector<float> MakeSine(int sample_rate, double frequency, double seconds, double amplitude)
{
    const auto count = static_cast<std::size_t>(std::ceil(sample_rate * seconds));
    std::vector<float> samples(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        samples[i] = static_cast<float>(
            amplitude * std::sin(2.0 * kPi * frequency * static_cast<double>(i) / sample_rate));
    }
    return samples;
}

std::vector<float> MakeFrameAmplitudeSine(int sample_rate, int frame_rate, double frequency,
                                          const std::vector<double> &amplitudes)
{
    const auto samples_per_frame = sample_rate / frame_rate;
    std::vector<float> samples(
        static_cast<std::size_t>(samples_per_frame) * amplitudes.size(), 0.0F);
    for (std::size_t frame = 0; frame < amplitudes.size(); ++frame)
    {
        for (int i = 0; i < samples_per_frame; ++i)
        {
            const auto sample_index = frame * static_cast<std::size_t>(samples_per_frame) +
                                      static_cast<std::size_t>(i);
            samples[sample_index] = static_cast<float>(
                amplitudes[frame] *
                std::sin(2.0 * kPi * frequency * static_cast<double>(sample_index) / sample_rate));
        }
    }
    return samples;
}

double CalculateReferenceLevel24k(const std::vector<float> &samples, double position_seconds,
                                  double low_cut, double high_cut)
{
    const auto start = static_cast<std::int64_t>(std::trunc(
        static_cast<float>(position_seconds) *
        static_cast<float>(aviutl1_lipsync::kTargetSampleRate)));
    std::vector<double> windowed(aviutl1_lipsync::kWindowSize, 0.0);
    for (int i = 0; i < aviutl1_lipsync::kWindowSize; ++i)
    {
        const auto source_index = start + i;
        const auto value =
            source_index >= 0 && source_index < static_cast<std::int64_t>(samples.size())
                ? samples[static_cast<std::size_t>(source_index)]
                : 0.0F;
        const auto scaled = std::clamp(static_cast<double>(value) * 32768.0, -32768.0, 32767.0);
        const auto quantized = static_cast<std::int16_t>(scaled);
        const auto hamming = 0.54 - 0.46 * std::cos(2.0 * kPi * static_cast<double>(i) /
                                                    aviutl1_lipsync::kWindowSize);
        windowed[static_cast<std::size_t>(i)] = static_cast<double>(quantized) * hamming / 32768.0;
    }

    const double frequency_per_bin =
        aviutl1_lipsync::kTargetSampleRate / aviutl1_lipsync::kWindowSize;
    double sum = 0.0;
    int count = 0;
    for (int bin = 0; bin < aviutl1_lipsync::kWindowSize / 2; ++bin)
    {
        const auto hz = bin * frequency_per_bin;
        if (hz < low_cut)
        {
            continue;
        }
        if (high_cut < hz)
        {
            break;
        }

        double real = 0.0;
        double imaginary = 0.0;
        for (int i = 0; i < aviutl1_lipsync::kWindowSize; ++i)
        {
            const auto angle = 2.0 * kPi * bin * i / aviutl1_lipsync::kWindowSize;
            real += windowed[static_cast<std::size_t>(i)] * std::cos(angle);
            imaginary -= windowed[static_cast<std::size_t>(i)] * std::sin(angle);
        }
        sum += std::sqrt(real * real + imaginary * imaginary);
        ++count;
    }
    return count == 0 ? 0.0 : sum / count;
}

void TestSilenceIsZero()
{
    MemoryAudioSource source(24000, std::vector<float>(24000, 0.0F));
    const aviutl1_lipsync::Analyzer analyzer;
    Require(analyzer.GetLevel(source, 0.25, 100.0, 1000.0) == 0.0, "silence must be zero");
}

void TestIncludedToneHasLevel()
{
    const auto samples = MakeSine(24000, 750.0, 1.0, 0.5);
    MemoryAudioSource source(24000, samples);
    const aviutl1_lipsync::Analyzer analyzer;
    const auto included = analyzer.GetLevel(source, 0.1, 100.0, 1000.0);
    const auto excluded = analyzer.GetLevel(source, 0.1, 1100.0, 2000.0);
    const auto reference = CalculateReferenceLevel24k(samples, 0.1, 100.0, 1000.0);
    Require(included > 2.0, "750 Hz tone must be detected in the speech band");
    Require(included > excluded * 20.0, "out-of-band level must remain much smaller");
    Require(IsNear(included, reference, 1e-5),
            "Ooura RDFT result must match an independent DFT of the AviUtl1 window");
}

void TestStereoIsAveraged()
{
    auto left = MakeSine(24000, 750.0, 1.0, 0.5);
    std::vector<float> right(left.size());
    std::transform(left.begin(), left.end(), right.begin(), [](float value) { return -value; });
    MemoryAudioSource source(24000, std::move(left), std::move(right));
    const aviutl1_lipsync::Analyzer analyzer;
    Require(IsNear(analyzer.GetLevel(source, 0.1, 100.0, 1000.0), 0.0, 1e-8),
            "opposite stereo channels must cancel during mono conversion");
}

void TestTimeUsesTruncated24KhzPosition()
{
    MemoryAudioSource source(44100, MakeSine(44100, 750.0, 1.0, 0.5));
    const aviutl1_lipsync::Analyzer analyzer;
    const auto base = analyzer.GetLevel(source, 0.2, 100.0, 1000.0);
    const auto within_same_output_sample =
        analyzer.GetLevel(source, 0.2 + 0.49 / 24000.0, 100.0, 1000.0);
    Require(base == within_same_output_sample,
            "positions in the same 24 kHz sample must produce identical levels");
}

void TestTimeMatchesAviUtl1FloatBoundary()
{
    MemoryAudioSource source(24000, std::vector<float>(40000, 0.0F));
    const aviutl1_lipsync::Analyzer analyzer;
    (void)analyzer.GetLevel(source, 1.3, 100.0, 1000.0);
    Require(source.GetLastReadIndex() == 31199,
            "AviUtl1 float narrowing must select sample 31199 at 1.3 seconds");
}

void TestDownsamplingUsesBlockLocalPhase()
{
    constexpr int source_rate = 44100;
    constexpr double position = 1.3;
    const auto samples = MakeSine(source_rate, 750.0, 2.0, 0.5);
    MemoryAudioSource source(source_rate, samples);
    const aviutl1_lipsync::Analyzer analyzer;
    const auto actual = analyzer.GetLevel(source, position, 100.0, 1000.0);

    const auto output_start = static_cast<std::int64_t>(std::trunc(
        static_cast<float>(position) *
        static_cast<float>(aviutl1_lipsync::kTargetSampleRate)));
    const auto source_start =
        output_start * source_rate / aviutl1_lipsync::kTargetSampleRate;
    std::vector<float> expected_window(aviutl1_lipsync::kWindowSize, 0.0F);
    for (int i = 0; i < aviutl1_lipsync::kWindowSize; ++i)
    {
        const auto local_product = static_cast<std::int64_t>(i) * source_rate;
        const auto source_index =
            source_start + local_product / aviutl1_lipsync::kTargetSampleRate;
        const auto remainder = local_product % aviutl1_lipsync::kTargetSampleRate;
        const auto numerator =
            remainder + source_rate - aviutl1_lipsync::kTargetSampleRate;
        const auto current = samples[static_cast<std::size_t>(source_index)];
        const auto following = samples[static_cast<std::size_t>(source_index + 1)];
        const auto current_sample = static_cast<std::int16_t>(
            std::clamp(static_cast<double>(current) * 32768.0, -32768.0, 32767.0));
        const auto following_sample = static_cast<std::int16_t>(
            std::clamp(static_cast<double>(following) * 32768.0, -32768.0, 32767.0));
        const auto interpolated =
            (static_cast<double>(current_sample) * (source_rate - numerator) +
             static_cast<double>(following_sample) * numerator) /
            source_rate;
        const auto expected_sample = static_cast<std::int16_t>(interpolated);
        expected_window[static_cast<std::size_t>(i)] =
            static_cast<float>(expected_sample) / 32768.0F;
    }
    const auto expected = CalculateReferenceLevel24k(expected_window, 0.0, 100.0, 1000.0);
    Require(IsNear(actual, expected, 1e-5),
            "AviUtl1 downsampling must restart its interpolation phase for each window");
}

void TestInvalidBandIsRejected()
{
    MemoryAudioSource source(24000, std::vector<float>(24000, 0.0F));
    const aviutl1_lipsync::Analyzer analyzer;
    Require(std::isnan(analyzer.GetLevel(source, 0.0, 1000.0, 100.0)),
            "inverted frequency band must be rejected");
}

void TestSyllablePulseOpensAroundLocalPeak()
{
    constexpr int frame_rate = 60;
    const std::vector<double> amplitudes = {0.0, 0.1, 0.2, 0.6, 0.2, 0.1, 0.0};
    MemoryAudioSource source(
        24000, MakeFrameAmplitudeSine(24000, frame_rate, 750.0, amplitudes));
    const aviutl1_lipsync::Analyzer analyzer;

    Require(analyzer.GetSyllablePulse(source, 0.0 / frame_rate, frame_rate, 100.0, 1000.0,
                                      20.0, 2, 3, 2) == 0.0,
            "pulse must remain closed before the peak lead window");
    for (int frame = 1; frame <= 3; ++frame)
    {
        Require(analyzer.GetSyllablePulse(source, static_cast<double>(frame) / frame_rate,
                                          frame_rate, 100.0, 1000.0, 20.0, 2, 3, 2) == 1.0,
                "pulse must open for the configured three-frame window");
    }
    Require(analyzer.GetSyllablePulse(source, 4.0 / frame_rate, frame_rate, 100.0, 1000.0,
                                      20.0, 2, 3, 2) == 0.0,
            "pulse must close immediately after the configured window");
}

void TestSyllablePulseSuppressesNearbyWeakerPeak()
{
    constexpr int frame_rate = 60;
    const std::vector<double> amplitudes = {0.0, 0.1, 0.2, 0.6, 0.1, 0.4,
                                            0.1, 0.5, 0.1, 0.0};
    MemoryAudioSource source(
        24000, MakeFrameAmplitudeSine(24000, frame_rate, 750.0, amplitudes));
    const aviutl1_lipsync::Analyzer analyzer;

    Require(analyzer.GetSyllablePulse(source, 5.0 / frame_rate, frame_rate, 100.0, 1000.0,
                                      20.0, 2, 3, 2) == 1.0,
            "a separated later peak must start a new pulse");
    Require(analyzer.GetSyllablePulse(source, 4.0 / frame_rate, frame_rate, 100.0, 1000.0,
                                      20.0, 2, 3, 2) == 0.0,
            "a nearby weaker peak must not create a pulse");
}

void TestProductionPulseLeavesVisibleClosedFrames()
{
    constexpr int frame_rate = 60;
    std::vector<double> amplitudes(18, 0.05);
    amplitudes[5] = 0.7;
    amplitudes[11] = 0.8;
    MemoryAudioSource source(
        24000, MakeFrameAmplitudeSine(24000, frame_rate, 750.0, amplitudes));
    const aviutl1_lipsync::Analyzer analyzer;

    for (int frame = 3; frame <= 6; ++frame)
    {
        Require(analyzer.GetSyllablePulse(source, static_cast<double>(frame) / frame_rate,
                                          frame_rate, 100.0, 1000.0, 20.0, 5, 4, 2) == 1.0,
                "the first production pulse must stay open for four frames");
    }
    for (int frame = 7; frame <= 8; ++frame)
    {
        Require(analyzer.GetSyllablePulse(source, static_cast<double>(frame) / frame_rate,
                                          frame_rate, 100.0, 1000.0, 20.0, 5, 4, 2) == 0.0,
                "production pulses must leave two visible closed frames");
    }
    for (int frame = 9; frame <= 12; ++frame)
    {
        Require(analyzer.GetSyllablePulse(source, static_cast<double>(frame) / frame_rate,
                                          frame_rate, 100.0, 1000.0, 20.0, 5, 4, 2) == 1.0,
                "the second production pulse must stay open for four frames");
    }
}

void TestPatternStateSequenceMatchesAviUtl1Transitions()
{
    constexpr int sample_rate = 24000;
    constexpr int frame_rate = 60;
    std::vector<double> amplitudes(12, 0.0);
    for (int frame = 0; frame < 6; ++frame)
    {
        amplitudes[static_cast<std::size_t>(frame)] = 0.6;
    }
    MemoryAudioSource source(
        sample_rate, MakeFrameAmplitudeSine(sample_rate, frame_rate, 750.0, amplitudes));
    aviutl1_lipsync::PatternStateSequence sequence({
        frame_rate,
        100.0,
        1000.0,
        20.0,
        1,
        1.0,
        2,
    });

    const std::vector<int> expected = {0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0};
    for (int frame = 0; frame < static_cast<int>(expected.size()); ++frame)
    {
        Require(sequence.GetState(source, frame) == expected[static_cast<std::size_t>(frame)],
                "deterministic state replay must match AviUtl1 open/close transitions");
    }
}

void TestPatternStateSequencePreservesFractionalSpeed()
{
    constexpr int sample_rate = 24000;
    constexpr int frame_rate = 60;
    MemoryAudioSource source(
        sample_rate,
        MakeFrameAmplitudeSine(sample_rate, frame_rate, 750.0,
                               std::vector<double>(8, 0.6)));
    aviutl1_lipsync::PatternStateSequence sequence({
        frame_rate,
        100.0,
        1000.0,
        20.0,
        1,
        1.12,
        3,
    });

    const std::vector<int> expected = {0, 0, 1, 1, 2, 2, 2, 2};
    for (int frame = 0; frame < static_cast<int>(expected.size()); ++frame)
    {
        Require(sequence.GetState(source, frame) == expected[static_cast<std::size_t>(frame)],
                "fractional speed must use AviUtl1's integer update counter");
    }
}

void TestPatternStateSequenceIsIndependentOfRequestOrder()
{
    constexpr int sample_rate = 24000;
    constexpr int frame_rate = 60;
    const std::vector<double> amplitudes = {
        0.0, 0.6, 0.6, 0.0, 0.0, 0.6, 0.6, 0.0, 0.6, 0.0, 0.0, 0.0,
    };
    MemoryAudioSource source(
        sample_rate, MakeFrameAmplitudeSine(sample_rate, frame_rate, 750.0, amplitudes));
    const aviutl1_lipsync::PatternSettings settings{
        frame_rate,
        100.0,
        1000.0,
        20.0,
        1,
        1.0,
        2,
    };
    aviutl1_lipsync::PatternStateSequence sequential(settings);
    std::vector<int> expected;
    for (int frame = 0; frame < static_cast<int>(amplitudes.size()); ++frame)
    {
        expected.push_back(sequential.GetState(source, frame));
    }

    aviutl1_lipsync::PatternStateSequence random_access(settings);
    Require(random_access.GetState(source, 10) == expected[10],
            "forward random access must replay all preceding AviUtl1 states");
    Require(random_access.GetState(source, 3) == expected[3],
            "backward random access must return the cached historical state");
    Require(random_access.GetState(source, 11) == expected[11],
            "continued access must remain deterministic after seeking backward");
    Require(random_access.GetCachedFrameCount() == 12,
            "state sequence must cache exactly the generated frame range");
}

void TestAdaptiveStateDetectsHighFrequencySpeechPulses()
{
    constexpr int sample_rate = 24000;
    constexpr int frame_rate = 60;
    std::vector<double> amplitudes(36, 0.0);
    const std::vector<int> expected_peaks = {6, 16, 26};
    for (const auto peak : expected_peaks)
    {
        amplitudes[static_cast<std::size_t>(peak - 1)] = 0.2;
        amplitudes[static_cast<std::size_t>(peak)] = 0.8;
        amplitudes[static_cast<std::size_t>(peak + 1)] = 0.2;
    }
    MemoryAudioSource source(
        sample_rate, MakeFrameAmplitudeSine(sample_rate, frame_rate, 2500.0, amplitudes));
    aviutl1_lipsync::PatternStateSequence legacy({
        frame_rate,
        100.0,
        1000.0,
        20.0,
        1,
        1.0,
        2,
    });
    aviutl1_lipsync::AdaptivePatternStateSequence adaptive({
        frame_rate,
        100.0,
        1000.0,
        20.0,
        1,
        1.0,
        2,
    });

    Require(legacy.GetState(source, 26) == 0,
            "legacy low-band detection must miss a high-frequency-only test signal");
    Require(adaptive.GetState(source, 35) == 0,
            "adaptive detection must close after the final speech pulse");
    const auto &actual_peaks = adaptive.GetPeakFrames();
    Require(actual_peaks.size() == expected_peaks.size(),
            "adaptive detection must retain each separated high-frequency pulse");
    for (std::size_t i = 0; i < expected_peaks.size(); ++i)
    {
        Require(std::abs(actual_peaks[i] - expected_peaks[i]) <= 1,
                "adaptive high-frequency pulse must stay aligned to its source peak");
        Require(adaptive.GetState(source, actual_peaks[i]) == 1,
                "adaptive high-frequency pulse must select the open mouth");
    }
}

void TestAdaptiveStateBridgesClosePulsesWithIntermediateMouth()
{
    constexpr int sample_rate = 24000;
    constexpr int frame_rate = 60;
    std::vector<double> amplitudes(32, 0.0);
    amplitudes[6] = 0.8;
    amplitudes[13] = 0.7;
    amplitudes[21] = 0.9;
    MemoryAudioSource source(
        sample_rate, MakeFrameAmplitudeSine(sample_rate, frame_rate, 750.0, amplitudes));
    aviutl1_lipsync::AdaptivePatternStateSequence adaptive({
        frame_rate,
        100.0,
        1000.0,
        20.0,
        1,
        1.0,
        3,
    });

    Require(adaptive.GetState(source, 31) == 0,
            "adaptive low-frequency detection must close after speech");
    const auto &peaks = adaptive.GetPeakFrames();
    Require(peaks.size() == 3,
            "adaptive low-frequency detection must preserve separated speech peaks");
    auto maximum_open_peaks = 0;
    auto intermediate_peaks = 0;
    for (std::size_t i = 0; i < peaks.size(); ++i)
    {
        const auto peak_state = adaptive.GetState(source, peaks[i]);
        maximum_open_peaks += peak_state == 2 ? 1 : 0;
        intermediate_peaks += peak_state == 1 ? 1 : 0;
        if (i == 0)
        {
            continue;
        }
        bool has_intermediate_frame = false;
        for (auto frame = peaks[i - 1] + 1; frame < peaks[i]; ++frame)
        {
            const auto state = adaptive.GetState(source, frame);
            Require(state > 0,
                    "close syllable peaks must not force a complete mouth closure");
            has_intermediate_frame = has_intermediate_frame || state == 1;
        }
        Require(has_intermediate_frame,
                "close syllable peaks must be joined by the intermediate mouth");
    }
    Require(maximum_open_peaks == 1 && intermediate_peaks == 2,
            "dense speech peaks must reserve maximum opening for spaced accents");
}

void TestAdaptiveStateClosesAcrossLongPauseWithIntermediateMouth()
{
    constexpr int sample_rate = 24000;
    constexpr int frame_rate = 60;
    std::vector<double> amplitudes(36, 0.0);
    amplitudes[6] = 0.8;
    amplitudes[26] = 0.9;
    MemoryAudioSource source(
        sample_rate, MakeFrameAmplitudeSine(sample_rate, frame_rate, 750.0, amplitudes));
    aviutl1_lipsync::AdaptivePatternStateSequence adaptive({
        frame_rate,
        100.0,
        1000.0,
        20.0,
        1,
        1.0,
        3,
    });

    Require(adaptive.GetState(source, 35) == 0,
            "intermediate-mouth detection must close after speech");
    const auto &peaks = adaptive.GetPeakFrames();
    Require(peaks.size() == 2,
            "long-pause test must preserve both separated speech peaks");
    bool has_closed_frame = false;
    for (auto frame = peaks[0] + 1; frame < peaks[1]; ++frame)
    {
        has_closed_frame = has_closed_frame || adaptive.GetState(source, frame) == 0;
    }
    Require(has_closed_frame,
            "a long pause must still reach the fully closed mouth");
}

void TestAdaptiveStateLimitsMaximumOpeningFrequency()
{
    constexpr int sample_rate = 24000;
    constexpr int frame_rate = 60;
    std::vector<double> amplitudes(44, 0.0);
    amplitudes[6] = 0.8;
    amplitudes[19] = 0.7;
    amplitudes[36] = 0.9;
    MemoryAudioSource source(
        sample_rate, MakeFrameAmplitudeSine(sample_rate, frame_rate, 750.0, amplitudes));
    aviutl1_lipsync::AdaptivePatternStateSequence adaptive({
        frame_rate,
        100.0,
        1000.0,
        20.0,
        1,
        1.0,
        5,
        3,
    });

    Require(adaptive.GetState(source, 43) == 0,
            "maximum-opening frequency test must close after speech");
    const auto &peaks = adaptive.GetPeakFrames();
    Require(peaks.size() == 3,
            "maximum-opening frequency test must preserve all speech peaks");
    std::vector<int> maximum_open_frames;
    auto intermediate_peaks = 0;
    for (const auto peak : peaks)
    {
        const auto state = adaptive.GetState(source, peak);
        if (state == 4)
        {
            maximum_open_frames.push_back(peak);
        }
        else
        {
            intermediate_peaks += state == 2 ? 1 : 0;
        }
    }
    Require(maximum_open_frames.size() == 2 && intermediate_peaks == 1,
            "dense peaks must keep one intermediate mouth without removing its pulse");
    Require(maximum_open_frames[1] - maximum_open_frames[0] >= 30,
            "60 fps maximum openings must remain at least 500 ms apart");
}

void TestAdaptiveStateBridgesOnlyClosedFlicker()
{
    constexpr int sample_rate = 24000;
    constexpr int frame_rate = 60;
    std::vector<double> amplitudes(28, 0.0);
    amplitudes[6] = 0.8;
    amplitudes[17] = 0.9;
    MemoryAudioSource source(
        sample_rate, MakeFrameAmplitudeSine(sample_rate, frame_rate, 750.0, amplitudes));
    aviutl1_lipsync::AdaptivePatternStateSequence adaptive({
        frame_rate,
        100.0,
        1000.0,
        20.0,
        1,
        1.0,
        5,
        2,
    });

    Require(adaptive.GetState(source, 27) == 0,
            "closed-flicker test must close after speech");
    Require(adaptive.GetState(source, 0) == 0,
            "closed-flicker test must remain closed before speech");
    const auto &peaks = adaptive.GetPeakFrames();
    Require(peaks.size() == 2,
            "closed-flicker test must preserve both speech peaks");
    for (auto frame = peaks[0]; frame <= peaks[1]; ++frame)
    {
        Require(adaptive.GetState(source, frame) > 0,
                "a sub-90 ms closed flicker must remain at the intermediate mouth");
    }
}

void TestAdaptiveStateKeepsBriefPauseClosed()
{
    constexpr int sample_rate = 24000;
    constexpr int frame_rate = 60;
    std::vector<double> amplitudes(31, 0.0);
    amplitudes[6] = 0.8;
    amplitudes[20] = 0.9;
    MemoryAudioSource source(
        sample_rate, MakeFrameAmplitudeSine(sample_rate, frame_rate, 750.0, amplitudes));
    aviutl1_lipsync::AdaptivePatternStateSequence adaptive({
        frame_rate,
        100.0,
        1000.0,
        20.0,
        1,
        1.0,
        5,
        2,
    });

    Require(adaptive.GetState(source, 30) == 0,
            "brief-pause test must close after speech");
    const auto &peaks = adaptive.GetPeakFrames();
    Require(peaks.size() == 2,
            "brief-pause test must preserve both speech peaks");
    auto has_closed_frame = false;
    for (auto frame = peaks[0] + 1; frame < peaks[1]; ++frame)
    {
        has_closed_frame = has_closed_frame || adaptive.GetState(source, frame) == 0;
    }
    Require(has_closed_frame,
            "a 100 ms speech pause must return to the closed mouth");
}

void TestAdaptiveStateKeepsTwoPatternMouthOpenLongEnough()
{
    constexpr int sample_rate = 24000;
    constexpr int frame_rate = 60;
    std::vector<double> amplitudes(20, 0.0);
    amplitudes[6] = 0.8;
    amplitudes[13] = 0.9;
    MemoryAudioSource source(
        sample_rate, MakeFrameAmplitudeSine(sample_rate, frame_rate, 750.0, amplitudes));
    aviutl1_lipsync::AdaptivePatternStateSequence adaptive({
        frame_rate,
        100.0,
        1000.0,
        20.0,
        1,
        1.0,
        2,
    });

    Require(adaptive.GetState(source, 19) == 0,
            "two-pattern syllable detection must close after speech");
    const auto &peaks = adaptive.GetPeakFrames();
    Require(peaks.size() == 2,
            "two-pattern syllable detection must preserve naturally separated peaks");
    for (const auto peak : peaks)
    {
        for (auto frame = peak - 1; frame <= peak + 2; ++frame)
        {
            Require(adaptive.GetState(source, frame) == 1,
                    "two-pattern mouth must remain visibly open for four frames");
        }
    }
    Require(adaptive.GetState(source, peaks[0] + 3) == 0,
            "two-pattern syllable pulses must leave one closed frame");
}

void TestAdaptiveStateHoldsStableIntermediateAndOpenMouths()
{
    constexpr int sample_rate = 24000;
    constexpr int frame_rate = 60;
    std::vector<double> amplitudes(24, 0.0);
    amplitudes[10] = 0.8;
    MemoryAudioSource source(
        sample_rate, MakeFrameAmplitudeSine(sample_rate, frame_rate, 750.0, amplitudes));

    aviutl1_lipsync::AdaptivePatternStateSequence four_patterns({
        frame_rate,
        100.0,
        1000.0,
        20.0,
        1,
        1.0,
        4,
    });
    const std::vector<int> expected_four = {2, 2, 3, 3, 3, 3, 3, 2, 2};
    Require(four_patterns.GetState(source, 23) == 0,
            "four-pattern syllable detection must close after speech");
    Require(four_patterns.GetPeakFrames().size() == 1,
            "four-pattern syllable detection must find the source pulse");
    const auto four_peak = four_patterns.GetPeakFrames().front();
    for (std::size_t offset = 0; offset < expected_four.size(); ++offset)
    {
        Require(four_patterns.GetState(
                    source, four_peak - 4 + static_cast<int>(offset)) ==
                    expected_four[offset],
                "four-pattern syllable envelope must hold stable mouth shapes");
    }

    aviutl1_lipsync::AdaptivePatternStateSequence five_patterns({
        frame_rate,
        100.0,
        1000.0,
        20.0,
        1,
        1.0,
        5,
    });
    const std::vector<int> expected_five = {2, 2, 4, 4, 4, 4, 4, 2, 2};
    Require(five_patterns.GetState(source, 23) == 0,
            "five-pattern syllable detection must close after speech");
    Require(five_patterns.GetPeakFrames().size() == 1,
            "five-pattern syllable detection must find the source pulse");
    const auto five_peak = five_patterns.GetPeakFrames().front();
    for (std::size_t offset = 0; offset < expected_five.size(); ++offset)
    {
        Require(five_patterns.GetState(
                    source, five_peak - 4 + static_cast<int>(offset)) ==
                    expected_five[offset],
                "five-pattern syllable envelope must hold stable mouth shapes");
    }
}

void TestAdaptiveStateScalesMouthShapeHoldWithFrameRate()
{
    constexpr int sample_rate = 24000;
    constexpr int frame_rate = 30;
    std::vector<double> amplitudes(18, 0.0);
    amplitudes[8] = 0.8;
    MemoryAudioSource source(
        sample_rate, MakeFrameAmplitudeSine(sample_rate, frame_rate, 750.0, amplitudes));
    aviutl1_lipsync::AdaptivePatternStateSequence adaptive({
        frame_rate,
        100.0,
        1000.0,
        20.0,
        1,
        1.0,
        5,
    });

    Require(adaptive.GetState(source, 17) == 0,
            "30 fps syllable detection must close after speech");
    Require(adaptive.GetPeakFrames().size() == 1,
            "30 fps syllable detection must find the source pulse");
    const auto peak = adaptive.GetPeakFrames().front();
    Require(adaptive.GetState(source, peak - 2) == 2,
            "30 fps syllable envelope must use the intermediate mouth before opening");
    Require(adaptive.GetState(source, peak - 1) == 4,
            "30 fps syllable envelope must open before the peak");
    Require(adaptive.GetState(source, peak) == 4,
            "30 fps syllable envelope must reach the open mouth at the peak");
    Require(adaptive.GetState(source, peak + 1) == 4,
            "30 fps syllable envelope must hold the open mouth after the peak");
    Require(adaptive.GetState(source, peak + 2) == 2,
            "30 fps syllable envelope must return through the intermediate mouth");
}

void TestAdaptiveStateSuppressesUnnaturallyRapidPeaks()
{
    constexpr int sample_rate = 24000;
    constexpr int frame_rate = 60;
    std::vector<double> amplitudes(24, 0.0);
    amplitudes[6] = 0.8;
    amplitudes[11] = 0.9;
    amplitudes[16] = 0.7;
    MemoryAudioSource source(
        sample_rate, MakeFrameAmplitudeSine(sample_rate, frame_rate, 750.0, amplitudes));
    aviutl1_lipsync::AdaptivePatternStateSequence adaptive({
        frame_rate,
        100.0,
        1000.0,
        20.0,
        1,
        1.0,
        5,
    });

    Require(adaptive.GetState(source, 23) == 0,
            "five-pattern syllable detection must close after rapid speech");
    const auto &peaks = adaptive.GetPeakFrames();
    Require(peaks.size() < 3,
            "five-pattern envelopes must not force every rapid syllable candidate");
    Require(peaks.size() >= 2,
            "rapid speech must retain enough peaks for visible mouth movement");
    auto maximum_open_peaks = 0;
    for (std::size_t index = 0; index < peaks.size(); ++index)
    {
        const auto peak_state = adaptive.GetState(source, peaks[index]);
        Require(peak_state == 2 || peak_state == 4,
                "rapid syllables must use an intermediate or fully open mouth");
        maximum_open_peaks += peak_state == 4 ? 1 : 0;
        if (index == 0)
        {
            continue;
        }
        bool has_intermediate_frame = false;
        for (auto frame = peaks[index - 1] + 1; frame < peaks[index]; ++frame)
        {
            const auto state = adaptive.GetState(source, frame);
            Require(state > 0,
                    "retained rapid syllables must not force a complete closure");
            has_intermediate_frame = has_intermediate_frame || state == 2;
        }
        Require(has_intermediate_frame,
                "retained rapid syllables must use an intermediate bridge");
    }
    Require(maximum_open_peaks == 1,
            "rapid speech must not force every retained peak fully open");
}

void TestAdaptiveStateHoldsContinuousSimilarSoundOpen()
{
    constexpr int sample_rate = 24000;
    constexpr int frame_rate = 60;
    std::vector<double> amplitudes(24, 0.0);
    for (int frame = 0; frame <= 17; ++frame)
    {
        amplitudes[static_cast<std::size_t>(frame)] = 0.7;
    }
    amplitudes[6] = 0.9;
    amplitudes[13] = 1.0;
    MemoryAudioSource source(
        sample_rate, MakeFrameAmplitudeSine(sample_rate, frame_rate, 750.0, amplitudes));
    aviutl1_lipsync::AdaptivePatternStateSequence adaptive({
        frame_rate,
        100.0,
        1000.0,
        20.0,
        1,
        1.0,
        5,
        2,
    });

    Require(adaptive.GetState(source, 23) == 0,
            "continuous-sound detection must close after speech");
    const auto &peaks = adaptive.GetPeakFrames();
    Require(peaks.size() == 2,
            "continuous-sound detection must preserve both spectral peaks");
    for (auto frame = peaks[0]; frame <= peaks[1]; ++frame)
    {
        Require(adaptive.GetState(source, frame) == 4,
                "similar continuous peaks must keep the fully open mouth");
    }
}

void TestAdaptiveStateIsIndependentOfRequestOrder()
{
    constexpr int sample_rate = 24000;
    constexpr int frame_rate = 60;
    std::vector<double> amplitudes(28, 0.0);
    amplitudes[5] = 0.7;
    amplitudes[12] = 0.8;
    amplitudes[20] = 0.6;
    MemoryAudioSource source(
        sample_rate, MakeFrameAmplitudeSine(sample_rate, frame_rate, 750.0, amplitudes));
    const aviutl1_lipsync::PatternSettings settings{
        frame_rate,
        100.0,
        1000.0,
        20.0,
        1,
        1.0,
        2,
    };
    aviutl1_lipsync::AdaptivePatternStateSequence sequential(settings);
    std::vector<int> expected;
    for (int frame = 0; frame < static_cast<int>(amplitudes.size()); ++frame)
    {
        expected.push_back(sequential.GetState(source, frame));
    }

    aviutl1_lipsync::AdaptivePatternStateSequence random_access(settings);
    Require(random_access.GetState(source, 24) == expected[24],
            "adaptive forward random access must build the deterministic state sequence");
    Require(random_access.GetState(source, 4) == expected[4],
            "adaptive backward random access must return the same cached state");
    Require(random_access.GetState(source, 27) == expected[27],
            "adaptive state replay must remain deterministic after seeking backward");
}

void TestGuidedAdaptiveStateHoldsSimilarContinuousSpeechOpen()
{
    constexpr int sample_rate = 24000;
    constexpr int frame_rate = 60;
    std::vector<double> amplitudes(32, 0.45);
    for (const auto frame : {6, 13, 20, 27})
    {
        amplitudes[static_cast<std::size_t>(frame)] = 0.9;
    }
    MemoryAudioSource source(
        sample_rate, MakeFrameAmplitudeSine(sample_rate, frame_rate, 750.0, amplitudes));
    const aviutl1_lipsync::PatternSettings settings{
        frame_rate,
        100.0,
        1000.0,
        20.0,
        1,
        1.0,
        3,
        4,
    };
    aviutl1_lipsync::AdaptivePatternStateSequence adaptive(settings);

    Require(adaptive.GetState(source, 31) >= 0,
            "guided syllable state must build for the complete source");
    const auto &peaks = adaptive.GetPeakFrames();
    Require(peaks.size() == 4,
            "guided syllable state must retain the requested pulse count");
    for (std::size_t index = 1; index < peaks.size(); ++index)
    {
        for (auto frame = peaks[index - 1]; frame <= peaks[index]; ++frame)
        {
            Require(adaptive.GetState(source, frame) == 2,
                    "similar continuous speech must hold the fully open mouth");
        }
    }
}

void TestAdaptiveStateFallsBackToAviUtl1WithoutSpectralPeaks()
{
    constexpr int sample_rate = 24000;
    constexpr int frame_rate = 60;
    const std::vector<double> amplitudes(2, 0.8);
    MemoryAudioSource source(
        sample_rate, MakeFrameAmplitudeSine(sample_rate, frame_rate, 750.0, amplitudes));
    const aviutl1_lipsync::PatternSettings settings{
        frame_rate,
        100.0,
        1000.0,
        20.0,
        1,
        1.0,
        2,
        1,
    };
    aviutl1_lipsync::PatternStateSequence legacy(settings);
    aviutl1_lipsync::AdaptivePatternStateSequence adaptive(settings);

    Require(adaptive.GetState(source, 2) >= 0,
            "adaptive fallback must build for a short voiced source");
    Require(adaptive.GetPeakFrames().empty(),
            "a source shorter than the peak window must not invent syllable peaks");
    bool has_open_frame = false;
    for (int frame = 0; frame <= 2; ++frame)
    {
        const auto legacy_state = legacy.GetState(source, frame);
        has_open_frame = has_open_frame || legacy_state > 0;
        Require(adaptive.GetState(source, frame) == legacy_state,
                "missing spectral peaks must fall back to the AviUtl1 volume state");
    }
    Require(has_open_frame,
            "fallback coverage must include an AviUtl1-open frame");
}

void TestJapaneseSyllableCountMatchesLipSyncUnits()
{
    Require(aviutl1_lipsync::CountJapaneseSyllables(L"マッシュルーム") == 4,
            "small katakana, geminate consonants, and long vowels must not add pulses");
    Require(aviutl1_lipsync::CountJapaneseSyllables(L"チョット") == 2,
            "contracted sounds and small tsu must stay within their spoken syllables");
    Require(aviutl1_lipsync::CountJapaneseSyllables(L"１ホン") == 3,
            "full-width digits must contribute one spoken syllable");
}

void TestGuidedSyllableStateUsesTextCountAsUpperBound()
{
    constexpr int sample_rate = 24000;
    constexpr int frame_rate = 60;
    std::vector<double> amplitudes(48, 0.0);
    for (int frame = 5; frame <= 38; ++frame)
    {
        amplitudes[static_cast<std::size_t>(frame)] =
            frame % 2 == 0 ? 0.75 : 0.25;
    }
    MemoryAudioSource source(
        sample_rate, MakeFrameAmplitudeSine(sample_rate, frame_rate, 2200.0, amplitudes));
    aviutl1_lipsync::AdaptivePatternStateSequence guided({
        frame_rate,
        100.0,
        1000.0,
        20.0,
        1,
        1.0,
        2,
        40,
    });

    Require(guided.GetState(source, 47) == 0,
            "guided syllable detection must close after speech");
    const auto &peaks = guided.GetPeakFrames();
    Require(peaks.size() < 40,
            "guided syllable detection must not force the requested pulse count");
    for (std::size_t index = 1; index < peaks.size(); ++index)
    {
        Require(peaks[index] - peaks[index - 1] >= 6,
                "60 fps syllable peaks must keep a 100 ms natural interval");
    }
}

void TestSyllablePulseRejectsInvalidSettings()
{
    MemoryAudioSource source(24000, std::vector<float>(24000, 0.0F));
    const aviutl1_lipsync::Analyzer analyzer;
    Require(std::isnan(analyzer.GetSyllablePulse(source, 0.0, 60.0, 100.0, 1000.0, 20.0,
                                                 0, 3, 2)),
            "zero peak radius must be rejected");
    Require(std::isnan(analyzer.GetSyllablePulse(source, 0.0, 60.0, 100.0, 1000.0, 20.0,
                                                 3, 3, 3)),
            "lead frames outside the open window must be rejected");
}

} // namespace

int main()
{
    TestWavAudioSourceReadsPcm16Stereo();
    TestSilenceIsZero();
    TestIncludedToneHasLevel();
    TestStereoIsAveraged();
    TestTimeUsesTruncated24KhzPosition();
    TestTimeMatchesAviUtl1FloatBoundary();
    TestDownsamplingUsesBlockLocalPhase();
    TestInvalidBandIsRejected();
    TestPatternStateSequenceMatchesAviUtl1Transitions();
    TestPatternStateSequencePreservesFractionalSpeed();
    TestPatternStateSequenceIsIndependentOfRequestOrder();
    TestAdaptiveStateDetectsHighFrequencySpeechPulses();
    TestAdaptiveStateBridgesClosePulsesWithIntermediateMouth();
    TestAdaptiveStateClosesAcrossLongPauseWithIntermediateMouth();
    TestAdaptiveStateLimitsMaximumOpeningFrequency();
    TestAdaptiveStateBridgesOnlyClosedFlicker();
    TestAdaptiveStateKeepsBriefPauseClosed();
    TestAdaptiveStateKeepsTwoPatternMouthOpenLongEnough();
    TestAdaptiveStateHoldsStableIntermediateAndOpenMouths();
    TestAdaptiveStateScalesMouthShapeHoldWithFrameRate();
    TestAdaptiveStateSuppressesUnnaturallyRapidPeaks();
    TestAdaptiveStateHoldsContinuousSimilarSoundOpen();
    TestAdaptiveStateIsIndependentOfRequestOrder();
    TestGuidedAdaptiveStateHoldsSimilarContinuousSpeechOpen();
    TestAdaptiveStateFallsBackToAviUtl1WithoutSpectralPeaks();
    TestJapaneseSyllableCountMatchesLipSyncUnits();
    TestGuidedSyllableStateUsesTextCountAsUpperBound();
    TestSyllablePulseOpensAroundLocalPeak();
    TestSyllablePulseSuppressesNearbyWeakerPeak();
    TestProductionPulseLeavesVisibleClosedFrames();
    TestSyllablePulseRejectsInvalidSettings();
    std::cout << "All LipSyncAviUtl1 analyzer tests passed.\n";
    return 0;
}
