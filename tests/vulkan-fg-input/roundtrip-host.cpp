#define VK_USE_PLATFORM_WIN32_KHR
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
#include <string>

static void Check(VkResult r, const char* step) {
    if (r != VK_SUCCESS) { std::printf("FAIL %s VkResult=%d\n", step, r); std::exit(2); }
}
static LRESULT CALLBACK WindowProc(HWND w, UINT m, WPARAM a, LPARAM b) {
    return DefWindowProcW(w, m, a, b);
}
static void Barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to,
                    VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from; b.newLayout = to; b.srcAccessMask = src; b.dstAccessMask = dst;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
}
static void Request(unsigned token, unsigned frames, unsigned mode) {
    FILE* f = nullptr;
    if (fopen_s(&f, "vk-present-adapter.request", "wb") || !f) std::exit(3);
    const int written = std::fprintf(f, "%u %u %u\n", token, frames, mode);
    const int closed = std::fclose(f);
    if (written < 0 || closed != 0) std::exit(3);
    std::printf("request_sent token=%u frames=%u mode=%u\n", token, frames, mode);
}
static unsigned long long Field(const std::string& line, const char* key) {
    const size_t at = line.find(key);
    if (at == std::string::npos) return UINT64_MAX;
    return std::strtoull(line.c_str() + at + std::string(key).size(), nullptr, 0);
}
static bool Completed(const std::string& log, unsigned token) {
    size_t at = 0;
    while ((at = log.find("[present-adapter] request complete:", at)) != std::string::npos) {
        const size_t end = log.find('\n', at);
        if (Field(log.substr(at, end - at), " token=") == token) return true;
        ++at;
    }
    return false;
}
struct Presented {
    int frame = -1;
    unsigned token = 0, bridge_frame = 0;
};
struct PixelStats {
    unsigned checked = 0, mismatches = 0, maximum[4] = {};
};
static uint32_t Fixture(unsigned x, unsigned y, unsigned frame, VkExtent2D size, VkFormat format) {
    static const unsigned palette[16][3] = {
        {0,0,0}, {8,8,8}, {32,32,32}, {64,64,64},
        {96,96,96}, {128,128,128}, {160,160,160}, {192,192,192},
        {224,224,224}, {255,255,255}, {32,96,192}, {192,64,16},
        {96,192,48}, {224,32,128}, {8,160,224}, {200,128,48}
    };
    unsigned cell = (x / 64 + 4 * (y / 64)) % 16;
    if ((x + frame * 3 + y) % 96 < 12) cell = (cell + frame + 5) % 16;
    if (x >= size.width - 48 && x < size.width - 32 &&
        y >= size.height - 48 && y < size.height - 32) {
        const unsigned rx = x - (size.width - 48), ry = y - (size.height - 48);
        cell = (rx / 4 + 4 * (ry / 4) + (ry >= 12 ? frame : 0)) % 16;
    }
    unsigned r = palette[cell][0], g = palette[cell][1], b = palette[cell][2];
    if (format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
        r = (r * 1023 + 127) / 255; g = (g * 1023 + 127) / 255; b = (b * 1023 + 127) / 255;
        return r | (g << 10) | (b << 20) | (3u << 30);
    }
    if (format == VK_FORMAT_B8G8R8A8_UNORM) std::swap(r, b);
    return r | (g << 8) | (b << 16) | (255u << 24);
}
static std::string BridgeLog() {
    FILE* f=nullptr;
    if(fopen_s(&f,"dlss5-bridge.log","rb") || !f) return {};
    std::string result; char buffer[4096]; size_t n=0;
    while((n=fread(buffer,1,sizeof(buffer),f))!=0 && result.size()<1024*1024) result.append(buffer,n);
    fclose(f);return result;
}
int main(int argc, char** argv) {
    bool hdr=false, full_resolution=false, load_rr=false;
    for(int i=1;i<argc;++i) {
        if(std::strcmp(argv[i],"hdr")==0) hdr=true;
        else if(std::strcmp(argv[i],"full")==0) full_resolution=true;
        else if(std::strcmp(argv[i],"load-rr")==0) load_rr=true;
        else return 64;
    }
    if(load_rr) {
        wchar_t rr_path[MAX_PATH]={};
        if(GetFullPathNameW(L"nvngx_dlssd.dll",MAX_PATH,rr_path,nullptr)==0 ||
           !LoadLibraryExW(rr_path,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) {
            std::printf("FAIL load-rr error=%lu\n",GetLastError());return 13;
        }
        std::printf("RR_MODULE_LOADED=nvngx_dlssd.dll\n");
    }
    const int width=full_resolution ? 5120 : 800, height=full_resolution ? 2160 : 600;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    WNDCLASSW wc{}; wc.lpfnWndProc = WindowProc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"VulkanPresentAdapterHost";
    if (!RegisterClassW(&wc)) return 4;
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName,
        L"Independent Vulkan colour transport test", WS_POPUP, -20000, -20000, width, height,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) return 5;
    const char* layer = "VK_LAYER_reshade";
    const char* ie[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
                       VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME};
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO}; ai.pApplicationName = "PresentAdapterTest";
    ai.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &ai;
    ici.enabledLayerCount = 1; ici.ppEnabledLayerNames = &layer;
    ici.enabledExtensionCount = hdr ? 3u : 2u; ici.ppEnabledExtensionNames = ie;
    VkInstance instance{}; Check(vkCreateInstance(&ici,nullptr,&instance),"instance");
    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = wc.hInstance; sci.hwnd = window;
    VkSurfaceKHR surface{}; Check(vkCreateWin32SurfaceKHR(instance,&sci,nullptr,&surface),"surface");
    uint32_t count = 0; Check(vkEnumeratePhysicalDevices(instance,&count,nullptr),"enumerate");
    std::vector<VkPhysicalDevice> devices(count); Check(vkEnumeratePhysicalDevices(instance,&count,devices.data()),"devices");
    VkPhysicalDevice physical{};
    for (auto d : devices) { VkPhysicalDeviceProperties p{}; vkGetPhysicalDeviceProperties(d,&p);
        if (p.vendorID == 0x10de) { physical = d; std::printf("GPU=%s\n",p.deviceName); break; } }
    if (!physical) return 6;
    uint32_t qcount = 0; vkGetPhysicalDeviceQueueFamilyProperties(physical,&qcount,nullptr);
    std::vector<VkQueueFamilyProperties> queues(qcount); vkGetPhysicalDeviceQueueFamilyProperties(physical,&qcount,queues.data());
    uint32_t family = qcount;
    for (uint32_t i=0;i<qcount;++i) { VkBool32 supports=0; Check(vkGetPhysicalDeviceSurfaceSupportKHR(physical,i,surface,&supports),"surface support");
        if (supports && (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { family=i;break; } }
    if (family==qcount) return 7;
    const float priority = 1;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex=family;qci.queueCount=1;qci.pQueuePriorities=&priority;
    const char* de[]={VK_KHR_SWAPCHAIN_EXTENSION_NAME,VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
                      VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    timeline.timelineSemaphore=VK_TRUE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dci.pNext=&timeline;
    dci.queueCreateInfoCount=1;dci.pQueueCreateInfos=&qci;dci.enabledExtensionCount=4;dci.ppEnabledExtensionNames=de;
    VkDevice device{};Check(vkCreateDevice(physical,&dci,nullptr,&device),"device");
    VkQueue queue{};vkGetDeviceQueue(device,family,0,&queue);
    VkSurfaceCapabilitiesKHR caps{};Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&caps),"caps");
    uint32_t fc=0;Check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&fc,nullptr),"formats");
    std::vector<VkSurfaceFormatKHR> formats(fc);Check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&fc,formats.data()),"formats data");
    VkSurfaceFormatKHR chosen{};
    if(hdr) {
        for(auto f:formats) if(f.format==VK_FORMAT_A2B10G10R10_UNORM_PACK32 &&
                              f.colorSpace==VK_COLOR_SPACE_HDR10_ST2084_EXT) {chosen=f;break;}
    } else {
        for(auto f:formats) if(f.format==VK_FORMAT_R8G8B8A8_UNORM) {chosen=f;break;}
        if(chosen.format==VK_FORMAT_UNDEFINED) for(auto f:formats) if(f.format==VK_FORMAT_B8G8R8A8_UNORM) {chosen=f;break;}
    }
    if(chosen.format==VK_FORMAT_UNDEFINED) {std::printf("SKIP requested HDR=%d surface unavailable\n",hdr);return 8;}
    VkExtent2D size = caps.currentExtent;
    if(size.width==UINT32_MAX) size={static_cast<uint32_t>(width),static_cast<uint32_t>(height)};
    if(size.width!=static_cast<uint32_t>(width) || size.height!=static_cast<uint32_t>(height)) {
        std::printf("FAIL requested_extent=%dx%d actual_extent=%ux%u\n",width,height,size.width,size.height);
        return 12;
    }
    const VkImageUsageFlags usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if((caps.supportedUsageFlags & usage)!=usage) return 9;
    VkSwapchainCreateInfoKHR swapci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swapci.surface=surface;swapci.minImageCount=std::max(caps.minImageCount,2u);
    if(caps.maxImageCount && swapci.minImageCount>caps.maxImageCount) swapci.minImageCount=caps.maxImageCount;
    swapci.imageFormat=chosen.format;swapci.imageColorSpace=chosen.colorSpace;swapci.imageExtent=size;
    swapci.imageArrayLayers=1;swapci.imageUsage=usage;swapci.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;
    swapci.preTransform=caps.currentTransform;swapci.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapci.presentMode=VK_PRESENT_MODE_FIFO_KHR;swapci.clipped=VK_TRUE;
    VkSwapchainKHR swap{};Check(vkCreateSwapchainKHR(device,&swapci,nullptr,&swap),"swapchain");
    uint32_t ic=0;Check(vkGetSwapchainImagesKHR(device,swap,&ic,nullptr),"images");
    std::vector<VkImage> images(ic);Check(vkGetSwapchainImagesKHR(device,swap,&ic,images.data()),"images data");
    std::printf("surface=%ux%u format=%u images=%u\n",size.width,size.height,chosen.format,ic);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bci.size=16*16*4;bci.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buffer{};Check(vkCreateBuffer(device,&bci,nullptr,&buffer),"readback buffer");
    VkMemoryRequirements req{};vkGetBufferMemoryRequirements(device,buffer,&req);
    VkPhysicalDeviceMemoryProperties mp{};vkGetPhysicalDeviceMemoryProperties(physical,&mp);
    uint32_t mt=mp.memoryTypeCount;
    for(uint32_t i=0;i<mp.memoryTypeCount;++i) if((req.memoryTypeBits&(1u<<i)) &&
        (mp.memoryTypes[i].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==
         (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {mt=i;break;}
    if(mt==mp.memoryTypeCount) return 10;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};mai.allocationSize=req.size;mai.memoryTypeIndex=mt;
    VkDeviceMemory memory{};Check(vkAllocateMemory(device,&mai,nullptr,&memory),"readback memory");
    Check(vkBindBufferMemory(device,buffer,memory,0),"bind readback");
    unsigned char* mapped=nullptr;Check(vkMapMemory(device,memory,0,VK_WHOLE_SIZE,0,reinterpret_cast<void**>(&mapped)),"map");
    if(size.width < 48 || size.height < 48) return 12;
    bci.size=static_cast<VkDeviceSize>(size.width)*size.height*4;bci.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer upload{};Check(vkCreateBuffer(device,&bci,nullptr,&upload),"upload buffer");
    vkGetBufferMemoryRequirements(device,upload,&req);
    mt=mp.memoryTypeCount;
    for(uint32_t i=0;i<mp.memoryTypeCount;++i) if((req.memoryTypeBits&(1u<<i)) &&
        (mp.memoryTypes[i].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==
         (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {mt=i;break;}
    if(mt==mp.memoryTypeCount) return 10;
    mai.allocationSize=req.size;mai.memoryTypeIndex=mt;
    VkDeviceMemory upload_memory{};Check(vkAllocateMemory(device,&mai,nullptr,&upload_memory),"upload memory");
    Check(vkBindBufferMemory(device,upload,upload_memory,0),"bind upload");
    uint32_t* upload_pixels=nullptr;
    Check(vkMapMemory(device,upload_memory,0,VK_WHOLE_SIZE,0,reinterpret_cast<void**>(&upload_pixels)),"map upload");
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pci.queueFamilyIndex=family;pci.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool{};Check(vkCreateCommandPool(device,&pci,nullptr,&pool),"pool");
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cai.commandPool=pool;cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cai.commandBufferCount=1;
    VkCommandBuffer command{};Check(vkAllocateCommandBuffers(device,&cai,&command),"command");
    VkSemaphoreCreateInfo se{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};VkSemaphore acquired{}, rendered{};
    Check(vkCreateSemaphore(device,&se,nullptr,&acquired),"acquired semaphore");Check(vkCreateSemaphore(device,&se,nullptr,&rendered),"render semaphore");
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VkFence fence{};Check(vkCreateFence(device,&fci,nullptr,&fence),"fence");
    std::vector<Presented> prior(ic);PixelStats stats[2];
    unsigned sent=0, pause_presents=0;
    bool accepted[4]={}, finished[4]={}, completed=false;
    ULONGLONG pause_start=0, pause_ms=0;
    const ULONGLONG start=GetTickCount64();
    std::printf("FIXTURE version=gray-colour-stripes-v1 hdr=%d roi=16x16 offset=-48,-48\n",hdr);
    for(unsigned frame=0;frame<600 && GetTickCount64()-start<45000;++frame) {
        MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
        const std::string log=BridgeLog();
        for(unsigned token=1;token<=4;++token) {
            if(!accepted[token-1] && log.find("[present-adapter] request "+std::to_string(token)+" accepted:")!=std::string::npos) {
                accepted[token-1]=true;std::printf("bridge_accepted token=%u\n",token);
            }
            if(!finished[token-1] && Completed(log,token)) {
                finished[token-1]=true;std::printf("bridge_completed token=%u\n",token);
            }
        }
        if(sent==0 && log.find("enabled, idle")!=std::string::npos) {Request(1,12,0);sent=1;}
        if(sent==1 && finished[0]) {Request(2,12,2);sent=2;}
        if(sent==2 && finished[1]) {Request(3,16,1);sent=3;}
        if(sent==3 && finished[2]) {
            if(!pause_start) pause_start=GetTickCount64();
            if(GetTickCount64()-pause_start>=750 && pause_presents>0) {
                pause_ms=GetTickCount64()-pause_start;
                Request(4,16,1);sent=4;
            }
        }
        if(sent==4 && finished[3]) {completed=true;break;}
        if(frame%30==0) std::printf("host_frame=%u last_sent_token=%u\n",frame,sent);
        Check(vkQueueWaitIdle(queue),"queue idle");
        uint32_t index=0;Check(vkAcquireNextImageKHR(device,swap,3000000000ull,acquired,VK_NULL_HANDLE,&index),"acquire");
        for(unsigned y=0;y<size.height;++y) for(unsigned x=0;x<size.width;++x)
            upload_pixels[static_cast<size_t>(y)*size.width+x]=Fixture(x,y,frame,size,chosen.format);
        Check(vkResetCommandBuffer(command,0),"reset command");VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        Check(vkBeginCommandBuffer(command,&cbi),"begin");
        if(prior[index].frame>=0) {
            Barrier(command,images[index],VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,0,VK_ACCESS_TRANSFER_READ_BIT);
            VkBufferImageCopy region{};region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
            region.imageOffset={static_cast<int32_t>(size.width)-48,static_cast<int32_t>(size.height)-48,0};region.imageExtent={16,16,1};
            vkCmdCopyImageToBuffer(command,images[index],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&region);
            VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};bb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;bb.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
            bb.srcQueueFamilyIndex=bb.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;bb.buffer=buffer;bb.size=VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&bb,0,nullptr);
            Barrier(command,images[index],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
        } else Barrier(command,images[index],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy upload_region{};upload_region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        upload_region.imageExtent={size.width,size.height,1};
        vkCmdCopyBufferToImage(command,upload,images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&upload_region);
        Barrier(command,images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_ACCESS_TRANSFER_WRITE_BIT,0);
        Check(vkEndCommandBuffer(command),"end");Check(vkResetFences(device,1,&fence),"reset fence");
        VkPipelineStageFlags stage=VK_PIPELINE_STAGE_TRANSFER_BIT;VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&acquired;submit.pWaitDstStageMask=&stage;
        submit.commandBufferCount=1;submit.pCommandBuffers=&command;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&rendered;
        Check(vkQueueSubmit(queue,1,&submit,fence),"submit");Check(vkWaitForFences(device,1,&fence,VK_TRUE,3000000000ull),"fence wait");
        const Presented old=prior[index];
        if(old.token==1 || old.token==2) {
            PixelStats& s=stats[old.token-1];
            const unsigned tolerance=old.token==1 ? 0u : (hdr ? 2u : 1u);
            unsigned maximum[4]={};
            for(unsigned y=0;y<16;++y) for(unsigned x=0;x<16;++x) {
                const uint32_t expected=Fixture(size.width-48+x,size.height-48+y,
                    static_cast<unsigned>(old.frame),size,chosen.format);
                const uint32_t actual=reinterpret_cast<const uint32_t*>(mapped)[y*16+x];
                for(unsigned c=0;c<4;++c) {
                    const unsigned channel=!hdr && chosen.format==VK_FORMAT_B8G8R8A8_UNORM && c<3 ? 2-c : c;
                    const unsigned shift=channel*(hdr ? 10u : 8u), mask=hdr ? (c==3 ? 3u : 1023u) : 255u;
                    const int delta=static_cast<int>((actual>>shift)&mask)-static_cast<int>((expected>>shift)&mask);
                    maximum[c]=std::max(maximum[c],static_cast<unsigned>(std::abs(delta)));
                }
            }
            bool match=maximum[3]==0;
            for(unsigned c=0;c<4;++c) {s.maximum[c]=std::max(s.maximum[c],maximum[c]);if(c<3 && maximum[c]>tolerance) match=false;}
            ++s.checked;if(!match) ++s.mismatches;
            std::printf("PIXEL_SAMPLE token=%u bridge_frame=%u host_frame=%d image=0x%llX max_error=%u,%u,%u,%u tolerance=%u\n",
                old.token,old.bridge_frame,old.frame,static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(images[index])),
                maximum[0],maximum[1],maximum[2],maximum[3],tolerance);
        }
        prior[index]={static_cast<int>(frame),0,0};
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};present.waitSemaphoreCount=1;present.pWaitSemaphores=&rendered;
        present.swapchainCount=1;present.pSwapchains=&swap;present.pImageIndices=&index;
        const size_t before=BridgeLog().size();
        Check(vkQueuePresentKHR(queue,&present),"present");
        const std::string after=BridgeLog();
        size_t at=before;
        while((at=after.find("[present-adapter] frame=",at))!=std::string::npos) {
            const std::string line=after.substr(at,after.find('\n',at)-at);
            const auto token=Field(line," token=");
            const bool mode=(token==1 && line.find(" mode=copy ")!=std::string::npos) ||
                            (token==2 && line.find(" mode=carrier ")!=std::string::npos);
            if(mode && Field(line," image=")==reinterpret_cast<uintptr_t>(images[index]) &&
                Field(line," completed=")==1 && Field(line," writeback=")==1) {
                prior[index].token=static_cast<unsigned>(token);
                prior[index].bridge_frame=static_cast<unsigned>(Field(line," frame="));
            }
            ++at;
        }
        if(pause_start && sent==3) ++pause_presents;
        Sleep(10);
    }
    Request(5,0,0);Check(vkDeviceWaitIdle(device),"final idle");
    unsigned accepted_count=0,completed_count=0;
    for(unsigned i=0;i<4;++i) {accepted_count+=accepted[i];completed_count+=finished[i];}
    std::printf("SCENARIO requests_sent=%u accepted=%u completed=%u pause_ms=%llu pause_presents=%u stop_token=5\n",
        sent,accepted_count,completed_count,pause_ms,pause_presents);
    for(unsigned i=0;i<2;++i) std::printf("%s_PIXELS checked=%u mismatches=%u max_error=%u,%u,%u,%u tolerance=%u\n",
        i==0 ? "COPY" : "CARRIER",stats[i].checked,stats[i].mismatches,
        stats[i].maximum[0],stats[i].maximum[1],stats[i].maximum[2],stats[i].maximum[3],i==0 ? 0u : (hdr ? 2u : 1u));
    vkUnmapMemory(device,upload_memory);vkDestroyBuffer(device,upload,nullptr);vkFreeMemory(device,upload_memory,nullptr);
    vkUnmapMemory(device,memory);vkDestroyBuffer(device,buffer,nullptr);vkFreeMemory(device,memory,nullptr);
    vkDestroyFence(device,fence,nullptr);vkDestroySemaphore(device,acquired,nullptr);vkDestroySemaphore(device,rendered,nullptr);
    vkDestroyCommandPool(device,pool,nullptr);vkDestroySwapchainKHR(device,swap,nullptr);vkDestroyDevice(device,nullptr);
    vkDestroySurfaceKHR(instance,surface,nullptr);vkDestroyInstance(instance,nullptr);DestroyWindow(window);
    return completed && accepted_count==4 && completed_count==4 && pause_ms>=750 && pause_presents>0 &&
        stats[0].checked>0 && stats[1].checked>0 && stats[0].mismatches==0 && stats[1].mismatches==0 ? 0 : 11;
}
