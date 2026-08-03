#pragma once

#include <filesystem>
#include <vector>

#include "lipsync_analyzer.h"

namespace aviutl1_lipsync
{

class WavAudioSource final : public AudioSource
{
  public:
    explicit WavAudioSource(const std::filesystem::path &path);

    AudioInfo GetInfo() const override;
    int Read(std::int64_t sample_index, int sample_count, float *left,
             float *right) const override;

  private:
    int sample_rate_ = 0;
    int channel_count_ = 0;
    std::vector<float> left_;
    std::vector<float> right_;
};

} // namespace aviutl1_lipsync
