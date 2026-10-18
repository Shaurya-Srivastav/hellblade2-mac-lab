// Asks the game's own Streamline whether DLSS Frame Generation is supported, and
// independently reads every value Streamline bases that decision on.
#include <windows.h>
#include <dxgi1_6.h>
#include <shellscalingapi.h>
#include <cstdio>
#include <cstdint>
#include <vector>

#include "sl.h"
#include "sl_dlss_g.h"
#include "sl_reflex.h"
#include "sl_dlss.h"
#include "sl_pcl.h"

typedef LONG NTSTATUS;

// d3dkmthk.h subset
typedef UINT D3DKMT_HANDLE;
struct D3DKMT_ADAPTERINFO { D3DKMT_HANDLE hAdapter; LUID AdapterLuid; ULONG NumOfSources; BOOL bPrecisePresentRegionsPreferred; };
struct D3DKMT_ENUMADAPTERS2 { ULONG NumAdapters; D3DKMT_ADAPTERINFO* pAdapters; };
struct D3DKMT_QUERYADAPTERINFO { D3DKMT_HANDLE hAdapter; UINT Type; VOID* pPrivateDriverData; UINT PrivateDriverDataSize; };
struct D3DKMT_WDDM_2_7_CAPS { union { struct { UINT HwSchSupported : 1; UINT HwSchEnabled : 1; UINT HwSchEnabledByDefault : 1; UINT IndependentVidPnVSyncControl : 1; UINT Reserved : 28; }; UINT Value; }; };
static const UINT KMTQAITYPE_WDDM_2_7_CAPS = 70;

static const char* resultName(sl::Result r)
{
    static const char* names[] = {"eOk", "eErrorIO", "eErrorDriverOutOfDate", "eErrorOSOutOfDate", "eErrorOSDisabledHWS",
        "eErrorDeviceNotCreated", "eErrorNoSupportedAdapterFound", "eErrorAdapterNotSupported", "eErrorNoPlugins",
        "eErrorVulkanAPI", "eErrorDXGIAPI", "eErrorD3DAPI", "eErrorNRDAPI", "eErrorNVAPI", "eErrorReflexAPI",
        "eErrorNGXFailed", "eErrorJSONParsing", "eErrorMissingProxy", "eErrorMissingResourceState",
        "eErrorInvalidIntegration", "eErrorMissingInputParameter", "eErrorNotInitialized", "eErrorComputeFailed",
        "eErrorInitNotCalled", "eErrorExceptionHandler", "eErrorInvalidParameter", "eErrorMissingConstants",
        "eErrorDuplicatedConstants", "eErrorMissingOrInvalidAPI", "eErrorCommonConstantsMissing",
        "eErrorUnsupportedInterface", "eErrorFeatureMissing", "eErrorFeatureNotSupported", "eErrorFeatureMissingHooks",
        "eErrorFeatureFailedToLoad", "eErrorFeatureWrongPriority", "eErrorFeatureMissingDependency",
        "eErrorFeatureManagerInvalidState", "eErrorInvalidState"};
    unsigned i = (unsigned)r;
    return i < sizeof(names) / sizeof(names[0]) ? names[i] : "?";
}

static void slLog(sl::LogType type, const char* msg) { printf("  [sl %d] %s", (int)type, msg); fflush(stdout); }

static void probeOS()
{
    typedef NTSTATUS(WINAPI * PFN)(PRTL_OSVERSIONINFOW);
    auto fn = (PFN)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    RTL_OSVERSIONINFOW v{sizeof(v)};
    if (fn && fn(&v) == 0) printf("os: RtlGetVersion %lu.%lu.%lu\n", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
}

static void probeHWS(const std::vector<LUID>& dxgiLuids)
{
    HMODULE gdi = LoadLibraryW(L"gdi32.dll");
    typedef NTSTATUS(WINAPI * PEnum)(D3DKMT_ENUMADAPTERS2*);
    typedef NTSTATUS(WINAPI * PQuery)(D3DKMT_QUERYADAPTERINFO*);
    auto enumAdapters = (PEnum)GetProcAddress(gdi, "D3DKMTEnumAdapters2");
    auto query = (PQuery)GetProcAddress(gdi, "D3DKMTQueryAdapterInfo");
    printf("hws: D3DKMTEnumAdapters2=%p D3DKMTQueryAdapterInfo=%p\n", (void*)enumAdapters, (void*)query);
    if (!enumAdapters) return;
    D3DKMT_ADAPTERINFO infos[8]{};
    D3DKMT_ENUMADAPTERS2 e{8, infos};
    NTSTATUS s = enumAdapters(&e);
    printf("hws: EnumAdapters2 status=0x%08lx count=%lu\n", (unsigned long)s, e.NumAdapters);
    for (ULONG i = 0; s == 0 && i < e.NumAdapters; i++) {
        bool matches = false;
        for (auto& l : dxgiLuids) matches |= l.HighPart == infos[i].AdapterLuid.HighPart && l.LowPart == infos[i].AdapterLuid.LowPart;
        D3DKMT_WDDM_2_7_CAPS caps{};
        D3DKMT_QUERYADAPTERINFO q{infos[i].hAdapter, KMTQAITYPE_WDDM_2_7_CAPS, &caps, sizeof(caps)};
        NTSTATUS qs = query ? query(&q) : -1;
        printf("hws:   kmt adapter %lu luid=%08lx:%08lx matchesDXGI=%d query=0x%08lx HwSchSupported=%u HwSchEnabled=%u\n", i,
               (unsigned long)infos[i].AdapterLuid.HighPart, infos[i].AdapterLuid.LowPart, matches, (unsigned long)qs,
               caps.HwSchSupported, caps.HwSchEnabled);
    }
}

static std::vector<LUID> probeDXGI()
{
    std::vector<LUID> luids;
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f))) { printf("dxgi: CreateDXGIFactory1 failed\n"); return luids; }
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        printf("dxgi: adapter %u '%ls' vendor=0x%04x device=0x%04x luid=%08lx:%08lx vram=%lluMB\n", i, d.Description, d.VendorId,
               d.DeviceId, (unsigned long)d.AdapterLuid.HighPart, d.AdapterLuid.LowPart, (unsigned long long)(d.DedicatedVideoMemory >> 20));
        luids.push_back(d.AdapterLuid);
        IDXGIOutput* o = nullptr;
        for (UINT j = 0; a->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; j++) {
            DXGI_OUTPUT_DESC od{};
            o->GetDesc(&od);
            printf("dxgi:   output %u '%ls' desktop=(%ld,%ld)-(%ld,%ld)\n", j, od.DeviceName, od.DesktopCoordinates.left,
                   od.DesktopCoordinates.top, od.DesktopCoordinates.right, od.DesktopCoordinates.bottom);
            for (DXGI_FORMAT fmt : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM}) {
                UINT n = 0;
                HRESULT hr = o->GetDisplayModeList(fmt, 0, &n, nullptr);
                std::vector<DXGI_MODE_DESC> modes(n);
                if (n) o->GetDisplayModeList(fmt, 0, &n, modes.data());
                printf("dxgi:     modes fmt=%d hr=0x%08lx count=%u:", fmt, (unsigned long)hr, n);
                for (UINT m = 0; m < n; m++)
                    printf(" %ux%u@%.0f", modes[m].Width, modes[m].Height,
                           modes[m].RefreshRate.Denominator ? (double)modes[m].RefreshRate.Numerator / modes[m].RefreshRate.Denominator : 0.0);
                printf("\n");
            }
            o->Release();
        }
        a->Release();
    }
    f->Release();
    return luids;
}

static void probeNvAPI()
{
    HMODULE nv = LoadLibraryW(L"nvapi64.dll");
    printf("nvapi: nvapi64.dll=%p\n", (void*)nv);
    if (!nv) return;
    typedef void*(__cdecl * PQI)(unsigned int);
    auto qi = (PQI)GetProcAddress(nv, "nvapi_QueryInterface");
    if (!qi) return;
    typedef int(__cdecl * PInit)();
    typedef int(__cdecl * PEnumGPUs)(void* handles[64], unsigned int* count);
    typedef int(__cdecl * PDriver)(unsigned int* version, char name[64]);
    typedef int(__cdecl * PArch)(void* gpu, unsigned int* info);
    typedef int(__cdecl * PName)(void* gpu, char name[64]);
    auto init = (PInit)qi(0x0150E828);
    auto enumGPUs = (PEnumGPUs)qi(0xE5AC921F);
    auto driver = (PDriver)qi(0x2926AAAD);
    auto arch = (PArch)qi(0xD8265D24);
    auto name = (PName)qi(0xCEEE8E9F);
    printf("nvapi: Initialize=%d\n", init ? init() : -999);
    unsigned int ver = 0; char branch[64] = {};
    printf("nvapi: SYS_GetDriverAndBranchVersion=%d version=%u.%02u branch=%s\n", driver ? driver(&ver, branch) : -999, ver / 100, ver % 100, branch);
    void* gpus[64] = {}; unsigned int n = 0;
    printf("nvapi: EnumPhysicalGPUs=%d count=%u\n", enumGPUs ? enumGPUs(gpus, &n) : -999, n);
    for (unsigned i = 0; i < n; i++) {
        unsigned int info[4] = {16 | (2 << 16), 0, 0, 0};
        char full[64] = {};
        int rn = name ? name(gpus[i], full) : -999;
        int ra = arch ? arch(gpus[i], info) : -999;
        printf("nvapi:   gpu %u name=%d '%s' GetArchInfo=%d architecture=0x%x implementation=0x%x revision=0x%x\n", i, rn, full, ra, info[1], info[2], info[3]);
    }
}

static BOOL CALLBACK monitorProc(HMONITOR m, HDC, LPRECT, LPARAM)
{
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(m, &mi);
    UINT dx = 0, dy = 0;
    GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &dx, &dy);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm);
    printf("display: monitor '%ls' rect=(%ld,%ld)-(%ld,%ld) work=(%ld,%ld)-(%ld,%ld) effectiveDPI=%u mode=%lux%lu@%lu\n", mi.szDevice,
           mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom, mi.rcWork.left, mi.rcWork.top,
           mi.rcWork.right, mi.rcWork.bottom, dx, (unsigned long)dm.dmPelsWidth, (unsigned long)dm.dmPelsHeight, (unsigned long)dm.dmDisplayFrequency);
    return TRUE;
}

static BOOL CALLBACK windowProc(HWND w, LPARAM)
{
    wchar_t cls[128] = {}, title[128] = {};
    GetClassNameW(w, cls, 128);
    GetWindowTextW(w, title, 128);
    if (!IsWindowVisible(w) || (wcsstr(cls, L"Unreal") == nullptr && wcsstr(title, L"Hellblade") == nullptr)) return TRUE;
    RECT wr{}, cr{};
    GetWindowRect(w, &wr);
    GetClientRect(w, &cr);
    printf("window: class='%ls' title='%ls' window=(%ld,%ld)-(%ld,%ld) client=%ldx%ld dpi=%u style=0x%lx\n", cls, title, wr.left, wr.top,
           wr.right, wr.bottom, cr.right, cr.bottom, GetDpiForWindow(w), (unsigned long)GetWindowLongW(w, GWL_STYLE));
    return TRUE;
}

int wmain(int argc, wchar_t** argv)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    printf("display: SM_CXSCREEN=%d SM_CYSCREEN=%d virtual=%dx%d systemDPI=%u\n", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
           GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN), GetDpiForSystem());
    EnumDisplayMonitors(nullptr, nullptr, monitorProc, 0);
    EnumWindows(windowProc, 0);
    probeOS();
    if (argc > 1 && wcscmp(argv[1], L"display") == 0) return 0;
    if (argc > 1 && wcscmp(argv[1], L"modes") == 0) {
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        printf("display: win32 modes:");
        for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); i++)
            printf(" %lux%lu@%lu/%lubpp", (unsigned long)dm.dmPelsWidth, (unsigned long)dm.dmPelsHeight,
                   (unsigned long)dm.dmDisplayFrequency, (unsigned long)dm.dmBitsPerPel);
        printf("\n");
        probeDXGI();
        return 0;
    }

    // slInit has to run before any DXGI or D3D12 call
    const wchar_t* slDir = argc > 1 ? argv[1] : L".";
    wchar_t interposer[MAX_PATH];
    swprintf(interposer, MAX_PATH, L"%ls\\sl.interposer.dll", slDir);
    HMODULE sl = LoadLibraryW(interposer);
    printf("sl: LoadLibrary('%ls')=%p error=%lu\n", interposer, (void*)sl, sl ? 0 : GetLastError());
    std::vector<LUID> luids;
    if (sl) {
        auto slInitFn = (PFun_slInit*)GetProcAddress(sl, "slInit");
        auto slSupported = (PFun_slIsFeatureSupported*)GetProcAddress(sl, "slIsFeatureSupported");
        auto slReqs = (PFun_slGetFeatureRequirements*)GetProcAddress(sl, "slGetFeatureRequirements");

        sl::Feature features[] = {sl::kFeatureDLSS, sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
        const wchar_t* paths[] = {slDir};
        sl::Preferences pref{};
        pref.logLevel = sl::LogLevel::eVerbose;
        pref.logMessageCallback = slLog;
        pref.pathsToPlugins = paths;
        pref.numPathsToPlugins = 1;
        pref.pathToLogsAndData = L"C:\\hb2lab";
        pref.flags = sl::PreferenceFlags::eDisableCLStateTracking;
        pref.featuresToLoad = features;
        pref.numFeaturesToLoad = 4;
        pref.engine = sl::EngineType::eUnreal;
        pref.engineVersion = "5.3";
        pref.projectId = "b2c5e3b0-6f51-4a7c-9c0e-3a0b1f8e7d21";
        pref.renderAPI = sl::RenderAPI::eD3D12;
        sl::Result r = slInitFn(pref, sl::kSDKVersion);
        printf("sl: slInit=%s\n", resultName(r));

        luids = probeDXGI();
        sl::AdapterInfo ai{};
        if (!luids.empty()) { ai.deviceLUID = (uint8_t*)&luids[0]; ai.deviceLUIDSizeInBytes = sizeof(LUID); }
        const char* names[] = {"DLSS", "DLSS_G", "Reflex", "PCL"};
        for (int i = 0; i < 4; i++) {
            sl::Result s = slSupported(features[i], ai);
            sl::FeatureRequirements req{};
            sl::Result q = slReqs(features[i], req);
            printf("sl: %-6s supported=%s requirements=%s flags=0x%x os=%u.%u.%u/%u.%u.%u driver=%u.%u/%u.%u\n", names[i], resultName(s),
                   resultName(q), (unsigned)req.flags, req.osVersionDetected.major, req.osVersionDetected.minor,
                   req.osVersionDetected.build, req.osVersionRequired.major, req.osVersionRequired.minor, req.osVersionRequired.build,
                   req.driverVersionDetected.major, req.driverVersionDetected.minor, req.driverVersionRequired.major,
                   req.driverVersionRequired.minor);
        }
    } else {
        luids = probeDXGI();
    }
    probeHWS(luids);
    probeNvAPI();
    return 0;
}
