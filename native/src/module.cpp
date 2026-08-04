#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "cache2.h"
#include "japanese_reading.h"
#include "lipsync_analyzer.h"
#include "module2.h"
#include "wav_audio_source.h"

namespace
{

CACHE_HANDLE *g_cache = nullptr;
constexpr std::size_t kMaximumStateSequences = 128;
constexpr std::size_t kMaximumWavSources = 128;
constexpr std::size_t kMaximumReadingCounts = 512;
std::mutex g_state_sequences_mutex;
std::mutex g_reading_counts_mutex;
std::unordered_map<std::wstring,
                   std::unique_ptr<aviutl1_lipsync::PatternStateSequence>>
    g_state_sequences;
std::unordered_map<std::wstring,
                   std::unique_ptr<aviutl1_lipsync::AdaptivePatternStateSequence>>
    g_adaptive_state_sequences;
std::unordered_map<std::wstring, std::shared_ptr<aviutl1_lipsync::WavAudioSource>>
    g_wav_sources;
std::unordered_map<std::wstring, int> g_reading_counts;

std::wstring Utf8ToWide(const char *value)
{
    if (value == nullptr || value[0] == '\0')
    {
        return {};
    }
    const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, nullptr, 0);
    if (length <= 0)
    {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, result.data(), length) <= 0)
    {
        return {};
    }
    result.resize(static_cast<std::size_t>(length - 1));
    return result;
}

std::wstring MultiByteToWide(const char *value, int length, unsigned int code_page,
                             unsigned long flags)
{
    if (value == nullptr || length <= 0)
    {
        return {};
    }
    const auto wide_length =
        MultiByteToWideChar(code_page, flags, value, length, nullptr, 0);
    if (wide_length <= 0)
    {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(wide_length), L'\0');
    if (MultiByteToWideChar(code_page, flags, value, length, result.data(),
                            wide_length) != wide_length)
    {
        return {};
    }
    return result;
}

std::wstring ReadSidecarText(const std::filesystem::path &path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        return {};
    }
    const std::vector<char> bytes((std::istreambuf_iterator<char>(stream)),
                                  std::istreambuf_iterator<char>());
    if (bytes.empty())
    {
        return {};
    }

    std::wstring text;
    const auto *data = reinterpret_cast<const unsigned char *>(bytes.data());
    if (bytes.size() >= 2 && data[0] == 0xFF && data[1] == 0xFE)
    {
        for (std::size_t index = 2; index + 1 < bytes.size(); index += 2)
        {
            text.push_back(static_cast<wchar_t>(
                data[index] | static_cast<unsigned int>(data[index + 1]) << 8));
        }
    }
    else if (bytes.size() >= 2 && data[0] == 0xFE && data[1] == 0xFF)
    {
        for (std::size_t index = 2; index + 1 < bytes.size(); index += 2)
        {
            text.push_back(static_cast<wchar_t>(
                static_cast<unsigned int>(data[index]) << 8 | data[index + 1]));
        }
    }
    else
    {
        const auto has_utf8_bom =
            bytes.size() >= 3 && data[0] == 0xEF && data[1] == 0xBB &&
            data[2] == 0xBF;
        const auto offset = has_utf8_bom ? 3 : 0;
        const auto *content = bytes.data() + offset;
        const auto length = static_cast<int>(bytes.size() - offset);
        text = MultiByteToWide(content, length, CP_UTF8, MB_ERR_INVALID_CHARS);
        if (text.empty() && length > 0)
        {
            text = MultiByteToWide(content, length, 932, 0);
        }
    }
    text.erase(std::remove_if(text.begin(), text.end(),
                              [](wchar_t character) {
                                  return character == L'\r' || character == L'\n' ||
                                         character == L'\0';
                              }),
               text.end());
    return text;
}

int ResolveTargetPulseCount(const std::wstring &audio_path)
{
    auto sidecar_path = std::filesystem::path(audio_path);
    sidecar_path.replace_extension(L".txt");
    std::error_code error;
    const auto file_size = std::filesystem::file_size(sidecar_path, error);
    if (error)
    {
        return 0;
    }
    const auto write_time = std::filesystem::last_write_time(sidecar_path, error);
    if (error)
    {
        return 0;
    }

    std::wostringstream identity_stream;
    identity_stream << sidecar_path.native() << L'\n' << file_size << L'\n'
                    << write_time.time_since_epoch().count();
    const auto identity = identity_stream.str();
    {
        std::lock_guard<std::mutex> lock(g_reading_counts_mutex);
        const auto cached = g_reading_counts.find(identity);
        if (cached != g_reading_counts.end())
        {
            return cached->second;
        }
    }

    int count = 0;
    try
    {
        const auto text = ReadSidecarText(sidecar_path);
        const auto reading = aviutl1_lipsync::GetJapaneseKatakanaReading(text);
        count = aviutl1_lipsync::CountJapaneseSyllables(reading);
    }
    catch (const std::exception &)
    {
        count = 0;
    }

    std::lock_guard<std::mutex> lock(g_reading_counts_mutex);
    if (g_reading_counts.size() >= kMaximumReadingCounts)
    {
        g_reading_counts.clear();
    }
    g_reading_counts.emplace(identity, count);
    return count;
}

class CacheAudioSource final : public aviutl1_lipsync::AudioSource
{
  public:
    CacheAudioSource(std::wstring path, int track, AUDIO_INFO info)
        : path_(std::move(path)), track_(track), info_(info)
    {
    }

    aviutl1_lipsync::AudioInfo GetInfo() const override
    {
        return {
            info_.sample_num,
            info_.rate,
            info_.channel,
        };
    }

    int Read(std::int64_t sample_index, int sample_count, float *left, float *right) const override
    {
        if (g_cache == nullptr || sample_index < 0 || sample_count <= 0)
        {
            return 0;
        }
        return g_cache->get_audio_file_data(path_.c_str(), track_, sample_index, sample_count, left,
                                            right);
    }

  private:
    std::wstring path_;
    int track_;
    AUDIO_INFO info_;
};

struct ResolvedAudioSource
{
    std::shared_ptr<aviutl1_lipsync::AudioSource> source;
    aviutl1_lipsync::AudioInfo info;
    std::wstring identity;
};

std::wstring BuildCacheIdentity(const std::wstring &path, int track,
                                const AUDIO_INFO &info)
{
    std::wostringstream stream;
    stream << L"cache\n" << path << L'\n' << track << L'\n' << info.sample_num << L'\n'
           << info.rate << L'\n' << info.channel;
    return stream.str();
}

ResolvedAudioSource ResolveAudioSource(const std::wstring &path, int track,
                                       const AUDIO_INFO &cache_info)
{
    if (track == 0 && _wcsicmp(std::filesystem::path(path).extension().c_str(), L".wav") == 0)
    {
        std::error_code error;
        const auto file_size = std::filesystem::file_size(path, error);
        if (!error)
        {
            const auto write_time = std::filesystem::last_write_time(path, error);
            if (!error)
            {
                std::wostringstream identity_stream;
                identity_stream << L"wav\n" << path << L'\n' << file_size << L'\n'
                                << write_time.time_since_epoch().count();
                const auto identity = identity_stream.str();

                std::lock_guard<std::mutex> lock(g_state_sequences_mutex);
                auto source = g_wav_sources.find(identity);
                if (source == g_wav_sources.end())
                {
                    try
                    {
                        if (g_wav_sources.size() >= kMaximumWavSources)
                        {
                            g_wav_sources.clear();
                        }
                        source =
                            g_wav_sources
                                .emplace(identity,
                                         std::make_shared<aviutl1_lipsync::WavAudioSource>(
                                             std::filesystem::path(path)))
                                .first;
                    }
                    catch (const std::exception &)
                    {
                        source = g_wav_sources.end();
                    }
                }
                if (source != g_wav_sources.end())
                {
                    return {source->second, source->second->GetInfo(), identity};
                }
            }
        }
    }

    auto source = std::make_shared<CacheAudioSource>(path, track, cache_info);
    return {
        source,
        source->GetInfo(),
        BuildCacheIdentity(path, track, cache_info),
    };
}

void GetLevel(SCRIPT_MODULE_PARAM *param)
{
    if (param->get_param_num() < 4 || param->get_param_num() > 5)
    {
        param->set_error(
            u8"get_level(file, position, low_cut, high_cut [, track]) requires 4 or 5 arguments");
        return;
    }
    if (g_cache == nullptr)
    {
        param->set_error(u8"AviUtl2 audio cache is unavailable");
        return;
    }

    const auto path = Utf8ToWide(param->get_param_string(0));
    const auto position = param->get_param_double(1);
    const auto low_cut = param->get_param_double(2);
    const auto high_cut = param->get_param_double(3);
    const auto track = param->get_param_num() >= 5 ? param->get_param_int(4) : 0;
    if (path.empty())
    {
        param->set_error(u8"file must be a valid UTF-8 path");
        return;
    }

    AUDIO_INFO info{};
    if (!g_cache->get_audio_file_info(path.c_str(), &info, sizeof(info)))
    {
        param->set_error(u8"failed to read audio file information");
        return;
    }
    if (track < 0 || track >= info.track_num)
    {
        param->set_error(u8"audio track is out of range");
        return;
    }

    const auto resolved = ResolveAudioSource(path, track, info);
    const aviutl1_lipsync::Analyzer analyzer;
    const auto raw_level =
        analyzer.GetLevel(*resolved.source, position, low_cut, high_cut);
    if (!std::isfinite(raw_level))
    {
        param->set_error(u8"invalid lip-sync analysis parameters");
        return;
    }

    // AviUtl1's Lua TalkState multiplied the native result by 100.
    param->push_result_double(raw_level * 100.0);
}

std::wstring BuildStateSequenceKey(const ResolvedAudioSource &source,
                                   const aviutl1_lipsync::PatternSettings &settings)
{
    std::wostringstream stream;
    stream << std::setprecision(17) << source.identity << L'\n'
           << source.info.sample_count << L'\n' << source.info.sample_rate << L'\n'
           << source.info.channel_count << L'\n' << settings.frame_rate << L'\n'
           << settings.low_cut_hz << L'\n' << settings.high_cut_hz << L'\n'
           << settings.threshold << L'\n' << settings.sensitivity_frames << L'\n'
           << settings.speed_frames << L'\n' << settings.pattern_count << L'\n'
           << settings.target_pulse_count;
    return stream.str();
}

void GetPatternState(SCRIPT_MODULE_PARAM *param)
{
    if (param->get_param_num() < 9 || param->get_param_num() > 10)
    {
        param->set_error(u8"get_state(file, position, frame_rate, low_cut, high_cut, threshold, "
                         u8"sensitivity, speed, pattern_count [, track]) requires 9 or 10 "
                         u8"arguments");
        return;
    }
    if (g_cache == nullptr)
    {
        param->set_error(u8"AviUtl2 audio cache is unavailable");
        return;
    }

    const auto path = Utf8ToWide(param->get_param_string(0));
    const auto position = param->get_param_double(1);
    aviutl1_lipsync::PatternSettings settings{
        param->get_param_double(2),
        param->get_param_double(3),
        param->get_param_double(4),
        param->get_param_double(5),
        param->get_param_int(6),
        param->get_param_double(7),
        param->get_param_int(8),
    };
    const auto track = param->get_param_num() >= 10 ? param->get_param_int(9) : 0;
    aviutl1_lipsync::PatternStateSequence candidate(settings);
    if (path.empty())
    {
        param->set_error(u8"file must be a valid UTF-8 path");
        return;
    }
    if (!std::isfinite(position) || position < 0.0 || !candidate.IsValid())
    {
        param->set_error(u8"invalid AviUtl1 lip-sync state parameters");
        return;
    }

    const auto frame_value = std::floor(position * settings.frame_rate + 1.0e-6);
    if (frame_value < 0.0 ||
        frame_value > static_cast<double>(std::numeric_limits<int>::max()))
    {
        param->set_error(u8"lip-sync frame is out of range");
        return;
    }

    AUDIO_INFO info{};
    if (!g_cache->get_audio_file_info(path.c_str(), &info, sizeof(info)))
    {
        param->set_error(u8"failed to read audio file information");
        return;
    }
    if (track < 0 || track >= info.track_num)
    {
        param->set_error(u8"audio track is out of range");
        return;
    }

    const auto source = ResolveAudioSource(path, track, info);
    const auto key = BuildStateSequenceKey(source, settings);
    std::lock_guard<std::mutex> lock(g_state_sequences_mutex);
    auto sequence = g_state_sequences.find(key);
    if (sequence == g_state_sequences.end())
    {
        if (g_state_sequences.size() >= kMaximumStateSequences)
        {
            g_state_sequences.clear();
        }
        sequence = g_state_sequences
                       .emplace(key, std::make_unique<aviutl1_lipsync::PatternStateSequence>(
                                         settings))
                       .first;
    }

    const auto state =
        sequence->second->GetState(*source.source, static_cast<int>(frame_value));
    if (state < 0)
    {
        param->set_error(u8"failed to calculate AviUtl1 lip-sync state");
        return;
    }
    param->push_result_int(state);
}

void GetSyllablePatternState(SCRIPT_MODULE_PARAM *param)
{
    if (param->get_param_num() < 9 || param->get_param_num() > 10)
    {
        param->set_error(u8"get_syllable_state(file, position, frame_rate, low_cut, high_cut, "
                         u8"threshold, sensitivity, speed, pattern_count [, track]) requires "
                         u8"9 or 10 arguments");
        return;
    }
    if (g_cache == nullptr)
    {
        param->set_error(u8"AviUtl2 audio cache is unavailable");
        return;
    }

    const auto path = Utf8ToWide(param->get_param_string(0));
    const auto position = param->get_param_double(1);
    aviutl1_lipsync::PatternSettings settings{
        param->get_param_double(2),
        param->get_param_double(3),
        param->get_param_double(4),
        param->get_param_double(5),
        param->get_param_int(6),
        param->get_param_double(7),
        param->get_param_int(8),
    };
    const auto track = param->get_param_num() >= 10 ? param->get_param_int(9) : 0;
    if (path.empty())
    {
        param->set_error(u8"file must be a valid UTF-8 path");
        return;
    }
    settings.target_pulse_count = ResolveTargetPulseCount(path);
    aviutl1_lipsync::AdaptivePatternStateSequence candidate(settings);
    if (!std::isfinite(position) || position < 0.0 || !candidate.IsValid())
    {
        param->set_error(u8"invalid syllable lip-sync state parameters");
        return;
    }

    const auto frame_value = std::floor(position * settings.frame_rate + 1.0e-6);
    if (frame_value < 0.0 ||
        frame_value > static_cast<double>(std::numeric_limits<int>::max()))
    {
        param->set_error(u8"lip-sync frame is out of range");
        return;
    }

    AUDIO_INFO info{};
    if (!g_cache->get_audio_file_info(path.c_str(), &info, sizeof(info)))
    {
        param->set_error(u8"failed to read audio file information");
        return;
    }
    if (track < 0 || track >= info.track_num)
    {
        param->set_error(u8"audio track is out of range");
        return;
    }

    const auto source = ResolveAudioSource(path, track, info);
    const auto key = L"syllable\n" + BuildStateSequenceKey(source, settings);
    std::lock_guard<std::mutex> lock(g_state_sequences_mutex);
    auto sequence = g_adaptive_state_sequences.find(key);
    if (sequence == g_adaptive_state_sequences.end())
    {
        if (g_adaptive_state_sequences.size() >= kMaximumStateSequences)
        {
            g_adaptive_state_sequences.clear();
        }
        sequence =
            g_adaptive_state_sequences
                .emplace(
                    key,
                    std::make_unique<aviutl1_lipsync::AdaptivePatternStateSequence>(settings))
                .first;
    }

    const auto state =
        sequence->second->GetState(*source.source, static_cast<int>(frame_value));
    if (state < 0)
    {
        param->set_error(u8"failed to calculate syllable lip-sync state");
        return;
    }
    param->push_result_int(state);
}

void GetSyllablePulse(SCRIPT_MODULE_PARAM *param)
{
    if (param->get_param_num() < 9 || param->get_param_num() > 10)
    {
        param->set_error(u8"get_syllable_pulse(file, position, low_cut, high_cut, threshold, "
                         u8"frame_rate, peak_radius, open_frames, lead_frames [, track]) "
                         u8"requires 9 or 10 arguments");
        return;
    }
    if (g_cache == nullptr)
    {
        param->set_error(u8"AviUtl2 audio cache is unavailable");
        return;
    }

    const auto path = Utf8ToWide(param->get_param_string(0));
    const auto position = param->get_param_double(1);
    const auto low_cut = param->get_param_double(2);
    const auto high_cut = param->get_param_double(3);
    const auto threshold = param->get_param_double(4);
    const auto frame_rate = param->get_param_double(5);
    const auto peak_radius = param->get_param_int(6);
    const auto open_frames = param->get_param_int(7);
    const auto lead_frames = param->get_param_int(8);
    const auto track = param->get_param_num() >= 10 ? param->get_param_int(9) : 0;
    if (path.empty())
    {
        param->set_error(u8"file must be a valid UTF-8 path");
        return;
    }

    AUDIO_INFO info{};
    if (!g_cache->get_audio_file_info(path.c_str(), &info, sizeof(info)))
    {
        param->set_error(u8"failed to read audio file information");
        return;
    }
    if (track < 0 || track >= info.track_num)
    {
        param->set_error(u8"audio track is out of range");
        return;
    }

    const auto source = ResolveAudioSource(path, track, info);
    const aviutl1_lipsync::Analyzer analyzer;
    const auto pulse = analyzer.GetSyllablePulse(
        *source.source, position, frame_rate, low_cut, high_cut, threshold, peak_radius, open_frames,
        lead_frames);
    if (!std::isfinite(pulse))
    {
        param->set_error(u8"invalid syllable-pulse analysis parameters");
        return;
    }
    param->push_result_double(pulse);
}

void GetVersion(SCRIPT_MODULE_PARAM *param)
{
    param->push_result_string("2.0.0");
}

SCRIPT_MODULE_FUNCTION kFunctions[] = {
    {L"get_level", GetLevel},
    {L"get_state", GetPatternState},
    {L"get_syllable_state", GetSyllablePatternState},
    {L"get_adaptive_state", GetSyllablePatternState},
    {L"get_syllable_pulse", GetSyllablePulse},
    {L"get_version", GetVersion},
    {nullptr},
};

SCRIPT_MODULE_TABLE kModuleTable = {
    L"AviUtl1-compatible PSDToolKit syllable lip-sync analyzer 2.0.0",
    kFunctions,
};

} // namespace

EXTERN_C __declspec(dllexport) void InitializeCache(CACHE_HANDLE *cache)
{
    g_cache = cache;
}

EXTERN_C __declspec(dllexport) bool InitializePlugin(DWORD)
{
    return true;
}

EXTERN_C __declspec(dllexport) void UninitializePlugin()
{
    {
        std::lock_guard<std::mutex> lock(g_state_sequences_mutex);
        g_state_sequences.clear();
        g_adaptive_state_sequences.clear();
        g_wav_sources.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_reading_counts_mutex);
        g_reading_counts.clear();
    }
    g_cache = nullptr;
}

EXTERN_C __declspec(dllexport) SCRIPT_MODULE_TABLE *GetScriptModuleTable()
{
    return &kModuleTable;
}
