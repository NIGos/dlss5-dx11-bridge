// Offline probe for the two pieces of the GPU relay that no other host covers:
//
//   1. The extension injection. The bridge appends VK_KHR_external_semaphore_win32
//      to the create info of every device built through ReShade's layer when
//      [Adapter] Import=1 is set. This host creates its device WITHOUT that
//      extension and, because a game creates its device seconds after the add-on
//      loads, waits before creating it -- which is the arrangement the injection
//      needs and the one the roundtrip host's immediate device creation cannot
//      give. The bridge log is then read for the record line; the extension calls
//      below are only made once that line is there.
//
//   2. The D3D12 <-> Vulkan shared fence, in both directions, on the device the
//      injection produced: a Vulkan timeline semaphore exported as a D3D12 fence
//      that a CPU signal advances, and a D3D12 shared fence imported as a Vulkan
//      timeline semaphore that a CPU signal releases. These are the relay's two
//      GPU hops; nothing else off-line exercises them.
//
// Run from a directory holding ReShade.ini (AddonPath=.), dlss5-bridge.addon64 and
// a vk-present-adapter.ini with Enabled=1, Source=fg-input, Pipeline=3, Import=1,
// with RESHADE_BASE_PATH_OVERRIDE set to it and VK_LOADER_LAYERS_DISABLE=~implicit~,
// the same environment the roundtrip evidence uses. Exit 0 means every check
// passed; 3 means the injection did not arrive in time, which says the hook was
// not in place before this device was created; 4/5/6 are the interop checks.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <d3d12.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static void Check(VkResult result, const char *name)
{
    if (result != VK_SUCCESS) { std::printf("FAIL %s result=%d\n", name, static_cast<int>(result)); std::exit(2); }
}

static std::string ReadFile(const char *path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

static bool WaitCounter(VkDevice device, VkSemaphore semaphore, uint64_t want, DWORD timeout_ms)
{
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;)
    {
        uint64_t value = 0;
        if (vkGetSemaphoreCounterValue(device, semaphore, &value) == VK_SUCCESS && value == want) return true;
        if (GetTickCount64() >= deadline) return false;
        Sleep(5);
    }
}

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    const int delay_ms = argc > 1 ? std::atoi(argv[1]) : 3000;

    const char *layer = "VK_LAYER_reshade";
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Vulkan injection probe"; app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app; ici.enabledLayerCount = 1; ici.ppEnabledLayerNames = &layer;
    VkInstance instance{};
    Check(vkCreateInstance(&ici, nullptr, &instance), "instance");
    std::printf("instance created; the add-on is loaded from here on\n");

    std::printf("waiting %d ms before device creation, so the hook worker has installed the create-device hook\n", delay_ms);
    Sleep(static_cast<DWORD>(delay_ms));

    uint32_t count = 0;
    Check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate");
    std::vector<VkPhysicalDevice> devices(count);
    Check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "devices");
    VkPhysicalDevice physical = nullptr;
    for (VkPhysicalDevice candidate : devices)
    {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (properties.vendorID == 0x10de) { physical = candidate; std::printf("GPU=%s\n", properties.deviceName); break; }
    }
    if (physical == nullptr) { std::puts("FAIL no NVIDIA physical device"); return 2; }

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
    uint32_t family = family_count;
    for (uint32_t i = 0; i < family_count; ++i)
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) { family = i; break; }
    if (family == family_count) { std::puts("FAIL no graphics queue family"); return 2; }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &priority;
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    timeline.timelineSemaphore = VK_TRUE;
    // Deliberately no VK_KHR_external_semaphore_win32: the bridge is supposed to add it.
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &timeline; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkDevice device{};
    Check(vkCreateDevice(physical, &dci, nullptr, &device), "device");
    std::printf("device created at %p without the extension in the caller's list\n", device);

    Sleep(250);
    const std::string log = ReadFile("dlss5-bridge.log");
    if (log.empty()) { std::puts("FAIL dlss5-bridge.log is not in the working directory"); return 2; }
    size_t at = 0; int injection_lines = 0; bool recorded = false;
    for (;;)
    {
        at = log.find("[fg-relay] Import=1", at);
        if (at == std::string::npos) break;
        const size_t end = log.find('\n', at);
        const std::string line = log.substr(at, end == std::string::npos ? std::string::npos : end - at);
        std::printf("bridge: %s\n", line.c_str());
        ++injection_lines;
        if (line.find("carries VK_KHR_external_semaphore_win32") != std::string::npos) recorded = true;
        at = end == std::string::npos ? log.size() : end + 1;
    }
    if (!recorded)
    {
        std::printf("FAIL the create-device hook did not record this device (%d injection lines in the log). "
                    "The extension calls below are skipped rather than made on a device that may not carry it.\n",
                    injection_lines);
        return 3;
    }
    std::puts("INJECTION: the hook appended the name and the device was created with it");

    ID3D12Device *d3d12 = nullptr;
    const HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                                        reinterpret_cast<void **>(&d3d12));
    if (FAILED(hr) || d3d12 == nullptr) { std::printf("FAIL D3D12CreateDevice hr=0x%08lx\n", hr); return 2; }

    const auto get_handle = reinterpret_cast<PFN_vkGetSemaphoreWin32HandleKHR>(
        vkGetDeviceProcAddr(device, "vkGetSemaphoreWin32HandleKHR"));
    const auto import_handle = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(
        vkGetDeviceProcAddr(device, "vkImportSemaphoreWin32HandleKHR"));
    if (get_handle == nullptr || import_handle == nullptr)
    { std::puts("FAIL the KHR entry points are not resolvable on this device"); return 2; }

    // Direction one: Vulkan timeline semaphore -> D3D12 fence. A CPU signal on the
    // D3D12 side must advance the semaphore the relay's copy wait travels on.
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE; type.initialValue = 0;
    VkExportSemaphoreCreateInfo export_info{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
    export_info.pNext = &type;
    export_info.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sci.pNext = &export_info;
    VkSemaphore exported{};
    Check(vkCreateSemaphore(device, &sci, nullptr, &exported), "exported semaphore");
    VkSemaphoreGetWin32HandleInfoKHR get{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
    get.semaphore = exported; get.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
    HANDLE exported_handle = nullptr;
    Check(get_handle(device, &get, &exported_handle), "vkGetSemaphoreWin32HandleKHR");
    ID3D12Fence *fence_from_vulkan = nullptr;
    const HRESULT open = d3d12->OpenSharedHandle(exported_handle, __uuidof(ID3D12Fence),
                                                reinterpret_cast<void **>(&fence_from_vulkan));
    if (FAILED(open) || fence_from_vulkan == nullptr)
    { std::printf("FAIL OpenSharedHandle on the exported semaphore hr=0x%08lx\n", open); return 4; }
    std::printf("export: D3D12 fence completed=%llu before the signal\n", fence_from_vulkan->GetCompletedValue());
    if (FAILED(fence_from_vulkan->Signal(7)))
    { std::puts("FAIL ID3D12Fence::Signal on the exported fence"); return 4; }
    if (!WaitCounter(device, exported, 7, 2000))
    { std::puts("FAIL the Vulkan semaphore did not reach 7 after the D3D12 signal"); return 4; }
    std::puts("PASS export: D3D12 Signal(7) advanced the Vulkan timeline semaphore to 7");

    // Direction two: D3D12 shared fence -> Vulkan timeline semaphore. This is the
    // release the park waits on.
    ID3D12Fence *shared = nullptr;
    if (FAILED(d3d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence),
                                  reinterpret_cast<void **>(&shared))) || shared == nullptr)
    { std::puts("FAIL CreateFence with D3D12_FENCE_FLAG_SHARED"); return 5; }
    HANDLE shared_handle = nullptr;
    if (FAILED(d3d12->CreateSharedHandle(shared, nullptr, GENERIC_ALL, nullptr, &shared_handle)) ||
        shared_handle == nullptr)
    { std::puts("FAIL CreateSharedHandle on the shared fence"); return 5; }
    VkSemaphoreTypeCreateInfo imported_type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    imported_type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE; imported_type.initialValue = 0;
    VkSemaphoreCreateInfo imported_sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    imported_sci.pNext = &imported_type;
    VkSemaphore imported{};
    Check(vkCreateSemaphore(device, &imported_sci, nullptr, &imported), "imported semaphore");
    VkImportSemaphoreWin32HandleInfoKHR import_info{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
    import_info.semaphore = imported; import_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
    import_info.handle = shared_handle;
    Check(import_handle(device, &import_info), "vkImportSemaphoreWin32HandleKHR");
    if (FAILED(shared->Signal(5)))
    { std::puts("FAIL ID3D12Fence::Signal on the shared fence"); return 6; }
    const uint64_t want = 5;
    VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wait.semaphoreCount = 1; wait.pSemaphores = &imported; wait.pValues = &want;
    if (vkWaitSemaphores(device, &wait, 2000ull * 1000000ull) != VK_SUCCESS)
    { std::puts("FAIL vkWaitSemaphores did not observe the D3D12 signal"); return 6; }
    std::puts("PASS import: the D3D12 shared fence released the Vulkan timeline semaphore");

    vkDestroySemaphore(device, imported, nullptr);
    vkDestroySemaphore(device, exported, nullptr);
    if (shared) shared->Release();
    if (fence_from_vulkan) fence_from_vulkan->Release();
    if (d3d12) d3d12->Release();
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    std::puts("PASS injection and both shared-fence directions verified offline; "
              "the relay's own arm/release chain still needs the game.");
    return 0;
}
