// usage: cvarpeek.exe <process.exe> <cvar> [<cvar> ...]
//        cvarpeek.exe <process.exe> --set <cvar> <float>
// Finds Unreal console variables in a running game through the console manager's
// TMap<FString, IConsoleObject*> (key string -> pair -> object) and dumps the objects.
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

struct Region { uintptr_t base; size_t size; };

static HANDLE proc;
static std::vector<Region> regions;

static bool readMem(uintptr_t addr, void* out, size_t n)
{
    SIZE_T got = 0;
    return ReadProcessMemory(proc, (LPCVOID)addr, out, n, &got) && got == n;
}

static void collectRegions()
{
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t a = 0x10000; VirtualQueryEx(proc, (LPCVOID)a, &mbi, sizeof(mbi)) == sizeof(mbi);
         a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize) {
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) &&
            !(mbi.Protect & PAGE_GUARD))
            regions.push_back({(uintptr_t)mbi.BaseAddress, mbi.RegionSize});
        if ((uintptr_t)mbi.BaseAddress + mbi.RegionSize < a) break;
    }
}

template <class F> static void scan(F&& visit)
{
    std::vector<uint8_t> buf;
    const size_t chunk = 16 << 20;
    for (const Region& r : regions) {
        for (size_t off = 0; off < r.size; off += chunk) {
            size_t n = r.size - off < chunk ? r.size - off : chunk;
            buf.resize(n);
            if (!readMem(r.base + off, buf.data(), n)) continue;
            visit(r.base + off, buf.data(), n);
        }
    }
}

static DWORD findProcess(const wchar_t* name)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{sizeof(pe)};
    DWORD pid = 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, name) == 0) pid = pe.th32ProcessID;
    CloseHandle(snap);
    return pid;
}

struct Found { std::wstring name; uintptr_t object; };

static std::vector<Found> locate(const std::vector<std::wstring>& names)
{
    // 1: every occurrence of each name as a whole, NUL-terminated UTF-16 string
    std::vector<std::vector<uintptr_t>> strings(names.size());
    scan([&](uintptr_t base, const uint8_t* p, size_t n) {
        for (size_t k = 0; k < names.size(); k++) {
            const size_t bytes = (names[k].size() + 1) * 2;
            const uint8_t* key = (const uint8_t*)names[k].c_str();
            for (size_t i = 0; i + bytes <= n; i += 2) {
                if (p[i] != key[0] || memcmp(p + i, key, bytes) != 0) continue;
                if (i >= 2 && (p[i - 2] || p[i - 1]) && (iswalnum(*(const wchar_t*)(p + i - 2)) || p[i - 2] == '.')) continue;
                strings[k].push_back(base + i);
            }
        }
    });
    // 2: FString {Data, Num, Max} pointing at one of them, followed by the object pointer
    std::vector<Found> out;
    scan([&](uintptr_t base, const uint8_t* p, size_t n) {
        for (size_t i = 0; i + 24 <= n; i += 8) {
            uint64_t v = *(const uint64_t*)(p + i);
            for (size_t k = 0; k < names.size(); k++) {
                for (uintptr_t s : strings[k]) {
                    if (v != s) continue;
                    int32_t num = *(const int32_t*)(p + i + 8), max = *(const int32_t*)(p + i + 12);
                    if (num != (int32_t)names[k].size() + 1 || max < num) continue;
                    out.push_back({names[k], (uintptr_t) * (const uint64_t*)(p + i + 16)});
                }
            }
        }
    });
    return out;
}

static void dump(const Found& f)
{
    uint8_t obj[128];
    if (!readMem(f.object, obj, sizeof(obj))) { wprintf(L"%ls: object %p unreadable\n", f.name.c_str(), (void*)f.object); return; }
    wprintf(L"%ls @ %p vtable=%p\n", f.name.c_str(), (void*)f.object, (void*)*(uint64_t*)obj);
    for (int i = 8; i < (int)sizeof(obj); i += 4) {
        uint32_t u = *(uint32_t*)(obj + i);
        float fl;
        memcpy(&fl, &u, 4);
        wprintf(L"  +%02x %08x  int=%-11d float=%g\n", i, u, (int)u, fl);
    }
}

// One line per cvar: object, SetBy priority, the two shadowed values of a TAutoConsoleVariable
// (+0x48/+0x4c) and, for an FAutoConsoleVariableRef, the referenced global (a pointer into the image).
static void brief(const Found& f)
{
    uint8_t obj[0x60];
    if (!readMem(f.object, obj, sizeof(obj))) { wprintf(L"%ls object=%p unreadable\n", f.name.c_str(), (void*)f.object); return; }
    uint32_t flags = *(uint32_t*)(obj + 0x18);
    wprintf(L"CVAR %ls object=%p setby=0x%02x", f.name.c_str(), (void*)f.object, flags >> 24);
    bool ref = false;
    for (int off = 0x20; off + 8 <= (int)sizeof(obj); off += 8) {
        uint64_t p = *(uint64_t*)(obj + off);
        if (p < 0x140000000ull || p >= 0x150000000ull || p == *(uint64_t*)obj) continue;
        uint32_t v;
        if (!readMem(p, &v, 4)) continue;
        float fl; memcpy(&fl, &v, 4);
        wprintf(L" ref+%02x=%p(int=%d float=%g)", off, (void*)p, (int)v, fl);
        ref = true;
    }
    float a, b; memcpy(&a, obj + 0x48, 4); memcpy(&b, obj + 0x4c, 4);
    wprintf(L" shadow48=%d/%g shadow4c=%d/%g%ls\n", *(int*)(obj + 0x48), a, *(int*)(obj + 0x4c), b, ref ? L"" : L" (no ref)");
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) { wprintf(L"usage: cvarpeek.exe <process.exe> [--brief] <cvar>... | --set <cvar> <offset hex> <float> | --write <hex address> i32|f32 <value>...\n"); return 1; }
    DWORD pid = findProcess(argv[1]);
    if (!pid) { wprintf(L"process %ls not found\n", argv[1]); return 1; }
    proc = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!proc) { wprintf(L"OpenProcess(%lu) failed: %lu\n", pid, GetLastError()); return 1; }
    if (wcscmp(argv[2], L"--write") == 0) {
        // Wine on macOS writes another process's memory by reading the whole page, patching it and
        // writing the page back, so a game thread writing to the same page in between loses its write.
        // Every thread of the game stays stopped until all values are written.
        std::vector<HANDLE> stopped;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te{sizeof(te)};
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid) continue;
            HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
            if (!t) continue;
            if (SuspendThread(t) == (DWORD)-1) { CloseHandle(t); continue; }
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            GetThreadContext(t, &ctx);
            stopped.push_back(t);
        }
        CloseHandle(snap);
        wprintf(L"stopped %zu threads\n", stopped.size());
        if (stopped.empty()) { wprintf(L"WRITE refused: could not stop the game's threads\n"); return 1; }
        for (int i = 3; i + 2 < argc; i += 3) {
            uintptr_t addr = (uintptr_t)wcstoull(argv[i], nullptr, 16);
            uint32_t raw;
            if (wcscmp(argv[i + 1], L"f32") == 0) { float f = (float)_wtof(argv[i + 2]); memcpy(&raw, &f, 4); }
            else raw = (uint32_t)_wtoi(argv[i + 2]);
            uint32_t before = 0, after = 0;
            readMem(addr, &before, 4);
            SIZE_T put = 0;
            BOOL ok = WriteProcessMemory(proc, (LPVOID)addr, &raw, 4, &put);
            readMem(addr, &after, 4);
            wprintf(L"WRITE %p %08x -> %08x %ls\n", (void*)addr, before, after, ok ? L"ok" : L"failed");
        }
        for (HANDLE t : stopped) { ResumeThread(t); CloseHandle(t); }
        return 0;
    }
    collectRegions();
    size_t total = 0;
    for (auto& r : regions) total += r.size;
    wprintf(L"pid %lu: %zu writable regions, %zu MB\n", pid, regions.size(), total >> 20);

    if (wcscmp(argv[2], L"--set") == 0 && argc >= 6) {
        std::vector<std::wstring> names{argv[3]};
        uint32_t offset = wcstoul(argv[4], nullptr, 16);
        float value = (float)_wtof(argv[5]);
        for (const Found& f : locate(names)) {
            SIZE_T put = 0;
            BOOL ok = WriteProcessMemory(proc, (LPVOID)(f.object + offset), &value, 4, &put);
            wprintf(L"%ls @ %p +%x = %g: %ls\n", f.name.c_str(), (void*)f.object, offset, value, ok ? L"written" : L"failed");
            dump(f);
        }
        return 0;
    }
    bool compact = wcscmp(argv[2], L"--brief") == 0;
    std::vector<std::wstring> names(argv + (compact ? 3 : 2), argv + argc);
    std::vector<Found> found = locate(names);
    for (auto& n : names) {
        bool any = false;
        for (auto& f : found) if (f.name == n) { compact ? brief(f) : dump(f); any = true; }
        if (!any) wprintf(L"%ls: not found\n", n.c_str());
    }
    return 0;
}
