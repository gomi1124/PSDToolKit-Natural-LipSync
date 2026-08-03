#include "lipsync_analyzer.h"
#include "wav_audio_source.h"

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

int wmain(int argc, wchar_t **argv)
{
    try
    {
        if (argc < 2 || argc > 5)
        {
            std::wcerr << L"Usage: LipSyncAviUtl1WavProbe <wav> [fps] [low-cut] [high-cut]\n";
            return 2;
        }
        const auto fps = argc >= 3 ? std::stod(argv[2]) : 60.0;
        const auto low_cut = argc >= 4 ? std::stod(argv[3]) : 100.0;
        const auto high_cut = argc >= 5 ? std::stod(argv[4]) : 1000.0;
        if (fps <= 0.0)
        {
            throw std::runtime_error("fps must be positive");
        }

        aviutl1_lipsync::WavAudioSource source(argv[1]);
        const auto info = source.GetInfo();
        const auto frame_count = static_cast<std::int64_t>(
            static_cast<double>(info.sample_count) * fps / info.sample_rate);
        const aviutl1_lipsync::Analyzer analyzer;
        std::cout << "frame\ttime\tlevel\n" << std::fixed << std::setprecision(9);
        for (std::int64_t frame = 0; frame <= frame_count; ++frame)
        {
            const auto time = static_cast<double>(frame) / fps;
            std::cout << frame << '\t' << time << '\t'
                      << analyzer.GetLevel(source, time, low_cut, high_cut) * 100.0 << '\n';
        }
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "ERROR: " << error.what() << '\n';
        return 1;
    }
}
