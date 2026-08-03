#include "lipsync_analyzer.h"
#include "wav_audio_source.h"

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int wmain(int argc, wchar_t **argv)
{
    try
    {
        if (argc < 2 || argc > 8)
        {
            std::wcerr << L"Usage: LipSyncAdaptiveWavProbe <wav> [fps] [low-cut] [high-cut] "
                          L"[threshold] [pattern-count] [target-pulse-count]\n";
            return 2;
        }
        const auto fps = argc >= 3 ? std::stod(argv[2]) : 60.0;
        const auto low_cut = argc >= 4 ? std::stod(argv[3]) : 100.0;
        const auto high_cut = argc >= 5 ? std::stod(argv[4]) : 1000.0;
        const auto threshold = argc >= 6 ? std::stod(argv[5]) : 20.0;
        const auto pattern_count = argc >= 7 ? std::stoi(argv[6]) : 2;
        const auto target_pulse_count = argc >= 8 ? std::stoi(argv[7]) : 0;

        aviutl1_lipsync::WavAudioSource source(argv[1]);
        const auto info = source.GetInfo();
        const auto frame_count = static_cast<int>(
                                     static_cast<double>(info.sample_count) * fps /
                                     static_cast<double>(info.sample_rate)) +
                                 1;
        const aviutl1_lipsync::PatternSettings settings{
            fps,
            low_cut,
            high_cut,
            threshold,
            1,
            1.0,
            pattern_count,
            target_pulse_count,
        };
        aviutl1_lipsync::AdaptivePatternStateSequence sequence(settings);
        aviutl1_lipsync::PatternStateSequence legacy_sequence(settings);
        if (!sequence.IsValid() || sequence.GetState(source, frame_count - 1) < 0)
        {
            throw std::runtime_error("failed to build adaptive state sequence");
        }

        const auto &peaks = sequence.GetPeakFrames();
        std::cout << "frame\ttime\tstate\tlegacy_state\tpeak\n"
                  << std::fixed << std::setprecision(9);
        for (auto frame = 0; frame < frame_count; ++frame)
        {
            const auto is_peak = std::binary_search(peaks.begin(), peaks.end(), frame);
            const auto legacy_state = legacy_sequence.GetState(source, frame);
            if (legacy_state < 0)
            {
                throw std::runtime_error("failed to build AviUtl1 state sequence");
            }
            std::cout << frame << '\t' << static_cast<double>(frame) / fps << '\t'
                      << sequence.GetState(source, frame) << '\t' << legacy_state << '\t'
                      << (is_peak ? 1 : 0) << '\n';
        }
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "ERROR: " << error.what() << '\n';
        return 1;
    }
}
