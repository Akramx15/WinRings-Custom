#pragma once

#include <Windows.h>
#include <cwctype>
#include <string>
#include <string_view>

namespace winrings
{
    enum class ClockCasing
    {
        CompactLower, // weekday & month TitleCase, am/pm lowercase -> "Fri 20 Nov  4:02 pm"
        Exact,        // weekday/month TitleCase, AM/PM uppercase
        Lower,        // everything lowercase
        Upper,        // everything uppercase
        Title         // weekday/month TitleCase, AM/PM uppercase
    };

    namespace detail
    {
        inline std::wstring ToLowerW(std::wstring s)
        {
            for (auto& ch : s)
                ch = static_cast<wchar_t>(std::towlower(ch));
            return s;
        }

        inline std::wstring ToUpperW(std::wstring s)
        {
            for (auto& ch : s)
                ch = static_cast<wchar_t>(std::towupper(ch));
            return s;
        }

        inline std::wstring Pad2(WORD value)
        {
            wchar_t buf[8]{};
            wsprintfW(buf, L"%02u", static_cast<unsigned>(value));
            return buf;
        }

        inline std::wstring Pad4(WORD value)
        {
            wchar_t buf[12]{};
            wsprintfW(buf, L"%04u", static_cast<unsigned>(value));
            return buf;
        }

        inline std::wstring NumW(WORD value)
        {
            wchar_t buf[12]{};
            wsprintfW(buf, L"%u", static_cast<unsigned>(value));
            return buf;
        }

        inline WORD ResolveWeekday(const SYSTEMTIME& st) noexcept
        {
            FILETIME ft{};
            SYSTEMTIME normalized{};
            if (SystemTimeToFileTime(&st, &ft) && FileTimeToSystemTime(&ft, &normalized))
                return normalized.wDayOfWeek % 7;
            return st.wDayOfWeek % 7;
        }
    } // namespace detail

    inline std::wstring FormatClock(const SYSTEMTIME& st, std::wstring_view format, ClockCasing casing)
    {
        static constexpr const wchar_t* kShortDays[7] = {
            L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat"
        };
        static constexpr const wchar_t* kFullDays[7] = {
            L"Sunday", L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday"
        };
        static constexpr const wchar_t* kShortMonths[12] = {
            L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
            L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"
        };
        static constexpr const wchar_t* kFullMonths[12] = {
            L"January", L"February", L"March", L"April", L"May", L"June",
            L"July", L"August", L"September", L"October", L"November", L"December"
        };

        if (format.empty())
            format = L"ddd d MMM  h:mm tt";

        const WORD dow = detail::ResolveWeekday(st);
        const WORD monthIdx = (st.wMonth >= 1 && st.wMonth <= 12) ? static_cast<WORD>(st.wMonth - 1) : 0;
        const WORD hour24 = st.wHour % 24;
        const WORD hour12 = (hour24 % 12 == 0) ? 12 : (hour24 % 12);
        const bool isPm = hour24 >= 12;

        auto applyWord = [casing](const wchar_t* titleText, [[maybe_unused]] bool isWeekday, bool isMeridiem) -> std::wstring
        {
            std::wstring s(titleText);
            switch (casing)
            {
            case ClockCasing::Lower:
                return detail::ToLowerW(std::move(s));
            case ClockCasing::Upper:
                return detail::ToUpperW(std::move(s));
            case ClockCasing::CompactLower:
                if (isMeridiem)
                    return detail::ToLowerW(std::move(s));
                return s;
            case ClockCasing::Exact:
            case ClockCasing::Title:
            default:
                return s;
            }
        };

        std::wstring out;
        out.reserve(format.size() + 16);

        for (size_t i = 0; i < format.size();)
        {
            const wchar_t ch = format[i];
            if (ch == L'\'')
            {
                ++i;
                while (i < format.size() && format[i] != L'\'')
                    out.push_back(format[i++]);
                if (i < format.size() && format[i] == L'\'')
                    ++i;
                continue;
            }

            auto countRun = [&](wchar_t token) -> size_t
            {
                size_t len = 0;
                while (i + len < format.size() && format[i + len] == token)
                    ++len;
                return len;
            };

            if (ch == L'd')
            {
                const size_t n = countRun(L'd');
                if (n >= 4) out += applyWord(kFullDays[dow], true, false);
                else if (n == 3) out += applyWord(kShortDays[dow], true, false);
                else if (n == 2) out += detail::Pad2(st.wDay);
                else out += detail::NumW(st.wDay);
                i += n;
            }
            else if (ch == L'M')
            {
                const size_t n = countRun(L'M');
                if (n >= 4) out += applyWord(kFullMonths[monthIdx], false, false);
                else if (n == 3) out += applyWord(kShortMonths[monthIdx], false, false);
                else if (n == 2) out += detail::Pad2(st.wMonth);
                else out += detail::NumW(st.wMonth);
                i += n;
            }
            else if (ch == L'y')
            {
                const size_t n = countRun(L'y');
                if (n >= 4) out += detail::Pad4(st.wYear);
                else if (n >= 2) out += detail::Pad2(static_cast<WORD>(st.wYear % 100));
                else out += detail::NumW(static_cast<WORD>(st.wYear % 100));
                i += n;
            }
            else if (ch == L'H')
            {
                const size_t n = countRun(L'H');
                out += (n >= 2) ? detail::Pad2(hour24) : detail::NumW(hour24);
                i += n;
            }
            else if (ch == L'h')
            {
                const size_t n = countRun(L'h');
                out += (n >= 2) ? detail::Pad2(hour12) : detail::NumW(hour12);
                i += n;
            }
            else if (ch == L'm')
            {
                const size_t n = countRun(L'm');
                out += (n >= 2) ? detail::Pad2(st.wMinute) : detail::NumW(st.wMinute);
                i += n;
            }
            else if (ch == L's')
            {
                const size_t n = countRun(L's');
                out += (n >= 2) ? detail::Pad2(st.wSecond) : detail::NumW(st.wSecond);
                i += n;
            }
            else if (ch == L't')
            {
                const size_t n = countRun(L't');
                const wchar_t* full = isPm ? L"PM" : L"AM";
                const wchar_t* shortM = isPm ? L"P" : L"A";
                out += applyWord(n >= 2 ? full : shortM, false, true);
                i += n;
            }
            else
            {
                out.push_back(ch);
                ++i;
            }
        }
        return out;
    }
} // namespace winrings
