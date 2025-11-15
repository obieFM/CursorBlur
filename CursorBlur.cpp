#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include <deque>
#include <chrono>
#include <algorithm>
#include <thread>
#include <functional>

// Constants
constexpr int kMaxTrailSize = 500; // Max count of trail samples

// Launch arguments
static float    gSensitivity = 0.03f; // Fade intensity relative to cursor speed
static float    gTrailFadeMs = 50.f;  // How long each sample takes to fade out
static BYTE     gTrailMaxAlpha = 10;  // Trail starting opacity
// Optional tint applied to trail
static BYTE TintR = 255;
static BYTE TintG = 255;
static BYTE TintB = 255;

// Cache of current cursor bitmap
static HCURSOR sLastCursor = nullptr;
static int     sLastW = 0, sLastH = 0;
static HDC     sTintDC = nullptr;
static HBITMAP sTintBMP = nullptr;
static HGDIOBJ sTintOld = nullptr;
static void* sTintBits = nullptr;

// Sample data
struct Sample final
{
    POINT pt;
    std::chrono::steady_clock::time_point t;
};

// Current cursor visual
struct CursorVisual final
{
    HCURSOR hCur = nullptr;
    int width = 0, height = 0, hotX = 0, hotY = 0;
};

// Returns bounding rectangle of the entire desktop
inline RECT GetVirtualScreenRect() noexcept
{
    RECT r{};
    r.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    r.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    r.right = r.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    r.bottom = r.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return r;
}

static BITMAPINFO MakeBitmapInfo(const int w, const int h)
{
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return bi;
}

// 32-bit backbuffer for full-screen overlay
struct Backbuffer final
{
    HDC memDC = nullptr;
    HBITMAP dib = nullptr;
    void* bits = nullptr;
    int w = 0, h = 0;

    void Release() noexcept
    {
        if (memDC)
            DeleteDC(memDC);
        if (dib)
            DeleteObject(dib);

        memDC = nullptr;
        dib = nullptr;
        bits = nullptr;
        w = h = 0;
    }

    [[nodiscard]] bool EnsureSize(HDC refDC, int W, int H) noexcept
    {
        if (W <= w && H <= h && memDC && dib)
            return true;
        Release();

        BITMAPINFO bi = MakeBitmapInfo(W, H);
        dib = CreateDIBSection(refDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib)
            return false;

        memDC = CreateCompatibleDC(refDC);
        SelectObject(memDC, dib);

        w = W; h = H;
        return true;
    }

    void Clear() noexcept
    {
        PatBlt(memDC, 0, 0, w, h, BLACKNESS);
    }
};

// Temporary surface to draw the cursor icon onto before final blending
struct TempIconSurf final
{
    HDC memDC = nullptr;
    HBITMAP dib = nullptr;
    void* bits = nullptr;
    int w = 0, h = 0;

    void Release() noexcept
    {
        if (memDC)
            DeleteDC(memDC);
        if (dib)
            DeleteObject(dib);

        memDC = nullptr;
        dib = nullptr;
        bits = nullptr;
        w = h = 0;
    }

    [[nodiscard]] bool EnsureSize(HDC refDC, int W, int H) noexcept
    {
        if (W <= 0 || H <= 0)
            W = H = 1;
        if (W == w && H == h && memDC && dib)
            return true;
        Release();

        BITMAPINFO bi = MakeBitmapInfo(W, H);
        dib = CreateDIBSection(refDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib)
            return false;
        memDC = CreateCompatibleDC(refDC);
        SelectObject(memDC, dib);

        w = W; h = H;
        return true;
    }
};

// Updates cursor visual data when system cursor changes
inline void RefreshCursorVisual(CursorVisual& cv, CURSORINFO ci) noexcept
{
    if (ci.hCursor == cv.hCur)
        return;

    cv.hCur = ci.hCursor;
    ICONINFO ii{};
    if (GetIconInfo(cv.hCur, &ii))
    {
        BITMAP bm{};
        if (ii.hbmColor)
            GetObject(ii.hbmColor, sizeof(bm), &bm);
        else if (ii.hbmMask)
        {
            GetObject(ii.hbmMask, sizeof(bm), &bm);
            bm.bmHeight /= 2;
        }

        cv.width = bm.bmWidth ? bm.bmWidth : 32;
        cv.height = bm.bmHeight ? bm.bmHeight : 32;
        cv.hotX = static_cast<int>(ii.xHotspot);
        cv.hotY = static_cast<int>(ii.yHotspot);

        if (ii.hbmMask)
            DeleteObject(ii.hbmMask);
        if (ii.hbmColor)
            DeleteObject(ii.hbmColor);
    }
}

inline void UpdateTrail(std::deque<Sample>& trail, const POINT& ptNow,
    std::chrono::steady_clock::time_point now) noexcept
{
    bool add = trail.empty();
    if (!add)
    {
        const POINT& p = trail.back().pt;
        const int dx = ptNow.x - p.x;
        const int dy = ptNow.y - p.y;

        // Should add a new sample if the cursor has moved at least 1px
        add = ((dx * dx + dy * dy) >= 1);
    }

    if (add)
    {
        trail.push_back({ ptNow, now });
        if (trail.size() > static_cast<size_t>(kMaxTrailSize))
            trail.pop_front();
    }

    while (!trail.empty() &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - trail.front().t).count() > (gTrailFadeMs + 50.f))
        trail.pop_front();
}

static inline void ReleaseTintCache()
{
    if (sTintDC)
    {
        if (sTintBMP)
        {
            if (sTintOld)
                SelectObject(sTintDC, sTintOld);
            DeleteObject(sTintBMP);
            sTintBMP = nullptr;
        }
        DeleteDC(sTintDC);
        sTintDC = nullptr;
    }

    sTintOld = nullptr;
    sTintBits = nullptr;
    sLastCursor = nullptr;
    sLastW = sLastH = 0;
}

static void DrawTrail(HWND hwnd, HDC screenDC, Backbuffer& bb, TempIconSurf& tmp,
    const CursorVisual& cv, const std::deque<Sample>& trail, const RECT& vs) noexcept
{
    if (!tmp.EnsureSize(screenDC, cv.width, cv.height))
        return;

    bb.Clear();
    const auto now = std::chrono::steady_clock::now();

    // (Re)create tinted bitmap when cursor shape/size changes
    if (!sTintBMP || !sTintDC || cv.hCur != sLastCursor || cv.width != sLastW || cv.height != sLastH)
    {
        sLastCursor = cv.hCur;
        sLastW = cv.width;
        sLastH = cv.height;

        if (!sTintDC)
            sTintDC = CreateCompatibleDC(screenDC);

        // Unselect old bitmap safely before deleting
        if (sTintBMP)
        {
            if (sTintOld)
                SelectObject(sTintDC, sTintOld);
            DeleteObject(sTintBMP);
            sTintBMP = nullptr;
            sTintOld = nullptr;
            sTintBits = nullptr;
        }

        BITMAPINFO bi = MakeBitmapInfo(cv.width, cv.height);
        sTintBMP = CreateDIBSection(screenDC, &bi, DIB_RGB_COLORS, &sTintBits, nullptr, 0);
        if (!sTintBMP || !sTintBits)
            return; // Skip frame if allocation failed

        sTintOld = SelectObject(sTintDC, sTintBMP);

        // Draw and tint cursor once
        PatBlt(sTintDC, 0, 0, cv.width, cv.height, BLACKNESS);
        DrawIconEx(sTintDC, 0, 0, cv.hCur, cv.width, cv.height, 0, nullptr, DI_NORMAL);

        DWORD* px = static_cast<DWORD*>(sTintBits);
        const UINT count = static_cast<UINT>(cv.width * cv.height);
        for (UINT i = 0; i < count; ++i)
        {
            BYTE* p = reinterpret_cast<BYTE*>(&px[i]);
            p[2] = static_cast<BYTE>((p[2] * TintR) / 255);
            p[1] = static_cast<BYTE>((p[1] * TintG) / 255);
            p[0] = static_cast<BYTE>((p[0] * TintB) / 255);
        }
    }


    // Copy cached tinted cursor into tmp for per-pixel alpha blends below
    PatBlt(tmp.memDC, 0, 0, tmp.w, tmp.h, BLACKNESS);
    if (!sTintBMP)
        return; // Safety: creation may have failed above
    BitBlt(tmp.memDC, 0, 0, cv.width, cv.height, sTintDC, 0, 0, SRCCOPY);

    // Draw samples in order from oldest to newest
    for (int i = static_cast<int>(trail.size()) - 2; i >= 0; --i)
    {
        const Sample& s0 = trail[i];
        const Sample& s1 = trail[i + 1];

        const float age0 = static_cast<float>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - s0.t).count());
        if (age0 > gTrailFadeMs)
            continue;

        const float dx = static_cast<float>(s1.pt.x - s0.pt.x);
        const float dy = static_cast<float>(s1.pt.y - s0.pt.y);
        const float distSq = dx * dx + dy * dy;
        if (distSq < 1.f)
            continue;

        const float dist = std::sqrtf(distSq);
        const int steps = static_cast<int>(std::ceilf(dist));
        const float stepFrac = 1.f / static_cast<float>(steps);

        // Interpolate between samples to fill gaps
        for (int j = steps; j >= 0; --j)
        {
            const float t = j * stepFrac;
            const POINT p{
                static_cast<LONG>(std::lround(s0.pt.x + dx * t)),
                static_cast<LONG>(std::lround(s0.pt.y + dy * t))
            };

            // Calculate alpha for sample
            const float fade = std::max(0.f, 1.f - (age0 + (age0 * t * 0.1f)) / gTrailFadeMs);
            const float speedFactor = std::clamp(dist * gSensitivity, 0.f, 1.f);
            const BYTE a = static_cast<BYTE>(std::clamp(gTrailMaxAlpha * fade * speedFactor, 0.f, 255.f));
            if (a < 3)
                continue;

            const int dstX = p.x - vs.left - cv.hotX;
            const int dstY = p.y - vs.top - cv.hotY;

            const BLENDFUNCTION bf{ AC_SRC_OVER, 0, a, AC_SRC_ALPHA };
            AlphaBlend(bb.memDC, dstX, dstY, cv.width, cv.height,
                tmp.memDC, 0, 0, cv.width, cv.height, bf);
        }
    }

    // Push entire frame to the overlay window
    const POINT ptSrc{ 0,0 };
    const SIZE sz{ bb.w, bb.h };
    const POINT ptWin{ vs.left, vs.top };
    const BLENDFUNCTION bfW{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(
        hwnd,
        screenDC,
        const_cast<POINT*>(&ptWin),
        const_cast<SIZE*>(&sz),
        bb.memDC,
        const_cast<POINT*>(&ptSrc),
        0,
        const_cast<BLENDFUNCTION*>(&bfW),
        ULW_ALPHA);
}

// Overlay window handler
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept
{
    RECT* pVs = reinterpret_cast<RECT*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    switch (msg)
    {
        case WM_DESTROY:
            PostQuitMessage(0);
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_DISPLAYCHANGE:
            if (pVs)
                *pVs = GetVirtualScreenRect();
            break;
        default:
            break;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Generic launch argument parser
template<typename T>
inline void ParseCommandValue(
    const wchar_t* token,
    const std::initializer_list<const wchar_t*>& names,
    wchar_t*& context,
    T& outVal,
    T minVal,
    T maxVal,
    std::function<void(const wchar_t* val)> customParser = nullptr)
{
    const wchar_t* trimmed = (*token == L'/' || *token == L'-') ? token + 1 : token;

    for (const auto* name : names)
    {
        if (_wcsicmp(trimmed, name) == 0)
        {
            if (wchar_t* val = wcstok_s(nullptr, L" ", &context))
            {
                if (customParser)
                    customParser(val);
                else
                {
                    if constexpr (std::is_floating_point_v<T>)
                        outVal = std::clamp(static_cast<T>(_wtof(val)), minVal, maxVal);
                    else if constexpr (std::is_integral_v<T>)
                        outVal = static_cast<T>(std::clamp(_wtoi(val), static_cast<int>(minVal), static_cast<int>(maxVal)));
                }
            }
            break;
        }
    }
}

// Main initializer and program loop
int WINAPI wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE,
    _In_ LPWSTR lpCmdLine,
    _In_ int)
{
    // Singleton process, do not allow multiple instances
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Global\\CursorTrailOverlay_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(hMutex);
        return 0;
    }

    // Parse arguments
    if (lpCmdLine && *lpCmdLine)
    {
        wchar_t* context = nullptr;
        wchar_t* token = wcstok_s(lpCmdLine, L" ", &context);

        while (token)
        {
            ParseCommandValue(token, { L"sensitivity", L"s" }, context, gSensitivity, 0.001f, 1.0f);
            ParseCommandValue(token, { L"fade", L"f" }, context, gTrailFadeMs, 1.f, 1000.f);
            ParseCommandValue(token, { L"alpha", L"a" }, context, gTrailMaxAlpha, (BYTE)1, (BYTE)255);

            COLORREF dummyColor{};
            ParseCommandValue(token, { L"color", L"c" }, context, dummyColor, COLORREF(0), COLORREF(0),
                [](const wchar_t* val)
                {
                    if (*val == L'#') ++val;
                    unsigned int rgb = 0;
                    if (swscanf_s(val, L"%x", &rgb) == 1) {
                        TintR = (rgb >> 16) & 0xFF;
                        TintG = (rgb >> 8) & 0xFF;
                        TintB = rgb & 0xFF;
                    }
                });

            token = wcstok_s(nullptr, L" ", &context);
        }
    }

    // High-DPI awareness
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        SetProcessDPIAware();

    // Register window class
    const wchar_t* kClass = L"CursorTrailOverlay_CustomCursor";
    WNDCLASSEXW wc{ sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    // Create full-screen transparent overlay window
    RECT vs = GetVirtualScreenRect();
    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kClass, L"", WS_POPUP,
        vs.left, vs.top, vs.right - vs.left, vs.bottom - vs.top,
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd)
    {
        CloseHandle(hMutex);
        return 0;
    }

    // Make the window click-through
    LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    ex |= WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE;
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex);

    SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&vs));
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // Exclude from desktop peek
    BOOL exclude = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_EXCLUDED_FROM_PEEK, &exclude, sizeof(exclude));

    // Initialize rendering resources
    HDC screenDC = GetDC(nullptr);
    Backbuffer bb;
    TempIconSurf tmp;
    if (!bb.EnsureSize(screenDC, vs.right - vs.left, vs.bottom - vs.top))
    {
        ReleaseDC(nullptr, screenDC);
        CloseHandle(hMutex);
        return 0;
    }

    std::deque<Sample> trail;
    CursorVisual cv{};
    auto lastTick = std::chrono::steady_clock::now();

    // Get maximum refresh rate
    float maxHz = 60.f;
    DISPLAY_DEVICE dd{};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i)
    {
        DEVMODE dm{};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm))
            maxHz = std::max(maxHz, static_cast<float>(dm.dmDisplayFrequency));
    }

    const auto frameInterval = std::chrono::milliseconds(
        static_cast<int>(std::round(1000.0 / std::clamp(maxHz, 30.0f, 240.0f))));

    // Main loop
    MSG msg{};
    while (true)
    {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            // Check if we need to quit the program
            if (msg.message == WM_QUIT)
            {
                bb.Release();
                tmp.Release();
                ReleaseDC(nullptr, screenDC);
                ReleaseTintCache();
                CloseHandle(hMutex);
                return 0;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        std::this_thread::sleep_until(lastTick + frameInterval);
        lastTick = std::chrono::steady_clock::now();

        // Sample current cursor position
        POINT cur{};
        GetCursorPos(&cur);
        UpdateTrail(trail, cur, lastTick);

        // Check if screen size needs update
        RECT curVS = GetVirtualScreenRect();
        if (curVS.left != vs.left || curVS.top != vs.top ||
            curVS.right != vs.right || curVS.bottom != vs.bottom)
        {
            vs = curVS;
            SetWindowPos(hwnd, nullptr, vs.left, vs.top,
                vs.right - vs.left, vs.bottom - vs.top,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);

            if (!bb.EnsureSize(screenDC, vs.right - vs.left, vs.bottom - vs.top))
            {
                ReleaseTintCache();
                ReleaseDC(nullptr, screenDC);
                CloseHandle(hMutex);
                return 0;
            }
        }

        CURSORINFO ci{ sizeof(ci) };
        if (!GetCursorInfo(&ci) || ci.flags != CURSOR_SHOWING || !ci.hCursor)
        {
            const auto now = std::chrono::steady_clock::now();
            while (!trail.empty() &&
                std::chrono::duration_cast<std::chrono::milliseconds>(now - trail.front().t).count() > (gTrailFadeMs + 50.f))
                trail.pop_front();

            if (!trail.empty())
                DrawTrail(hwnd, screenDC, bb, tmp, cv, trail, vs);
        }
        else
        {
            RefreshCursorVisual(cv, ci);
            DrawTrail(hwnd, screenDC, bb, tmp, cv, trail, vs);
        }
    }
}