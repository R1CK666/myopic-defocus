#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <d3dcompiler.h>
#include <dwmapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kClassName[] = L"MyopicDefocusGPUOverlay";
constexpr wchar_t kSettingsClass[] = L"MyopicDefocusGPUSettings";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kTrayId = 1;
constexpr UINT kMenuToggle = 1001;
constexpr UINT kMenuSettings = 1002;
constexpr UINT kMenuExit = 1003;
constexpr UINT kMenuOpenIni = 1004;
constexpr UINT kHotkeyToggle = 2001;
constexpr UINT kHotkeyUp = 2002;
constexpr UINT kHotkeyDown = 2003;
constexpr UINT kSaveSettings = 3001;
constexpr WORD kAppIcon = 101;

struct Settings {
    float diagonalInches = 24.0f;
    float distanceCm = 60.0f;
    float pupilMm = 6.5f;
    float strength = 0.70f;
};

struct alignas(16) ShaderConstants {
    float texelX = 0;
    float texelY = 0;
    float pairCount = 0;
    float strength = 0;
    // x/y = combined green/blue weights; z/w = their bilinear offsets.
    // 48 pairs cover a Gaussian radius of 96 px (sigma up to 32 px at 3σ).
    float weights[49][4]{};
};

HINSTANCE gInstance = nullptr;
HWND gOverlay = nullptr;
HWND gSettingsWindow = nullptr;
NOTIFYICONDATAW gTray{};
Settings gSettings;
bool gEnabled = true;
std::wstring gIniPath;

// --- Z-order maintenance -------------------------------------------------
// Context menus, combo drop-downs and tooltips are themselves WS_EX_TOPMOST.
// Whenever one is shown it is placed at the top of the topmost band, i.e.
// above this overlay, so it renders unfiltered. Re-assert our position on
// every relevant shell event and on a slow periodic tick as a safety net.
HWINEVENTHOOK gShowHook = nullptr;
HWINEVENTHOOK gMenuHook = nullptr;
ULONGLONG gLastRaise = 0;

// DirectComposition was removed in 1.2.0: it requires WS_EX_NOREDIRECTIONBITMAP,
// which cannot coexist with the WS_EX_LAYERED style that full-screen mouse
// pass-through depends on, and it produced no measurable win over the layered
// window path.
bool gUseDirtyRects = true;
bool gUseWaitable = true;
bool gRendererReady = false;
bool gOverlayOpaque = false;
bool gDisplayResetRequested = false;
ULONGLONG gDisplayResetNotBefore = 0;

RECT PrimaryScreenBounds() {
    return RECT{0, 0, GetSystemMetrics(SM_CXSCREEN),
                GetSystemMetrics(SM_CYSCREEN)};
}

bool OverlayMatchesPrimaryScreen() {
    if (!gOverlay) return false;
    RECT window{}, screen = PrimaryScreenBounds();
    if (!GetWindowRect(gOverlay, &window)) return false;
    return window.left == screen.left && window.top == screen.top &&
           window.right == screen.right && window.bottom == screen.bottom;
}

bool ResizeOverlayToPrimaryScreen() {
    if (!gOverlay) return false;
    const RECT b = PrimaryScreenBounds();
    if (!SetWindowPos(gOverlay, HWND_TOPMOST, b.left, b.top, b.right - b.left,
                 b.bottom - b.top,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING))
        return false;
    // Reassert capture exclusion after a display-topology transition.
    return SetWindowDisplayAffinity(gOverlay, 0x00000011) != FALSE;
}

void RequestDisplayReset(ULONGLONG delayMs) {
    gDisplayResetRequested = true;
    gDisplayResetNotBefore =
        std::max(gDisplayResetNotBefore, GetTickCount64() + delayMs);
    if (gOverlay) {
        // Make the layered window transparent before hiding it. If Windows
        // briefly resurrects the surface during resume, it cannot expose the
        // black class background or an uninitialised swap-chain buffer.
        SetLayeredWindowAttributes(gOverlay, 0, 0, LWA_ALPHA);
        gOverlayOpaque = false;
        ShowWindow(gOverlay, SW_HIDE);
    }
}

void RaiseOverlay(bool force) {
    if (!gOverlay || !gEnabled) return;
    // Do not reinsert the overlay into the topmost band merely because a
    // timer or an accessibility event fired. Even a no-op SetWindowPos can
    // cause cursor hit-testing / composition work while text is being edited.
    // Raise only when a visible topmost window actually overlaps this screen
    // above us. Keep the periodic check as a menu/tooltip safety net.
    RECT overlayBounds{};
    if (GetWindowRect(gOverlay, &overlayBounds)) {
        bool obscured = false;
        unsigned visited = 0;
        HWND above = GetWindow(gOverlay, GW_HWNDPREV);
        for (; above && visited < 256;
             above = GetWindow(above, GW_HWNDPREV), ++visited) {
            if (above == gSettingsWindow || !IsWindowVisible(above) ||
                IsIconic(above))
                continue;
            if (!(GetWindowLongPtrW(above, GWL_EXSTYLE) & WS_EX_TOPMOST))
                continue;
            RECT bounds{}, intersection{};
            if (GetWindowRect(above, &bounds) &&
                IntersectRect(&intersection, &overlayBounds, &bounds)) {
                obscured = true;
                break;
            }
        }
        if (!obscured && !above) return;
    }
    ULONGLONG now = GetTickCount64();
    if (!force && now - gLastRaise < 16) return; // coalesce event bursts
    gLastRaise = now;
    SetWindowPos(gOverlay, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING |
                     SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
}

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject,
                           LONG, DWORD, DWORD) {
    if (hwnd == gOverlay) return;
    if (idObject != OBJID_WINDOW && event != EVENT_SYSTEM_MENUPOPUPSTART) return;
    // A new top-level surface appeared or took focus; get back above it.
    RaiseOverlay(event == EVENT_SYSTEM_MENUPOPUPSTART);
}

const char* kVertexShader = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(uint id : SV_VertexID) {
    float2 p = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
    o.uv = p;
    return o;
})";

const char* kBlurShader = R"(
Texture2D inputTex : register(t0);
SamplerState linearClamp : register(s0);
cbuffer Params : register(b0) {
    float2 texel;
    float pairCount;
    float strength;
    float4 weights[49];
};
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    // Bilinear-packed separable blur: weights[i].zw hold the fractional tap
    // offset for the green/blue pair, so one hardware fetch covers two taps.
    // Only green and blue are filtered; red stays sharp and is not needed here.
    // This pass is always the horizontal half of the separable blur; the
    // composite pass performs the vertical half.
    float2 step = float2(texel.x, 0.0);
    float green = inputTex.Sample(linearClamp, uv).g * weights[0].x;
    float blue = inputTex.Sample(linearClamp, uv).b * weights[0].y;
    [loop] for (int i = 1; i <= (int)pairCount; ++i) {
        float2 dg = step * weights[i].z;
        float2 db = step * weights[i].w;
        green += (inputTex.Sample(linearClamp, uv + dg).g +
                  inputTex.Sample(linearClamp, uv - dg).g) * weights[i].x;
        blue += (inputTex.Sample(linearClamp, uv + db).b +
                 inputTex.Sample(linearClamp, uv - db).b) * weights[i].y;
    }
    return float4(green, blue, 0.0, 1.0);
})";

const char* kCompositeShader = R"(
Texture2D blurredTex : register(t0);
Texture2D originalTex : register(t1);
SamplerState linearClamp : register(s0);
cbuffer Params : register(b0) {
    float2 texel;
    float pairCount;
    float strength;
    float4 weights[49];
};
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float4 original = originalTex.Sample(linearClamp, uv);
    // Vertical pass over the full-resolution RG intermediate, where
    // .r carries green and .g carries blue.
    float2 step = float2(0.0, texel.y);
    float green = blurredTex.Sample(linearClamp, uv).r * weights[0].x;
    float blue = blurredTex.Sample(linearClamp, uv).g * weights[0].y;
    [loop] for (int i = 1; i <= (int)pairCount; ++i) {
        float2 dg = step * weights[i].z;
        float2 db = step * weights[i].w;
        green += (blurredTex.Sample(linearClamp, uv + dg).r +
                  blurredTex.Sample(linearClamp, uv - dg).r) * weights[i].x;
        blue += (blurredTex.Sample(linearClamp, uv + db).g +
                 blurredTex.Sample(linearClamp, uv - db).g) * weights[i].y;
    }
    float3 filtered = float3(original.r, green, blue);
    return float4(lerp(original.rgb, filtered, strength), 1.0);
})";

std::wstring ErrorText(HRESULT hr) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, static_cast<DWORD>(hr), 0,
                   reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::wstring result = msg ? msg : L"Unknown error";
    if (msg) LocalFree(msg);
    return result;
}

std::wstring IniPath() {
    // Portable mode first: an INI sitting next to the executable wins, which
    // is what users of a single-file portable app expect.
    auto preferMigrated = [](const std::filesystem::path& target,
                             const std::filesystem::path& legacy) {
        std::error_code ec;
        if (std::filesystem::exists(target, ec)) return target.wstring();
        ec.clear();
        if (std::filesystem::exists(legacy, ec)) {
            ec.clear();
            std::filesystem::copy_file(legacy, target,
                                      std::filesystem::copy_options::none, ec);
            // Read-only portable directories retain the old path rather than
            // silently replacing the user's optical settings with defaults.
            return ec ? legacy.wstring() : target.wstring();
        }
        return std::wstring{};
    };
    std::vector<wchar_t> exe(32768);
    const DWORD length =
        GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (length && length < exe.size()) {
        const auto folder = std::filesystem::path(exe.data()).parent_path();
        auto portable = preferMigrated(folder / L"MyopicDefocus.ini",
                                      folder / L"MyopicDefocusGPU.ini");
        if (!portable.empty()) return portable;
    }
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)))
        return L"MyopicDefocus.ini";
    const std::filesystem::path base(raw);
    CoTaskMemFree(raw);
    const auto dir = base / L"MyopicDefocus";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    auto migrated = preferMigrated(dir / L"settings.ini",
                                  base / L"MyopicDefocusGPU" / L"settings.ini");
    if (!migrated.empty()) return migrated;
    return (dir / L"settings.ini").wstring();
}

void LogFailure(const char* operation, HRESULT hr) {
    // Low-volume recovery log; no desktop images, input text or cursor data.
    const auto path = std::filesystem::path(gIniPath).parent_path() /
                      L"MyopicDefocus.log";
    std::error_code ec;
    const bool truncate = std::filesystem::exists(path, ec) &&
                          std::filesystem::file_size(path, ec) > 1024 * 1024;
    FILE* f = _wfopen(path.c_str(), truncate ? L"wb" : L"ab");
    if (!f) return;
    SYSTEMTIME t{};
    GetLocalTime(&t);
    std::fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u %s hr=0x%08lX\n",
                 t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                 operation, static_cast<unsigned long>(hr));
    std::fclose(f);
}

bool ParseFiniteFloat(const wchar_t* text, float* result) {
    errno = 0;
    wchar_t* end = nullptr;
    const float value = std::wcstof(text, &end);
    if (end == text || errno == ERANGE || !std::isfinite(value)) return false;
    while (*end && std::iswspace(*end)) ++end;
    if (*end) return false;
    *result = value;
    return true;
}

// Writes a fully commented settings file on first run so every option is
// discoverable without reading the documentation.
void EnsureIniExists() {
    std::error_code ec;
    if (std::filesystem::exists(gIniPath, ec)) return;
    HANDLE f = CreateFileW(gIniPath.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    static const char kTemplate[] =
        "; Myopic Defocus settings\r\n"
        "; Restart the application after editing this file.\r\n"
        "\r\n"
        "[Settings]\r\n"
        "DiagonalInches=24.000\r\n"
        "DistanceCm=60.000\r\n"
        "; Pupil, not iris. 3-5 mm is typical for screen use in a lit room.\r\n"
        "PupilMm=6.500\r\n"
        "; 0.000 - 1.000\r\n"
        "Strength=0.700\r\n"
        "\r\n"
        "; ---- Rendering options ----\r\n"
        "; Both default to 1. Set to 0 if the desktop misbehaves.\r\n"
        "\r\n"
        "; FLIP_DISCARD + frame latency waitable object\r\n"
        "FrameLatencyWait=1\r\n"
        "; Incremental redraw of changed screen regions only\r\n"
        "DirtyRects=1\r\n"
        "ConfigVersion=2\r\n";
    DWORD written = 0;
    WriteFile(f, kTemplate, sizeof(kTemplate) - 1, &written, nullptr);
    CloseHandle(f);
}

float ReadFloat(const wchar_t* key, float fallback) {
    wchar_t buf[64]{};
    GetPrivateProfileStringW(L"Settings", key, L"", buf, 64, gIniPath.c_str());
    if (!buf[0]) return fallback;
    float value = fallback;
    return ParseFiniteFloat(buf, &value) ? value : fallback;
}

// An INI written by 1.1.x carries the old rendering defaults and a now-removed
// Renderer key. Bring those forward without touching the optical settings.
void MigrateIni() {
    if (GetPrivateProfileIntW(L"Settings", L"ConfigVersion", 1,
                              gIniPath.c_str()) >= 2)
        return;
    WritePrivateProfileStringW(L"Settings", L"Renderer", nullptr,
                               gIniPath.c_str());
    WritePrivateProfileStringW(L"Settings", L"FrameLatencyWait", L"1",
                               gIniPath.c_str());
    WritePrivateProfileStringW(L"Settings", L"DirtyRects", L"1",
                               gIniPath.c_str());
    WritePrivateProfileStringW(L"Settings", L"ConfigVersion", L"2",
                               gIniPath.c_str());
}

void LoadSettings() {
    gSettings.diagonalInches =
        std::clamp(ReadFloat(L"DiagonalInches", 24.0f), 8.0f, 100.0f);
    gSettings.distanceCm =
        std::clamp(ReadFloat(L"DistanceCm", 60.0f), 20.0f, 300.0f);
    gSettings.pupilMm =
        std::clamp(ReadFloat(L"PupilMm", 6.5f), 2.0f, 8.0f);
    gSettings.strength =
        std::clamp(ReadFloat(L"Strength", 0.70f), 0.0f, 1.0f);

    gUseDirtyRects =
        GetPrivateProfileIntW(L"Settings", L"DirtyRects", 1, gIniPath.c_str()) != 0;
    gUseWaitable = GetPrivateProfileIntW(L"Settings", L"FrameLatencyWait", 1,
                                         gIniPath.c_str()) != 0;
}

void WriteFloat(const wchar_t* key, float value) {
    wchar_t buf[64]{};
    swprintf(buf, 64, L"%.3f", value);
    WritePrivateProfileStringW(L"Settings", key, buf, gIniPath.c_str());
}

void SaveSettings() {
    WriteFloat(L"DiagonalInches", gSettings.diagonalInches);
    WriteFloat(L"DistanceCm", gSettings.distanceCm);
    WriteFloat(L"PupilMm", gSettings.pupilMm);
    WriteFloat(L"Strength", gSettings.strength);
}

struct GpuTimer {
    ComPtr<ID3D11Query> disjoint, start, end;
    bool pending = false;
};

class Renderer {
public:
    bool Initialize(HWND window, bool quiet = false) {
        RECT r{};
        if (!GetWindowRect(window, &r) || r.right <= r.left ||
            r.bottom <= r.top)
            return false;
        width_ = static_cast<UINT>(r.right - r.left);
        height_ = static_cast<UINT>(r.bottom - r.top);

        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;

        ComPtr<IDXGIAdapter1> primaryAdapter;
        ComPtr<IDXGIOutput> primaryOutput;

        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> adapter;
            const HRESULT enumerated = factory->EnumAdapters1(i, &adapter);
            if (enumerated == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(enumerated)) return false;
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(adapter->GetDesc1(&desc))) return false;
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            for (UINT j = 0;; ++j) {
                ComPtr<IDXGIOutput> output;
                const HRESULT found = adapter->EnumOutputs(j, &output);
                if (found == DXGI_ERROR_NOT_FOUND) break;
                if (FAILED(found)) return false;
                DXGI_OUTPUT_DESC od{};
                if (FAILED(output->GetDesc(&od))) return false;
                if (!od.AttachedToDesktop) continue;
                // In Windows coordinates the primary display contains (0, 0).
                if (od.DesktopCoordinates.left <= 0 &&
                    od.DesktopCoordinates.right > 0 &&
                    od.DesktopCoordinates.top <= 0 &&
                    od.DesktopCoordinates.bottom > 0) {
                    primaryAdapter = adapter;
                    primaryOutput = output;
                    break;
                }
            }
            if (primaryOutput) break;
        }
        adapter_ = primaryAdapter;
        output_ = primaryOutput;
        if (!adapter_ || !output_) {
            if (!quiet)
                MessageBoxW(window,
                            L"No active hardware display adapter was found.",
                            L"Myopic Defocus", MB_ICONERROR);
            return false;
        }

        // Every device call happens on the render thread, so the runtime can
        // skip its internal synchronisation.
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                     D3D11_CREATE_DEVICE_SINGLETHREADED;
#ifdef _DEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,
                                     D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL actual{};
        HRESULT hr = D3D11CreateDevice(
            adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
            ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, &actual, &context_);
        if (FAILED(hr)) {
            if (!quiet)
                MessageBoxW(window, ErrorText(hr).c_str(),
                            L"D3D11CreateDevice failed", MB_ICONERROR);
            return false;
        }

        ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(device_.As(&dxgiDevice))) return false;
        ComPtr<IDXGIAdapter> adapter0;
        if (FAILED(dxgiDevice->GetAdapter(&adapter0))) return false;
        ComPtr<IDXGIFactory2> factory2;
        if (FAILED(adapter0->GetParent(IID_PPV_ARGS(&factory2)))) return false;

        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = width_;
        sd.Height = height_;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        // Low-latency is requested by default; retry a basic sequential flip
        // chain if the requested flags/effect are unavailable.
        sd.SwapEffect = gUseWaitable ? DXGI_SWAP_EFFECT_FLIP_DISCARD
                                     : DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        sd.Flags =
            gUseWaitable ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0;

        {
            hr = factory2->CreateSwapChainForHwnd(device_.Get(), window, &sd,
                                                  nullptr, nullptr, &swapChain_);
            if (FAILED(hr)) {
                // Waitable swap chains are not valid on every layered setup.
                sd.Flags = 0;
                sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
                hr = factory2->CreateSwapChainForHwnd(
                    device_.Get(), window, &sd, nullptr, nullptr, &swapChain_);
            }
            if (FAILED(hr)) {
                LogFailure("CreateSwapChainForHwnd", hr);
                return false;
            }
            factory2->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        }

        if (gUseWaitable) {
            ComPtr<IDXGISwapChain2> swapChain2;
            if (SUCCEEDED(swapChain_.As(&swapChain2))) {
                if (SUCCEEDED(swapChain2->SetMaximumFrameLatency(1)))
                    frameLatencyWaitable_ =
                        swapChain2->GetFrameLatencyWaitableObject();
            }
        }

        ComPtr<IDXGIOutput1> output1;
        if (FAILED(output_.As(&output1))) return false;
        hr = output1->DuplicateOutput(device_.Get(), &duplication_);
        if (FAILED(hr)) {
            LogFailure("DuplicateOutput", hr);
            if (!quiet)
                MessageBoxW(window,
                            (L"Desktop Duplication initialization failed:\n" +
                             ErrorText(hr))
                                .c_str(),
                            L"Myopic Defocus", MB_ICONERROR);
            return false;
        }

        DXGI_OUTDUPL_DESC dd{};
        duplication_->GetDesc(&dd);
        captureWidth_ = dd.ModeDesc.Width;
        captureHeight_ = dd.ModeDesc.Height;
        // Never stretch/copy a transient or rotated capture into a differently
        // sized screen. Fail hidden and retry after the display stabilises.
        if (!captureWidth_ || !captureHeight_ ||
            captureWidth_ != width_ || captureHeight_ != height_ ||
            (dd.Rotation != DXGI_MODE_ROTATION_IDENTITY &&
             dd.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED)) {
            LogFailure("UnsupportedCaptureSizeOrRotation", E_INVALIDARG);
            if (!quiet)
                MessageBoxW(window,
                    L"Capture size does not match the primary screen, or the "
                    L"display is rotated. Keep the primary display in "
                    L"landscape orientation and retry after its mode settles.",
                    L"Myopic Defocus", MB_ICONERROR);
            return false;
        }
        if (!CreateResources()) return false;
        UpdateConstants();
        return true;
    }

    void UpdateConstants() {
        if (!context_ || !constantBuffer_) return;
        ShaderConstants c{};
        c.texelX = 1.0f / std::max(1u, captureWidth_);
        c.texelY = 1.0f / std::max(1u, captureHeight_);
        c.strength = gEnabled ? gSettings.strength : 0.0f;

        const float diagPx =
            std::sqrt(float(captureWidth_) * captureWidth_ +
                      float(captureHeight_) * captureHeight_);
        const float mmPerPx = (gSettings.diagonalInches * 25.4f) / diagPx;
        const float screenMm = gSettings.distanceCm * 10.0f;
        // Exact algebra used by the public Refractify extension:
        // blur-circle diameter = pupil * distance * LCA / 1000 / pixel pitch,
        // and SVG Gaussian stdDeviation = diameter * 0.32. Green and blue are
        // 0.47 D and 1.33 D away from the untouched red plane respectively.
        // A 32 px sigma ceiling only protects pathological configurations; it
        // is over five times the former ceiling and covers normal desktop use.
        constexpr float kMaxSigma = 32.0f;
        const float sigmaGreen = std::clamp(
            gSettings.pupilMm * screenMm * 0.47f / 1000.0f / mmPerPx * 0.32f,
            0.05f, kMaxSigma);
        const float sigmaBlue = std::clamp(
            gSettings.pupilMm * screenMm * 1.33f / 1000.0f / mmPerPx * 0.32f,
            0.05f, kMaxSigma);
        const int radiusGreen =
            std::clamp(static_cast<int>(std::ceil(3.0f * sigmaGreen)), 1, 96);
        const int radiusBlue =
            std::clamp(static_cast<int>(std::ceil(3.0f * sigmaBlue)), 1, 96);
        const int pairCount =
            (std::max(radiusGreen, radiusBlue) + 1) / 2;
        c.pairCount = static_cast<float>(pairCount);
        blurReach_ = std::max(radiusGreen, radiusBlue);

        // Build a full ±3σ Gaussian, then fold adjacent tap pairs into one
        // bilinear fetch. This preserves the Gaussian weights while reducing
        // texture reads. Unlike 1.2.x, the radius grows with sigma.
        auto makeWeights = [](float sigma, int radius, int pairCount,
                              int channel, ShaderConstants& out) {
            float w[97]{};
            float sum = 0.0f;
            for (int i = 0; i <= radius; ++i) {
                w[i] = std::exp(-0.5f * float(i * i) / (sigma * sigma));
                sum += i == 0 ? w[i] : 2.0f * w[i];
            }
            for (int i = 0; i <= radius; ++i) w[i] /= sum;

            out.weights[0][channel] = w[0];
            out.weights[0][channel + 2] = 0.0f;
            for (int p = 1; p <= pairCount; ++p) {
                const int a = 2 * p - 1, b = 2 * p;
                const float wa = a <= radius ? w[a] : 0.0f;
                const float wb = b <= radius ? w[b] : 0.0f;
                const float combined = wa + wb;
                out.weights[p][channel] = combined;
                out.weights[p][channel + 2] =
                    combined > 0.0f
                        ? (float(a) * wa + float(b) * wb) / combined
                        : float(a);
            }
        };
        makeWeights(sigmaGreen, radiusGreen, pairCount, 0, c);
        makeWeights(sigmaBlue, radiusBlue, pairCount, 1, c);
        context_->UpdateSubresource(constantBuffer_.Get(), 0, nullptr, &c, 0, 0);
        // A parameter change affects every pixel. Dirty-rect metadata only
        // describes desktop-content changes, so the next draw must ignore it
        // and regenerate the complete filtered frame.
        forceFullRedraw_ = true;
    }

    bool RenderFrame() {
        if (!duplication_) return false;
        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        HRESULT hr = duplication_->AcquireNextFrame(8, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            // The desktop may be completely static when settings change. Use
            // the retained capture texture instead of waiting for mouse or
            // window activity to produce a dirty region.
            if (forceFullRedraw_ && hasFrame_ && gEnabled) {
                if (frameLatencyWaitable_)
                    WaitForSingleObjectEx(frameLatencyWaitable_, 20, TRUE);
                if (!Draw({})) return false;
                forceFullRedraw_ = false;
            }
            return true;
        }
        if (FAILED(hr)) {
            LogFailure("AcquireNextFrame", hr);
            return false;
        }

        // LastPresentTime == 0 means only the cursor moved. The desktop image
        // is unchanged, so re-blurring and re-presenting it is pure waste.
        const bool desktopChanged = info.LastPresentTime.QuadPart != 0;
        if (!desktopChanged && hasFrame_) {
            duplication_->ReleaseFrame();
            if (forceFullRedraw_ && gEnabled) {
                if (frameLatencyWaitable_)
                    WaitForSingleObjectEx(frameLatencyWaitable_, 20, TRUE);
                if (!Draw({})) return false;
                forceFullRedraw_ = false;
            }
            return true;
        }

        ComPtr<ID3D11Texture2D> frame;
        if (FAILED(resource.As(&frame))) {
            duplication_->ReleaseFrame();
            LogFailure("CaptureTextureInterface", E_NOINTERFACE);
            return false;
        }
        D3D11_TEXTURE2D_DESC actual{};
        frame->GetDesc(&actual);
        if (actual.Width != captureWidth_ || actual.Height != captureHeight_ ||
            actual.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
            actual.MipLevels != 1 || actual.ArraySize != 1 ||
            actual.SampleDesc.Count != 1) {
            duplication_->ReleaseFrame();
            LogFailure("CaptureTextureDescriptionChanged", E_INVALIDARG);
            return false;
        }
        context_->CopyResource(captureTexture_.Get(), frame.Get());

        // Collect the regions the compositor actually changed. The first frame
        // after startup, and any frame with too many regions to be worth
        // tracking, falls back to a full redraw.
        dirtyScratch_.clear();
        const bool partial = gUseDirtyRects && hasFrame_ && !forceFullRedraw_ &&
                             CollectDirtyRects(info, &dirtyScratch_);
        duplication_->ReleaseFrame();
        hasFrame_ = true;

        // Block until the compositor is ready for another frame. With
        // SetMaximumFrameLatency(1) this removes the queued-frame backlog that
        // otherwise adds a frame or more of input lag.
        if (frameLatencyWaitable_)
            WaitForSingleObjectEx(frameLatencyWaitable_, 20, TRUE);

        if (!Draw(partial ? dirtyScratch_ : std::vector<RECT>{})) return false;
        forceFullRedraw_ = false;
        return true;
    }

    // Returns true when a bounded set of dirty regions was produced. Move
    // rects contribute both their source and destination areas.
    bool CollectDirtyRects(const DXGI_OUTDUPL_FRAME_INFO& info,
                           std::vector<RECT>* out) {
        constexpr UINT kMaxRects = 24;
        if (info.TotalMetadataBufferSize == 0) return false;

        std::vector<BYTE> buffer(info.TotalMetadataBufferSize);
        UINT used = 0;
        if (FAILED(duplication_->GetFrameMoveRects(
                static_cast<UINT>(buffer.size()),
                reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(buffer.data()), &used)))
            return false;
        const UINT moveCount = used / sizeof(DXGI_OUTDUPL_MOVE_RECT);
        auto* moves = reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(buffer.data());
        for (UINT i = 0; i < moveCount; ++i) {
            const RECT& d = moves[i].DestinationRect;
            const POINT& s = moves[i].SourcePoint;
            out->push_back(d);
            out->push_back(RECT{s.x, s.y, s.x + (d.right - d.left),
                                s.y + (d.bottom - d.top)});
        }

        UINT dirtyUsed = 0;
        if (FAILED(duplication_->GetFrameDirtyRects(
                static_cast<UINT>(buffer.size() - used),
                reinterpret_cast<RECT*>(buffer.data() + used), &dirtyUsed)))
            return false;
        const UINT dirtyCount = dirtyUsed / sizeof(RECT);
        auto* dirty = reinterpret_cast<RECT*>(buffer.data() + used);
        for (UINT i = 0; i < dirtyCount; ++i) out->push_back(dirty[i]);

        if (out->empty() || out->size() > kMaxRects) {
            out->clear();
            return false;
        }
        // Grow each region by the blur reach so filtered pixels near an edge
        // are regenerated from up-to-date neighbours.
        for (RECT& r : *out) {
            r.left = std::max(0l, r.left - blurReach_);
            r.top = std::max(0l, r.top - blurReach_);
            r.right = std::min(static_cast<LONG>(captureWidth_),
                               r.right + blurReach_);
            r.bottom = std::min(static_cast<LONG>(captureHeight_),
                                r.bottom + blurReach_);
        }
        return true;
    }

private:
    bool CompileShader(const char* source, const char* target, ID3DBlob** out) {
        ComPtr<ID3DBlob> errors;
        HRESULT hr = D3DCompile(source, strlen(source), nullptr, nullptr, nullptr,
                                "main", target,
                                D3DCOMPILE_ENABLE_STRICTNESS |
                                    D3DCOMPILE_OPTIMIZATION_LEVEL3,
                                0, out, &errors);
        if (FAILED(hr)) {
            std::string msg = errors
                                  ? std::string(static_cast<char*>(
                                                    errors->GetBufferPointer()),
                                                errors->GetBufferSize())
                                  : "Shader compilation failed";
            MessageBoxA(nullptr, msg.c_str(), "Myopic Defocus",
                        MB_ICONERROR);
            return false;
        }
        return true;
    }

    bool CreateResources() {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = captureWidth_;
        td.Height = captureHeight_;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &captureTexture_)))
            return false;
        if (FAILED(device_->CreateShaderResourceView(captureTexture_.Get(), nullptr,
                                                     &captureSrv_)))
            return false;

        // Full resolution is required to match Refractify's SVG Gaussian
        // semantics. The 1.2.x half-resolution shortcut added an unintended
        // resampling blur, especially visible in the green channel.
        td.Width = captureWidth_;
        td.Height = captureHeight_;
        // Only the green and blue channels are filtered, so a two-channel
        // target halves intermediate bandwidth versus BGRA8. 16-bit float
        // keeps enough precision to avoid banding in smooth gradients.
        td.Format = DXGI_FORMAT_R16G16_FLOAT;
        td.BindFlags =
            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &intermediateTexture_)))
            return false;
        device_->CreateShaderResourceView(intermediateTexture_.Get(), nullptr,
                                          &intermediateSrv_);
        device_->CreateRenderTargetView(intermediateTexture_.Get(), nullptr,
                                        &intermediateRtv_);

        // Persistent full-resolution composition target. Dirty-rect rendering
        // updates only changed regions, so the result must survive across
        // frames; a FLIP_DISCARD back buffer does not.
        td.Width = captureWidth_;
        td.Height = captureHeight_;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &composedTexture_)))
            return false;
        if (FAILED(device_->CreateRenderTargetView(composedTexture_.Get(),
                                                   nullptr, &composedRtv_)))
            return false;

        D3D11_RASTERIZER_DESC rs{};
        rs.FillMode = D3D11_FILL_SOLID;
        rs.CullMode = D3D11_CULL_NONE;
        rs.DepthClipEnable = TRUE;
        rs.ScissorEnable = TRUE;
        device_->CreateRasterizerState(&rs, &scissorState_);

        ComPtr<ID3DBlob> vsBlob, ps1Blob, ps2Blob;
        if (!CompileShader(kVertexShader, "vs_5_0", &vsBlob) ||
            !CompileShader(kBlurShader, "ps_5_0", &ps1Blob) ||
            !CompileShader(kCompositeShader, "ps_5_0", &ps2Blob))
            return false;
        device_->CreateVertexShader(vsBlob->GetBufferPointer(),
                                    vsBlob->GetBufferSize(), nullptr, &vs_);
        device_->CreatePixelShader(ps1Blob->GetBufferPointer(),
                                   ps1Blob->GetBufferSize(), nullptr, &blurPs_);
        device_->CreatePixelShader(ps2Blob->GetBufferPointer(),
                                   ps2Blob->GetBufferSize(), nullptr,
                                   &compositePs_);

        D3D11_SAMPLER_DESC samp{};
        samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = samp.AddressV = samp.AddressW =
            D3D11_TEXTURE_ADDRESS_CLAMP;
        samp.MaxLOD = D3D11_FLOAT32_MAX;
        device_->CreateSamplerState(&samp, &sampler_);

        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(ShaderConstants);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        device_->CreateBuffer(&bd, nullptr, &constantBuffer_);

        // Four query slots: never overwrite a result still in flight.
        D3D11_QUERY_DESC qd{};
        for (auto& timer : timers_) {
            qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            device_->CreateQuery(&qd, &timer.disjoint);
            qd.Query = D3D11_QUERY_TIMESTAMP;
            device_->CreateQuery(&qd, &timer.start);
            device_->CreateQuery(&qd, &timer.end);
            if (!timer.disjoint || !timer.start || !timer.end) {
                timer.disjoint.Reset(); timer.start.Reset(); timer.end.Reset();
            }
        }
        lastStatsTick_ = GetTickCount64();

        return captureSrv_ && intermediateSrv_ && intermediateRtv_ &&
               composedRtv_ && scissorState_ && vs_ &&
               blurPs_ && compositePs_ && sampler_ && constantBuffer_;
    }

    // Renders the regions in `rects` (full-resolution desktop coordinates)
    // into the persistent composition target. An empty list means full frame.
    bool Draw(const std::vector<RECT>& rects) {
        // While disabled the overlay window is hidden, so skip all GPU work
        // and presentation: the desktop then runs at native latency.
        if (!gEnabled) return true;
        CollectTiming();
        GpuTimer* timer = nullptr;
        for (auto& candidate : timers_) {
            if (candidate.disjoint && !candidate.pending) {
                timer = &candidate;
                break;
            }
        }
        if (timer) {
            context_->Begin(timer->disjoint.Get());
            context_->End(timer->start.Get());
        }
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vs_.Get(), nullptr, 0);
        context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
        context_->PSSetConstantBuffers(0, 1, constantBuffer_.GetAddressOf());
        context_->RSSetState(scissorState_.Get());

        ID3D11ShaderResourceView* nullSrvs[2] = {nullptr, nullptr};
        const int fullW = static_cast<int>(captureWidth_);
        const int fullH = static_cast<int>(captureHeight_);

        // Pass 1: full-resolution horizontal blur of G/B. Refractify's active
        // SVG filter applies feGaussianBlur at source resolution; 1.2.x used a
        // half-resolution shortcut that softened fine green/blue detail.
        D3D11_VIEWPORT vp{};
        vp.Width = static_cast<float>(fullW);
        vp.Height = static_cast<float>(fullH);
        vp.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &vp);
        context_->OMSetRenderTargets(1, intermediateRtv_.GetAddressOf(),
                                     nullptr);
        context_->PSSetShaderResources(0, 1, captureSrv_.GetAddressOf());
        context_->PSSetShader(blurPs_.Get(), nullptr, 0);
        if (rects.empty()) {
            SetScissor(0, 0, fullW, fullH, fullW, fullH);
            context_->Draw(3, 0);
        } else {
            for (const RECT& r : rects) {
                // Pass 2 reads this target vertically, so grow the rows we
                // regenerate by the current Gaussian reach.
                SetScissor(r.left, r.top - blurReach_, r.right,
                           r.bottom + blurReach_, fullW, fullH);
                context_->Draw(3, 0);
            }
        }
        context_->PSSetShaderResources(0, 2, nullSrvs);

        // Pass 2 always writes the persistent composition target. The former
        // full-frame path wrote only the swap-chain back buffer, leaving this
        // target uninitialised/stale for the following incremental frame.
        vp.Width = static_cast<float>(captureWidth_);
        vp.Height = static_cast<float>(captureHeight_);
        context_->RSSetViewports(1, &vp);
        context_->OMSetRenderTargets(1, composedRtv_.GetAddressOf(), nullptr);
        ID3D11ShaderResourceView* second[] = {intermediateSrv_.Get(),
                                              captureSrv_.Get()};
        context_->PSSetShaderResources(0, 2, second);
        context_->PSSetShader(compositePs_.Get(), nullptr, 0);
        if (rects.empty()) {
            SetScissor(0, 0, static_cast<int>(width_), static_cast<int>(height_),
                       static_cast<int>(width_), static_cast<int>(height_));
            context_->Draw(3, 0);
        } else {
            for (const RECT& r : rects) {
                SetScissor(r.left, r.top, r.right, r.bottom,
                           static_cast<int>(captureWidth_),
                           static_cast<int>(captureHeight_));
                context_->Draw(3, 0);
            }
        }
        context_->PSSetShaderResources(0, 2, nullSrvs);
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        context_->RSSetState(nullptr);

        const bool presented = Present(timer);
        if (presented) ++framesPresented_;
        return presented;
    }

    void SetScissor(int l, int t, int r, int b, int maxW, int maxH) {
        const int left = std::clamp(l, 0, maxW);
        const int top = std::clamp(t, 0, maxH);
        D3D11_RECT sr{};
        sr.left = left;
        sr.top = top;
        sr.right = std::clamp(r, left, maxW);
        sr.bottom = std::clamp(b, top, maxH);
        context_->RSSetScissorRects(1, &sr);
    }

    bool Present(GpuTimer* timer) {
        {
            // Every present receives a complete valid image; release the
            // temporary back-buffer reference before flipping.
            ComPtr<ID3D11Texture2D> backBuffer;
            const HRESULT bufferHr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
            if (FAILED(bufferHr)) {
                EndTiming(timer);
                LogFailure("GetBackBuffer", bufferHr);
                return false;
            }
            context_->CopyResource(backBuffer.Get(), composedTexture_.Get());
        }
        // Timestamp GPU work before entering CPU Present/vsync waiting.
        EndTiming(timer);
        const HRESULT hr = swapChain_->Present(1, 0);
        if (hr != S_OK) {
            LogFailure("Present", hr);
            return false;
        }
        hasPresentedFrame_ = true;
        return true;
    }

    // Non-blocking GPU timer read-back. Results are one or more frames stale,
    // which is fine for a rolling average.
    void CollectTiming() {
        for (auto& timer : timers_) {
            if (!timer.pending) continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            UINT64 t0 = 0, t1 = 0;
            if (context_->GetData(timer.disjoint.Get(), &dj, sizeof(dj),
                                  D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                context_->GetData(timer.start.Get(), &t0, sizeof(t0),
                                  D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                context_->GetData(timer.end.Get(), &t1, sizeof(t1),
                                  D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
                continue;
            timer.pending = false;
            if (dj.Disjoint || dj.Frequency == 0 || t1 <= t0) continue;
            const double ms = double(t1 - t0) * 1000.0 / double(dj.Frequency);
            gpuMsAvg_ = gpuMsAvg_ == 0.0 ? ms : gpuMsAvg_ * 0.9 + ms * 0.1;
            hasGpuTiming_ = true;
        }
    }

    void EndTiming(GpuTimer* timer) {
        if (!timer) return;
        context_->End(timer->end.Get());
        context_->End(timer->disjoint.Get());
        timer->pending = true;
    }

public:
    bool HasPresentedFrame() const { return hasPresentedFrame_; }
    bool HasGpuTiming() const { return hasGpuTiming_; }
    bool UsesFrameLatencyWait() const {
        return frameLatencyWaitable_ != nullptr;
    }
    void InvalidatePresentation() { hasPresentedFrame_ = false; }

    // Releases every device object so the overlay window can be rebuilt for a
    // different back end.
    void Shutdown() {
        if (context_) {
            context_->ClearState();
            // Submit deferred releases before recreating a flip chain for
            // the same HWND; Flush submits work, it is not a GPU completion wait.
            context_->Flush();
        }
        duplication_.Reset();
        if (frameLatencyWaitable_) CloseHandle(frameLatencyWaitable_);
        frameLatencyWaitable_ = nullptr;
        swapChain_.Reset();
        composedRtv_.Reset();
        composedTexture_.Reset();
        intermediateRtv_.Reset();
        intermediateSrv_.Reset();
        intermediateTexture_.Reset();
        captureSrv_.Reset();
        captureTexture_.Reset();
        scissorState_.Reset();
        for (auto& timer : timers_) {
            timer.disjoint.Reset(); timer.start.Reset(); timer.end.Reset();
            timer.pending = false;
        }
        hasGpuTiming_ = false;
        constantBuffer_.Reset();
        sampler_.Reset();
        compositePs_.Reset();
        blurPs_.Reset();
        vs_.Reset();
        context_.Reset();
        device_.Reset();
        hasFrame_ = false;
        hasPresentedFrame_ = false;
        forceFullRedraw_ = true;
        adapter_.Reset();
        output_.Reset();
        gpuMsAvg_ = fpsAvg_ = 0.0;
        framesPresented_ = 0;
    }

    // Returns GPU milliseconds per processed frame and presented frames/sec.
    void GetStats(double* gpuMs, double* fps) {
        ULONGLONG now = GetTickCount64();
        if (now > lastStatsTick_) {
            fpsAvg_ = double(framesPresented_) * 1000.0 /
                      double(now - lastStatsTick_);
            framesPresented_ = 0;
            lastStatsTick_ = now;
        }
        *gpuMs = gpuMsAvg_;
        *fps = fpsAvg_;
    }

private:

    UINT width_ = 0, height_ = 0;
    UINT captureWidth_ = 0, captureHeight_ = 0;
    bool hasFrame_ = false;
    bool hasPresentedFrame_ = false;
    bool forceFullRedraw_ = true;
    int blurReach_ = 1;
    std::array<GpuTimer, 4> timers_{};
    bool hasGpuTiming_ = false;
    ComPtr<ID3D11Texture2D> composedTexture_;
    ComPtr<ID3D11RenderTargetView> composedRtv_;
    ComPtr<ID3D11RasterizerState> scissorState_;
    std::vector<RECT> dirtyScratch_;
    HANDLE frameLatencyWaitable_ = nullptr;
    double gpuMsAvg_ = 0.0;
    double fpsAvg_ = 0.0;
    UINT framesPresented_ = 0;
    ULONGLONG lastStatsTick_ = 0;
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<IDXGIOutput> output_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<IDXGIOutputDuplication> duplication_;
    ComPtr<ID3D11Texture2D> captureTexture_, intermediateTexture_;
    ComPtr<ID3D11ShaderResourceView> captureSrv_, intermediateSrv_;
    ComPtr<ID3D11RenderTargetView> intermediateRtv_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> blurPs_, compositePs_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11Buffer> constantBuffer_;
};

Renderer gRenderer;

void UpdateTrayTip() {
    wcscpy_s(gTray.szTip,
             gEnabled ? L"Myopic Defocus — Enabled"
                      : L"Myopic Defocus — Disabled");
    gTray.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &gTray);
}

void SetEnabled(bool enabled) {
    gEnabled = enabled;
    if (gOverlay) {
        if (!enabled || !gRendererReady) {
            SetLayeredWindowAttributes(gOverlay, 0, 0, LWA_ALPHA);
            gOverlayOpaque = false;
            // Hiding removes the layer from DWM composition entirely, so the
            // disabled desktop returns to native latency.
            ShowWindow(gOverlay, SW_HIDE);
        } else {
            // Warm up transparently. The layer becomes opaque only after a
            // newly filtered frame has been presented successfully.
            SetLayeredWindowAttributes(gOverlay, 0, 0, LWA_ALPHA);
            gOverlayOpaque = false;
            ShowWindow(gOverlay, SW_SHOWNA);
            RaiseOverlay(true);
        }
    }
    // An old successful Present must not make a just-reenabled layer opaque
    // before its newly requested full redraw completes.
    gRenderer.InvalidatePresentation();
    gRenderer.UpdateConstants();
    UpdateTrayTip();
}

bool GetEditFloat(HWND parent, int id, float* value) {
    wchar_t text[64]{};
    GetWindowTextW(GetDlgItem(parent, id), text, 64);
    return ParseFiniteFloat(text, value);
}

// Dialog metrics are authored at 96 DPI and scaled to the window's actual DPI.
// Dialog metrics are authored at 96 DPI and scaled to the window's actual DPI.
// The label column is measured from the real strings so the edit boxes sit
// right after the longest label instead of after a fixed, mostly empty column.
constexpr int kDlgEditW = 90;
constexpr int kDlgMargin = 18;
constexpr int kDlgRowH = 38;
constexpr int kDlgGap = 12;

const wchar_t* const kDlgLabels[] = {
    L"Monitor diagonal (inches)",
    L"Viewing distance (cm)",
    L"Pupil diameter (mm)",
    L"Effect strength (0-100 %)",
};

UINT DpiOf(HWND hwnd) {
    UINT dpi = hwnd ? GetDpiForWindow(hwnd) : GetDpiForSystem();
    return dpi ? dpi : 96;
}

int ScaleFor(UINT dpi, int v) { return MulDiv(v, static_cast<int>(dpi), 96); }

int Scaled(HWND hwnd, int v) { return ScaleFor(DpiOf(hwnd), v); }

// The shell's message font (Segoe UI). Without this, controls fall back to the
// legacy bitmap System font, which is both ugly and wider than expected.
HFONT DialogFontFor(UINT dpi) {
    struct FontCache {
        std::vector<std::pair<UINT, HFONT>> fonts;
        ~FontCache() {
            for (const auto& entry : fonts) DeleteObject(entry.second);
        }
    };
    static FontCache cache;
    for (const auto& entry : cache.fonts)
        if (entry.first == dpi) return entry.second;
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    HFONT font = nullptr;
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0,
                                   dpi))
        font = CreateFontIndirectW(&ncm.lfMessageFont);
    if (!font) return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    cache.fonts.emplace_back(dpi, font);
    return font;
}

HFONT DialogFont(HWND hwnd) { return DialogFontFor(DpiOf(hwnd)); }

// Widest label in device pixels, so no row carries dead space.
int LabelColumnWidth(UINT dpi) {
    HDC dc = GetDC(nullptr);
    if (!dc) return ScaleFor(dpi, 200);
    HGDIOBJ prev = SelectObject(dc, DialogFontFor(dpi));
    int widest = 0;
    for (const wchar_t* label : kDlgLabels) {
        SIZE sz{};
        if (GetTextExtentPoint32W(dc, label, static_cast<int>(wcslen(label)),
                                  &sz))
            widest = std::max<int>(widest, sz.cx);
    }
    SelectObject(dc, prev);
    ReleaseDC(nullptr, dc);
    return widest + ScaleFor(dpi, 4);
}

void AddLabelAndEdit(HWND parent, const wchar_t* label, int row, int id,
                     float value) {
    const UINT dpi = DpiOf(parent);
    const int y = ScaleFor(dpi, kDlgMargin + row * kDlgRowH);
    const int labelW = LabelColumnWidth(dpi);
    const int margin = ScaleFor(dpi, kDlgMargin);
    HFONT font = DialogFontFor(dpi);
    HWND s = CreateWindowW(L"STATIC", label, WS_CHILD | WS_VISIBLE, margin,
                           y + ScaleFor(dpi, 4), labelW, ScaleFor(dpi, 24),
                           parent, nullptr, gInstance, nullptr);
    SendMessageW(s, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    wchar_t buf[32]{};
    swprintf(buf, 32, L"%.2f", value);
    HWND e = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", buf,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_RIGHT,
        margin + labelW + ScaleFor(dpi, kDlgGap), y, ScaleFor(dpi, kDlgEditW),
        ScaleFor(dpi, 26), parent, reinterpret_cast<HMENU>(id), gInstance,
        nullptr);
    SendMessageW(e, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        AddLabelAndEdit(hwnd, kDlgLabels[0], 0, 3101,
                        gSettings.diagonalInches);
        AddLabelAndEdit(hwnd, kDlgLabels[1], 1, 3102,
                        gSettings.distanceCm);
        AddLabelAndEdit(hwnd, kDlgLabels[2], 2, 3103,
                        gSettings.pupilMm);
        AddLabelAndEdit(hwnd, kDlgLabels[3], 3, 3104,
                        gSettings.strength * 100.0f);
        {
            HWND b = CreateWindowW(
                L"BUTTON", L"Save",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                Scaled(hwnd, kDlgMargin) + LabelColumnWidth(DpiOf(hwnd)) +
                    Scaled(hwnd, kDlgGap + kDlgEditW - 100),
                Scaled(hwnd, kDlgMargin + 4 * kDlgRowH + 8), Scaled(hwnd, 100),
                Scaled(hwnd, 30), hwnd, reinterpret_cast<HMENU>(kSaveSettings),
                gInstance, nullptr);
            SendMessageW(b, WM_SETFONT,
                         reinterpret_cast<WPARAM>(DialogFont(hwnd)), TRUE);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == kSaveSettings) {
            float values[4]{};
            constexpr float minimum[] = {8, 20, 2, 0};
            constexpr float maximum[] = {100, 300, 8, 100};
            for (int i = 0; i < 4; ++i) {
                if (!GetEditFloat(hwnd, 3101 + i, &values[i]) ||
                    values[i] < minimum[i] || values[i] > maximum[i]) {
                    wchar_t message[256]{};
                    swprintf(message, ARRAYSIZE(message),
                             L"%s: enter a number from %.0f to %.0f.\n"
                             L"Use a decimal point (for example 6.5).",
                             kDlgLabels[i], minimum[i], maximum[i]);
                    MessageBoxW(hwnd, message, L"Myopic Defocus", MB_ICONWARNING);
                    SetFocus(GetDlgItem(hwnd, 3101 + i));
                    return 0;
                }
            }
            gSettings.diagonalInches = values[0];
            gSettings.distanceCm = values[1];
            gSettings.pupilMm = values[2];
            gSettings.strength = values[3] / 100.0f;
            SaveSettings();
            gRenderer.UpdateConstants();
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        gSettingsWindow = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void OpenSettings() {
    if (gSettingsWindow) {
        SetForegroundWindow(gSettingsWindow);
        return;
    }
    // Size the client area to the scaled content, then grow for the frame.
    const UINT dpi = DpiOf(nullptr);
    const int clientW = ScaleFor(dpi, kDlgMargin) + LabelColumnWidth(dpi) +
                        ScaleFor(dpi, kDlgGap + kDlgEditW + kDlgMargin);
    const int clientH =
        ScaleFor(dpi, kDlgMargin + 4 * kDlgRowH + 8 + 30 + kDlgMargin);
    RECT r{0, 0, clientW, clientH};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_TOOLWINDOW, dpi);
    gSettingsWindow = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kSettingsClass,
        L"Myopic Defocus Settings", style, CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top, nullptr, nullptr, gInstance,
        nullptr);
    ShowWindow(gSettingsWindow, SW_SHOW);
}

void ShowTrayMenu(HWND hwnd) {
    POINT p{};
    GetCursorPos(&p);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (gEnabled ? MF_CHECKED : 0), kMenuToggle,
                L"Enabled\tCtrl+Alt+F8");
    AppendMenuW(menu, MF_STRING, kMenuSettings, L"Settings…");
    AppendMenuW(menu, MF_STRING, kMenuOpenIni, L"Open config file…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SETCURSOR:
        // The underlying application owns the cursor (including I-beam).
        // Do not call DefWindowProc, SetCursor(nullptr), or set an arrow here:
        // the former may restore a class cursor, the latter would hide/change
        // the shared desktop cursor. TRUE means leave its current state alone.
        return TRUE;
    case WM_NCHITTEST:
        // The full-screen output window is visual-only. Never let it become a
        // mouse target; Windows should continue hit-testing the application
        // beneath it.
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_DISPLAYCHANGE:
        // During resume Windows can briefly report a fallback resolution and
        // restore the real mode a moment later. Rebuild only after it settles.
        RequestDisplayReset(1500);
        return 0;
    case WM_DPICHANGED:
        RequestDisplayReset(1000);
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_APMSUSPEND) {
            RequestDisplayReset(0);
            return TRUE;
        }
        if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND ||
            wp == PBT_APMRESUMECRITICAL) {
            RequestDisplayReset(1500);
            return TRUE;
        }
        break;
    case kTrayMessage:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU)
            ShowTrayMenu(hwnd);
        else if (LOWORD(lp) == WM_LBUTTONDBLCLK)
            SetEnabled(!gEnabled);
        return 0;
    case WM_HOTKEY:
        if (wp == kHotkeyToggle) SetEnabled(!gEnabled);
        if (wp == kHotkeyUp) {
            gSettings.strength =
                std::min(1.0f, gSettings.strength + 0.05f);
            SaveSettings();
            gRenderer.UpdateConstants();
        }
        if (wp == kHotkeyDown) {
            gSettings.strength =
                std::max(0.0f, gSettings.strength - 0.05f);
            SaveSettings();
            gRenderer.UpdateConstants();
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case kMenuToggle:
            SetEnabled(!gEnabled);
            return 0;
        case kMenuSettings:
            OpenSettings();
            return 0;
        case kMenuOpenIni:
            // Shows the real path, which is not next to the executable unless
            // a portable INI was placed there.
            ShellExecuteW(nullptr, L"open", L"notepad.exe", gIniPath.c_str(),
                          nullptr, SW_SHOWNORMAL);
            return 0;
        case kMenuExit:
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &gTray);
        UnregisterHotKey(hwnd, kHotkeyToggle);
        UnregisterHotKey(hwnd, kHotkeyUp);
        UnregisterHotKey(hwnd, kHotkeyDown);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool RegisterClasses() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = gInstance;
    wc.lpfnWndProc = OverlayProc;
    wc.lpszClassName = kClassName;
    // The visual-only overlay must not supply a fallback arrow cursor.
    wc.hCursor = nullptr;
    wc.hIcon = LoadIconW(gInstance, MAKEINTRESOURCEW(kAppIcon));
    wc.hIconSm = static_cast<HICON>(LoadImageW(
        gInstance, MAKEINTRESOURCEW(kAppIcon), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    if (!RegisterClassExW(&wc)) return false;
    wc.lpfnWndProc = SettingsProc;
    wc.lpszClassName = kSettingsClass;
    // The actual settings dialog remains an ordinary interactive window.
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    return RegisterClassExW(&wc) != 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    struct HandleOwner {
        HANDLE handle;
        ~HandleOwner() { if (handle) CloseHandle(handle); }
    };
    HandleOwner instanceMutex{
        CreateMutexW(nullptr, FALSE, L"Local\\R1CK.MyopicDefocus.SingleInstance")};
    if (!instanceMutex.handle) return 5;
    const bool alreadyRunning = GetLastError() == ERROR_ALREADY_EXISTS ||
                                FindWindowW(kClassName, nullptr) != nullptr;
    if (alreadyRunning) {
        MessageBoxW(nullptr, L"Myopic Defocus is already running.\n"
                             L"Exit the existing version from its tray menu first.",
                    L"Myopic Defocus", MB_ICONINFORMATION);
        return 0;
    }
    gInstance = instance;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    struct Cleanup {
        bool comInitialized;
        ~Cleanup() {
            if (gOverlay && IsWindow(gOverlay)) {
                SetLayeredWindowAttributes(gOverlay, 0, 0, LWA_ALPHA);
                ShowWindow(gOverlay, SW_HIDE);
            }
            if (gShowHook) UnhookWinEvent(gShowHook);
            if (gMenuHook) UnhookWinEvent(gMenuHook);
            gRenderer.Shutdown();
            if (gSettingsWindow && IsWindow(gSettingsWindow))
                DestroyWindow(gSettingsWindow);
            if (gOverlay && IsWindow(gOverlay)) DestroyWindow(gOverlay);
            if (comInitialized) CoUninitialize();
        }
    } cleanup{SUCCEEDED(com)};
    gIniPath = IniPath();
    EnsureIniExists();
    MigrateIni();
    LoadSettings();
    if (!RegisterClasses()) return 1;

    RECT bounds = PrimaryScreenBounds();

    gOverlay = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
            WS_EX_TOOLWINDOW,
        kClassName, L"Myopic Defocus", WS_POPUP, bounds.left, bounds.top,
        bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr, nullptr,
        instance, nullptr);
    if (!gOverlay) return 2;

    // Start fully transparent. It is made opaque only after the first filtered
    // frame has successfully reached the swap chain, preventing startup and
    // resume from exposing a black uninitialised surface.
    SetLayeredWindowAttributes(gOverlay, 0, 0, LWA_ALPHA);

    // Windows 10 2004+ (build 19041+): keep the overlay out of screen capture
    // so Desktop Duplication does not feed our own output back into itself.
    // Do NOT fall back to WDA_MONITOR: that renders the window black in the
    // capture, which would blank the whole screen.
    if (!SetWindowDisplayAffinity(gOverlay, 0x00000011 /* EXCLUDEFROMCAPTURE */)) {
        MessageBoxW(nullptr,
                    L"This build requires Windows 10 version 2004 (build 19041) "
                    L"or newer for capture exclusion.",
                    L"Myopic Defocus", MB_ICONERROR);
        return 4;
    }

    // Stay above context menus, drop-downs and tooltips, which are topmost too.
    gMenuHook = SetWinEventHook(EVENT_SYSTEM_MENUPOPUPSTART,
                                EVENT_SYSTEM_MENUPOPUPSTART, nullptr,
                                WinEventProc, 0, 0,
                                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    gShowHook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr,
                                WinEventProc, 0, 0,
                                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    if (!gRenderer.Initialize(gOverlay)) return 3;

    gTray.cbSize = sizeof(gTray);
    gTray.hWnd = gOverlay;
    gTray.uID = kTrayId;
    gTray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    gTray.uCallbackMessage = kTrayMessage;
    gTray.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(kAppIcon));
    wcscpy_s(gTray.szTip, L"Myopic Defocus — Enabled");
    Shell_NotifyIconW(NIM_ADD, &gTray);

    RegisterHotKey(gOverlay, kHotkeyToggle, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT,
                   VK_F8);
    RegisterHotKey(gOverlay, kHotkeyUp, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT,
                   VK_UP);
    RegisterHotKey(gOverlay, kHotkeyDown, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT,
                   VK_DOWN);

    ShowWindow(gOverlay, SW_SHOWNA);
    UpdateWindow(gOverlay);

    MSG msg{};
    bool running = true;
    gRendererReady = true;
    ULONGLONG nextRecoveryAttempt = 0;
    ULONGLONG lastTick = 0;
    while (running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        // Safety net for surfaces that appear without a hooked event, plus a
        // once-per-second stats refresh shown in the tray tooltip.
        ULONGLONG now = GetTickCount64();
        if (now - lastTick >= 1000) {
            lastTick = now;
            // WM_DISPLAYCHANGE can be missed around sleep/resume. Comparing
            // against the current primary-monitor bounds is a second guard
            // against leaving a stale, smaller overlay at the top-left.
            if (!OverlayMatchesPrimaryScreen()) RequestDisplayReset(1000);
            if (gRendererReady) {
                RaiseOverlay(true);
                double gpuMs = 0.0, fps = 0.0;
                gRenderer.GetStats(&gpuMs, &fps);
                wchar_t timing[48]{};
                if (gRenderer.HasGpuTiming())
                    swprintf_s(timing, L"GPU %.2f ms", gpuMs);
                else
                    wcscpy_s(timing, L"GPU n/a");
                swprintf_s(gTray.szTip, L"Myopic Defocus | %s%s%s\n%s | %.0f upd/s",
                           gEnabled ? L"Enabled" : L"Disabled",
                           gRenderer.UsesFrameLatencyWait() ? L" | LowLatency" : L"",
                           gUseDirtyRects ? L" | Dirty" : L"", timing, fps);
                gTray.uFlags = NIF_TIP;
                Shell_NotifyIconW(NIM_MODIFY, &gTray);
            }
        }
        if (running && gDisplayResetRequested) {
            gDisplayResetRequested = false;
            if (gRendererReady) {
                gRendererReady = false;
                SetLayeredWindowAttributes(gOverlay, 0, 0, LWA_ALPHA);
                gOverlayOpaque = false;
                ShowWindow(gOverlay, SW_HIDE);
                gRenderer.Shutdown();
            }
            nextRecoveryAttempt =
                std::max(gDisplayResetNotBefore, GetTickCount64() + 250);
            wcscpy_s(gTray.szTip,
                     L"Myopic Defocus — Waiting for display…");
            gTray.uFlags = NIF_TIP;
            Shell_NotifyIconW(NIM_MODIFY, &gTray);
        }
        if (running && gRendererReady) {
            if (!gRenderer.RenderFrame()) {
                // Never leave a failed/uninitialised back buffer visible.
                SetLayeredWindowAttributes(gOverlay, 0, 0, LWA_ALPHA);
                gOverlayOpaque = false;
                gRendererReady = false;
                ShowWindow(gOverlay, SW_HIDE);
                gRenderer.Shutdown();
                nextRecoveryAttempt = GetTickCount64() + 1000;
                wcscpy_s(gTray.szTip,
                         L"Myopic Defocus — Recovering display capture…");
                gTray.uFlags = NIF_TIP;
                Shell_NotifyIconW(NIM_MODIFY, &gTray);
            } else if (gEnabled && gRenderer.HasPresentedFrame() &&
                       !gOverlayOpaque) {
                // Reveal atomically only after a valid filtered frame exists.
                SetLayeredWindowAttributes(gOverlay, 0, 255, LWA_ALPHA);
                gOverlayOpaque = true;
                ShowWindow(gOverlay, SW_SHOWNA);
                RaiseOverlay(true);
            }
        }
        if (running && !gRendererReady) {
            const ULONGLONG retryNow = GetTickCount64();
            if (retryNow >= nextRecoveryAttempt) {
                // Initialization is intentionally quiet during recovery:
                // screen lock and mode switches may keep Duplication
                // unavailable for several seconds.
                if (ResizeOverlayToPrimaryScreen() &&
                    gRenderer.Initialize(gOverlay, true)) {
                    gRendererReady = true;
                    if (gEnabled) {
                        SetLayeredWindowAttributes(gOverlay, 0, 0, LWA_ALPHA);
                        gOverlayOpaque = false;
                        ShowWindow(gOverlay, SW_SHOWNA);
                        RaiseOverlay(true);
                    }
                    UpdateTrayTip();
                } else {
                    gRenderer.Shutdown();
                    nextRecoveryAttempt = retryNow + 2000;
                }
            }
            Sleep(20);
        }
    }
    return 0;
}
