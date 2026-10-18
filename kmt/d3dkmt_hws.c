// The two D3DKMT entry points Wine 7.7's gdi32 lacks. Streamline reads hardware-accelerated
// GPU scheduling through them and refuses DLSS Frame Generation without it. D3DMetal's GPU
// scheduling is Metal's, so its adapter is reported with it enabled.
// D3DMetal's dxgi.dll calls D3DKMTEnumAdapters2 itself from its callback thread, so nothing
// here may call back into DXGI or D3DMetal.
#include <windows.h>

typedef LONG NTSTATUS;
#undef STATUS_INVALID_PARAMETER
#define STATUS_SUCCESS ((NTSTATUS)0x00000000)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000D)
#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xC0000023)
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xC00000BB)

typedef UINT D3DKMT_HANDLE;
typedef struct { D3DKMT_HANDLE hAdapter; LUID AdapterLuid; ULONG NumOfSources; BOOL bPrecisePresentRegionsPreferred; } D3DKMT_ADAPTERINFO;
typedef struct { ULONG NumAdapters; D3DKMT_ADAPTERINFO *pAdapters; } D3DKMT_ENUMADAPTERS2;
typedef struct { D3DKMT_HANDLE hAdapter; UINT Type; VOID *pPrivateDriverData; UINT PrivateDriverDataSize; } D3DKMT_QUERYADAPTERINFO;

#define KMTQAITYPE_WDDM_2_7_CAPS 70
/* HwSchSupported | HwSchEnabled | HwSchEnabledByDefault */
#define WDDM_2_7_CAPS_HWSCH 0x7u
#define ADAPTER_HANDLE 0x4b4d0001u
#define ADAPTER_LUID_LOW 0x0000d3d1u
#define ADAPTER_LUID_HIGH 0

#ifdef TRACE
#define trace(...) do { char b[256]; wsprintfA(b, __VA_ARGS__); OutputDebugStringA(b); } while (0)
#else
#define trace(...) do {} while (0)
#endif

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}

NTSTATUS WINAPI D3DKMTEnumAdapters2(D3DKMT_ENUMADAPTERS2 *desc)
{
    trace("d3dkmt_hws: EnumAdapters2(%p) tid=%04lx num=%lu adapters=%p\n", desc, GetCurrentThreadId(),
          desc ? desc->NumAdapters : 0, desc ? desc->pAdapters : NULL);
    if (!desc) return STATUS_INVALID_PARAMETER;
    if (!desc->pAdapters)
    {
        desc->NumAdapters = 1;
        return STATUS_SUCCESS;
    }
    if (desc->NumAdapters < 1)
    {
        desc->NumAdapters = 1;
        return STATUS_BUFFER_TOO_SMALL;
    }
    desc->pAdapters[0].hAdapter = ADAPTER_HANDLE;
    desc->pAdapters[0].AdapterLuid.LowPart = ADAPTER_LUID_LOW;
    desc->pAdapters[0].AdapterLuid.HighPart = ADAPTER_LUID_HIGH;
    desc->pAdapters[0].NumOfSources = 1;
    desc->pAdapters[0].bPrecisePresentRegionsPreferred = FALSE;
    desc->NumAdapters = 1;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI D3DKMTQueryAdapterInfo(D3DKMT_QUERYADAPTERINFO *info)
{
    trace("d3dkmt_hws: QueryAdapterInfo(%p) tid=%04lx handle=%x type=%u size=%u\n", info, GetCurrentThreadId(),
          info ? info->hAdapter : 0, info ? info->Type : 0, info ? info->PrivateDriverDataSize : 0);
#ifdef TRACE
    if (info && info->Type == 48 && info->pPrivateDriverData && info->PrivateDriverDataSize >= 544)
    {
        const UINT *q = info->pPrivateDriverData;
        trace("d3dkmt_hws:   QUERYREGISTRY type=%u flags=%x name='%ls' valueType=%u\n", q[0], q[1], (const WCHAR *)(q + 2),
              q[(8 + 520) / 4]);
    }
#endif
    if (!info || info->hAdapter != ADAPTER_HANDLE || !info->pPrivateDriverData) return STATUS_INVALID_PARAMETER;
    if (info->Type != KMTQAITYPE_WDDM_2_7_CAPS) return STATUS_NOT_SUPPORTED;
    if (info->PrivateDriverDataSize < sizeof(UINT)) return STATUS_BUFFER_TOO_SMALL;
    *(UINT *)info->pPrivateDriverData = WDDM_2_7_CAPS_HWSCH;
    return STATUS_SUCCESS;
}
