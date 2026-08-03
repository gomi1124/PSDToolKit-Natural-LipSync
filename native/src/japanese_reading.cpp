#include "japanese_reading.h"

#include <windows.h>
#include <msime.h>
#include <objbase.h>

#include <stdexcept>
#include <vector>

namespace aviutl1_lipsync
{
namespace
{

class ComApartment
{
  public:
    ComApartment()
    {
        const auto result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (result == RPC_E_CHANGED_MODE)
        {
            return;
        }
        if (FAILED(result))
        {
            throw std::runtime_error("CoInitializeEx failed");
        }
        should_uninitialize_ = true;
    }

    ~ComApartment()
    {
        if (should_uninitialize_)
        {
            CoUninitialize();
        }
    }

  private:
    bool should_uninitialize_ = false;
};

class FeLanguage
{
  public:
    FeLanguage()
    {
        CLSID class_id{};
        IID interface_id{};
        if (FAILED(CLSIDFromProgID(L"MSIME.Japan", &class_id)) ||
            FAILED(IIDFromString(L"{019F7152-E6DB-11D0-83C3-00C04FDDB82E}",
                                 &interface_id)) ||
            FAILED(CoCreateInstance(class_id, nullptr, CLSCTX_INPROC_SERVER, interface_id,
                                    reinterpret_cast<void **>(&language_))) ||
            language_ == nullptr || FAILED(language_->Open()))
        {
            throw std::runtime_error("Microsoft Japanese IME language service is unavailable");
        }
        is_open_ = true;
    }

    ~FeLanguage()
    {
        if (language_ == nullptr)
        {
            return;
        }
        if (is_open_)
        {
            language_->Close();
        }
        language_->Release();
    }

    std::wstring GetReading(const std::wstring &text) const
    {
        std::vector<DWORD> column_info(text.size(), 0);
        MORRSLT *result = nullptr;
        const auto status = language_->GetJMorphResult(
            FELANG_REQ_REV,
            FELANG_CMODE_KATAKANAOUT | FELANG_CMODE_BESTFIRST |
                FELANG_CMODE_NOINVISIBLECHAR,
            static_cast<int>(text.size()), text.c_str(), column_info.data(), &result);
        if (FAILED(status) || result == nullptr)
        {
            throw std::runtime_error("Japanese IME reverse conversion failed");
        }

        const std::wstring reading(result->pwchOutput, result->cchOutput);
        CoTaskMemFree(result);
        return reading;
    }

  private:
    IFELanguage *language_ = nullptr;
    bool is_open_ = false;
};

bool IsSmallKatakana(wchar_t character)
{
    switch (character)
    {
    case L'ァ':
    case L'ィ':
    case L'ゥ':
    case L'ェ':
    case L'ォ':
    case L'ッ':
    case L'ャ':
    case L'ュ':
    case L'ョ':
    case L'ヮ':
    case L'ヵ':
    case L'ヶ':
        return true;
    default:
        return false;
    }
}

} // namespace

std::wstring GetJapaneseKatakanaReading(const std::wstring &text)
{
    if (text.empty())
    {
        return {};
    }
    const ComApartment apartment;
    const FeLanguage language;
    return language.GetReading(text);
}

int CountJapaneseSyllables(std::wstring_view reading)
{
    int count = 0;
    for (const auto character : reading)
    {
        if ((character >= L'0' && character <= L'9') ||
            (character >= L'０' && character <= L'９'))
        {
            ++count;
            continue;
        }
        if (character == L'ー' || IsSmallKatakana(character))
        {
            continue;
        }
        if ((character >= L'ァ' && character <= L'ヺ'))
        {
            ++count;
        }
    }
    return count;
}

} // namespace aviutl1_lipsync
