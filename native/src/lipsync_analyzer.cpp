#include "lipsync_analyzer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <set>
#include <vector>

extern "C" void rdft(int n, int isgn, float *a, int *ip, float *w);

namespace aviutl1_lipsync
{
namespace
{

constexpr float kPi = 3.14159265358979323846F;
constexpr int kSyllableFftSize = 2048;
constexpr double kSyllableLowCutHz = 80.0;
constexpr double kSyllableHighCutHz = 6000.0;
constexpr double kSyllableMinimumPeakIntervalSeconds = 0.10;
constexpr double kSyllableIntermediateBridgeSeconds = 0.166;
constexpr double kClosedFlickerMaximumSeconds = 0.09;
constexpr double kMaximumOpenMinimumIntervalSeconds = 0.50;
constexpr double kMaximumOpenHoldSeconds = 0.08;
constexpr double kSyllableActivityThreshold = 0.05;
constexpr double kSyllableStrengthThreshold = 0.15;
constexpr double kSyllableLocalDelta = 0.05;
constexpr int kSpectralFingerprintBandCount = 5;
constexpr std::array<double, kSpectralFingerprintBandCount + 1>
    kSpectralFingerprintBandEdges = {80.0, 500.0, 1000.0, 2000.0, 4000.0,
                                     6000.0};
constexpr double kContinuousPeakMaximumIntervalSeconds = 0.14;
constexpr double kContinuousPeakMinimumValleyRatio = 0.75;
constexpr double kContinuousPeakMaximumFingerprintDistance = 0.55;

bool IsValidPulseSettings(double frame_rate, double threshold, int peak_radius_frames,
                          int open_frames, int lead_frames)
{
    return std::isfinite(frame_rate) && frame_rate > 0.0 && std::isfinite(threshold) &&
           threshold >= 0.0 && peak_radius_frames >= 1 && peak_radius_frames <= 30 &&
           open_frames >= 1 && open_frames <= 30 && lead_frames >= 0 &&
           lead_frames < open_frames;
}

float GetMonoSample(const std::vector<float> &left, const std::vector<float> &right,
                    int channel_count, std::int64_t relative_index, int samples_read)
{
    if (relative_index < 0 || relative_index >= samples_read)
    {
        return 0.0F;
    }

    const auto index = static_cast<std::size_t>(relative_index);
    if (channel_count >= 2)
    {
        return (left[index] + right[index]) * 0.5F;
    }
    return left[index];
}

std::int16_t QuantizeToInt16(float value)
{
    const auto scaled = static_cast<double>(value) * 32768.0;
    const auto clamped = std::clamp(scaled, -32768.0, 32767.0);
    return static_cast<std::int16_t>(clamped);
}

double GetPercentile(std::vector<double> values, double fraction)
{
    if (values.empty())
    {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(std::floor(
        static_cast<double>(values.size() - 1) * std::clamp(fraction, 0.0, 1.0)));
    return values[index];
}

std::vector<double> NormalizePercentiles(const std::vector<double> &values)
{
    const auto floor = GetPercentile(values, 0.10);
    const auto ceiling = GetPercentile(values, 0.90);
    const auto range = std::max(1.0e-9, ceiling - floor);
    std::vector<double> normalized;
    normalized.reserve(values.size());
    for (const auto value : values)
    {
        normalized.push_back(std::clamp((value - floor) / range, 0.0, 1.0));
    }
    return normalized;
}

double GetMedian(std::vector<double> values)
{
    if (values.empty())
    {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const auto middle = values.size() / 2;
    if (values.size() % 2 == 0)
    {
        return (values[middle - 1] + values[middle]) * 0.5;
    }
    return values[middle];
}

struct PeakCandidate
{
    int frame = 0;
    double score = 0.0;
};

std::vector<PeakCandidate> SelectPeaksWithLimit(
    const std::vector<PeakCandidate> &candidates, int target_count,
    int minimum_peak_distance)
{
    if (candidates.empty() || target_count <= 0)
    {
        return {};
    }

    std::vector<std::size_t> compatible_counts(candidates.size(), 0);
    for (std::size_t index = 0; index < candidates.size(); ++index)
    {
        auto compatible = index;
        while (compatible > 0 &&
               candidates[index].frame - candidates[compatible - 1].frame <
                   minimum_peak_distance)
        {
            --compatible;
        }
        compatible_counts[index] = compatible;
    }

    const auto maximum_count =
        std::min(static_cast<std::size_t>(target_count), candidates.size());
    const auto column_count = maximum_count + 1;
    constexpr double kNegativeInfinity = -1.0e100;
    std::vector<double> scores((candidates.size() + 1) * column_count,
                               kNegativeInfinity);
    std::vector<unsigned char> choices(scores.size(), 0);
    const auto cell = [column_count](std::size_t candidate_count,
                                     std::size_t selected_count) {
        return candidate_count * column_count + selected_count;
    };
    for (std::size_t candidate_count = 0; candidate_count <= candidates.size();
         ++candidate_count)
    {
        scores[cell(candidate_count, 0)] = 0.0;
    }

    for (std::size_t candidate_count = 1; candidate_count <= candidates.size();
         ++candidate_count)
    {
        const auto candidate_index = candidate_count - 1;
        const auto compatible_count = compatible_counts[candidate_index];
        for (std::size_t selected_count = 1; selected_count <= maximum_count;
             ++selected_count)
        {
            const auto skip_score = scores[cell(candidate_count - 1, selected_count)];
            auto take_score = scores[cell(compatible_count, selected_count - 1)];
            if (take_score > kNegativeInfinity / 2.0)
            {
                take_score += candidates[candidate_index].score;
            }
            if (take_score > skip_score)
            {
                scores[cell(candidate_count, selected_count)] = take_score;
                choices[cell(candidate_count, selected_count)] = 1;
            }
            else
            {
                scores[cell(candidate_count, selected_count)] = skip_score;
            }
        }
    }

    auto selected_count = maximum_count;
    while (selected_count > 0 &&
           scores[cell(candidates.size(), selected_count)] <= kNegativeInfinity / 2.0)
    {
        --selected_count;
    }
    std::vector<PeakCandidate> selected;
    auto candidate_count = candidates.size();
    while (candidate_count > 0 && selected_count > 0)
    {
        if (choices[cell(candidate_count, selected_count)] != 0)
        {
            const auto candidate_index = candidate_count - 1;
            selected.push_back(candidates[candidate_index]);
            candidate_count = compatible_counts[candidate_index];
            --selected_count;
        }
        else
        {
            --candidate_count;
        }
    }
    std::reverse(selected.begin(), selected.end());
    return selected;
}

std::vector<PeakCandidate> SelectUnguidedPeaks(
    const std::vector<PeakCandidate> &candidates, int minimum_peak_distance)
{
    std::vector<PeakCandidate> selected;
    for (const auto &candidate : candidates)
    {
        if (candidate.score < kSyllableStrengthThreshold)
        {
            continue;
        }
        if (selected.empty() ||
            candidate.frame - selected.back().frame >= minimum_peak_distance)
        {
            selected.push_back(candidate);
            continue;
        }
        if (candidate.score > selected.back().score)
        {
            selected.back() = candidate;
        }
    }
    return selected;
}

std::vector<PeakCandidate> SelectMaximumOpenPeaks(
    const std::vector<PeakCandidate> &candidates, int minimum_peak_distance)
{
    auto ranked = candidates;
    std::sort(ranked.begin(), ranked.end(), [](const auto &left, const auto &right) {
        if (left.score != right.score)
        {
            return left.score > right.score;
        }
        return left.frame < right.frame;
    });

    std::set<int> selected_frames;
    for (const auto &candidate : ranked)
    {
        const auto following = selected_frames.lower_bound(candidate.frame);
        if (following != selected_frames.end() &&
            *following - candidate.frame < minimum_peak_distance)
        {
            continue;
        }
        if (following != selected_frames.begin() &&
            candidate.frame - *std::prev(following) < minimum_peak_distance)
        {
            continue;
        }
        selected_frames.insert(candidate.frame);
    }

    std::vector<PeakCandidate> selected;
    selected.reserve(selected_frames.size());
    for (const auto &candidate : candidates)
    {
        if (selected_frames.find(candidate.frame) != selected_frames.end())
        {
            selected.push_back(candidate);
        }
    }
    return selected;
}

std::vector<int> BuildSyllableEnvelope(int pattern_count, double frame_rate)
{
    if (pattern_count == 2)
    {
        return {1, 1, 1, 1};
    }
    if (pattern_count < 3 || pattern_count > 5)
    {
        return {};
    }

    const auto intermediate_state = pattern_count / 2;
    const auto open_state = pattern_count - 1;
    const auto hold_frames = std::max(
        1, static_cast<int>(std::lround(frame_rate / 30.0)));
    std::vector<int> envelope;
    envelope.reserve(static_cast<std::size_t>(hold_frames * 3));
    envelope.insert(envelope.end(), hold_frames, intermediate_state);
    envelope.insert(envelope.end(), hold_frames, open_state);
    envelope.insert(envelope.end(), hold_frames, intermediate_state);
    return envelope;
}

int GetSpectralFingerprintBand(double frequency)
{
    for (int band = 0; band < kSpectralFingerprintBandCount; ++band)
    {
        if (frequency <
            kSpectralFingerprintBandEdges[static_cast<std::size_t>(band + 1)])
        {
            return band;
        }
    }
    return kSpectralFingerprintBandCount - 1;
}

} // namespace

double Analyzer::GetLevel(const AudioSource &source, double position_seconds, double low_cut_hz,
                          double high_cut_hz) const
{
    const auto info = source.GetInfo();
    if (info.sample_rate <= 0 || info.channel_count <= 0 || info.sample_count < 0 ||
        !std::isfinite(position_seconds) || position_seconds < 0.0 || !std::isfinite(low_cut_hz) ||
        !std::isfinite(high_cut_hz) || low_cut_hz < 0.0 || high_cut_hz < low_cut_hz)
    {
        return std::numeric_limits<double>::quiet_NaN();
    }

    // AviUtl1 first narrowed the Lua time to float, then used truncf on the
    // 24 kHz output timeline. The narrowing changes boundary frames such as
    // 1.3 seconds from sample 31200 to 31199.
    const auto aviutl1_position = static_cast<float>(position_seconds);
    const auto output_sample_index = static_cast<std::int64_t>(
        std::trunc(aviutl1_position * static_cast<float>(kTargetSampleRate)));
    const auto source_step =
        static_cast<double>(info.sample_rate) / static_cast<double>(kTargetSampleRate);
    const bool uses_aviutl1_downsampling = info.sample_rate >= kTargetSampleRate;
    const auto first_source_position = static_cast<double>(output_sample_index) * source_step;
    const auto first_source_index = uses_aviutl1_downsampling
                                        ? output_sample_index * info.sample_rate / kTargetSampleRate
                                        : static_cast<std::int64_t>(
                                              std::floor(first_source_position));
    const auto requested_samples = uses_aviutl1_downsampling
                                       ? ((kWindowSize - 1) * info.sample_rate /
                                          kTargetSampleRate) +
                                             2
                                       : static_cast<int>(std::floor(
                                                              static_cast<double>(
                                                                  output_sample_index +
                                                                  kWindowSize - 1) *
                                                              source_step) -
                                                          first_source_index + 2);

    std::vector<float> left(static_cast<std::size_t>(requested_samples), 0.0F);
    std::vector<float> right(static_cast<std::size_t>(requested_samples), 0.0F);
    const auto samples_read =
        std::clamp(source.Read(first_source_index, requested_samples, left.data(), right.data()), 0,
                   requested_samples);

    std::array<float, kWindowSize> fft_buffer{};
    for (int i = 0; i < kWindowSize; ++i)
    {
        std::int64_t source_index = 0;
        float fraction = 0.0F;
        std::int64_t interpolation_numerator = 0;
        if (uses_aviutl1_downsampling)
        {
            // avi_file_read_audio_sample() restarts its linear-resampling phase
            // at every requested block. PSDToolKit requested one 256-sample
            // block per level calculation, so the phase depends on the local
            // window index instead of the absolute 24 kHz sample index.
            const auto local_product = static_cast<std::int64_t>(i) * info.sample_rate;
            source_index = first_source_index + local_product / kTargetSampleRate;
            const auto remainder = local_product % kTargetSampleRate;
            interpolation_numerator =
                remainder + info.sample_rate - kTargetSampleRate;
        }
        else
        {
            const auto source_position =
                static_cast<double>(output_sample_index + i) * source_step;
            source_index = static_cast<std::int64_t>(std::floor(source_position));
            fraction = static_cast<float>(source_position - std::floor(source_position));
        }
        const auto relative_index = source_index - first_source_index;
        const auto current =
            GetMonoSample(left, right, info.channel_count, relative_index, samples_read);
        const auto following =
            GetMonoSample(left, right, info.channel_count, relative_index + 1, samples_read);
        std::int16_t sample = 0;
        if (uses_aviutl1_downsampling)
        {
            const auto current_sample = QuantizeToInt16(current);
            const auto following_sample = QuantizeToInt16(following);
            const auto weighted =
                (static_cast<double>(current_sample) *
                     static_cast<double>(info.sample_rate - interpolation_numerator) +
                 static_cast<double>(following_sample) *
                     static_cast<double>(interpolation_numerator)) /
                static_cast<double>(info.sample_rate);
            sample = static_cast<std::int16_t>(std::clamp(weighted, -32768.0, 32767.0));
        }
        else
        {
            const auto interpolated = current + (following - current) * fraction;
            sample = QuantizeToInt16(interpolated);
        }
        const auto window =
            (1.0F / 32768.0F) * (0.54F - 0.46F * std::cos(2.0F * kPi * static_cast<float>(i) /
                                                          static_cast<float>(kWindowSize)));
        fft_buffer[static_cast<std::size_t>(i)] = static_cast<float>(sample) * window;
    }

    // Ooura requires at least 2 + sqrt(n / 2) entries (13 for n=256).
    std::array<int, 16> work_int{};
    std::array<float, kWindowSize / 2> work_float{};
    rdft(kWindowSize, 1, fft_buffer.data(), work_int.data(), work_float.data());

    // Keep AviUtl1's integer division here: 24000 / 256 == 93.
    const float frequency_per_bin = kTargetSampleRate / kWindowSize;
    float sum = 0.0F;
    int count = 0;
    for (int i = 0; i < kWindowSize / 2; ++i)
    {
        const auto hz = static_cast<float>(i) * frequency_per_bin;
        if (hz < static_cast<float>(low_cut_hz))
        {
            continue;
        }
        if (static_cast<float>(high_cut_hz) < hz)
        {
            break;
        }
        const auto x = fft_buffer[static_cast<std::size_t>(i * 2)];
        const auto y = fft_buffer[static_cast<std::size_t>(i * 2 + 1)];
        sum += std::sqrt(x * x + y * y);
        ++count;
    }

    if (count == 0)
    {
        return 0.0;
    }
    return static_cast<double>(sum / static_cast<float>(count));
}

double Analyzer::GetSyllablePulse(const AudioSource &source, double position_seconds,
                                  double frame_rate, double low_cut_hz, double high_cut_hz,
                                  double threshold, int peak_radius_frames, int open_frames,
                                  int lead_frames) const
{
    if (!std::isfinite(position_seconds) || position_seconds < 0.0 ||
        !IsValidPulseSettings(frame_rate, threshold, peak_radius_frames, open_frames,
                              lead_frames))
    {
        return std::numeric_limits<double>::quiet_NaN();
    }

    const auto current_frame = static_cast<std::int64_t>(
        std::floor(position_seconds * frame_rate + 1.0e-6));
    const auto trailing_frames = open_frames - lead_frames - 1;
    const auto first_peak = current_frame - trailing_frames;
    const auto last_peak = current_frame + lead_frames;
    const auto first_level_frame = first_peak - peak_radius_frames;
    const auto last_level_frame = last_peak + peak_radius_frames;

    std::vector<double> levels(
        static_cast<std::size_t>(last_level_frame - first_level_frame + 1), 0.0);
    for (auto frame = first_level_frame; frame <= last_level_frame; ++frame)
    {
        if (frame < 0)
        {
            continue;
        }
        const auto level =
            GetLevel(source, static_cast<double>(frame) / frame_rate, low_cut_hz, high_cut_hz);
        if (!std::isfinite(level))
        {
            return std::numeric_limits<double>::quiet_NaN();
        }
        levels[static_cast<std::size_t>(frame - first_level_frame)] = level * 100.0;
    }

    for (auto peak_frame = first_peak; peak_frame <= last_peak; ++peak_frame)
    {
        if (peak_frame < 0)
        {
            continue;
        }
        const auto peak_level =
            levels[static_cast<std::size_t>(peak_frame - first_level_frame)];
        if (peak_level < threshold)
        {
            continue;
        }

        bool is_peak = true;
        for (auto neighbor_frame = peak_frame - peak_radius_frames;
             neighbor_frame <= peak_frame + peak_radius_frames; ++neighbor_frame)
        {
            if (neighbor_frame < 0 || neighbor_frame == peak_frame)
            {
                continue;
            }
            const auto neighbor_level =
                levels[static_cast<std::size_t>(neighbor_frame - first_level_frame)];
            if (neighbor_level > peak_level ||
                (neighbor_level == peak_level && neighbor_frame < peak_frame))
            {
                is_peak = false;
                break;
            }
        }
        if (is_peak)
        {
            return 1.0;
        }
    }
    return 0.0;
}

PatternStateSequence::PatternStateSequence(PatternSettings settings) : settings_(settings)
{
}

bool PatternStateSequence::IsValid() const
{
    return std::isfinite(settings_.frame_rate) && settings_.frame_rate > 0.0 &&
           std::isfinite(settings_.low_cut_hz) && settings_.low_cut_hz >= 0.0 &&
           std::isfinite(settings_.high_cut_hz) &&
           settings_.high_cut_hz >= settings_.low_cut_hz &&
           std::isfinite(settings_.threshold) && settings_.threshold >= 0.0 &&
           settings_.sensitivity_frames >= 1 && settings_.sensitivity_frames <= 100 &&
           std::isfinite(settings_.speed_frames) && settings_.speed_frames >= 0.0 &&
           settings_.speed_frames <= 100.0 && settings_.pattern_count >= 2 &&
           settings_.pattern_count <= 5;
}

int PatternStateSequence::GetState(const AudioSource &source, int frame_index)
{
    if (!IsValid() || frame_index < 0)
    {
        return -1;
    }

    while (states_.size() <= static_cast<std::size_t>(frame_index))
    {
        if (!AppendNextState(source))
        {
            return -1;
        }
    }
    return states_[static_cast<std::size_t>(frame_index)];
}

std::size_t PatternStateSequence::GetCachedFrameCount() const
{
    return states_.size();
}

bool PatternStateSequence::AppendNextState(const AudioSource &source)
{
    ++update_counter_;
    if (static_cast<double>(update_counter_) >= settings_.speed_frames)
    {
        const auto frame_index = states_.size();
        const auto position_seconds =
            static_cast<double>(frame_index) / settings_.frame_rate;
        const auto raw_level = analyzer_.GetLevel(
            source, position_seconds, settings_.low_cut_hz, settings_.high_cut_hz);
        if (!std::isfinite(raw_level))
        {
            return false;
        }

        volumes_.push_back(raw_level * 100.0);
        if (volumes_.size() > static_cast<std::size_t>(settings_.sensitivity_frames))
        {
            volumes_.erase(volumes_.begin());
        }

        double volume_sum = 0.0;
        for (const auto volume : volumes_)
        {
            volume_sum += volume;
        }
        const auto averaged_volume =
            volume_sum / static_cast<double>(settings_.sensitivity_frames);

        if (averaged_volume >= settings_.threshold)
        {
            if (pattern_ < settings_.pattern_count - 1)
            {
                ++pattern_;
                update_counter_ = 0;
            }
        }
        else if (pattern_ > 0)
        {
            --pattern_;
            update_counter_ = 0;
        }
    }

    states_.push_back(pattern_);
    return true;
}

AdaptivePatternStateSequence::AdaptivePatternStateSequence(PatternSettings settings)
    : settings_(settings)
{
}

bool AdaptivePatternStateSequence::IsValid() const
{
    return std::isfinite(settings_.frame_rate) && settings_.frame_rate > 0.0 &&
           std::isfinite(settings_.low_cut_hz) && settings_.low_cut_hz >= 0.0 &&
           std::isfinite(settings_.high_cut_hz) &&
           settings_.high_cut_hz >= settings_.low_cut_hz &&
           std::isfinite(settings_.threshold) && settings_.threshold >= 0.0 &&
           settings_.sensitivity_frames >= 1 && settings_.sensitivity_frames <= 100 &&
           std::isfinite(settings_.speed_frames) && settings_.speed_frames >= 0.0 &&
           settings_.speed_frames <= 100.0 && settings_.pattern_count >= 2 &&
           settings_.pattern_count <= 5 && settings_.target_pulse_count >= 0 &&
           settings_.target_pulse_count <= 10000;
}

int AdaptivePatternStateSequence::GetState(const AudioSource &source, int frame_index)
{
    if (!IsValid() || frame_index < 0)
    {
        return -1;
    }
    if (!is_built_ && !BuildStates(source))
    {
        return -1;
    }
    if (frame_index >= static_cast<int>(states_.size()))
    {
        return 0;
    }
    return states_[static_cast<std::size_t>(frame_index)];
}

std::size_t AdaptivePatternStateSequence::GetCachedFrameCount() const
{
    return states_.size();
}

const std::vector<int> &AdaptivePatternStateSequence::GetPeakFrames() const
{
    return peak_frames_;
}

bool AdaptivePatternStateSequence::BuildStates(const AudioSource &source)
{
    const auto info = source.GetInfo();
    if (info.sample_count < 0 || info.sample_rate <= 0 || info.channel_count <= 0)
    {
        return false;
    }

    const auto frame_count_value =
        std::floor(static_cast<double>(info.sample_count) * settings_.frame_rate /
                   static_cast<double>(info.sample_rate)) +
        1.0;
    if (!std::isfinite(frame_count_value) || frame_count_value < 1.0 ||
        frame_count_value > static_cast<double>(std::numeric_limits<int>::max()))
    {
        return false;
    }
    const auto frame_count = static_cast<int>(frame_count_value);

    const auto first_frequency_bin = std::max(
        1, static_cast<int>(std::ceil(kSyllableLowCutHz * kSyllableFftSize /
                                      static_cast<double>(info.sample_rate))));
    const auto last_frequency_bin = std::min(
        kSyllableFftSize / 2 - 1,
        static_cast<int>(std::floor(kSyllableHighCutHz * kSyllableFftSize /
                                    static_cast<double>(info.sample_rate))));
    if (last_frequency_bin < first_frequency_bin)
    {
        return false;
    }
    const auto frequency_bin_count = last_frequency_bin - first_frequency_bin + 1;
    std::vector<double> raw_activity(static_cast<std::size_t>(frame_count), 0.0);
    std::vector<double> logarithmic_activity(static_cast<std::size_t>(frame_count), 0.0);
    std::vector<double> raw_novelty(static_cast<std::size_t>(frame_count), 0.0);
    std::vector<double> spectral_fingerprints(
        static_cast<std::size_t>(frame_count) * kSpectralFingerprintBandCount, 0.0);
    std::vector<float> previous_spectrum(
        static_cast<std::size_t>(frequency_bin_count), 0.0F);
    std::vector<float> current_spectrum(
        static_cast<std::size_t>(frequency_bin_count), 0.0F);
    std::array<int, 64> work_int{};
    std::array<float, kSyllableFftSize / 2> work_float{};
    std::vector<float> left(static_cast<std::size_t>(kSyllableFftSize), 0.0F);
    std::vector<float> right(static_cast<std::size_t>(kSyllableFftSize), 0.0F);
    std::array<float, kSyllableFftSize> fft_buffer{};
    for (int frame = 0; frame < frame_count; ++frame)
    {
        std::fill(fft_buffer.begin(), fft_buffer.end(), 0.0F);
        const auto center_sample = static_cast<std::int64_t>(std::floor(
            static_cast<double>(frame) * static_cast<double>(info.sample_rate) /
            settings_.frame_rate));
        const auto first_sample = center_sample - kSyllableFftSize / 2;
        const auto read_start = std::max<std::int64_t>(0, first_sample);
        const auto read_end = std::min<std::int64_t>(
            info.sample_count, first_sample + kSyllableFftSize);
        const auto requested_samples =
            static_cast<int>(std::max<std::int64_t>(0, read_end - read_start));
        std::fill(left.begin(), left.end(), 0.0F);
        std::fill(right.begin(), right.end(), 0.0F);
        const auto samples_read =
            requested_samples > 0
                ? std::clamp(source.Read(read_start, requested_samples, left.data(),
                                         right.data()),
                             0, requested_samples)
                : 0;

        double squared_sum = 0.0;
        const auto destination_offset = static_cast<int>(read_start - first_sample);
        for (int index = 0; index < samples_read; ++index)
        {
            const auto mono = info.channel_count >= 2
                                  ? (left[static_cast<std::size_t>(index)] +
                                     right[static_cast<std::size_t>(index)]) *
                                        0.5F
                                  : left[static_cast<std::size_t>(index)];
            const auto window_index = destination_offset + index;
            const auto window =
                0.5F -
                0.5F * std::cos(2.0F * kPi * static_cast<float>(window_index) /
                                static_cast<float>(kSyllableFftSize - 1));
            const auto windowed = mono * window;
            fft_buffer[static_cast<std::size_t>(window_index)] = windowed;
            squared_sum += static_cast<double>(windowed) *
                           static_cast<double>(windowed);
        }
        const auto frame_activity =
            std::sqrt(squared_sum / static_cast<double>(kSyllableFftSize));
        raw_activity[static_cast<std::size_t>(frame)] = frame_activity;
        logarithmic_activity[static_cast<std::size_t>(frame)] =
            std::log1p(1000.0 * frame_activity);

        rdft(kSyllableFftSize, 1, fft_buffer.data(), work_int.data(),
             work_float.data());
        double positive_difference_sum = 0.0;
        for (int bin = first_frequency_bin; bin <= last_frequency_bin; ++bin)
        {
            const auto real = fft_buffer[static_cast<std::size_t>(bin * 2)];
            const auto imaginary =
                fft_buffer[static_cast<std::size_t>(bin * 2 + 1)];
            const auto magnitude = std::sqrt(real * real + imaginary * imaginary);
            const auto frequency =
                static_cast<double>(bin) * static_cast<double>(info.sample_rate) /
                static_cast<double>(kSyllableFftSize);
            const auto fingerprint_band = GetSpectralFingerprintBand(frequency);
            spectral_fingerprints[
                static_cast<std::size_t>(frame) * kSpectralFingerprintBandCount +
                static_cast<std::size_t>(fingerprint_band)] += magnitude;
            const auto spectrum_index =
                static_cast<std::size_t>(bin - first_frequency_bin);
            current_spectrum[spectrum_index] = std::log1p(magnitude);
            if (frame == 0)
            {
                continue;
            }
            auto previous_maximum = previous_spectrum[spectrum_index];
            if (spectrum_index > 0)
            {
                previous_maximum =
                    std::max(previous_maximum,
                             previous_spectrum[spectrum_index - 1]);
            }
            if (spectrum_index + 1 < previous_spectrum.size())
            {
                previous_maximum =
                    std::max(previous_maximum,
                             previous_spectrum[spectrum_index + 1]);
            }
            positive_difference_sum +=
                std::max(0.0, static_cast<double>(
                                  current_spectrum[spectrum_index] -
                                  previous_maximum));
        }
        if (frame > 0)
        {
            raw_novelty[static_cast<std::size_t>(frame)] =
                positive_difference_sum /
                static_cast<double>(frequency_bin_count);
        }
        const auto fingerprint_offset =
            static_cast<std::size_t>(frame) * kSpectralFingerprintBandCount;
        double fingerprint_sum = 0.0;
        for (int band = 0; band < kSpectralFingerprintBandCount; ++band)
        {
            fingerprint_sum += spectral_fingerprints[
                fingerprint_offset + static_cast<std::size_t>(band)];
        }
        if (fingerprint_sum > 0.0)
        {
            for (int band = 0; band < kSpectralFingerprintBandCount; ++band)
            {
                spectral_fingerprints[
                    fingerprint_offset + static_cast<std::size_t>(band)] /=
                    fingerprint_sum;
            }
        }
        previous_spectrum.swap(current_spectrum);
    }

    const auto activity = NormalizePercentiles(logarithmic_activity);
    const auto novelty = NormalizePercentiles(raw_novelty);

    std::vector<PeakCandidate> strong_candidates;
    strong_candidates.reserve(static_cast<std::size_t>(frame_count));
    for (int frame = 2; frame + 1 < frame_count; ++frame)
    {
        const auto index = static_cast<std::size_t>(frame);
        if (activity[index] < kSyllableActivityThreshold ||
            novelty[index] < novelty[index - 1] ||
            novelty[index] <= novelty[index + 1])
        {
            continue;
        }
        const auto first = std::max(0, frame - 6);
        const auto last = std::min(frame_count, frame + 7);
        std::vector<double> local_values;
        local_values.reserve(static_cast<std::size_t>(last - first - 1));
        for (auto neighbor = first; neighbor < last; ++neighbor)
        {
            if (neighbor != frame)
            {
                local_values.push_back(
                    novelty[static_cast<std::size_t>(neighbor)]);
            }
        }
        const auto baseline = GetMedian(std::move(local_values));
        if (novelty[index] >= baseline + kSyllableLocalDelta)
        {
            strong_candidates.push_back({frame, novelty[index]});
        }
    }

    const auto envelope =
        BuildSyllableEnvelope(settings_.pattern_count, settings_.frame_rate);
    if (envelope.empty())
    {
        return false;
    }
    const auto minimum_peak_distance = std::max(
        1, static_cast<int>(std::ceil(settings_.frame_rate *
                                      kSyllableMinimumPeakIntervalSeconds)));
    const auto selected = settings_.target_pulse_count > 0
                              ? SelectPeaksWithLimit(strong_candidates,
                                                     settings_.target_pulse_count,
                                                     minimum_peak_distance)
                              : SelectUnguidedPeaks(strong_candidates,
                                                    minimum_peak_distance);
    std::vector<PeakCandidate> maximum_open_peaks;
    if (settings_.pattern_count > 2)
    {
        const auto minimum_maximum_open_distance = std::max(
            1, static_cast<int>(std::ceil(
                   settings_.frame_rate * kMaximumOpenMinimumIntervalSeconds)));
        maximum_open_peaks =
            SelectMaximumOpenPeaks(selected, minimum_maximum_open_distance);
    }

    peak_frames_.clear();
    peak_frames_.reserve(selected.size());
    states_.assign(static_cast<std::size_t>(frame_count), 0);
    std::size_t maximum_open_peak_index = 0;
    for (std::size_t peak_index = 0; peak_index < selected.size(); ++peak_index)
    {
        const auto &peak = selected[peak_index];
        const auto is_maximum_open_peak =
            settings_.pattern_count == 2 ||
            (maximum_open_peak_index < maximum_open_peaks.size() &&
             maximum_open_peaks[maximum_open_peak_index].frame == peak.frame);
        if (settings_.pattern_count > 2 && is_maximum_open_peak)
        {
            ++maximum_open_peak_index;
        }
        peak_frames_.push_back(peak.frame);
        const auto desired_first_open_frame =
            peak.frame - static_cast<int>((envelope.size() - 1) / 2);
        auto first_open_frame = std::max(0, desired_first_open_frame);
        auto last_open_frame = std::min(
            frame_count - 1,
            desired_first_open_frame + static_cast<int>(envelope.size()) - 1);
        if (peak_index > 0)
        {
            const auto previous_closed_frame =
                (selected[peak_index - 1].frame + peak.frame + 1) / 2;
            first_open_frame = std::max(first_open_frame, previous_closed_frame + 1);
        }
        if (peak_index + 1 < selected.size())
        {
            const auto next_closed_frame =
                (peak.frame + selected[peak_index + 1].frame + 1) / 2;
            last_open_frame = std::min(last_open_frame, next_closed_frame - 1);
        }
        for (auto frame = first_open_frame; frame <= last_open_frame; ++frame)
        {
            const auto envelope_index =
                static_cast<std::size_t>(frame - desired_first_open_frame);
            auto envelope_state = envelope[envelope_index];
            if (!is_maximum_open_peak &&
                envelope_state == settings_.pattern_count - 1)
            {
                envelope_state = envelope.front();
            }
            states_[static_cast<std::size_t>(frame)] = envelope_state;
        }
    }

    if (settings_.pattern_count > 2)
    {
        const auto bridge_state = envelope.front();
        const auto maximum_bridge_distance = std::max(
            1, static_cast<int>(std::ceil(settings_.frame_rate *
                                          kSyllableIntermediateBridgeSeconds)));
        for (std::size_t peak_index = 1; peak_index < selected.size(); ++peak_index)
        {
            const auto previous_peak = selected[peak_index - 1].frame;
            const auto current_peak = selected[peak_index].frame;
            if (current_peak - previous_peak > maximum_bridge_distance)
            {
                continue;
            }
            for (auto frame = previous_peak + 1; frame < current_peak; ++frame)
            {
                auto &state = states_[static_cast<std::size_t>(frame)];
                if (state == 0)
                {
                    state = bridge_state;
                }
            }
        }

        const auto maximum_open_hold_frames = std::max(
            1, static_cast<int>(std::ceil(
                   settings_.frame_rate * kMaximumOpenHoldSeconds)));
        const auto intermediate_hold_frames = std::max(
            1, static_cast<int>(std::lround(settings_.frame_rate / 30.0)));
        for (const auto &peak : maximum_open_peaks)
        {
            const auto first_maximum_open_frame =
                peak.frame - (maximum_open_hold_frames - 1) / 2;
            const auto last_maximum_open_frame =
                first_maximum_open_frame + maximum_open_hold_frames - 1;
            const auto first_gesture_frame =
                first_maximum_open_frame - intermediate_hold_frames;
            const auto last_gesture_frame =
                last_maximum_open_frame + intermediate_hold_frames;
            for (auto frame = std::max(0, first_gesture_frame);
                 frame <= std::min(frame_count - 1, last_gesture_frame); ++frame)
            {
                auto &state = states_[static_cast<std::size_t>(frame)];
                if (frame >= first_maximum_open_frame &&
                    frame <= last_maximum_open_frame)
                {
                    state = settings_.pattern_count - 1;
                }
                else
                {
                    state = std::max(state, bridge_state);
                }
            }
        }
    }

    if (settings_.pattern_count > 2)
    {
        const auto maximum_continuous_distance = std::max(
            1, static_cast<int>(std::floor(
                   settings_.frame_rate * kContinuousPeakMaximumIntervalSeconds)));
        const auto get_average_fingerprint = [&](int peak_frame) {
            std::array<double, kSpectralFingerprintBandCount> average{};
            const auto last_frame = std::min(frame_count - 1, peak_frame + 2);
            const auto average_count = last_frame - peak_frame + 1;
            for (auto frame = peak_frame; frame <= last_frame; ++frame)
            {
                const auto offset =
                    static_cast<std::size_t>(frame) * kSpectralFingerprintBandCount;
                for (int band = 0; band < kSpectralFingerprintBandCount; ++band)
                {
                    average[static_cast<std::size_t>(band)] +=
                        spectral_fingerprints[
                            offset + static_cast<std::size_t>(band)];
                }
            }
            for (auto &value : average)
            {
                value /= static_cast<double>(average_count);
            }
            return average;
        };
        for (std::size_t peak_index = 1; peak_index < selected.size(); ++peak_index)
        {
            const auto previous_peak = selected[peak_index - 1].frame;
            const auto current_peak = selected[peak_index].frame;
            if (current_peak - previous_peak > maximum_continuous_distance)
            {
                continue;
            }

            const auto weaker_peak_activity = std::min(
                raw_activity[static_cast<std::size_t>(previous_peak)],
                raw_activity[static_cast<std::size_t>(current_peak)]);
            if (weaker_peak_activity <= 0.0)
            {
                continue;
            }
            auto valley_activity = weaker_peak_activity;
            for (auto frame = previous_peak + 1; frame < current_peak; ++frame)
            {
                valley_activity = std::min(
                    valley_activity,
                    raw_activity[static_cast<std::size_t>(frame)]);
            }
            if (valley_activity / weaker_peak_activity <
                kContinuousPeakMinimumValleyRatio)
            {
                continue;
            }

            const auto previous_fingerprint =
                get_average_fingerprint(previous_peak);
            const auto current_fingerprint = get_average_fingerprint(current_peak);
            double fingerprint_distance = 0.0;
            for (int band = 0; band < kSpectralFingerprintBandCount; ++band)
            {
                fingerprint_distance += std::abs(
                    previous_fingerprint[static_cast<std::size_t>(band)] -
                    current_fingerprint[static_cast<std::size_t>(band)]);
            }
            if (fingerprint_distance >
                kContinuousPeakMaximumFingerprintDistance)
            {
                continue;
            }

            const auto open_state = settings_.pattern_count - 1;
            for (auto frame = previous_peak; frame <= current_peak; ++frame)
            {
                states_[static_cast<std::size_t>(frame)] = open_state;
            }
        }
    }

    if (settings_.pattern_count > 2)
    {
        const auto bridge_state = envelope.front();
        const auto maximum_flicker_frames = std::max(
            1, static_cast<int>(std::floor(
                   settings_.frame_rate * kClosedFlickerMaximumSeconds)));
        auto frame = 1;
        while (frame + 1 < frame_count)
        {
            if (states_[static_cast<std::size_t>(frame)] != 0)
            {
                ++frame;
                continue;
            }
            const auto first_closed_frame = frame;
            while (frame < frame_count &&
                   states_[static_cast<std::size_t>(frame)] == 0)
            {
                ++frame;
            }
            const auto closed_frame_count = frame - first_closed_frame;
            if (frame >= frame_count ||
                closed_frame_count > maximum_flicker_frames ||
                states_[static_cast<std::size_t>(first_closed_frame - 1)] == 0)
            {
                continue;
            }
            for (auto closed_frame = first_closed_frame;
                 closed_frame < frame; ++closed_frame)
            {
                states_[static_cast<std::size_t>(closed_frame)] = bridge_state;
            }
        }
    }

    if (selected.empty())
    {
        PatternStateSequence legacy_sequence({
            settings_.frame_rate,
            settings_.low_cut_hz,
            settings_.high_cut_hz,
            settings_.threshold,
            settings_.sensitivity_frames,
            settings_.speed_frames,
            settings_.pattern_count,
        });
        for (int frame = 0; frame < frame_count; ++frame)
        {
            const auto legacy_state = legacy_sequence.GetState(source, frame);
            if (legacy_state < 0)
            {
                return false;
            }
            states_[static_cast<std::size_t>(frame)] = legacy_state;
        }
    }

    is_built_ = true;
    return true;
}

} // namespace aviutl1_lipsync
