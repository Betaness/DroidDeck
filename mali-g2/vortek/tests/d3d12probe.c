/* d3d12probe.exe: create a D3D12 device at the highest feature level it accepts, print its options,
 * then clear a render target and draw nothing for N frames on a command queue (fence round trips),
 * so vkd3d-proton's device creation and submission path over Vortek can be checked from a log. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <d3d12.h>
#include <dxgi1_4.h>

static const char* levelName(D3D_FEATURE_LEVEL l) {
    switch (l) {
        case ((D3D_FEATURE_LEVEL)0xc200): return "12_2";
        case D3D_FEATURE_LEVEL_12_1: return "12_1";
        case D3D_FEATURE_LEVEL_12_0: return "12_0";
        case D3D_FEATURE_LEVEL_11_1: return "11_1";
        case D3D_FEATURE_LEVEL_11_0: return "11_0";
        default: return "?";
    }
}

static FILE* out;
#define printf(...) (fprintf(out, __VA_ARGS__), fflush(out))
int main(void) {
    out = fopen("C:\\d3d12probe.txt", "w");
    static const D3D_FEATURE_LEVEL levels[] = {((D3D_FEATURE_LEVEL)0xc200), D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
                                               D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    ID3D12Device* dev = NULL;
    D3D_FEATURE_LEVEL got = 0;
    for (int i = 0; i < 5 && !dev; i++) {
        HRESULT hr = D3D12CreateDevice(NULL, levels[i], &IID_ID3D12Device, (void**)&dev);
        printf("D3D12PROBE CreateDevice(%s) -> 0x%08lx\n", levelName(levels[i]), (unsigned long)hr);
        if (SUCCEEDED(hr)) got = levels[i];
    }
    if (!dev) { printf("D3D12PROBE RESULT no device\n"); return 1; }
    D3D12_FEATURE_DATA_D3D12_OPTIONS o = {0};
    ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o));
    D3D12_FEATURE_DATA_SHADER_MODEL sm = {((D3D_SHADER_MODEL)0x67)};
    HRESULT smr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm));
    printf("D3D12PROBE level %s, resource binding tier %d, tiled resources tier %d, shader model 0x%x (hr 0x%08lx)\n",
           levelName(got), o.ResourceBindingTier, o.TiledResourcesTier, sm.HighestShaderModel, (unsigned long)smr);

    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
    ID3D12CommandQueue* q = NULL;
    ID3D12CommandAllocator* alloc = NULL;
    ID3D12GraphicsCommandList* list = NULL;
    ID3D12Fence* fence = NULL;
    if (FAILED(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void**)&q)) ||
        FAILED(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void**)&alloc)) ||
        FAILED(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void**)&list)) ||
        FAILED(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&fence))) {
        printf("D3D12PROBE RESULT queue setup failed\n");
        return 2;
    }
    ID3D12GraphicsCommandList_Close(list);
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    const int N = 300;
    for (UINT64 i = 1; i <= N; i++) {
        ID3D12CommandAllocator_Reset(alloc);
        ID3D12GraphicsCommandList_Reset(list, alloc, NULL);
        ID3D12GraphicsCommandList_Close(list);
        ID3D12CommandQueue_ExecuteCommandLists(q, 1, (ID3D12CommandList**)&list);
        ID3D12CommandQueue_Signal(q, fence, i);
        ID3D12Fence_SetEventOnCompletion(fence, i, ev);
        WaitForSingleObject(ev, 5000);
    }
    QueryPerformanceCounter(&t1);
    double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart / N;
    printf("D3D12PROBE RESULT ok: %d submit+fence round trips, %.3f ms each\n", N, ms);
    return 0;
}
