#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace aviutl1_lipsync
{

inline constexpr int kTargetSampleRate = 24000;
inline constexpr int kWindowSize = 256;

struct AudioInfo
{
    std::int64_t sample_count = 0;
    int sample_rate = 0;
    int channel_count = 0;
};

class AudioSource
{
  public:
    virtual ~AudioSource() = default;

    virtual AudioInfo GetInfo() const = 0;
    virtual int Read(std::int64_t sample_index, int sample_count, float *left,
                     float *right) const = 0;
};

class Analyzer
{
  public:
    double GetLevel(const AudioSource &source, double position_seconds, double low_cut_hz,
                    double high_cut_hz) const;

    double GetSyllablePulse(const AudioSource &source, double position_seconds,
                            double frame_rate, double low_cut_hz, double high_cut_hz,
                            double threshold, int peak_radius_frames, int open_frames,
                            int lead_frames) const;
};

struct PatternSettings
{
    double frame_rate = 0.0;
    double low_cut_hz = 0.0;
    double high_cut_hz = 0.0;
    double threshold = 0.0;
    int sensitivity_frames = 1;
    double speed_frames = 1.0;
    int pattern_count = 2;
    int target_pulse_count = 0;
};

class PatternStateSequence
{
  public:
    explicit PatternStateSequence(PatternSettings settings);

    bool IsValid() const;
    int GetState(const AudioSource &source, int frame_index);
    std::size_t GetCachedFrameCount() const;

  private:
    bool AppendNextState(const AudioSource &source);

    PatternSettings settings_;
    Analyzer analyzer_;
    std::vector<int> states_;
    std::vector<double> volumes_;
    int update_counter_ = -1;
    int pattern_ = 0;
};

class AdaptivePatternStateSequence
{
  public:
    explicit AdaptivePatternStateSequence(PatternSettings settings);

    bool IsValid() const;
    int GetState(const AudioSource &source, int frame_index);
    std::size_t GetCachedFrameCount() const;
    const std::vector<int> &GetPeakFrames() const;

  private:
    bool BuildStates(const AudioSource &source);

    PatternSettings settings_;
    Analyzer analyzer_;
    std::vector<int> states_;
    std::vector<int> peak_frames_;
    bool is_built_ = false;
};

} // namespace aviutl1_lipsync
