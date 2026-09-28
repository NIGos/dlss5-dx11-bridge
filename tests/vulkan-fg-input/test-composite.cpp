// Offline check of the FG-input UI-preserving composite on a D3D12 device.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <vector>
#include "../../src/fg-composite-shader.h"

static const UINT kW = 64, kH = 48;
static ID3D12Device* g_dev;
static ID3D12CommandQueue* g_queue;
static ID3D12CommandAllocator* g_alloc;
static ID3D12GraphicsCommandList* g_list;
static ID3D12Fence* g_fence;
static UINT64 g_fv;
static HANDLE g_ev;

static void Fail(const char* what, HRESULT hr = S_OK) { std::printf("FAIL %s hr=0x%08lX\n", what, hr); std::exit(2); }
static void Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{}; x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r; x.Transition.StateBefore = a; x.Transition.StateAfter = b;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; g_list->ResourceBarrier(1, &x);
}
static void Submit() {
    g_list->Close(); ID3D12CommandList* l[] = { g_list }; g_queue->ExecuteCommandLists(1, l);
    g_queue->Signal(g_fence, ++g_fv);
    if (g_fence->GetCompletedValue() < g_fv) { g_fence->SetEventOnCompletion(g_fv, g_ev); WaitForSingleObject(g_ev, 10000); }
    g_alloc->Reset(); g_list->Reset(g_alloc, nullptr);
}
static ID3D12Resource* Texture(bool uav) {
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = kW; rd.Height = kH;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count = 1;
    rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* r = nullptr;
    HRESULT hr = g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&r));
    if (FAILED(hr)) Fail("texture", hr);
    return r;
}
static ID3D12Resource* Buffer(UINT64 size, D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES hp{ type };
    D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = size; rd.Height = 1;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    HRESULT hr = g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&r));
    if (FAILED(hr)) Fail("buffer", hr);
    return r;
}
static const UINT kPitch = 256;  // kW*4 rounded to D3D12 row pitch alignment
static void Upload(ID3D12Resource* tex, const std::vector<uint32_t>& pixels) {
    ID3D12Resource* up = Buffer(kPitch * kH, D3D12_HEAP_TYPE_UPLOAD);
    uint8_t* mapped = nullptr; up->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    for (UINT y = 0; y < kH; ++y) memcpy(mapped + y * kPitch, &pixels[y * kW], kW * 4);
    up->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = up; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, kW, kH, 1, kPitch };
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    Barrier(tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    Submit();
}
static std::vector<uint32_t> Download(ID3D12Resource* tex) {
    ID3D12Resource* rb = Buffer(kPitch * kH, D3D12_HEAP_TYPE_READBACK);
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = tex; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, kW, kH, 1, kPitch };
    Barrier(tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(tex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    Submit();
    std::vector<uint32_t> out(kW * kH);
    uint8_t* mapped = nullptr; rb->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    for (UINT y = 0; y < kH; ++y) memcpy(&out[y * kW], mapped + y * kPitch, kW * 4);
    rb->Unmap(0, nullptr);
    return out;
}
static bool IsUi(UINT x, UINT y) { return x >= 8 && x < 24 && y >= 8 && y < 20; }

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&g_dev)))) Fail("device");
    D3D12_COMMAND_QUEUE_DESC qd{};
    g_dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&g_queue));
    g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&g_alloc));
    g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, nullptr, __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&g_list));
    g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&g_fence));
    g_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    std::vector<uint32_t> hud(kW * kH), nr(kW * kH), back(kW * kH);
    for (UINT y = 0; y < kH; ++y) for (UINT x = 0; x < kW; ++x) {
        const uint32_t scene = (x * 4) | ((y * 5) << 8) | (((x + y) * 3 & 255) << 16) | (255u << 24);
        hud[y * kW + x] = scene;
        nr[y * kW + x] = ((x * 4 + 17) & 255) | (((y * 5 + 31) & 255) << 8) | ((((x + y) * 3 + 90) & 255) << 16) | (255u << 24);
        // UI covers scene opaquely in one block and blends by one code value on its right edge.
        uint32_t b = scene;
        if (IsUi(x, y)) b = 0x80F0F0F0u;
        else if (x == 24 && y >= 8 && y < 20) b = scene + 1;  // one-code-value blend must count as UI
        back[y * kW + x] = b;
    }
    ID3D12Resource *th = Texture(false), *tn = Texture(false), *tb = Texture(true);
    Upload(th, hud); Upload(tn, nr); Upload(tb, back);

    ID3DBlob *code = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(kFgCompositeShader, sizeof(kFgCompositeShader) - 1, "fgc", nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &err)))
        Fail(err ? static_cast<const char*>(err->GetBufferPointer()) : "compile");
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = 2;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[1].NumDescriptors = 1; ranges[1].OffsetInDescriptorsFromTableStart = 2;
    D3D12_ROOT_PARAMETER param{}; param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 2; param.DescriptorTable.pDescriptorRanges = ranges;
    D3D12_ROOT_SIGNATURE_DESC rsd{}; rsd.NumParameters = 1; rsd.pParameters = &param;
    ID3DBlob* rsb = nullptr; if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, nullptr))) Fail("rootsig");
    ID3D12RootSignature* rs = nullptr; g_dev->CreateRootSignature(0, rsb->GetBufferPointer(), rsb->GetBufferSize(), __uuidof(ID3D12RootSignature), reinterpret_cast<void**>(&rs));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature = rs; pd.CS = { code->GetBufferPointer(), code->GetBufferSize() };
    ID3D12PipelineState* pso = nullptr; if (FAILED(g_dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&pso)))) Fail("pso");
    D3D12_DESCRIPTOR_HEAP_DESC hd{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap* heap = nullptr; g_dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&heap));
    const UINT inc = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{}; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sd.Texture2D.MipLevels = 1; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    g_dev->CreateShaderResourceView(th, &sd, cpu); cpu.ptr += inc;
    g_dev->CreateShaderResourceView(tn, &sd, cpu); cpu.ptr += inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{}; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    g_dev->CreateUnorderedAccessView(tb, nullptr, &ud, cpu);

    Barrier(th, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(tn, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(tb, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap* heaps[] = { heap };
    g_list->SetDescriptorHeaps(1, heaps); g_list->SetComputeRootSignature(rs); g_list->SetPipelineState(pso);
    g_list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    g_list->Dispatch((kW + 7) / 8, (kH + 7) / 8, 1);
    Barrier(tb, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    Barrier(th, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(tn, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Submit();

    const std::vector<uint32_t> out = Download(tb);
    auto channel = [](uint32_t v, unsigned c) { return static_cast<int>((v >> (8 * c)) & 255u); };
    unsigned scene_ok = 0, ui_ok = 0, blend_ok = 0, bad = 0;
    for (UINT y = 0; y < kH; ++y) for (UINT x = 0; x < kW; ++x) {
        const uint32_t v = out[y * kW + x], b = back[y * kW + x], s = hud[y * kW + x], n = nr[y * kW + x];
        int diff = 0;
        for (unsigned c = 0; c < 3; ++c) diff = diff > std::abs(channel(b, c) - channel(s, c)) ? diff : std::abs(channel(b, c) - channel(s, c));
        const float weight = 1.0f - (diff / 255.0f) / 0.25f > 0.0f ? 1.0f - (diff / 255.0f) / 0.25f : 0.0f;
        bool match = channel(v, 3) == channel(b, 3);
        for (unsigned c = 0; c < 3; ++c) {
            const float raw = channel(b, c) + (channel(n, c) - channel(s, c)) * weight;
            const float expected = raw < 0.0f ? 0.0f : raw > 255.0f ? 255.0f : raw;
            if (std::abs(channel(v, c) - expected) > 1.0f) match = false;
        }
        if (!match) { ++bad; continue; }
        if (IsUi(x, y)) { if (v == b) ++ui_ok; else ++bad; }
        else if (x == 24 && y >= 8 && y < 20) ++blend_ok;
        else ++scene_ok;
    }
    std::printf("COMPOSITE scene_pixels=%u opaque_ui_kept=%u translucent_blended=%u mismatches=%u\n", scene_ok, ui_ok, blend_ok, bad);
    return bad == 0 && ui_ok == 16 * 12 && blend_ok == 12 && scene_ok == kW * kH - ui_ok - blend_ok ? 0 : 1;
}
