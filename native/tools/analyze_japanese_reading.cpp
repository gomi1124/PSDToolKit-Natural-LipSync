#include "japanese_reading.h"

#include <windows.h>

#include <iostream>
#include <stdexcept>
#include <string>

namespace
{

std::string WideToUtf8(const std::wstring &value)
{
    if (value.empty())
    {
        return {};
    }
    const auto byte_count =
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                            nullptr, 0, nullptr, nullptr);
    if (byte_count <= 0)
    {
        throw std::runtime_error("UTF-8 conversion failed");
    }
    std::string result(static_cast<std::size_t>(byte_count), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value.data(),
                           static_cast<int>(value.size()), result.data(), byte_count,
                           nullptr, nullptr) != byte_count)
    {
        throw std::runtime_error("UTF-8 conversion failed");
    }
    return result;
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
    try
    {
        if (argc != 2)
        {
            std::wcerr << L"Usage: JapaneseReadingProbe <text>\n";
            return 2;
        }
        const auto reading =
            aviutl1_lipsync::GetJapaneseKatakanaReading(argv[1]);
        const auto syllable_count =
            aviutl1_lipsync::CountJapaneseSyllables(reading);
        std::cout << "{\"reading\":\"" << WideToUtf8(reading)
                  << "\",\"syllables\":" << syllable_count << "}\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "ERROR: " << error.what() << '\n';
        return 1;
    }
}
