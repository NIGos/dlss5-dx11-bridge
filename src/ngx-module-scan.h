#pragma once
#include <windows.h>
#include <cstring>

enum class NgxScanResult { skipped, acquired, retry, unavailable };

struct NgxModuleScanner
{
    using Lock = LONG (NTAPI *)(ULONG, ULONG *, ULONG_PTR *);
    using Unlock = LONG (NTAPI *)(ULONG, ULONG_PTR);
    Lock lock;
    Unlock unlock;

    NgxModuleScanner()
    {
        const HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        lock = reinterpret_cast<Lock>(GetProcAddress(nt, "LdrLockLoaderLock"));
        unlock = reinterpret_cast<Unlock>(GetProcAddress(nt, "LdrUnlockLoaderLock"));
    }

    static bool Range(DWORD rva, size_t bytes, DWORD size)
    {
        return rva < size && bytes <= size - rva;
    }

    static bool HasExports(HMODULE module, bool vulkan)
    {
        __try {
            const auto base = reinterpret_cast<const BYTE *>(module);
            const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) return false;
            const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE ||
                nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC ||
                nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) return false;
            const DWORD size = nt->OptionalHeader.SizeOfImage;
            const auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            if (!dir.VirtualAddress || dir.Size < sizeof(IMAGE_EXPORT_DIRECTORY) ||
                !Range(dir.VirtualAddress, dir.Size, size)) return false;
            const auto exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY *>(base + dir.VirtualAddress);
            if (!Range(exports->AddressOfNames, size_t(exports->NumberOfNames) * sizeof(DWORD), size) ||
                !Range(exports->AddressOfNameOrdinals, size_t(exports->NumberOfNames) * sizeof(WORD), size) ||
                !Range(exports->AddressOfFunctions, size_t(exports->NumberOfFunctions) * sizeof(DWORD), size))
                return false;
            const auto names = reinterpret_cast<const DWORD *>(base + exports->AddressOfNames);
            const auto ordinals = reinterpret_cast<const WORD *>(base + exports->AddressOfNameOrdinals);
            const auto functions = reinterpret_cast<const DWORD *>(base + exports->AddressOfFunctions);
            const char *wanted[] = {
                "NVSDK_NGX_D3D11_CreateFeature", "NVSDK_NGX_D3D11_EvaluateFeature",
                "NVSDK_NGX_D3D11_EvaluateFeature_C", "NVSDK_NGX_VULKAN_CreateFeature",
                "NVSDK_NGX_VULKAN_CreateFeature1", "NVSDK_NGX_VULKAN_EvaluateFeature",
                "NVSDK_NGX_VULKAN_EvaluateFeature_C"
            };
            unsigned found = 0;
            for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
                if (!Range(names[i], 10, size) || std::memcmp(base + names[i], "NVSDK_NGX_", 10) != 0 ||
                    ordinals[i] >= exports->NumberOfFunctions ||
                    functions[ordinals[i]] == 0 || functions[ordinals[i]] >= size) continue;
                for (unsigned j = 0; j < (vulkan ? 7u : 3u); ++j) {
                    const size_t length = std::strlen(wanted[j]) + 1;
                    if (Range(names[i], length, size) &&
                        std::memcmp(base + names[i], wanted[j], length) == 0) found |= 1u << j;
                }
                if (((found & 1u) && (found & 6u)) || ((found & 24u) && (found & 96u))) return true;
            }
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
                  EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
            return false;
        }
        return false;
    }

    NgxScanResult Acquire(HMODULE module, bool vulkan, HMODULE &held) const
    {
        held = nullptr;
        if (!lock || !unlock) return NgxScanResult::unavailable;
        ULONG disposition = 0;
        ULONG_PTR cookie = 0;
        if (lock(2, &disposition, &cookie) < 0) return NgxScanResult::unavailable;
        if (disposition != 1) return NgxScanResult::retry;
        HMODULE current = nullptr;
        NgxScanResult result = NgxScanResult::skipped;
        // No reference for non-NGX addons; do not resolve forwarders or run DLL code under this lock.
        if (module && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(module), &current) && current == module &&
            HasExports(current, vulkan) &&
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCWSTR>(module), &held)) result = NgxScanResult::acquired;
        unlock(0, cookie);
        return result;
    }
};
