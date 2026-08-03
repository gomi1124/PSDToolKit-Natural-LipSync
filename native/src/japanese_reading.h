#pragma once

#include <string>
#include <string_view>

namespace aviutl1_lipsync
{

std::wstring GetJapaneseKatakanaReading(const std::wstring &text);
int CountJapaneseSyllables(std::wstring_view reading);

} // namespace aviutl1_lipsync
