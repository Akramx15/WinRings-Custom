#include "WinRingsConfig.h"

#include <ShlObj.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <variant>

namespace winrings
{
    std::wstring Utf8ToWide(const std::string& utf8)
    {
        if (utf8.empty())
            return {};
        const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (needed <= 0)
            return {};
        std::wstring wide(static_cast<size_t>(needed), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), needed);
        return wide;
    }

    std::string WideToUtf8(const std::wstring& wide)
    {
        if (wide.empty())
            return {};
        const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        if (needed <= 0)
            return {};
        std::string utf8(static_cast<size_t>(needed), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), needed, nullptr, nullptr);
        return utf8;
    }

    const wchar_t* ToString(MouseTriggerButton v) noexcept
    {
        switch (v)
        {
        case MouseTriggerButton::SideBack: return L"SideBack";
        case MouseTriggerButton::SideForward: return L"SideForward";
        case MouseTriggerButton::Middle: return L"Middle";
        case MouseTriggerButton::RightLeftChord: return L"RightLeftChord";
        case MouseTriggerButton::Disabled: return L"Disabled";
        }
        return L"SideBack";
    }

    const wchar_t* ToString(ScrollWheelBehavior v) noexcept
    {
        switch (v)
        {
        case ScrollWheelBehavior::CycleSlotsOrWindows: return L"CycleSlotsOrWindows";
        case ScrollWheelBehavior::CycleSubmenuOnly: return L"CycleSubmenuOnly";
        case ScrollWheelBehavior::Disabled: return L"Disabled";
        }
        return L"CycleSlotsOrWindows";
    }

    const wchar_t* ToString(WindowViewMode v) noexcept
    {
        switch (v)
        {
        case WindowViewMode::OuterArc: return L"OuterArc";
        case WindowViewMode::TitleColumn: return L"TitleColumn";
        }
        return L"OuterArc";
    }

    const wchar_t* ToString(RingTheme v) noexcept
    {
        switch (v)
        {
        case RingTheme::A4S: return L"A4S";
        case RingTheme::DarkWood: return L"DarkWood";
        case RingTheme::Blue: return L"Blue";
        }
        return L"A4S";
    }

    const wchar_t* ToString(TopBarBackdrop v) noexcept
    {
        switch (v)
        {
        case TopBarBackdrop::Clear: return L"Clear";
        case TopBarBackdrop::Frosted: return L"Frosted";
        case TopBarBackdrop::Native: return L"Native";
        }
        return L"Clear";
    }

    const wchar_t* ToString(ClockCasing v) noexcept
    {
        switch (v)
        {
        case ClockCasing::CompactLower: return L"CompactLower";
        case ClockCasing::Exact: return L"Exact";
        case ClockCasing::Lower: return L"Lower";
        case ClockCasing::Upper: return L"Upper";
        case ClockCasing::Title: return L"Title";
        }
        return L"CompactLower";
    }

    const wchar_t* ToString(SlotKind v) noexcept
    {
        switch (v)
        {
        case SlotKind::App: return L"App";
        case SlotKind::Website: return L"Website";
        case SlotKind::File: return L"File";
        case SlotKind::Folder: return L"Folder";
        case SlotKind::Script: return L"Script";
        case SlotKind::Collection: return L"Collection";
        case SlotKind::RecycleBin: return L"RecycleBin";
        }
        return L"App";
    }

    const wchar_t* ToString(SelectionIndicatorStyle v) noexcept
    {
        switch (v)
        {
        case SelectionIndicatorStyle::GlowOnly: return L"GlowOnly";
        case SelectionIndicatorStyle::SectorWedge: return L"SectorWedge";
        case SelectionIndicatorStyle::PointerLine: return L"PointerLine";
        }
        return L"SectorWedge";
    }

    const wchar_t* ToString(HotCornerPos v) noexcept
    {
        switch (v)
        {
        case HotCornerPos::TopLeft: return L"TopLeft";
        case HotCornerPos::TopRight: return L"TopRight";
        }
        return L"TopLeft";
    }

    const wchar_t* ToString(HotCornerAction v) noexcept
    {
        switch (v)
        {
        case HotCornerAction::TaskView: return L"TaskView";
        case HotCornerAction::OpenRing: return L"OpenRing";
        }
        return L"TaskView";
    }

    const wchar_t* ToString(TouchpadCornerPos v) noexcept
    {
        switch (v)
        {
        case TouchpadCornerPos::TopLeft: return L"TopLeft";
        case TouchpadCornerPos::TopRight: return L"TopRight";
        }
        return L"TopRight";
    }

    const wchar_t* ToString(MediaIslandPos v) noexcept
    {
        switch (v)
        {
        case MediaIslandPos::Center: return L"Center";
        case MediaIslandPos::Left: return L"Left";
        case MediaIslandPos::NearTray: return L"NearTray";
        }
        return L"Left";
    }

    const wchar_t* ToString(MediaIslandFullscreenMode v) noexcept
    {
        switch (v)
        {
        case MediaIslandFullscreenMode::SyncWithTaskbar: return L"SyncWithTaskbar";
        case MediaIslandFullscreenMode::GamesOnly: return L"GamesOnly";
        case MediaIslandFullscreenMode::NeverHide: return L"NeverHide";
        }
        return L"SyncWithTaskbar";
    }

    namespace
    {
        struct JsonValue;
        using JsonObject = std::map<std::string, JsonValue, std::less<>>;
        using JsonArray = std::vector<JsonValue>;

        struct JsonValue
        {
            std::variant<std::nullptr_t, bool, double, std::string, JsonArray, JsonObject> data{ nullptr };

            bool IsObject() const noexcept { return std::holds_alternative<JsonObject>(data); }
            bool IsArray() const noexcept { return std::holds_alternative<JsonArray>(data); }
            bool IsString() const noexcept { return std::holds_alternative<std::string>(data); }
            bool IsBool() const noexcept { return std::holds_alternative<bool>(data); }
            bool IsNumber() const noexcept { return std::holds_alternative<double>(data); }

            const JsonObject* AsObject() const noexcept { return std::get_if<JsonObject>(&data); }
            const JsonArray* AsArray() const noexcept { return std::get_if<JsonArray>(&data); }
            const std::string* AsString() const noexcept { return std::get_if<std::string>(&data); }
        };

        class JsonParser
        {
        public:
            explicit JsonParser(const std::string& src) : m_src(src) {}

            bool ParseRoot(JsonValue& out)
            {
                SkipWs();
                if (!ParseValue(out))
                    return false;
                SkipWs();
                return m_pos == m_src.size();
            }

        private:
            const std::string& m_src;
            size_t m_pos = 0;

            void SkipWs() noexcept
            {
                while (m_pos < m_src.size())
                {
                    const unsigned char c = static_cast<unsigned char>(m_src[m_pos]);
                    if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                        ++m_pos;
                    else
                        break;
                }
            }

            bool Consume(char expected) noexcept
            {
                SkipWs();
                if (m_pos < m_src.size() && m_src[m_pos] == expected)
                {
                    ++m_pos;
                    return true;
                }
                return false;
            }

            static void AppendUtf8CodePoint(std::string& dst, uint32_t cp)
            {
                if (cp <= 0x7F)
                {
                    dst.push_back(static_cast<char>(cp));
                }
                else if (cp <= 0x7FF)
                {
                    dst.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                    dst.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
                else if (cp <= 0xFFFF)
                {
                    dst.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                    dst.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    dst.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
                else
                {
                    dst.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                    dst.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                    dst.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    dst.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
            }

            bool ParseHex4(uint32_t& out) noexcept
            {
                if (m_pos + 4 > m_src.size())
                    return false;
                uint32_t val = 0;
                for (int i = 0; i < 4; ++i)
                {
                    const char c = m_src[m_pos++];
                    val <<= 4;
                    if (c >= '0' && c <= '9') val |= static_cast<uint32_t>(c - '0');
                    else if (c >= 'a' && c <= 'f') val |= static_cast<uint32_t>(10 + c - 'a');
                    else if (c >= 'A' && c <= 'F') val |= static_cast<uint32_t>(10 + c - 'A');
                    else return false;
                }
                out = val;
                return true;
            }

            bool ParseString(std::string& out)
            {
                if (m_pos >= m_src.size() || m_src[m_pos] != '"')
                    return false;
                ++m_pos;
                out.clear();
                while (m_pos < m_src.size())
                {
                    const char c = m_src[m_pos++];
                    if (c == '"')
                        return true;
                    if (c == '\\')
                    {
                        if (m_pos >= m_src.size())
                            return false;
                        const char esc = m_src[m_pos++];
                        switch (esc)
                        {
                        case '"': out.push_back('"'); break;
                        case '\\': out.push_back('\\'); break;
                        case '/': out.push_back('/'); break;
                        case 'b': out.push_back('\b'); break;
                        case 'f': out.push_back('\f'); break;
                        case 'n': out.push_back('\n'); break;
                        case 'r': out.push_back('\r'); break;
                        case 't': out.push_back('\t'); break;
                        case 'u':
                        {
                            uint32_t cp = 0;
                            if (!ParseHex4(cp))
                                return false;
                            if (cp >= 0xD800 && cp <= 0xDBFF && m_pos + 6 <= m_src.size() &&
                                m_src[m_pos] == '\\' && m_src[m_pos + 1] == 'u')
                            {
                                m_pos += 2;
                                uint32_t low = 0;
                                if (!ParseHex4(low))
                                    return false;
                                if (low >= 0xDC00 && low <= 0xDFFF)
                                    cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                            }
                            AppendUtf8CodePoint(out, cp);
                            break;
                        }
                        default:
                            return false;
                        }
                    }
                    else
                    {
                        out.push_back(c);
                    }
                }
                return false;
            }

            bool ParseNumber(double& out) noexcept
            {
                const char* start = m_src.c_str() + m_pos;
                char* end = nullptr;
                out = std::strtod(start, &end);
                if (end == start)
                    return false;
                m_pos = static_cast<size_t>(end - m_src.c_str());
                return true;
            }

            bool ParseValue(JsonValue& out)
            {
                SkipWs();
                if (m_pos >= m_src.size())
                    return false;

                const char c = m_src[m_pos];
                if (c == '{')
                {
                    ++m_pos;
                    JsonObject obj;
                    SkipWs();
                    if (m_pos < m_src.size() && m_src[m_pos] == '}')
                    {
                        ++m_pos;
                        out.data = std::move(obj);
                        return true;
                    }
                    for (;;)
                    {
                        SkipWs();
                        std::string key;
                        if (!ParseString(key) || !Consume(':'))
                            return false;
                        JsonValue val;
                        if (!ParseValue(val))
                            return false;
                        obj.insert_or_assign(std::move(key), std::move(val));
                        SkipWs();
                        if (m_pos < m_src.size() && m_src[m_pos] == ',')
                        {
                            ++m_pos;
                            continue;
                        }
                        if (m_pos < m_src.size() && m_src[m_pos] == '}')
                        {
                            ++m_pos;
                            out.data = std::move(obj);
                            return true;
                        }
                        return false;
                    }
                }
                if (c == '[')
                {
                    ++m_pos;
                    JsonArray arr;
                    SkipWs();
                    if (m_pos < m_src.size() && m_src[m_pos] == ']')
                    {
                        ++m_pos;
                        out.data = std::move(arr);
                        return true;
                    }
                    for (;;)
                    {
                        JsonValue item;
                        if (!ParseValue(item))
                            return false;
                        arr.push_back(std::move(item));
                        SkipWs();
                        if (m_pos < m_src.size() && m_src[m_pos] == ',')
                        {
                            ++m_pos;
                            continue;
                        }
                        if (m_pos < m_src.size() && m_src[m_pos] == ']')
                        {
                            ++m_pos;
                            out.data = std::move(arr);
                            return true;
                        }
                        return false;
                    }
                }
                if (c == '"')
                {
                    std::string s;
                    if (!ParseString(s))
                        return false;
                    out.data = std::move(s);
                    return true;
                }
                if (m_src.compare(m_pos, 4, "true") == 0)
                {
                    m_pos += 4;
                    out.data = true;
                    return true;
                }
                if (m_src.compare(m_pos, 5, "false") == 0)
                {
                    m_pos += 5;
                    out.data = false;
                    return true;
                }
                if (m_src.compare(m_pos, 4, "null") == 0)
                {
                    m_pos += 4;
                    out.data = nullptr;
                    return true;
                }
                if (c == '-' || (c >= '0' && c <= '9'))
                {
                    double d = 0.0;
                    if (!ParseNumber(d))
                        return false;
                    out.data = d;
                    return true;
                }
                return false;
            }
        };

        void AppendEscapedJsonString(std::string& out, const std::string& s)
        {
            out.push_back('"');
            for (unsigned char c : s)
            {
                switch (c)
                {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20)
                    {
                        char buf[8]{};
                        std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                        out += buf;
                    }
                    else
                    {
                        out.push_back(static_cast<char>(c));
                    }
                    break;
                }
            }
            out.push_back('"');
        }

        template <typename T>
        bool TryGetBool(const JsonObject& obj, const char* key, T& dst)
        {
            const auto it = obj.find(key);
            if (it != obj.end() && it->second.IsBool())
            {
                dst = std::get<bool>(it->second.data);
                return true;
            }
            return false;
        }

        bool TryGetInt(const JsonObject& obj, const char* key, int& dst)
        {
            const auto it = obj.find(key);
            if (it != obj.end() && it->second.IsNumber())
            {
                dst = static_cast<int>(std::llround(std::get<double>(it->second.data)));
                return true;
            }
            return false;
        }

        bool TryGetUInt(const JsonObject& obj, const char* key, UINT& dst)
        {
            const auto it = obj.find(key);
            if (it != obj.end() && it->second.IsNumber())
            {
                const double val = std::get<double>(it->second.data);
                dst = static_cast<UINT>((std::max)(0.0, std::round(val)));
                return true;
            }
            return false;
        }

        bool TryGetDouble(const JsonObject& obj, const char* key, double& dst)
        {
            const auto it = obj.find(key);
            if (it != obj.end() && it->second.IsNumber())
            {
                dst = std::get<double>(it->second.data);
                return true;
            }
            return false;
        }

        bool TryGetWString(const JsonObject& obj, const char* key, std::wstring& dst)
        {
            const auto it = obj.find(key);
            if (it != obj.end() && it->second.IsString())
            {
                dst = Utf8ToWide(*it->second.AsString());
                return true;
            }
            return false;
        }

        template <typename EnumT>
        EnumT ParseEnum(const std::wstring& value, EnumT fallback,
            std::initializer_list<std::pair<const wchar_t*, EnumT>> entries) noexcept
        {
            for (const auto& entry : entries)
            {
                if (_wcsicmp(value.c_str(), entry.first) == 0)
                    return entry.second;
            }
            return fallback;
        }

        PinnedSlotConfig ParseSlot(const JsonObject& obj)
        {
            PinnedSlotConfig slot;
            TryGetWString(obj, "id", slot.id);
            std::wstring kindStr;
            if (TryGetWString(obj, "kind", kindStr))
            {
                slot.kind = ParseEnum<SlotKind>(kindStr, SlotKind::App, {
                    { L"App", SlotKind::App },
                    { L"Website", SlotKind::Website },
                    { L"File", SlotKind::File },
                    { L"Folder", SlotKind::Folder },
                    { L"Script", SlotKind::Script },
                    { L"Collection", SlotKind::Collection },
                    { L"RecycleBin", SlotKind::RecycleBin }
                });
            }
            TryGetWString(obj, "label", slot.label);
            TryGetWString(obj, "pathOrUrl", slot.pathOrUrl);
            TryGetWString(obj, "iconCacheFile", slot.iconCacheFile);
            TryGetInt(obj, "maxFolderItems", slot.maxFolderItems);
            slot.maxFolderItems = std::clamp(slot.maxFolderItems, 1, 9);

            const auto itemsIt = obj.find("items");
            if (itemsIt != obj.end() && itemsIt->second.IsArray())
            {
                for (const auto& childVal : *itemsIt->second.AsArray())
                {
                    if (const auto* childObj = childVal.AsObject())
                        slot.items.push_back(ParseSlot(*childObj));
                }
            }
            return slot;
        }

        void SerializeSlot(std::string& out, const PinnedSlotConfig& slot, int indentLevel)
        {
            const std::string ind(static_cast<size_t>(indentLevel * 2), ' ');
            const std::string ind2(static_cast<size_t>((indentLevel + 1) * 2), ' ');
            out += ind + "{\n";
            out += ind2 + "\"id\": "; AppendEscapedJsonString(out, WideToUtf8(slot.id)); out += ",\n";
            out += ind2 + "\"kind\": "; AppendEscapedJsonString(out, WideToUtf8(ToString(slot.kind))); out += ",\n";
            out += ind2 + "\"label\": "; AppendEscapedJsonString(out, WideToUtf8(slot.label)); out += ",\n";
            out += ind2 + "\"pathOrUrl\": "; AppendEscapedJsonString(out, WideToUtf8(slot.pathOrUrl)); out += ",\n";
            out += ind2 + "\"iconCacheFile\": "; AppendEscapedJsonString(out, WideToUtf8(slot.iconCacheFile)); out += ",\n";
            out += ind2 + "\"maxFolderItems\": " + std::to_string(slot.maxFolderItems) + ",\n";
            out += ind2 + "\"items\": [";
            if (!slot.items.empty())
            {
                out += "\n";
                for (size_t i = 0; i < slot.items.size(); ++i)
                {
                    SerializeSlot(out, slot.items[i], indentLevel + 2);
                    if (i + 1 < slot.items.size())
                        out += ",";
                    out += "\n";
                }
                out += ind2;
            }
            out += "]\n";
            out += ind + "}";
        }
    } // namespace

    std::wstring SwitcherConfig::AppDataDirectory()
    {
        PWSTR raw = nullptr;
        std::wstring base;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw)) && raw)
        {
            base = raw;
            CoTaskMemFree(raw);
        }
        else
        {
            wchar_t buf[MAX_PATH]{};
            const DWORD len = GetEnvironmentVariableW(L"APPDATA", buf, _countof(buf));
            if (len > 0 && len < _countof(buf))
                base = buf;
            else
                base = L".";
        }
        return base + L"\\WinRings";
    }

    std::wstring SwitcherConfig::ConfigPath()
    {
        return AppDataDirectory() + L"\\config.json";
    }

    SwitcherConfig SwitcherConfig::CreateDefault()
    {
        SwitcherConfig cfg;
        cfg.pinnedSlots = {
            PinnedSlotConfig{ L"pin-explorer", SlotKind::App, L"Explorer", L"C:\\Windows\\explorer.exe", L"", 8, {} },
            PinnedSlotConfig{ L"pin-downloads", SlotKind::Folder, L"Downloads", L"%USERPROFILE%\\Downloads", L"", 8, {} },
            PinnedSlotConfig{ L"pin-trash", SlotKind::RecycleBin, L"Recycle Bin", L"", L"", 8, {} }
        };
        return cfg;
    }

    bool SwitcherConfig::ParseJsonUtf8(const std::string& jsonUtf8, SwitcherConfig& outConfig)
    {
        JsonValue rootVal;
        JsonParser parser(jsonUtf8);
        if (!parser.ParseRoot(rootVal))
            return false;
        const auto* obj = rootVal.AsObject();
        if (!obj)
            return false;

        SwitcherConfig cfg = CreateDefault();
        TryGetInt(*obj, "version", cfg.version);

        std::wstring s;
        if (TryGetWString(*obj, "mouseTrigger", s))
        {
            cfg.mouseTrigger = ParseEnum<MouseTriggerButton>(s, cfg.mouseTrigger, {
                { L"SideBack", MouseTriggerButton::SideBack },
                { L"SideForward", MouseTriggerButton::SideForward },
                { L"Middle", MouseTriggerButton::Middle },
                { L"RightLeftChord", MouseTriggerButton::RightLeftChord },
                { L"Disabled", MouseTriggerButton::Disabled }
            });
        }
        if (TryGetWString(*obj, "scrollWheelMode", s))
        {
            cfg.scrollWheelMode = ParseEnum<ScrollWheelBehavior>(s, cfg.scrollWheelMode, {
                { L"CycleSlotsOrWindows", ScrollWheelBehavior::CycleSlotsOrWindows },
                { L"CycleSubmenuOnly", ScrollWheelBehavior::CycleSubmenuOnly },
                { L"Disabled", ScrollWheelBehavior::Disabled }
            });
        }
        TryGetBool(*obj, "doubleTapLastApp", cfg.doubleTapLastApp);
        TryGetInt(*obj, "tapThresholdMs", cfg.tapThresholdMs);
        TryGetInt(*obj, "doubleTapWindowMs", cfg.doubleTapWindowMs);
        TryGetBool(*obj, "hotkeyEnabled", cfg.hotkeyEnabled);
        TryGetUInt(*obj, "hotkeyModifiers", cfg.hotkeyModifiers);
        TryGetUInt(*obj, "hotkeyVk", cfg.hotkeyVk);
        TryGetBool(*obj, "menuKeyEnabled", cfg.menuKeyEnabled);
        TryGetBool(*obj, "interceptWinTab", cfg.interceptWinTab);
        TryGetBool(*obj, "interceptAltTab", cfg.interceptAltTab);

        TryGetDouble(*obj, "deadzoneDip", cfg.deadzoneDip);
        TryGetDouble(*obj, "ringRadiusDip", cfg.ringRadiusDip);
        TryGetDouble(*obj, "iconSizeDip", cfg.iconSizeDip);
        TryGetDouble(*obj, "thumbnailScale", cfg.thumbnailScale);
        TryGetBool(*obj, "disableOpeningAnimation", cfg.disableOpeningAnimation);
        if (TryGetWString(*obj, "windowViewMode", s))
        {
            cfg.windowViewMode = ParseEnum<WindowViewMode>(s, cfg.windowViewMode, {
                { L"OuterArc", WindowViewMode::OuterArc },
                { L"TitleColumn", WindowViewMode::TitleColumn }
            });
        }
        TryGetBool(*obj, "fanOutSingleWindow", cfg.fanOutSingleWindow);
        if (TryGetWString(*obj, "theme", s))
        {
            cfg.theme = ParseEnum<RingTheme>(s, cfg.theme, {
                { L"A4S", RingTheme::A4S },
                { L"A4SMenu", RingTheme::A4S },
                { L"DarkWood", RingTheme::DarkWood },
                { L"Blue", RingTheme::Blue },
                { L"LiquidGlass", RingTheme::A4S },
                { L"Glass", RingTheme::A4S },
                { L"Kiwi", RingTheme::A4S },
                { L"Aura", RingTheme::A4S },
                { L"Orbit", RingTheme::A4S },
                { L"Outline", RingTheme::A4S }
            });
        }
        TryGetInt(*obj, "glassLevel", cfg.glassLevel);
        TryGetInt(*obj, "lightDarkMode", cfg.lightDarkMode);
        TryGetWString(*obj, "accentColor", cfg.accentColor);
        TryGetBool(*obj, "pointerFollow", cfg.pointerFollow);
        TryGetBool(*obj, "shiftCenterOnActiveMonitor", cfg.shiftCenterOnActiveMonitor);
        TryGetBool(*obj, "includeUnpinnedRunningApps", cfg.includeUnpinnedRunningApps);
        TryGetInt(*obj, "maxSlots", cfg.maxSlots);

        TryGetBool(*obj, "topBarEnabled", cfg.topBarEnabled);
        TryGetBool(*obj, "topBarDockTop", cfg.topBarDockTop);
        TryGetInt(*obj, "topBarHeightDip", cfg.topBarHeightDip);
        TryGetBool(*obj, "topBarPreserveFontSize", cfg.topBarPreserveFontSize);
        TryGetBool(*obj, "topBarClockSingleLine", cfg.topBarClockSingleLine);
        TryGetWString(*obj, "topBarClockFormat", cfg.topBarClockFormat);
        if (TryGetWString(*obj, "topBarClockCasing", s))
        {
            cfg.topBarClockCasing = ParseEnum<ClockCasing>(s, cfg.topBarClockCasing, {
                { L"CompactLower", ClockCasing::CompactLower },
                { L"Exact", ClockCasing::Exact },
                { L"Lower", ClockCasing::Lower },
                { L"Upper", ClockCasing::Upper },
                { L"Title", ClockCasing::Title }
            });
        }
        TryGetBool(*obj, "topBarHideAppButtons", cfg.topBarHideAppButtons);
        TryGetBool(*obj, "topBarHideStart", cfg.topBarHideStart);
        TryGetBool(*obj, "topBarHideSearch", cfg.topBarHideSearch);
        TryGetBool(*obj, "topBarHideTaskView", cfg.topBarHideTaskView);
        TryGetBool(*obj, "topBarHideWidgets", cfg.topBarHideWidgets);
        if (TryGetWString(*obj, "topBarBackdrop", s))
        {
            cfg.topBarBackdrop = ParseEnum<TopBarBackdrop>(s, cfg.topBarBackdrop, {
                { L"Clear", TopBarBackdrop::Clear },
                { L"Frosted", TopBarBackdrop::Frosted },
                { L"Native", TopBarBackdrop::Native }
            });
        }
        TryGetBool(*obj, "topBarAllMonitors", cfg.topBarAllMonitors);

        TryGetBool(*obj, "disableRingBlur", cfg.disableRingBlur);
        if (TryGetWString(*obj, "selectionIndicator", s))
        {
            cfg.selectionIndicator = ParseEnum<SelectionIndicatorStyle>(s, cfg.selectionIndicator, {
                { L"GlowOnly", SelectionIndicatorStyle::GlowOnly },
                { L"SectorWedge", SelectionIndicatorStyle::SectorWedge },
                { L"PointerLine", SelectionIndicatorStyle::PointerLine }
            });
        }

        TryGetBool(*obj, "winKeyTrigger", cfg.winKeyTrigger);
        TryGetBool(*obj, "directSwitchEnabled", cfg.directSwitchEnabled);
        TryGetUInt(*obj, "directSwitchModifiers", cfg.directSwitchModifiers);
        TryGetUInt(*obj, "directSwitchVk", cfg.directSwitchVk);

        TryGetBool(*obj, "touchpadPhysLeftClickTrigger", cfg.touchpadPhysLeftClickTrigger);
        TryGetBool(*obj, "touchpadCornerTrigger", cfg.touchpadCornerTrigger);
        if (TryGetWString(*obj, "touchpadCornerPos", s))
        {
            cfg.touchpadCornerPos = ParseEnum<TouchpadCornerPos>(s, cfg.touchpadCornerPos, {
                { L"TopLeft", TouchpadCornerPos::TopLeft },
                { L"TopRight", TouchpadCornerPos::TopRight }
            });
        }
        TryGetInt(*obj, "touchpadCornerSizePct", cfg.touchpadCornerSizePct);

        TryGetBool(*obj, "shakeTriggerEnabled", cfg.shakeTriggerEnabled);
        TryGetInt(*obj, "shakeMinReversals", cfg.shakeMinReversals);
        TryGetInt(*obj, "shakeWindowMs", cfg.shakeWindowMs);
        TryGetInt(*obj, "shakeMinAmplitudeDip", cfg.shakeMinAmplitudeDip);
        TryGetBool(*obj, "shakeSuppressFullscreen", cfg.shakeSuppressFullscreen);

        TryGetBool(*obj, "hotCornerEnabled", cfg.hotCornerEnabled);
        if (TryGetWString(*obj, "hotCornerPos", s))
        {
            cfg.hotCornerPos = ParseEnum<HotCornerPos>(s, cfg.hotCornerPos, {
                { L"TopLeft", HotCornerPos::TopLeft },
                { L"TopRight", HotCornerPos::TopRight }
            });
        }
        if (TryGetWString(*obj, "hotCornerAction", s))
        {
            cfg.hotCornerAction = ParseEnum<HotCornerAction>(s, cfg.hotCornerAction, {
                { L"TaskView", HotCornerAction::TaskView },
                { L"OpenRing", HotCornerAction::OpenRing }
            });
        }
        TryGetInt(*obj, "hotCornerPressureThreshold", cfg.hotCornerPressureThreshold);

        TryGetBool(*obj, "fullWorkAreaEnabled", cfg.fullWorkAreaEnabled);
        TryGetBool(*obj, "fullWorkAreaPillClip", cfg.fullWorkAreaPillClip);
        TryGetBool(*obj, "fullWorkAreaHotkeys", cfg.fullWorkAreaHotkeys);

        TryGetBool(*obj, "mediaIslandEnabled", cfg.mediaIslandEnabled);
        if (TryGetWString(*obj, "mediaIslandPos", s))
        {
            cfg.mediaIslandPos = ParseEnum<MediaIslandPos>(s, cfg.mediaIslandPos, {
                { L"Center", MediaIslandPos::Center },
                { L"Left", MediaIslandPos::Left },
                { L"NearTray", MediaIslandPos::NearTray }
            });
        }
        TryGetBool(*obj, "mediaIslandHoverExpand", cfg.mediaIslandHoverExpand);
        TryGetBool(*obj, "mediaIslandHideWhenPaused", cfg.mediaIslandHideWhenPaused);
        TryGetBool(*obj, "mediaIslandScrollVolume", cfg.mediaIslandScrollVolume);
        TryGetBool(*obj, "mediaIslandMarquee", cfg.mediaIslandMarquee);
        if (TryGetWString(*obj, "mediaIslandFullscreenMode", s))
        {
            cfg.mediaIslandFullscreenMode = ParseEnum<MediaIslandFullscreenMode>(s, cfg.mediaIslandFullscreenMode, {
                { L"SyncWithTaskbar", MediaIslandFullscreenMode::SyncWithTaskbar },
                { L"GamesOnly", MediaIslandFullscreenMode::GamesOnly },
                { L"NeverHide", MediaIslandFullscreenMode::NeverHide }
            });
        }
        TryGetInt(*obj, "mediaIslandOffsetXPct", cfg.mediaIslandOffsetXPct);

        TryGetBool(*obj, "dragToSnapEnabled", cfg.dragToSnapEnabled);
        TryGetBool(*obj, "dragToSnapLaunchClosed", cfg.dragToSnapLaunchClosed);
        TryGetInt(*obj, "dragToSnapHoldMs", cfg.dragToSnapHoldMs);
        cfg.dragToSnapHoldMs = (std::clamp)(cfg.dragToSnapHoldMs, 0, 1000);
        TryGetInt(*obj, "dragToSnapTearOffDip", cfg.dragToSnapTearOffDip);
        cfg.dragToSnapTearOffDip = (std::clamp)(cfg.dragToSnapTearOffDip, 12, 160);

        const auto pinsIt = obj->find("pinnedSlots");
        if (pinsIt != obj->end() && pinsIt->second.IsArray())
        {
            cfg.pinnedSlots.clear();
            for (const auto& val : *pinsIt->second.AsArray())
            {
                if (const auto* slotObj = val.AsObject())
                    cfg.pinnedSlots.push_back(ParseSlot(*slotObj));
            }
        }

        cfg.deadzoneDip = std::clamp(cfg.deadzoneDip, 8.0, 48.0);
        cfg.ringRadiusDip = std::clamp(cfg.ringRadiusDip, 72.0, 240.0);
        cfg.iconSizeDip = std::clamp(cfg.iconSizeDip, 24.0, 72.0);
        cfg.thumbnailScale = std::clamp(cfg.thumbnailScale, 0.5, 2.0);
        cfg.glassLevel = std::clamp(cfg.glassLevel, 0, 100);
        cfg.lightDarkMode = std::clamp(cfg.lightDarkMode, 0, 100);
        cfg.maxSlots = std::clamp(cfg.maxSlots, 4, 24);
        cfg.topBarHeightDip = std::clamp(cfg.topBarHeightDip, 24, 52);
        cfg.touchpadCornerSizePct = std::clamp(cfg.touchpadCornerSizePct, 10, 30);
        cfg.shakeMinReversals = std::clamp(cfg.shakeMinReversals, 3, 8);
        cfg.shakeWindowMs = std::clamp(cfg.shakeWindowMs, 200, 800);
        cfg.shakeMinAmplitudeDip = std::clamp(cfg.shakeMinAmplitudeDip, 15, 80);
        cfg.hotCornerPressureThreshold = std::clamp(cfg.hotCornerPressureThreshold, 50, 400);
        if (cfg.mediaIslandOffsetXPct >= 0)
            cfg.mediaIslandOffsetXPct = std::clamp(cfg.mediaIslandOffsetXPct, 0, 100);
        else
            cfg.mediaIslandOffsetXPct = -1;

        outConfig = std::move(cfg);
        return true;
    }

    std::string SwitcherConfig::ToJsonUtf8() const
    {
        std::string out;
        out.reserve(2048);
        auto addBool = [&](const char* k, bool v)
        {
            out += "  \""; out += k; out += "\": "; out += (v ? "true" : "false"); out += ",\n";
        };
        auto addInt = [&](const char* k, int v)
        {
            out += "  \""; out += k; out += "\": "; out += std::to_string(v); out += ",\n";
        };
        auto addUInt = [&](const char* k, UINT v)
        {
            out += "  \""; out += k; out += "\": "; out += std::to_string(v); out += ",\n";
        };
        auto addDouble = [&](const char* k, double v)
        {
            char buf[64]{};
            std::snprintf(buf, sizeof(buf), "%.4g", v);
            out += "  \""; out += k; out += "\": "; out += buf; out += ",\n";
        };
        auto addWStr = [&](const char* k, const std::wstring& v)
        {
            out += "  \""; out += k; out += "\": "; AppendEscapedJsonString(out, WideToUtf8(v)); out += ",\n";
        };

        out += "{\n";
        addInt("version", version);

        addWStr("mouseTrigger", ToString(mouseTrigger));
        addWStr("scrollWheelMode", ToString(scrollWheelMode));
        addBool("doubleTapLastApp", doubleTapLastApp);
        addInt("tapThresholdMs", tapThresholdMs);
        addInt("doubleTapWindowMs", doubleTapWindowMs);
        addBool("hotkeyEnabled", hotkeyEnabled);
        addUInt("hotkeyModifiers", hotkeyModifiers);
        addUInt("hotkeyVk", hotkeyVk);
        addBool("menuKeyEnabled", menuKeyEnabled);
        addBool("interceptWinTab", interceptWinTab);
        addBool("interceptAltTab", interceptAltTab);

        addDouble("deadzoneDip", deadzoneDip);
        addDouble("ringRadiusDip", ringRadiusDip);
        addDouble("iconSizeDip", iconSizeDip);
        addDouble("thumbnailScale", thumbnailScale);
        addBool("disableOpeningAnimation", disableOpeningAnimation);
        addWStr("windowViewMode", ToString(windowViewMode));
        addBool("fanOutSingleWindow", fanOutSingleWindow);
        addWStr("theme", ToString(theme));
        addInt("glassLevel", glassLevel);
        addInt("lightDarkMode", lightDarkMode);
        addWStr("accentColor", accentColor);
        addBool("pointerFollow", pointerFollow);
        addBool("shiftCenterOnActiveMonitor", shiftCenterOnActiveMonitor);
        addBool("includeUnpinnedRunningApps", includeUnpinnedRunningApps);
        addInt("maxSlots", maxSlots);

        addBool("topBarEnabled", topBarEnabled);
        addBool("topBarDockTop", topBarDockTop);
        addInt("topBarHeightDip", topBarHeightDip);
        addBool("topBarPreserveFontSize", topBarPreserveFontSize);
        addBool("topBarClockSingleLine", topBarClockSingleLine);
        addWStr("topBarClockFormat", topBarClockFormat);
        addWStr("topBarClockCasing", ToString(topBarClockCasing));
        addBool("topBarHideAppButtons", topBarHideAppButtons);
        addBool("topBarHideStart", topBarHideStart);
        addBool("topBarHideSearch", topBarHideSearch);
        addBool("topBarHideTaskView", topBarHideTaskView);
        addBool("topBarHideWidgets", topBarHideWidgets);
        addWStr("topBarBackdrop", ToString(topBarBackdrop));
        addBool("topBarAllMonitors", topBarAllMonitors);

        addBool("disableRingBlur", disableRingBlur);
        addWStr("selectionIndicator", ToString(selectionIndicator));

        addBool("winKeyTrigger", winKeyTrigger);
        addBool("directSwitchEnabled", directSwitchEnabled);
        addUInt("directSwitchModifiers", directSwitchModifiers);
        addUInt("directSwitchVk", directSwitchVk);

        addBool("touchpadPhysLeftClickTrigger", touchpadPhysLeftClickTrigger);
        addBool("touchpadCornerTrigger", touchpadCornerTrigger);
        addWStr("touchpadCornerPos", ToString(touchpadCornerPos));
        addInt("touchpadCornerSizePct", touchpadCornerSizePct);

        addBool("shakeTriggerEnabled", shakeTriggerEnabled);
        addInt("shakeMinReversals", shakeMinReversals);
        addInt("shakeWindowMs", shakeWindowMs);
        addInt("shakeMinAmplitudeDip", shakeMinAmplitudeDip);
        addBool("shakeSuppressFullscreen", shakeSuppressFullscreen);

        addBool("hotCornerEnabled", hotCornerEnabled);
        addWStr("hotCornerPos", ToString(hotCornerPos));
        addWStr("hotCornerAction", ToString(hotCornerAction));
        addInt("hotCornerPressureThreshold", hotCornerPressureThreshold);

        addBool("fullWorkAreaEnabled", fullWorkAreaEnabled);
        addBool("fullWorkAreaPillClip", fullWorkAreaPillClip);
        addBool("fullWorkAreaHotkeys", fullWorkAreaHotkeys);

        addBool("mediaIslandEnabled", mediaIslandEnabled);
        addWStr("mediaIslandPos", ToString(mediaIslandPos));
        addBool("mediaIslandHoverExpand", mediaIslandHoverExpand);
        addBool("mediaIslandHideWhenPaused", mediaIslandHideWhenPaused);
        addBool("mediaIslandScrollVolume", mediaIslandScrollVolume);
        addBool("mediaIslandMarquee", mediaIslandMarquee);
        addWStr("mediaIslandFullscreenMode", ToString(mediaIslandFullscreenMode));
        addInt("mediaIslandOffsetXPct", mediaIslandOffsetXPct);

        addBool("dragToSnapEnabled", dragToSnapEnabled);
        addBool("dragToSnapLaunchClosed", dragToSnapLaunchClosed);
        addInt("dragToSnapHoldMs", dragToSnapHoldMs);
        addInt("dragToSnapTearOffDip", dragToSnapTearOffDip);

        out += "  \"pinnedSlots\": [";
        if (!pinnedSlots.empty())
        {
            out += "\n";
            for (size_t i = 0; i < pinnedSlots.size(); ++i)
            {
                SerializeSlot(out, pinnedSlots[i], 2);
                if (i + 1 < pinnedSlots.size())
                    out += ",";
                out += "\n";
            }
            out += "  ";
        }
        out += "]\n";
        out += "}\n";
        return out;
    }

    bool SwitcherConfig::LoadFromFile(const std::wstring& path, SwitcherConfig& outConfig)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;

        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 4 * 1024 * 1024)
        {
            CloseHandle(file);
            return false;
        }
        std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
        DWORD read = 0;
        const BOOL ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
        CloseHandle(file);
        if (!ok || read != bytes.size())
            return false;

        return ParseJsonUtf8(bytes, outConfig);
    }

    bool SwitcherConfig::SaveToFile(const std::wstring& path) const
    {
        const size_t slash = path.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
        {
            const std::wstring dir = path.substr(0, slash);
            SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        }

        const std::string json = ToJsonUtf8();
        const std::wstring tmpPath = path + L".tmp";
        HANDLE file = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;

        DWORD written = 0;
        const BOOL ok = WriteFile(file, json.data(), static_cast<DWORD>(json.size()), &written, nullptr)
            && written == json.size();
        FlushFileBuffers(file);
        CloseHandle(file);
        if (!ok)
        {
            DeleteFileW(tmpPath.c_str());
            return false;
        }

        if (!MoveFileExW(tmpPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            DeleteFileW(tmpPath.c_str());
            return false;
        }
        return true;
    }

    SwitcherConfig SwitcherConfig::LoadOrCreate()
    {
        SwitcherConfig cfg;
        const std::wstring path = ConfigPath();
        if (LoadFromFile(path, cfg))
        {
            if (cfg.version < 3)
            {
                cfg.theme = RingTheme::A4S;
                cfg.glassLevel = 50;
                cfg.lightDarkMode = 0;
                if (_wcsicmp(cfg.accentColor.c_str(), L"#3186FF") == 0)
                    cfg.accentColor = L"#0078D4";
                cfg.version = 3;
                cfg.SaveToFile(path);
            }
            return cfg;
        }

        cfg = CreateDefault();
        cfg.SaveToFile(path);
        return cfg;
    }

    bool SwitcherConfig::Save() const
    {
        return SaveToFile(ConfigPath());
    }
} // namespace winrings
