#include "MediaIsland.h"

#include <Unknwn.h>
#include <commctrl.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <robuffer.h>
#include <shellapi.h>
#include <wincodec.h>
#include <windowsx.h>

#undef GetCurrentTime
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "ShellController.h"

namespace winrings
{
    namespace
    {
        constexpr wchar_t kIslandClass[] = L"WinRings.MediaIsland.v1";
        constexpr UINT kMsgMediaUpdated = WM_APP + 501;

        constexpr UINT_PTR kTimerExpand = 1;
        constexpr UINT_PTR kTimerVolumeHide = 2;
        constexpr UINT_PTR kTimerMarquee = 3;
        constexpr UINT_PTR kTimerFullscreenPoll = 4;

        constexpr int kThumbPx = 64;
        constexpr float kCompactMinW = 196.0f;
        constexpr float kCompactMaxW = 250.0f;
        constexpr float kExpandedW = 296.0f;
        constexpr float kExpandedH = 76.0f;

        bool ContainsRtlChars(const std::wstring& text) noexcept
        {
            for (wchar_t ch : text)
            {
                if ((ch >= 0x0590 && ch <= 0x08FF) || (ch >= 0xFB1D && ch <= 0xFEFF))
                    return true;
            }
            return false;
        }

        namespace wmc = winrt::Windows::Media::Control;
        namespace wss = winrt::Windows::Storage::Streams;

        using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);

        double QueryPrimaryMonitorDpiScale(HMONITOR hMon) noexcept
        {
            if (HMODULE shcore = LoadLibraryW(L"shcore.dll"))
            {
                auto fn = reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor"));
                if (fn)
                {
                    UINT dx = 96, dy = 96;
                    if (SUCCEEDED(fn(hMon, 0 /* MDT_EFFECTIVE_DPI */, &dx, &dy)) && dx >= 72)
                    {
                        FreeLibrary(shcore);
                        return static_cast<double>(dx) / 96.0;
                    }
                }
                FreeLibrary(shcore);
            }
            const UINT sysDpi = GetDpiForSystem();
            return sysDpi >= 72 ? (static_cast<double>(sysDpi) / 96.0) : 1.0;
        }

        bool IsSystemD3dFullscreen() noexcept
        {
            QUERY_USER_NOTIFICATION_STATE quns = QUNS_ACCEPTS_NOTIFICATIONS;
            if (SUCCEEDED(SHQueryUserNotificationState(&quns)))
                return quns == QUNS_RUNNING_D3D_FULL_SCREEN || quns == QUNS_PRESENTATION_MODE;
            return false;
        }

        bool IsWindowAboveInZOrder(HWND targetTop, HWND baseBelow) noexcept
        {
            if (!targetTop || !baseBelow || targetTop == baseBelow)
                return false;
            HWND cur = GetWindow(baseBelow, GW_HWNDPREV);
            for (int step = 0; cur && step < 96; ++step)
            {
                if (cur == targetTop)
                    return true;
                cur = GetWindow(cur, GW_HWNDPREV);
            }
            return false;
        }

        bool DecodeThumbnailToBgra(
            IWICImagingFactory* wic,
            const BYTE* encodedData,
            UINT encodedSize,
            std::vector<BYTE>& outBgra,
            D2D1_COLOR_F& outAccent)
        {
            if (!wic || !encodedData || encodedSize == 0)
                return false;

            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, encodedSize);
            if (!hMem)
                return false;

            if (void* locked = GlobalLock(hMem))
            {
                std::memcpy(locked, encodedData, encodedSize);
                GlobalUnlock(hMem);
            }
            else
            {
                GlobalFree(hMem);
                return false;
            }

            Microsoft::WRL::ComPtr<IStream> stream;
            if (FAILED(CreateStreamOnHGlobal(hMem, TRUE, stream.GetAddressOf())) || !stream)
            {
                GlobalFree(hMem);
                return false;
            }

            Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
            if (FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf())) || !decoder)
                return false;

            Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
            if (FAILED(decoder->GetFrame(0, frame.GetAddressOf())) || !frame)
                return false;

            Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
            if (FAILED(wic->CreateBitmapScaler(scaler.GetAddressOf())) || !scaler)
                return false;
            if (FAILED(scaler->Initialize(frame.Get(), kThumbPx, kThumbPx, WICBitmapInterpolationModeFant)))
                return false;

            Microsoft::WRL::ComPtr<IWICFormatConverter> conv;
            if (FAILED(wic->CreateFormatConverter(conv.GetAddressOf())) || !conv)
                return false;
            if (FAILED(conv->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
                return false;

            outBgra.resize(static_cast<size_t>(kThumbPx) * kThumbPx * 4u);
            if (FAILED(conv->CopyPixels(nullptr, kThumbPx * 4u, static_cast<UINT>(outBgra.size()), outBgra.data())))
            {
                outBgra.clear();
                return false;
            }

            // Compute subtle average bright accent from center pixels
            double sumR = 0.0, sumG = 0.0, sumB = 0.0;
            int cnt = 0;
            for (int y = 16; y < 48; y += 2)
            {
                for (int x = 16; x < 48; x += 2)
                {
                    const size_t idx = (static_cast<size_t>(y) * kThumbPx + x) * 4u;
                    const double a = outBgra[idx + 3] / 255.0;
                    if (a > 0.25)
                    {
                        sumB += (outBgra[idx + 0] / 255.0) / a;
                        sumG += (outBgra[idx + 1] / 255.0) / a;
                        sumR += (outBgra[idx + 2] / 255.0) / a;
                        ++cnt;
                    }
                }
            }
            if (cnt > 0)
            {
                const float r = static_cast<float>((std::clamp)(sumR / cnt, 0.20, 0.95));
                const float g = static_cast<float>((std::clamp)(sumG / cnt, 0.20, 0.95));
                const float b = static_cast<float>((std::clamp)(sumB / cnt, 0.20, 0.95));
                outAccent = D2D1::ColorF(r, g, b, 1.0f);
            }
            return true;
        }
    } // namespace

    MediaIsland::~MediaIsland()
    {
        Shutdown();
    }

    bool MediaIsland::Initialize(HMODULE shellModule, const SwitcherConfig& cfg, Callbacks callbacks)
    {
        Shutdown();
        m_module = shellModule;
        m_config = cfg;
        m_callbacks = std::move(callbacks);
        m_stopping.store(false, std::memory_order_release);

        if (!CreateRenderFactories())
            return false;

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WndProc;
        wc.hInstance = m_module;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kIslandClass;
        if (RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
            m_classRegistered = true;

        m_hwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kIslandClass,
            L"WinRings Media Island",
            WS_POPUP,
            0, 0, 220, 24,
            nullptr, nullptr, m_module, this);
        if (!m_hwnd)
            return false;

        m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_wakeEvent = CreateEventW(nullptr, FALSE, TRUE, nullptr);
        if (!m_stopEvent || !m_wakeEvent)
        {
            Shutdown();
            return false;
        }

        m_workerThread = CreateThread(nullptr, 0, WorkerThreadProc, this, 0, nullptr);
        if (!m_workerThread)
        {
            Shutdown();
            return false;
        }

        ShellDiagLog(L"MediaIsland", L"Init enabled=%d pos=%s fs=%s offset=%d",
            cfg.mediaIslandEnabled ? 1 : 0, ToString(cfg.mediaIslandPos),
            ToString(cfg.mediaIslandFullscreenMode), cfg.mediaIslandOffsetXPct);
        UpdateWindowBoundsAndRender();
        return true;
    }

    void MediaIsland::UpdateConfig(const SwitcherConfig& cfg)
    {
        m_config = cfg;
        if (!cfg.mediaIslandHoverExpand)
            m_expanded = false;
        if (m_wakeEvent)
            SetEvent(m_wakeEvent);
        UpdateWindowBoundsAndRender();
    }

    void MediaIsland::Shutdown()
    {
        m_stopping.store(true, std::memory_order_release);
        if (m_stopEvent)
            SetEvent(m_stopEvent);
        if (m_workerThread)
        {
            WaitForSingleObject(m_workerThread, 1500);
            CloseHandle(m_workerThread);
            m_workerThread = nullptr;
        }
        if (m_stopEvent)
        {
            CloseHandle(m_stopEvent);
            m_stopEvent = nullptr;
        }
        if (m_wakeEvent)
        {
            CloseHandle(m_wakeEvent);
            m_wakeEvent = nullptr;
        }
        if (m_hwnd && IsWindow(m_hwnd))
        {
            KillTimer(m_hwnd, kTimerExpand);
            KillTimer(m_hwnd, kTimerVolumeHide);
            KillTimer(m_hwnd, kTimerMarquee);
            KillTimer(m_hwnd, kTimerFullscreenPoll);
            DestroyWindow(m_hwnd);
            m_hwnd = nullptr;
        }
        if (m_classRegistered && m_module)
        {
            UnregisterClassW(kIslandClass, m_module);
            m_classRegistered = false;
        }
        ReleaseRenderFactories();
        m_callbacks = {};
        m_expanded = false;
        m_mouseTracked = false;
        m_hoveredBtn = -1;
        m_cachedThumbGen = 0;
        m_marqueeTimerRunning = false;
        m_marqueeOffset = 0.0f;
        m_marqueeMaxOffset = 0.0f;
        m_marqueePauseTicks = 0;
        m_marqueePhase = 0;
        m_marqueeLastTitle.clear();
        m_wheelAccum = 0;
        m_wheelLastTick = 0;
        m_draggingPos = false;
        m_fullscreenTimerRunning = false;
        m_lastFullscreenHide = false;
    }

    void MediaIsland::OnTaskbarVisibilityChanged(bool taskbarHidden)
    {
        m_taskbarHidden = taskbarHidden;
        UpdateWindowBoundsAndRender();
    }

    void MediaIsland::OnForegroundChanged()
    {
        UpdateWindowBoundsAndRender();
    }

    void MediaIsland::RefreshLayoutAndVisibility()
    {
        UpdateWindowBoundsAndRender();
    }

    void MediaIsland::NotifyLayoutRefresh() noexcept
    {
        if (m_hwnd && IsWindow(m_hwnd) && !m_stopping.load(std::memory_order_acquire))
            PostMessageW(m_hwnd, kMsgMediaUpdated, 0, 0);
    }

    bool MediaIsland::IsActiveVisible() const noexcept
    {
        return m_hwnd && IsWindow(m_hwnd) && IsWindowVisible(m_hwnd) != FALSE;
    }

    bool MediaIsland::HitTestScreenPoint(POINT screenPt) const noexcept
    {
        if (!m_hwnd || !IsWindowVisible(m_hwnd) ||
            !m_config.mediaIslandEnabled || !m_config.mediaIslandScrollVolume || m_draggingPos)
            return false;
        RECT rc{};
        if (!GetWindowRect(m_hwnd, &rc))
            return false;
        return PtInRect(&rc, screenPt) != FALSE;
    }

    void MediaIsland::PostExternalWheel(short wheelDelta) noexcept
    {
        if (m_hwnd && IsWindow(m_hwnd))
            PostMessageW(m_hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(wheelDelta)), 0);
    }

    bool MediaIsland::CreateRenderFactories()
    {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.ReleaseAndGetAddressOf())) || !m_d2dFactory)
            return false;

        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
        if (FAILED(m_d2dFactory->CreateDCRenderTarget(&props, m_dcTarget.ReleaseAndGetAddressOf())) || !m_dcTarget)
            return false;

        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(m_dwriteFactory.ReleaseAndGetAddressOf()))) || !m_dwriteFactory)
            return false;

        auto makeFmt = [&](float size, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** out) {
            if (SUCCEEDED(m_dwriteFactory->CreateTextFormat(
                L"Segoe UI Variable Text", nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                size, L"en-us", out)) && *out)
            {
                (*out)->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                (*out)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
                Microsoft::WRL::ComPtr<IDWriteInlineObject> ell;
                m_dwriteFactory->CreateEllipsisTrimmingSign(*out, ell.GetAddressOf());
                (*out)->SetTrimming(&trim, ell.Get());
            }
        };

        makeFmt(13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, m_titleFmt.ReleaseAndGetAddressOf());
        makeFmt(11.5f, DWRITE_FONT_WEIGHT_NORMAL, m_artistFmt.ReleaseAndGetAddressOf());
        makeFmt(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, m_pillFmt.ReleaseAndGetAddressOf());
        makeFmt(10.5f, DWRITE_FONT_WEIGHT_MEDIUM, m_smallFmt.ReleaseAndGetAddressOf());
        return true;
    }

    void MediaIsland::ReleaseRenderFactories()
    {
        m_thumbBitmap.Reset();
        m_smallFmt.Reset();
        m_pillFmt.Reset();
        m_artistFmt.Reset();
        m_titleFmt.Reset();
        m_dwriteFactory.Reset();
        m_dcTarget.Reset();
        m_d2dFactory.Reset();
    }

    DWORD WINAPI MediaIsland::WorkerThreadProc(void* param)
    {
        static_cast<MediaIsland*>(param)->WorkerLoop();
        return 0;
    }

    void MediaIsland::WorkerLoop()
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);

        Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(wicFactory.GetAddressOf()));

        wmc::GlobalSystemMediaTransportControlsSessionManager manager{ nullptr };
        try
        {
            manager = wmc::GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        }
        catch (...) {}

        wmc::GlobalSystemMediaTransportControlsSessionManager::CurrentSessionChanged_revoker curRev;
        wmc::GlobalSystemMediaTransportControlsSessionManager::SessionsChanged_revoker sessRev;
        wmc::GlobalSystemMediaTransportControlsSession activeSession{ nullptr };
        wmc::GlobalSystemMediaTransportControlsSession::MediaPropertiesChanged_revoker propRev;
        wmc::GlobalSystemMediaTransportControlsSession::PlaybackInfoChanged_revoker playRev;

        auto signalWake = [this]() {
            if (!m_stopping.load(std::memory_order_acquire) && m_wakeEvent)
                SetEvent(m_wakeEvent);
        };

        if (manager)
        {
            curRev = manager.CurrentSessionChanged(winrt::auto_revoke,
                [signalWake](auto const&, auto const&) { signalWake(); });
            sessRev = manager.SessionsChanged(winrt::auto_revoke,
                [signalWake](auto const&, auto const&) { signalWake(); });
        }

        std::wstring prevTitle, prevArtist;
        uint64_t thumbGenCounter = 0;

        HANDLE waitHandles[2] = { m_stopEvent, m_wakeEvent };
        while (!m_stopping.load(std::memory_order_acquire))
        {
            const DWORD waitRes = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
            if (waitRes == WAIT_OBJECT_0 || m_stopping.load(std::memory_order_acquire))
                break;

            if (!manager)
            {
                try { manager = wmc::GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get(); }
                catch (...) {}
                if (manager)
                {
                    curRev = manager.CurrentSessionChanged(winrt::auto_revoke,
                        [signalWake](auto const&, auto const&) { signalWake(); });
                    sessRev = manager.SessionsChanged(winrt::auto_revoke,
                        [signalWake](auto const&, auto const&) { signalWake(); });
                }
            }

            // Pick active session: prefer CurrentSession, else any Playing session, else first session
            wmc::GlobalSystemMediaTransportControlsSession chosen{ nullptr };
            if (manager)
            {
                try
                {
                    chosen = manager.GetCurrentSession();
                    if (!chosen)
                    {
                        auto sessions = manager.GetSessions();
                        for (uint32_t i = 0; i < sessions.Size(); ++i)
                        {
                            auto s = sessions.GetAt(i);
                            if (!chosen)
                                chosen = s;
                            auto pb = s.GetPlaybackInfo();
                            if (pb && pb.PlaybackStatus() == wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing)
                            {
                                chosen = s;
                                break;
                            }
                        }
                    }
                }
                catch (...) {}
            }

            if (chosen != activeSession)
            {
                propRev.revoke();
                playRev.revoke();
                activeSession = chosen;
                if (activeSession)
                {
                    try
                    {
                        propRev = activeSession.MediaPropertiesChanged(winrt::auto_revoke,
                            [signalWake](auto const&, auto const&) { signalWake(); });
                        playRev = activeSession.PlaybackInfoChanged(winrt::auto_revoke,
                            [signalWake](auto const&, auto const&) { signalWake(); });
                    }
                    catch (...) {}
                }
            }

            // Execute any queued transport command before refreshing snapshot
            const int cmd = m_pendingCmd.exchange(0, std::memory_order_acq_rel);
            if (cmd != 0 && activeSession)
            {
                try
                {
                    if (cmd == static_cast<int>(TransportCmd::Previous))
                        activeSession.TrySkipPreviousAsync().get();
                    else if (cmd == static_cast<int>(TransportCmd::TogglePlayPause))
                        activeSession.TryTogglePlayPauseAsync().get();
                    else if (cmd == static_cast<int>(TransportCmd::Next))
                        activeSession.TrySkipNextAsync().get();
                }
                catch (...) {}
            }

            MediaSnapshot nextSnap{};
            if (activeSession)
            {
                try
                {
                    auto pb = activeSession.GetPlaybackInfo();
                    const auto status = pb
                        ? pb.PlaybackStatus()
                        : wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Closed;

                    auto props = activeSession.TryGetMediaPropertiesAsync().get();
                    std::wstring title = props ? std::wstring(props.Title()) : std::wstring();
                    std::wstring artist = props ? std::wstring(props.Artist()) : std::wstring();
                    if (title.empty())
                        title = artist.empty() ? std::wstring(L"Media Playing") : artist;

                    nextSnap.hasSession = !title.empty() &&
                        (status != wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Closed);
                    nextSnap.isPlaying = (status == wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing);
                    nextSnap.title = title;
                    nextSnap.artist = artist;

                    const bool trackChanged = (title != prevTitle || artist != prevArtist);
                    if (props && props.Thumbnail() && (trackChanged || thumbGenCounter == 0))
                    {
                        auto stream = props.Thumbnail().OpenReadAsync().get();
                        if (stream)
                        {
                            const uint64_t rawLen = stream.Size();
                            if (rawLen > 0 && rawLen <= 4ull * 1024ull * 1024ull)
                            {
                                const uint32_t sz = static_cast<uint32_t>(rawLen);
                                wss::Buffer buf(sz);
                                auto readBuf = stream.ReadAsync(buf, sz, wss::InputStreamOptions::None).get();
                                if (readBuf && readBuf.Length() > 0)
                                {
                                    auto ba = readBuf.as<::Windows::Storage::Streams::IBufferByteAccess>();
                                    BYTE* rawPtr = nullptr;
                                    if (SUCCEEDED(ba->Buffer(&rawPtr)) && rawPtr)
                                    {
                                        std::vector<BYTE> bgra;
                                        D2D1_COLOR_F acc{ 0.35f, 0.56f, 0.96f, 1.0f };
                                        if (DecodeThumbnailToBgra(wicFactory.Get(), rawPtr, readBuf.Length(), bgra, acc))
                                        {
                                            ++thumbGenCounter;
                                            nextSnap.thumbGen = thumbGenCounter;
                                            nextSnap.thumbW = kThumbPx;
                                            nextSnap.thumbH = kThumbPx;
                                            nextSnap.thumbBgra = std::move(bgra);
                                            nextSnap.accent = acc;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    else if (!trackChanged)
                    {
                        std::lock_guard<std::mutex> lock(m_stateMutex);
                        nextSnap.thumbGen = m_snapshot.thumbGen;
                        nextSnap.thumbW = m_snapshot.thumbW;
                        nextSnap.thumbH = m_snapshot.thumbH;
                        nextSnap.thumbBgra = m_snapshot.thumbBgra;
                        nextSnap.accent = m_snapshot.accent;
                    }

                    prevTitle = title;
                    prevArtist = artist;
                }
                catch (...) {}
            }
            else
            {
                prevTitle.clear();
                prevArtist.clear();
            }

            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                m_snapshot = std::move(nextSnap);
            }

            if (m_hwnd && IsWindow(m_hwnd) && !m_stopping.load(std::memory_order_acquire))
                PostMessageW(m_hwnd, kMsgMediaUpdated, 0, 0);
        }

        propRev.revoke();
        playRev.revoke();
        curRev.revoke();
        sessRev.revoke();
        wicFactory.Reset();
        winrt::uninit_apartment();
    }

    bool MediaIsland::ShouldHideForFullscreen() const noexcept
    {
        if (m_config.mediaIslandFullscreenMode == MediaIslandFullscreenMode::NeverHide)
            return false;
        if (IsSystemD3dFullscreen())
            return true;
        if (m_config.mediaIslandFullscreenMode == MediaIslandFullscreenMode::GamesOnly)
            return false;

        HWND tb = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (!tb || !IsWindowVisible(tb))
            return true;

        HWND fg = GetForegroundWindow();
        if (!fg || !IsWindowVisible(fg) || IsIconic(fg))
            return false;

        wchar_t cls[128]{};
        if (GetClassNameW(fg, cls, _countof(cls)) > 0)
        {
            if (_wcsicmp(cls, L"Progman") == 0 ||
                _wcsicmp(cls, L"WorkerW") == 0 ||
                _wcsicmp(cls, L"Shell_TrayWnd") == 0 ||
                _wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
                wcsncmp(cls, L"WinRings.", 9) == 0)
            {
                return false;
            }
        }

        const POINT ptZero{ 0, 0 };
        HMONITOR primMon = MonitorFromPoint(ptZero, MONITOR_DEFAULTTOPRIMARY);
        HMONITOR fgMon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
        if (fgMon != primMon)
            return false;

        MONITORINFO mi{ sizeof(mi) };
        RECT rcFg{};
        if (!GetMonitorInfoW(primMon, &mi) || !GetWindowRect(fg, &rcFg))
            return false;

        const bool coversMonitor =
            rcFg.left <= mi.rcMonitor.left + 2 &&
            rcFg.top <= mi.rcMonitor.top + 2 &&
            rcFg.right >= mi.rcMonitor.right - 2 &&
            rcFg.bottom >= mi.rcMonitor.bottom - 2;
        if (!coversMonitor)
            return false;

        HWND fgRoot = GetAncestor(fg, GA_ROOTOWNER);
        return IsWindowAboveInZOrder(fg, tb) || (fgRoot && IsWindowAboveInZOrder(fgRoot, tb));
    }

    bool MediaIsland::ShouldBeVisible(const MediaSnapshot& snap) const noexcept
    {
        if (!m_config.mediaIslandEnabled || m_taskbarHidden)
            return false;
        if (!snap.hasSession)
            return false;
        if (m_config.mediaIslandHideWhenPaused && !snap.isPlaying)
            return false;
        if (ShouldHideForFullscreen())
            return false;
        return true;
    }

    void MediaIsland::ShowIslandContextMenu(POINT screenPt)
    {
        if (!m_hwnd || !IsWindow(m_hwnd))
            return;
        HMENU menu = CreatePopupMenu();
        if (!menu)
            return;

        constexpr UINT kCmdToggleIsland = 1;
        constexpr UINT kCmdResetPos = 2;
        constexpr UINT kCmdOpenSettings = 3;

        AppendMenuW(menu, MF_STRING | (m_config.mediaIslandEnabled ? MF_CHECKED : MF_UNCHECKED),
            kCmdToggleIsland, L"Show Media Island");
        AppendMenuW(menu, MF_STRING | (m_config.mediaIslandOffsetXPct < 0 ? MF_GRAYED : MF_ENABLED),
            kCmdResetPos, L"Reset Position (Auto)");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kCmdOpenSettings, L"Settings...");

        SetForegroundWindow(m_hwnd);
        const UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPt.x, screenPt.y, m_hwnd, nullptr);
        DestroyMenu(menu);
        PostMessageW(m_hwnd, WM_NULL, 0, 0);

        if (cmd == kCmdToggleIsland && m_callbacks.onToggleEnabled)
            m_callbacks.onToggleEnabled();
        else if (cmd == kCmdResetPos && m_callbacks.onSaveOffsetXPct)
            m_callbacks.onSaveOffsetXPct(-1);
        else if (cmd == kCmdOpenSettings && m_callbacks.onOpenSettings)
            m_callbacks.onOpenSettings();
    }

    void MediaIsland::EnsureThumbBitmap(const MediaSnapshot& snap)
    {
        if (!m_dcTarget)
            return;
        if (snap.thumbGen == 0 || snap.thumbBgra.empty() || snap.thumbW <= 0 || snap.thumbH <= 0)
        {
            m_thumbBitmap.Reset();
            m_cachedThumbGen = 0;
            return;
        }
        if (m_cachedThumbGen == snap.thumbGen && m_thumbBitmap)
            return;

        D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        m_thumbBitmap.Reset();
        if (SUCCEEDED(m_dcTarget->CreateBitmap(
            D2D1::SizeU(static_cast<UINT32>(snap.thumbW), static_cast<UINT32>(snap.thumbH)),
            snap.thumbBgra.data(),
            static_cast<UINT32>(snap.thumbW * 4),
            &bp,
            m_thumbBitmap.GetAddressOf())))
        {
            m_cachedThumbGen = snap.thumbGen;
        }
    }

    void MediaIsland::UpdateWindowBoundsAndRender()
    {
        if (!m_hwnd || !IsWindow(m_hwnd))
            return;

        MediaSnapshot snap;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            snap = m_snapshot;
        }

        const bool wantFsPoll = m_config.mediaIslandEnabled && snap.hasSession && !m_taskbarHidden &&
            m_config.mediaIslandFullscreenMode != MediaIslandFullscreenMode::NeverHide &&
            !(m_config.mediaIslandHideWhenPaused && !snap.isPlaying);
        if (wantFsPoll && !m_fullscreenTimerRunning)
        {
            SetTimer(m_hwnd, kTimerFullscreenPoll, 350, nullptr);
            m_fullscreenTimerRunning = true;
        }
        else if (!wantFsPoll && m_fullscreenTimerRunning)
        {
            KillTimer(m_hwnd, kTimerFullscreenPoll);
            m_fullscreenTimerRunning = false;
        }

        const bool fsHide = ShouldHideForFullscreen();
        m_lastFullscreenHide = fsHide;
        if (!m_config.mediaIslandEnabled || m_taskbarHidden || !snap.hasSession ||
            (m_config.mediaIslandHideWhenPaused && !snap.isPlaying) || fsHide)
        {
            m_expanded = false;
            m_draggingPos = false;
            if (m_marqueeTimerRunning && m_hwnd && IsWindow(m_hwnd))
            {
                KillTimer(m_hwnd, kTimerMarquee);
                m_marqueeTimerRunning = false;
            }
            ShowWindow(m_hwnd, SW_HIDE);
            return;
        }
        if (!m_config.mediaIslandHoverExpand)
            m_expanded = false;

        const POINT ptZero{ 0, 0 };
        HMONITOR hMon = MonitorFromPoint(ptZero, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{ sizeof(mi) };
        if (!GetMonitorInfoW(hMon, &mi))
            return;

        m_dpiScale = QueryPrimaryMonitorDpiScale(hMon);
        const int monLeft = mi.rcMonitor.left;
        const int monTop = mi.rcMonitor.top;
        const int monRight = mi.rcMonitor.right;
        const int monW = monRight - monLeft;

        float pillW = kExpandedW;
        float pillH = kExpandedH;
        float fullTextW = 0.0f;
        const float barDip = static_cast<float>((std::clamp)(m_config.topBarHeightDip, 24, 48));

        if (snap.title != m_marqueeLastTitle)
        {
            m_marqueeLastTitle = snap.title;
            m_marqueeOffset = 0.0f;
            m_marqueeMaxOffset = 0.0f;
            m_marqueePauseTicks = 0;
            m_marqueePhase = 0;
        }

        if (!m_expanded)
        {
            pillH = (std::clamp)(barDip - 4.0f, 18.0f, 24.0f);
            float textW = 76.0f;
            const std::wstring displayTitle = m_showVolumeOverlay
                ? (L"Vol " + std::to_wstring(m_lastVolumePct) + L"%")
                : snap.title;

            if (m_dwriteFactory && m_pillFmt && !displayTitle.empty())
            {
                Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
                if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                    displayTitle.c_str(), static_cast<UINT32>(displayTitle.size()),
                    m_pillFmt.Get(), 1024.0f, 24.0f, layout.GetAddressOf())) && layout)
                {
                    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                    const DWRITE_TRIMMING noTrim{ DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0 };
                    layout->SetTrimming(&noTrim, nullptr);
                    if (ContainsRtlChars(displayTitle))
                        layout->SetReadingDirection(DWRITE_READING_DIRECTION_RIGHT_TO_LEFT);
                    DWRITE_TEXT_METRICS tm{};
                    if (SUCCEEDED(layout->GetMetrics(&tm)))
                    {
                        fullTextW = tm.widthIncludingTrailingWhitespace;
                        textW = (std::clamp)(fullTextW + 6.0f, 54.0f, 150.0f);
                    }
                }
            }
            // Left art (26) + title + right controls (62) + padding (12)
            pillW = (std::clamp)(26.0f + textW + 62.0f + 12.0f, kCompactMinW, kCompactMaxW);
        }

        int widthPx = static_cast<int>(std::ceil(pillW * m_dpiScale));
        const int heightPx = static_cast<int>(std::ceil(pillH * m_dpiScale));
        const int barPx = static_cast<int>(std::round(barDip * m_dpiScale));

        int dstX = monLeft + (monW - widthPx) / 2;
        const int maxRightX = (std::max)(monLeft + 4, monRight - widthPx - 4);
        if (m_draggingPos)
        {
            dstX = (std::clamp)(m_dragCurrentX, monLeft + 4, maxRightX);
        }
        else if (m_config.mediaIslandOffsetXPct >= 0)
        {
            const int spanPx = (std::max)(1, monW - widthPx - 8);
            dstX = monLeft + 4 + (spanPx * (std::clamp)(m_config.mediaIslandOffsetXPct, 0, 100)) / 100;
            dstX = (std::clamp)(dstX, monLeft + 4, maxRightX);
        }
        else
        {
            const int trayWDip = m_callbacks.getSystemTrayWidthDip ? m_callbacks.getSystemTrayWidthDip() : 0;
            const int bufPx = static_cast<int>(std::round(12.0 * m_dpiScale));
            const int safeRightPx = (trayWDip > 40)
                ? (monRight - static_cast<int>(std::ceil(trayWDip * m_dpiScale)) - bufPx)
                : (monRight - (monW * 44) / 100);

            if (m_config.mediaIslandPos == MediaIslandPos::Left)
                dstX = monLeft + (monW * 26) / 100;
            else if (m_config.mediaIslandPos == MediaIslandPos::NearTray)
                dstX = safeRightPx - widthPx;

            if (dstX + widthPx > safeRightPx)
                dstX = safeRightPx - widthPx;
            if (dstX < monLeft + 8)
            {
                dstX = monLeft + 8;
                const int availPx = safeRightPx - dstX;
                if (!m_expanded && availPx > 100)
                {
                    const float availDip = static_cast<float>(availPx) / static_cast<float>(m_dpiScale);
                    if (availDip < pillW)
                    {
                        pillW = (std::max)(150.0f, availDip);
                        widthPx = static_cast<int>(std::ceil(pillW * m_dpiScale));
                    }
                }
            }
            const int clampedMaxRight = (std::max)(monLeft + 4, monRight - widthPx - 4);
            dstX = (std::clamp)(dstX, monLeft + 4, clampedMaxRight);
        }

        bool wantMarquee = false;
        if (!m_expanded)
        {
            const float artSzEst = (std::min)(17.0f, pillH - 5.0f);
            const float clipW = (std::max)(20.0f, pillW - 79.5f - artSzEst);
            const float overflow = (std::max)(0.0f, fullTextW - clipW);
            if (!m_showVolumeOverlay && m_config.mediaIslandMarquee && overflow > 2.0f)
            {
                m_marqueeMaxOffset = overflow;
                m_marqueeOffset = (std::min)(m_marqueeOffset, m_marqueeMaxOffset);
                wantMarquee = (snap.isPlaying || m_mouseTracked);
            }
            else
            {
                m_marqueeMaxOffset = 0.0f;
            }
        }
        else
        {
            m_marqueeMaxOffset = 0.0f;
        }

        if (wantMarquee && !m_marqueeTimerRunning && m_hwnd && IsWindow(m_hwnd))
        {
            SetTimer(m_hwnd, kTimerMarquee, 45, nullptr);
            m_marqueeTimerRunning = true;
        }
        else if (!wantMarquee && m_marqueeTimerRunning && m_hwnd && IsWindow(m_hwnd))
        {
            KillTimer(m_hwnd, kTimerMarquee);
            m_marqueeTimerRunning = false;
            m_marqueeOffset = 0.0f;
            m_marqueePauseTicks = 0;
            m_marqueePhase = 0;
        }

        const int dstY = m_expanded
            ? (monTop + static_cast<int>(std::round(2.0 * m_dpiScale)))
            : (monTop + (std::max)(1, (barPx - heightPx) / 2));

        Render(snap, widthPx, heightPx, dstX, dstY);
    }

    void MediaIsland::Render(const MediaSnapshot& snap, int widthPx, int heightPx, int dstX, int dstY)
    {
        if (!m_d2dFactory || !m_dcTarget || widthPx <= 0 || heightPx <= 0)
            return;

        HDC screenDc = GetDC(nullptr);
        if (!screenDc)
            return;
        HDC memDc = CreateCompatibleDC(screenDc);
        if (!memDc)
        {
            ReleaseDC(nullptr, screenDc);
            return;
        }

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = widthPx;
        bmi.bmiHeader.biHeight = -heightPx;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib || !bits)
        {
            if (dib) DeleteObject(dib);
            DeleteDC(memDc);
            ReleaseDC(nullptr, screenDc);
            return;
        }
        std::memset(bits, 0, static_cast<size_t>(widthPx) * heightPx * 4u);
        HGDIOBJ oldBmp = SelectObject(memDc, dib);

        const RECT bindRc{ 0, 0, widthPx, heightPx };
        if (SUCCEEDED(m_dcTarget->BindDC(memDc, &bindRc)))
        {
            EnsureThumbBitmap(snap);

            const float w = static_cast<float>(widthPx / m_dpiScale);
            const float h = static_cast<float>(heightPx / m_dpiScale);
            const float radius = m_expanded ? 17.0f : (h * 0.5f);

            m_dcTarget->BeginDraw();
            m_dcTarget->Clear(D2D1::ColorF(0, 0, 0, 0));
            m_dcTarget->SetTransform(D2D1::Matrix3x2F::Scale(
                static_cast<float>(m_dpiScale), static_cast<float>(m_dpiScale)));

            auto mkBrush = [&](D2D1_COLOR_F c) {
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> b;
                m_dcTarget->CreateSolidColorBrush(c, b.GetAddressOf());
                return b;
            };

            const D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(
                D2D1::RectF(0.75f, 0.75f, w - 0.75f, h - 0.75f), radius, radius);

            // Base dark frosted island capsule
            const D2D1_COLOR_F bgCol = m_expanded
                ? D2D1::ColorF(18.0f / 255.0f, 19.0f / 255.0f, 24.0f / 255.0f, 0.94f)
                : D2D1::ColorF(16.0f / 255.0f, 17.0f / 255.0f, 21.0f / 255.0f, 0.94f);
            if (auto b = mkBrush(bgCol))
                m_dcTarget->FillRoundedRectangle(pill, b.Get());

            // Subtle album-art radial glow inside card
            {
                D2D1_COLOR_F g0 = snap.accent;
                g0.a = m_expanded ? 0.22f : 0.16f;
                D2D1_COLOR_F g1 = snap.accent;
                g1.a = 0.0f;
                D2D1_GRADIENT_STOP stops[2] = { { 0.0f, g0 }, { 1.0f, g1 } };
                Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> gsc;
                if (SUCCEEDED(m_dcTarget->CreateGradientStopCollection(stops, 2, gsc.GetAddressOf())) && gsc)
                {
                    Microsoft::WRL::ComPtr<ID2D1RadialGradientBrush> glow;
                    const float gx = m_expanded ? 36.0f : 14.0f;
                    const float gy = h * 0.5f;
                    const float gr = m_expanded ? 110.0f : 44.0f;
                    if (SUCCEEDED(m_dcTarget->CreateRadialGradientBrush(
                        D2D1::RadialGradientBrushProperties(D2D1::Point2F(gx, gy), D2D1::Point2F(0, 0), gr, gr),
                        gsc.Get(), glow.GetAddressOf())) && glow)
                    {
                        m_dcTarget->FillRoundedRectangle(pill, glow.Get());
                    }
                }
            }

            // Crisp hairline border
            if (auto b = mkBrush(D2D1::ColorF(90.0f / 255.0f, 94.0f / 255.0f, 108.0f / 255.0f, 0.72f)))
                m_dcTarget->DrawRoundedRectangle(pill, b.Get(), 0.9f);

            auto drawThumb = [&](D2D1_RECT_F artRc, float artRad) {
                if (m_thumbBitmap)
                {
                    Microsoft::WRL::ComPtr<ID2D1BitmapBrush> bmpBrush;
                    if (SUCCEEDED(m_dcTarget->CreateBitmapBrush(m_thumbBitmap.Get(), bmpBrush.GetAddressOf())) && bmpBrush)
                    {
                        const D2D1_SIZE_F sz = m_thumbBitmap->GetSize();
                        const float sx = (artRc.right - artRc.left) / (std::max)(1.0f, sz.width);
                        const float sy = (artRc.bottom - artRc.top) / (std::max)(1.0f, sz.height);
                        bmpBrush->SetTransform(
                            D2D1::Matrix3x2F::Scale(sx, sy) *
                            D2D1::Matrix3x2F::Translation(artRc.left, artRc.top));
                        m_dcTarget->FillRoundedRectangle(D2D1::RoundedRect(artRc, artRad, artRad), bmpBrush.Get());
                    }
                }
                else
                {
                    D2D1_COLOR_F artBg = snap.accent;
                    artBg.a = 0.85f;
                    if (auto ab = mkBrush(artBg))
                        m_dcTarget->FillRoundedRectangle(D2D1::RoundedRect(artRc, artRad, artRad), ab.Get());
                    if (auto nb = mkBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f)))
                    {
                        const float cx = (artRc.left + artRc.right) * 0.5f;
                        const float cy = (artRc.top + artRc.bottom) * 0.5f;
                        const float r = (artRc.right - artRc.left) * 0.14f;
                        m_dcTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx - r * 0.5f, cy + r), r, r * 0.8f), nb.Get());
                        m_dcTarget->DrawLine(D2D1::Point2F(cx + r * 0.35f, cy + r), D2D1::Point2F(cx + r * 0.35f, cy - r * 1.4f), nb.Get(), 1.6f);
                        m_dcTarget->DrawLine(D2D1::Point2F(cx + r * 0.35f, cy - r * 1.4f), D2D1::Point2F(cx + r * 1.6f, cy - r * 0.8f), nb.Get(), 1.6f);
                    }
                }
            };

            auto drawTriangle = [&](D2D1_POINT_2F p0, D2D1_POINT_2F p1, D2D1_POINT_2F p2, ID2D1Brush* brush) {
                Microsoft::WRL::ComPtr<ID2D1PathGeometry> geo;
                if (SUCCEEDED(m_d2dFactory->CreatePathGeometry(geo.GetAddressOf())) && geo)
                {
                    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(geo->Open(sink.GetAddressOf())) && sink)
                    {
                        sink->BeginFigure(p0, D2D1_FIGURE_BEGIN_FILLED);
                        sink->AddLine(p1);
                        sink->AddLine(p2);
                        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                        sink->Close();
                        m_dcTarget->FillGeometry(geo.Get(), brush);
                    }
                }
            };

            auto drawTransport = [&](int idx, const D2D1_RECT_F& rc, bool primary) {
                const bool hot = (m_hoveredBtn == idx);
                const float cx = (rc.left + rc.right) * 0.5f;
                const float cy = (rc.top + rc.bottom) * 0.5f;
                const float r = (std::min)(rc.right - rc.left, rc.bottom - rc.top) * 0.5f;

                if (primary && m_expanded)
                {
                    D2D1_COLOR_F bg = hot
                        ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f)
                        : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.88f);
                    if (auto b = mkBrush(bg))
                        m_dcTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), b.Get());
                }
                else if (hot)
                {
                    if (auto b = mkBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f)))
                        m_dcTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), b.Get());
                }

                const D2D1_COLOR_F fg = (primary && m_expanded)
                    ? D2D1::ColorF(0.08f, 0.09f, 0.11f, 1.0f)
                    : (hot ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f) : D2D1::ColorF(0.90f, 0.92f, 0.96f, 0.92f));
                auto fgBrush = mkBrush(fg);
                if (!fgBrush)
                    return;

                const float s = m_expanded ? 4.8f : 3.4f;
                if (idx == 0) // Previous
                {
                    m_dcTarget->DrawLine(D2D1::Point2F(cx - s * 0.85f, cy - s * 0.85f),
                        D2D1::Point2F(cx - s * 0.85f, cy + s * 0.85f), fgBrush.Get(), 1.5f);
                    drawTriangle(
                        D2D1::Point2F(cx - s * 0.55f, cy),
                        D2D1::Point2F(cx + s * 0.85f, cy - s * 0.85f),
                        D2D1::Point2F(cx + s * 0.85f, cy + s * 0.85f),
                        fgBrush.Get());
                }
                else if (idx == 1) // Play / Pause
                {
                    if (snap.isPlaying)
                    {
                        const float bw = m_expanded ? 2.4f : 1.9f;
                        const float bh = s * 1.7f;
                        const float gap = m_expanded ? 2.4f : 1.7f;
                        m_dcTarget->FillRoundedRectangle(D2D1::RoundedRect(
                            D2D1::RectF(cx - gap - bw, cy - bh * 0.5f, cx - gap, cy + bh * 0.5f), 0.9f, 0.9f), fgBrush.Get());
                        m_dcTarget->FillRoundedRectangle(D2D1::RoundedRect(
                            D2D1::RectF(cx + gap, cy - bh * 0.5f, cx + gap + bw, cy + bh * 0.5f), 0.9f, 0.9f), fgBrush.Get());
                    }
                    else
                    {
                        drawTriangle(
                            D2D1::Point2F(cx - s * 0.55f, cy - s * 0.95f),
                            D2D1::Point2F(cx + s * 0.95f, cy),
                            D2D1::Point2F(cx - s * 0.55f, cy + s * 0.95f),
                            fgBrush.Get());
                    }
                }
                else if (idx == 2) // Next
                {
                    drawTriangle(
                        D2D1::Point2F(cx - s * 0.85f, cy - s * 0.85f),
                        D2D1::Point2F(cx + s * 0.55f, cy),
                        D2D1::Point2F(cx - s * 0.85f, cy + s * 0.85f),
                        fgBrush.Get());
                    m_dcTarget->DrawLine(D2D1::Point2F(cx + s * 0.85f, cy - s * 0.85f),
                        D2D1::Point2F(cx + s * 0.85f, cy + s * 0.85f), fgBrush.Get(), 1.5f);
                }
            };

            if (!m_expanded)
            {
                const float artSz = (std::min)(17.0f, h - 5.0f);
                const float artY = (h - artSz) * 0.5f;
                drawThumb(D2D1::RectF(5.5f, artY, 5.5f + artSz, artY + artSz), 4.5f);

                const float btnSz = 18.0f;
                const float btnY = (h - btnSz) * 0.5f;
                const float rightEdge = w - 6.0f;
                m_btnNextDip = D2D1::RectF(rightEdge - btnSz, btnY, rightEdge, btnY + btnSz);
                m_btnPlayDip = D2D1::RectF(m_btnNextDip.left - btnSz - 2.0f, btnY, m_btnNextDip.left - 2.0f, btnY + btnSz);
                m_btnPrevDip = D2D1::RectF(m_btnPlayDip.left - btnSz - 2.0f, btnY, m_btnPlayDip.left - 2.0f, btnY + btnSz);

                const std::wstring text = m_showVolumeOverlay
                    ? (L"Vol " + std::to_wstring(m_lastVolumePct) + L"%")
                    : snap.title;
                const D2D1_COLOR_F txtCol = m_showVolumeOverlay
                    ? D2D1::ColorF(0.45f, 0.82f, 1.0f, 1.0f)
                    : D2D1::ColorF(0.96f, 0.97f, 0.99f, 0.96f);
                if (m_pillFmt && !text.empty())
                {
                    if (auto tb = mkBrush(txtCol))
                    {
                        const D2D1_RECT_F tr = D2D1::RectF(5.5f + artSz + 6.0f, 1.0f, m_btnPrevDip.left - 4.0f, h - 1.0f);
                        const float clipW = (std::max)(1.0f, tr.right - tr.left);
                        bool drewMarquee = false;
                        if (!m_showVolumeOverlay && m_config.mediaIslandMarquee && m_dwriteFactory)
                        {
                            Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
                            if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                                text.c_str(), static_cast<UINT32>(text.size()),
                                m_pillFmt.Get(), 1024.0f, (std::max)(1.0f, tr.bottom - tr.top),
                                layout.GetAddressOf())) && layout)
                            {
                                layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                                const DWRITE_TRIMMING noTrim{ DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0 };
                                layout->SetTrimming(&noTrim, nullptr);
                                const bool rtl = ContainsRtlChars(text);
                                layout->SetReadingDirection(rtl
                                    ? DWRITE_READING_DIRECTION_RIGHT_TO_LEFT
                                    : DWRITE_READING_DIRECTION_LEFT_TO_RIGHT);
                                layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

                                DWRITE_TEXT_METRICS tm{};
                                if (SUCCEEDED(layout->GetMetrics(&tm)))
                                {
                                    const float fullW = tm.widthIncludingTrailingWhitespace;
                                    const float overflow = fullW - clipW;
                                    if (overflow > 2.0f)
                                    {
                                        const float clampedOff = (std::clamp)(m_marqueeOffset, 0.0f, overflow);
                                        const float originX = rtl
                                            ? (tr.right - (tm.left + fullW) + clampedOff)
                                            : (tr.left - tm.left - clampedOff);
                                        m_dcTarget->PushAxisAlignedClip(tr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                                        m_dcTarget->DrawTextLayout(
                                            D2D1::Point2F(originX, tr.top),
                                            layout.Get(),
                                            tb.Get(),
                                            D2D1_DRAW_TEXT_OPTIONS_NONE);
                                        m_dcTarget->PopAxisAlignedClip();
                                        drewMarquee = true;
                                    }
                                }
                            }
                        }
                        if (!drewMarquee)
                            m_dcTarget->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), m_pillFmt.Get(), tr, tb.Get());
                    }
                }

                drawTransport(0, m_btnPrevDip, false);
                drawTransport(1, m_btnPlayDip, false);
                drawTransport(2, m_btnNextDip, false);
            }
            else
            {
                const float artSz = 52.0f;
                const float artX = 12.0f;
                const float artY = (h - artSz) * 0.5f;
                drawThumb(D2D1::RectF(artX, artY, artX + artSz, artY + artSz), 10.0f);

                const float textLeft = artX + artSz + 12.0f;
                const float textRight = w - 12.0f;

                if (m_titleFmt && !snap.title.empty())
                {
                    if (auto tb = mkBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.98f)))
                    {
                        m_dcTarget->DrawTextW(snap.title.c_str(), static_cast<UINT32>(snap.title.size()),
                            m_titleFmt.Get(), D2D1::RectF(textLeft, 10.0f, textRight, 28.0f), tb.Get());
                    }
                }
                const std::wstring subText = snap.artist.empty()
                    ? (snap.isPlaying ? L"Playing" : L"Paused")
                    : snap.artist;
                if (m_artistFmt && !subText.empty())
                {
                    if (auto ab = mkBrush(D2D1::ColorF(0.68f, 0.72f, 0.78f, 0.92f)))
                    {
                        m_dcTarget->DrawTextW(subText.c_str(), static_cast<UINT32>(subText.size()),
                            m_artistFmt.Get(), D2D1::RectF(textLeft, 27.0f, textRight, 43.0f), ab.Get());
                    }
                }

                const float btnY = 44.0f;
                m_btnPrevDip = D2D1::RectF(textLeft, btnY, textLeft + 26.0f, btnY + 26.0f);
                m_btnPlayDip = D2D1::RectF(m_btnPrevDip.right + 8.0f, btnY - 1.0f, m_btnPrevDip.right + 36.0f, btnY + 27.0f);
                m_btnNextDip = D2D1::RectF(m_btnPlayDip.right + 8.0f, btnY, m_btnPlayDip.right + 34.0f, btnY + 26.0f);

                drawTransport(0, m_btnPrevDip, false);
                drawTransport(1, m_btnPlayDip, true);
                drawTransport(2, m_btnNextDip, false);

                if (m_smallFmt)
                {
                    const std::wstring badge = m_showVolumeOverlay
                        ? (L"Vol " + std::to_wstring(m_lastVolumePct) + L"%")
                        : (snap.isPlaying ? L"Playing" : L"Paused");
                    const D2D1_COLOR_F bCol = m_showVolumeOverlay
                        ? D2D1::ColorF(0.45f, 0.82f, 1.0f, 1.0f)
                        : D2D1::ColorF(0.62f, 0.66f, 0.74f, 0.85f);
                    if (auto sb = mkBrush(bCol))
                    {
                        m_smallFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                        m_dcTarget->DrawTextW(badge.c_str(), static_cast<UINT32>(badge.size()),
                            m_smallFmt.Get(), D2D1::RectF(m_btnNextDip.right + 4.0f, btnY, textRight, btnY + 26.0f), sb.Get());
                        m_smallFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                    }
                }
            }

            const HRESULT hrEnd = m_dcTarget->EndDraw();
            if (hrEnd == D2DERR_RECREATE_TARGET)
            {
                SelectObject(memDc, oldBmp);
                DeleteObject(dib);
                DeleteDC(memDc);
                ReleaseDC(nullptr, screenDc);
                ReleaseRenderFactories();
                CreateRenderFactories();
                return;
            }

            POINT ptDst{ dstX, dstY };
            SIZE sz{ widthPx, heightPx };
            POINT ptSrc{ 0, 0 };
            BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            UpdateLayeredWindow(m_hwnd, screenDc, &ptDst, &sz, memDc, &ptSrc, 0, &bf, ULW_ALPHA);
        }

        SelectObject(memDc, oldBmp);
        DeleteObject(dib);
        DeleteDC(memDc);
        ReleaseDC(nullptr, screenDc);

        SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    int MediaIsland::HitTestControl(POINT clientPt) const noexcept
    {
        const float dx = static_cast<float>(clientPt.x / m_dpiScale);
        const float dy = static_cast<float>(clientPt.y / m_dpiScale);
        auto hit = [dx, dy](const D2D1_RECT_F& r) {
            return dx >= r.left && dx <= r.right && dy >= r.top && dy <= r.bottom;
        };
        if (hit(m_btnPrevDip)) return 0;
        if (hit(m_btnPlayDip)) return 1;
        if (hit(m_btnNextDip)) return 2;
        return -1;
    }

    void MediaIsland::SendTransportCommand(TransportCmd cmd)
    {
        m_pendingCmd.store(static_cast<int>(cmd), std::memory_order_release);
        if (m_wakeEvent)
            SetEvent(m_wakeEvent);
    }

    void MediaIsland::AdjustMasterVolume(int wheelDelta)
    {
        if (!m_config.mediaIslandScrollVolume || wheelDelta == 0)
            return;

        const ULONGLONG now = GetTickCount64();
        if ((now - m_wheelLastTick) > 600 ||
            (m_wheelAccum > 0 && wheelDelta < 0) ||
            (m_wheelAccum < 0 && wheelDelta > 0))
        {
            m_wheelAccum = 0;
        }
        m_wheelLastTick = now;
        m_wheelAccum += wheelDelta;

        constexpr int kWheelStepThreshold = 60;
        int steps = 0;
        while (std::abs(m_wheelAccum) >= kWheelStepThreshold && std::abs(steps) < 5)
        {
            if (m_wheelAccum > 0)
            {
                ++steps;
                m_wheelAccum -= kWheelStepThreshold;
            }
            else
            {
                --steps;
                m_wheelAccum += kWheelStepThreshold;
            }
        }
        if (steps == 0)
            return;

        const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool applied = false;
        int appliedPct = m_lastVolumePct;
        {
            Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
            Microsoft::WRL::ComPtr<IMMDevice> device;
            Microsoft::WRL::ComPtr<IAudioEndpointVolume> epVol;
            float current = 0.0f;
            if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(enumerator.GetAddressOf()))) && enumerator &&
                SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.GetAddressOf())) && device &&
                SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr,
                    reinterpret_cast<void**>(epVol.GetAddressOf()))) && epVol &&
                SUCCEEDED(epVol->GetMasterVolumeLevelScalar(&current)))
            {
                const float next = (std::clamp)(current + static_cast<float>(steps) * 0.02f, 0.0f, 1.0f);
                if (SUCCEEDED(epVol->SetMasterVolumeLevelScalar(next, nullptr)))
                {
                    appliedPct = static_cast<int>(std::lround(next * 100.0f));
                    applied = true;
                }
            }
        }
        if (SUCCEEDED(hrCom))
            CoUninitialize();

        if (applied)
        {
            m_lastVolumePct = appliedPct;
            m_showVolumeOverlay = true;
            if (m_hwnd)
                SetTimer(m_hwnd, kTimerVolumeHide, 1200, nullptr);
            UpdateWindowBoundsAndRender();
        }
    }

    LRESULT CALLBACK MediaIsland::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) noexcept
    {
        auto* self = reinterpret_cast<MediaIsland*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE)
        {
            self = static_cast<MediaIsland*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            if (self)
                self->m_hwnd = hwnd;
        }
        if (self)
        {
            try { return self->HandleMessage(hwnd, msg, wp, lp); }
            catch (...) {}
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT MediaIsland::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case kMsgMediaUpdated:
            UpdateWindowBoundsAndRender();
            return 0;

        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            UpdateWindowBoundsAndRender();
            return 0;

        case WM_LBUTTONDOWN:
        {
            if ((GetKeyState(VK_MENU) & 0x8000) != 0)
            {
                POINT cur{};
                RECT rc{};
                if (GetCursorPos(&cur) && GetWindowRect(hwnd, &rc))
                {
                    m_draggingPos = true;
                    m_dragStartCursorX = cur.x;
                    m_dragStartWinX = rc.left;
                    m_dragCurrentX = rc.left;
                    SetCapture(hwnd);
                }
            }
            return 0;
        }

        case WM_MOUSEMOVE:
        {
            if (m_draggingPos)
            {
                POINT cur{};
                if (GetCursorPos(&cur))
                {
                    m_dragCurrentX = m_dragStartWinX + (cur.x - m_dragStartCursorX);
                    UpdateWindowBoundsAndRender();
                }
                return 0;
            }
            if (!m_mouseTracked)
            {
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                m_mouseTracked = true;
                if (m_config.mediaIslandHoverExpand && !m_expanded)
                    SetTimer(hwnd, kTimerExpand, 160, nullptr);
                else if (m_config.mediaIslandMarquee && !m_marqueeTimerRunning && m_marqueeMaxOffset > 2.0f)
                    UpdateWindowBoundsAndRender();
            }
            const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            const int hit = HitTestControl(pt);
            if (hit != m_hoveredBtn)
            {
                m_hoveredBtn = hit;
                UpdateWindowBoundsAndRender();
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            m_mouseTracked = false;
            KillTimer(hwnd, kTimerExpand);
            if (m_expanded || m_hoveredBtn != -1 || m_marqueeTimerRunning)
            {
                m_expanded = false;
                m_hoveredBtn = -1;
                UpdateWindowBoundsAndRender();
            }
            return 0;

        case WM_LBUTTONUP:
        {
            if (m_draggingPos)
            {
                m_draggingPos = false;
                ReleaseCapture();
                const POINT ptZero{ 0, 0 };
                HMONITOR hMon = MonitorFromPoint(ptZero, MONITOR_DEFAULTTOPRIMARY);
                MONITORINFO mi{ sizeof(mi) };
                RECT rc{};
                if (GetMonitorInfoW(hMon, &mi) && GetWindowRect(hwnd, &rc))
                {
                    const int monW = mi.rcMonitor.right - mi.rcMonitor.left;
                    const int winW = rc.right - rc.left;
                    const int spanPx = (std::max)(1, monW - winW - 8);
                    const int pct = (std::clamp)(static_cast<int>((rc.left - (mi.rcMonitor.left + 4)) * 100 / spanPx), 0, 100);
                    m_config.mediaIslandOffsetXPct = pct;
                    if (m_callbacks.onSaveOffsetXPct)
                        m_callbacks.onSaveOffsetXPct(pct);
                }
                return 0;
            }
            const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            const int hit = HitTestControl(pt);
            if (hit == 0)
                SendTransportCommand(TransportCmd::Previous);
            else if (hit == 1)
                SendTransportCommand(TransportCmd::TogglePlayPause);
            else if (hit == 2)
                SendTransportCommand(TransportCmd::Next);
            return 0;
        }

        case WM_CAPTURECHANGED:
            if (m_draggingPos)
            {
                m_draggingPos = false;
                UpdateWindowBoundsAndRender();
            }
            return 0;

        case WM_RBUTTONUP:
        {
            POINT screenPt{};
            GetCursorPos(&screenPt);
            ShowIslandContextMenu(screenPt);
            return 0;
        }

        case WM_MOUSEWHEEL:
        {
            const short delta = GET_WHEEL_DELTA_WPARAM(wp);
            AdjustMasterVolume(delta);
            return 0;
        }

        case WM_TIMER:
            if (wp == kTimerFullscreenPoll)
            {
                if (ShouldHideForFullscreen() != m_lastFullscreenHide)
                    UpdateWindowBoundsAndRender();
                return 0;
            }
            if (wp == kTimerExpand)
            {
                KillTimer(hwnd, kTimerExpand);
                if (m_mouseTracked && m_config.mediaIslandHoverExpand && !m_expanded)
                {
                    m_expanded = true;
                    UpdateWindowBoundsAndRender();
                }
                return 0;
            }
            if (wp == kTimerVolumeHide)
            {
                KillTimer(hwnd, kTimerVolumeHide);
                m_showVolumeOverlay = false;
                UpdateWindowBoundsAndRender();
                return 0;
            }
            if (wp == kTimerMarquee)
            {
                if (m_expanded || m_showVolumeOverlay || !m_config.mediaIslandMarquee || m_marqueeMaxOffset <= 2.0f)
                {
                    KillTimer(hwnd, kTimerMarquee);
                    m_marqueeTimerRunning = false;
                    m_marqueeOffset = 0.0f;
                    m_marqueePauseTicks = 0;
                    m_marqueePhase = 0;
                    UpdateWindowBoundsAndRender();
                    return 0;
                }
                if (m_marqueePhase == 0)
                {
                    if (++m_marqueePauseTicks >= 44)
                    {
                        m_marqueePauseTicks = 0;
                        m_marqueePhase = 1;
                    }
                    return 0;
                }
                if (m_marqueePhase == 1)
                {
                    m_marqueeOffset += 1.1f;
                    if (m_marqueeOffset >= m_marqueeMaxOffset)
                    {
                        m_marqueeOffset = m_marqueeMaxOffset;
                        m_marqueePauseTicks = 0;
                        m_marqueePhase = 2;
                    }
                    UpdateWindowBoundsAndRender();
                    return 0;
                }
                if (++m_marqueePauseTicks >= 36)
                {
                    m_marqueePauseTicks = 0;
                    m_marqueePhase = 0;
                    m_marqueeOffset = 0.0f;
                    UpdateWindowBoundsAndRender();
                }
                return 0;
            }
            break;

        default:
            break;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
} // namespace winrings
