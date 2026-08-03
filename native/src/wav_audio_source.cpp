#include "wav_audio_source.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace aviutl1_lipsync
{
namespace
{

std::uint16_t ReadU16(std::istream &stream)
{
    std::uint8_t bytes[2]{};
    stream.read(reinterpret_cast<char *>(bytes), sizeof(bytes));
    return static_cast<std::uint16_t>(bytes[0]) |
           (static_cast<std::uint16_t>(bytes[1]) << 8U);
}

std::uint32_t ReadU32(std::istream &stream)
{
    std::uint8_t bytes[4]{};
    stream.read(reinterpret_cast<char *>(bytes), sizeof(bytes));
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

std::string ReadId(std::istream &stream)
{
    char id[4]{};
    stream.read(id, sizeof(id));
    return std::string(id, sizeof(id));
}

} // namespace

WavAudioSource::WavAudioSource(const std::filesystem::path &path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        throw std::runtime_error("failed to open WAV file");
    }
    if (ReadId(stream) != "RIFF")
    {
        throw std::runtime_error("input is not a RIFF file");
    }
    (void)ReadU32(stream);
    if (ReadId(stream) != "WAVE")
    {
        throw std::runtime_error("input is not a WAVE file");
    }

    std::uint16_t format = 0;
    std::uint16_t bits_per_sample = 0;
    std::uint16_t block_align = 0;
    std::vector<std::uint8_t> pcm;
    while (stream && (!format || pcm.empty()))
    {
        const auto id = ReadId(stream);
        const auto size = ReadU32(stream);
        if (!stream)
        {
            break;
        }
        if (id == "fmt ")
        {
            format = ReadU16(stream);
            channel_count_ = ReadU16(stream);
            sample_rate_ = static_cast<int>(ReadU32(stream));
            (void)ReadU32(stream);
            block_align = ReadU16(stream);
            bits_per_sample = ReadU16(stream);
            if (size > 16)
            {
                stream.seekg(static_cast<std::streamoff>(size - 16), std::ios::cur);
            }
        }
        else if (id == "data")
        {
            pcm.resize(size);
            stream.read(reinterpret_cast<char *>(pcm.data()),
                        static_cast<std::streamsize>(pcm.size()));
        }
        else
        {
            stream.seekg(static_cast<std::streamoff>(size), std::ios::cur);
        }
        if ((size & 1U) != 0U)
        {
            stream.seekg(1, std::ios::cur);
        }
    }

    if (format != 1 || bits_per_sample != 16 ||
        (channel_count_ != 1 && channel_count_ != 2) || sample_rate_ <= 0 ||
        block_align != channel_count_ * sizeof(std::int16_t) || pcm.empty())
    {
        throw std::runtime_error("only 16-bit mono/stereo PCM WAV is supported");
    }

    const auto frame_count = pcm.size() / block_align;
    left_.resize(frame_count);
    if (channel_count_ == 2)
    {
        right_.resize(frame_count);
    }
    for (std::size_t frame = 0; frame < frame_count; ++frame)
    {
        const auto offset = frame * block_align;
        std::int16_t left = 0;
        std::memcpy(&left, pcm.data() + offset, sizeof(left));
        left_[frame] = static_cast<float>(left) / 32768.0F;
        if (channel_count_ == 2)
        {
            std::int16_t right = 0;
            std::memcpy(&right, pcm.data() + offset + sizeof(left), sizeof(right));
            right_[frame] = static_cast<float>(right) / 32768.0F;
        }
    }
}

AudioInfo WavAudioSource::GetInfo() const
{
    return {static_cast<std::int64_t>(left_.size()), sample_rate_, channel_count_};
}

int WavAudioSource::Read(std::int64_t sample_index, int sample_count, float *left,
                         float *right) const
{
    if (sample_index < 0 || sample_index >= static_cast<std::int64_t>(left_.size()) ||
        sample_count <= 0)
    {
        return 0;
    }
    const auto available = static_cast<int>(std::min<std::int64_t>(
        sample_count, static_cast<std::int64_t>(left_.size()) - sample_index));
    for (int i = 0; i < available; ++i)
    {
        const auto index = static_cast<std::size_t>(sample_index + i);
        left[i] = left_[index];
        right[i] = channel_count_ == 2 ? right_[index] : 0.0F;
    }
    return available;
}

} // namespace aviutl1_lipsync
