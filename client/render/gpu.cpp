// =============================================================================
//  gpu.cpp - Implementación de texturas y desenfoque
// =============================================================================
#include "render/gpu.h"

#include <d3dcompiler.h>  // D3DCompile: compilar shaders en tiempo de ejecución
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/logger.h"

using Microsoft::WRL::ComPtr;

namespace luma::render::gpu {

namespace {

// --- Tabla de descriptores ----------------------------------------------------------
constexpr UINT kSlotCount = 128;  // fuente, desenfoque, logo y los íconos de ítems
constexpr UINT kSceneSlot = 1;      // copia de la imagen del juego
constexpr UINT kBlurSlots[2] = {2, 3};  // las dos texturas del desenfoque (ping-pong)
constexpr UINT kFirstFreeSlot = 4;  // de aquí en adelante: logo, íconos, etc.

ComPtr<ID3D12Device> g_device;
ID3D12CommandQueue* g_queue = nullptr;  // de la cola se encarga el overlay
ComPtr<ID3D12DescriptorHeap> g_srvHeap;
UINT g_srvSize = 0;
UINT g_nextSlot = kFirstFreeSlot;

// --- Subida de texturas ------------------------------------------------------------
ComPtr<ID3D12CommandAllocator> g_uploadAllocator;
ComPtr<ID3D12GraphicsCommandList> g_uploadList;
ComPtr<ID3D12Fence> g_uploadFence;
UINT64 g_uploadValue = 0;
HANDLE g_uploadEvent = nullptr;
std::vector<ComPtr<ID3D12Resource>> g_textures;  // las mantenemos vivas

// --- Desenfoque ----------------------------------------------------------------------
ComPtr<ID3D12RootSignature> g_rootSignature;
ComPtr<ID3D12PipelineState> g_pipeline;
ComPtr<ID3D12PipelineState> g_postPipeline;  // post-procesado (formato del back buffer)
UINT g_sceneWidth = 0;
UINT g_sceneHeight = 0;
ComPtr<ID3D12DescriptorHeap> g_blurRtvHeap;
ComPtr<ID3D12Resource> g_scene;
ComPtr<ID3D12Resource> g_blur[2];
D3D12_RESOURCE_STATES g_sceneState = D3D12_RESOURCE_STATE_COPY_DEST;
D3D12_RESOURCE_STATES g_blurState[2] = {};
UINT g_blurWidth = 0;
UINT g_blurHeight = 0;
bool g_blurReady = false;
bool g_blurActive = false;  // ¿se desenfoca en este frame?

// El shader del desenfoque, en HLSL.
//  - VSMain dibuja un triángulo que cubre toda la pantalla (sin necesitar
//    vértices: los calcula a partir de su número 0, 1, 2).
//  - PSMain, por cada píxel, promedia 5 muestras de la textura a lo largo de
//    una dirección (horizontal o vertical), con pesos de una curva gaussiana.
//    Gracias al filtrado bilineal de la GPU, esas 5 muestras equivalen a 9.
constexpr const char* kBlurShader = R"(
cbuffer Params : register(b0) { float2 texel; float2 dir; };
Texture2D source : register(t0);
SamplerState linearClamp : register(s0);

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 PSMain(VSOut i) : SV_Target {
    float2 d = dir * texel;
    float3 c = source.Sample(linearClamp, i.uv).rgb * 0.2270270270;
    c += source.Sample(linearClamp, i.uv + d * 1.3846153846).rgb * 0.3162162162;
    c += source.Sample(linearClamp, i.uv - d * 1.3846153846).rgb * 0.3162162162;
    c += source.Sample(linearClamp, i.uv + d * 3.2307692308).rgb * 0.0702702703;
    c += source.Sample(linearClamp, i.uv - d * 3.2307692308).rgb * 0.0702702703;
    return float4(c, 1.0);
}

// --- Post-procesado ---------------------------------------------------------
// Las constantes llegan en el mismo cbuffer (16 números): los primeros 4 son
// "texel" y "dir" (del desenfoque), los siguientes, los ajustes de color.
cbuffer Post : register(b0) {
    float2 ptexel; float2 unused;
    float brightness; float contrast; float saturation; float gamma;
    float temperature; float tint; float vibrance; float vignette;
    float sharpen; float exposure; float pad0; float pad1;
};

float4 PSPost(VSOut i) : SV_Target {
    float3 c = source.Sample(linearClamp, i.uv).rgb;
    // Nitidez: el píxel menos el promedio de sus 4 vecinos, sumado de vuelta.
    if (sharpen > 0.001) {
        float3 n = source.Sample(linearClamp, i.uv + float2(ptexel.x, 0)).rgb
                 + source.Sample(linearClamp, i.uv - float2(ptexel.x, 0)).rgb
                 + source.Sample(linearClamp, i.uv + float2(0, ptexel.y)).rgb
                 + source.Sample(linearClamp, i.uv - float2(0, ptexel.y)).rgb;
        c += (c - n * 0.25) * sharpen * 1.5;
    }
    c *= exp2(exposure);                                   // exposición
    c = pow(max(c, 0.0), 1.0 / gamma);                     // gamma
    c += brightness * 0.25;                                // brillo
    c = (c - 0.5) * contrast + 0.5;                        // contraste
    float lum = dot(c, float3(0.2126, 0.7152, 0.0722));    // "cuánta luz" tiene el color
    c = lerp(lum.xxx, c, saturation);                      // saturación
    float sat = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
    c = lerp(lum.xxx, c, 1.0 + vibrance * (1.0 - sat));    // intensidad (vibrance)
    c += float3(temperature * 0.08, tint * 0.06, -temperature * 0.08);  // temperatura y tinte
    float d = distance(i.uv, float2(0.5, 0.5)) * 1.41421;  // 0 en el centro, 1 en la esquina
    c *= 1.0 - vignette * smoothstep(0.45, 1.0, d);        // viñeta
    return float4(saturate(c), 1.0);
}
)";

ComPtr<ID3DBlob> Compile(const char* entry, const char* target) {
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(kBlurShader, std::strlen(kBlurShader), "luma_blur", nullptr, nullptr,
                                  entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        log::Error("No se pudo compilar el shader de desenfoque ({}): {}", entry,
                   errors ? static_cast<const char*>(errors->GetBufferPointer()) : "sin detalle");
        return nullptr;
    }
    return code;
}

// "Root signature": la lista de entradas que recibe el shader (la textura,
// cuatro números con el tamaño de texel y la dirección, y el muestreador).
bool CreatePipeline() {
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;  // t0

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &range;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 0;  // b0
    params[1].Constants.Num32BitValues = 16;  // 4 del desenfoque + 12 del post-procesado
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;  // s0
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 2;
    desc.pParameters = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
        FAILED(g_device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                             IID_PPV_ARGS(&g_rootSignature)))) {
        log::Error("No se pudo crear la root signature del desenfoque.");
        return false;
    }

    ComPtr<ID3DBlob> vs = Compile("VSMain", "vs_5_0");
    ComPtr<ID3DBlob> ps = Compile("PSMain", "ps_5_0");
    if (!vs || !ps) {
        return false;
    }

    // "Pipeline state": cómo se dibuja (shaders, formato de salida, sin mezcla).
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g_rootSignature.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.SampleDesc.Count = 1;
    if (FAILED(g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_pipeline)))) {
        log::Error("No se pudo crear el pipeline del desenfoque.");
        return false;
    }
    return true;
}

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
             D3D12_RESOURCE_STATES after) {
    if (before == after) {
        return;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}

D3D12_CPU_DESCRIPTOR_HANDLE BlurRtv(int index) {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = g_blurRtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) *
                  g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return handle;
}

ComPtr<ID3D12Resource> CreateTexture2D(UINT width, UINT height, DXGI_FORMAT format,
                                       D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> resource;
    if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                 IID_PPV_ARGS(&resource)))) {
        return nullptr;
    }
    return resource;
}

void CreateSrv(ID3D12Resource* resource, DXGI_FORMAT format, UINT slot) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(resource, &srv, CpuSlot(slot));
}

}  // namespace

bool Init(ID3D12Device* device, ID3D12CommandQueue* queue) {
    g_device = device;
    g_queue = queue;

    D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.NumDescriptors = kSlotCount;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&g_srvHeap)))) {
        log::Error("No se pudo crear la tabla de descriptores.");
        return false;
    }
    g_srvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = 2;
    if (FAILED(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g_blurRtvHeap)))) {
        log::Error("No se pudo crear el heap de RTV del desenfoque.");
        return false;
    }

    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&g_uploadAllocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_uploadAllocator.Get(), nullptr,
                                         IID_PPV_ARGS(&g_uploadList))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_uploadFence)))) {
        log::Error("No se pudo preparar la subida de texturas.");
        return false;
    }
    g_uploadList->Close();
    g_uploadEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (!CreatePipeline()) {
        log::Warn("Sin desenfoque: los paneles serán translúcidos pero sin vidrio esmerilado.");
    }
    return true;
}

void Shutdown() {
    DestroyBlurTargets();
    g_pipeline.Reset();
    g_postPipeline.Reset();
    g_rootSignature.Reset();
    g_blurRtvHeap.Reset();
    g_textures.clear();
    g_uploadList.Reset();
    g_uploadAllocator.Reset();
    g_uploadFence.Reset();
    if (g_uploadEvent != nullptr) {
        CloseHandle(g_uploadEvent);
        g_uploadEvent = nullptr;
    }
    g_srvHeap.Reset();
    g_device.Reset();
    g_queue = nullptr;
    g_nextSlot = kFirstFreeSlot;
}

ID3D12DescriptorHeap* SrvHeap() {
    return g_srvHeap.Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE CpuSlot(UINT slot) {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(slot) * g_srvSize;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE GpuSlot(UINT slot) {
    D3D12_GPU_DESCRIPTOR_HANDLE handle = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(slot) * g_srvSize;
    return handle;
}

ImTextureID CreateTexture(const std::uint8_t* rgba, UINT width, UINT height) {
    if (!g_device || g_queue == nullptr || g_nextSlot >= kSlotCount || rgba == nullptr) {
        return 0;
    }
    ComPtr<ID3D12Resource> texture = CreateTexture2D(width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
                                                     D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!texture) {
        return 0;
    }

    // La GPU pide que cada fila de la imagen ocupe un múltiplo de 256 bytes en
    // la memoria de subida. GetCopyableFootprints calcula ese formato.
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 rowSize = 0;
    UINT64 totalSize = 0;
    g_device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowSize, &totalSize);

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bufferDesc{};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = totalSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> upload;
    if (FAILED(g_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(&upload)))) {
        return 0;
    }
    std::uint8_t* mapped = nullptr;
    if (FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) {
        return 0;
    }
    for (UINT y = 0; y < height; ++y) {
        std::memcpy(mapped + footprint.Offset + static_cast<SIZE_T>(y) * footprint.Footprint.RowPitch,
                    rgba + static_cast<SIZE_T>(y) * width * 4, static_cast<SIZE_T>(width) * 4);
    }
    upload->Unmap(0, nullptr);

    // Grabar la copia, mandarla a la cola del juego y esperar a que termine.
    g_uploadAllocator->Reset();
    g_uploadList->Reset(g_uploadAllocator.Get(), nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;
    g_uploadList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(g_uploadList.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    g_uploadList->Close();
    ID3D12CommandList* lists[] = {g_uploadList.Get()};
    g_queue->ExecuteCommandLists(1, lists);
    const UINT64 value = ++g_uploadValue;
    g_queue->Signal(g_uploadFence.Get(), value);
    if (g_uploadFence->GetCompletedValue() < value) {
        g_uploadFence->SetEventOnCompletion(value, g_uploadEvent);
        WaitForSingleObject(g_uploadEvent, 2000);
    }

    const UINT slot = g_nextSlot++;
    CreateSrv(texture.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, slot);
    g_textures.push_back(texture);
    return static_cast<ImTextureID>(GpuSlot(slot).ptr);
}

// El pipeline del post-procesado escribe en el back buffer, así que se crea
// con SU formato (lo sabemos recién al tener la swapchain).
void CreatePostPipeline(DXGI_FORMAT format) {
    g_postPipeline.Reset();
    ComPtr<ID3DBlob> vs = Compile("VSMain", "vs_5_0");
    ComPtr<ID3DBlob> ps = Compile("PSPost", "ps_5_0");
    if (!vs || !ps || !g_rootSignature) {
        return;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g_rootSignature.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = format;
    pso.SampleDesc.Count = 1;
    if (FAILED(g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_postPipeline)))) {
        log::Warn("No se pudo crear el pipeline del post-procesado.");
        g_postPipeline.Reset();
    }
}

bool CreateBlurTargets(UINT width, UINT height, DXGI_FORMAT backBufferFormat) {
    DestroyBlurTargets();
    if (!g_pipeline || width == 0 || height == 0) {
        return false;
    }
    g_sceneWidth = width;
    g_sceneHeight = height;
    CreatePostPipeline(backBufferFormat);
    // Copia del juego al mismo tamaño y formato (lo exige CopyResource).
    g_scene = CreateTexture2D(width, height, backBufferFormat, D3D12_RESOURCE_FLAG_NONE,
                              D3D12_RESOURCE_STATE_COPY_DEST);
    g_sceneState = D3D12_RESOURCE_STATE_COPY_DEST;
    // El desenfoque trabaja a 1/4 de la resolución: es 16 veces menos trabajo
    // y, como el resultado va borroso, no se nota.
    g_blurWidth = std::max(1u, width / 4);
    g_blurHeight = std::max(1u, height / 4);
    for (int i = 0; i < 2; ++i) {
        g_blur[i] = CreateTexture2D(g_blurWidth, g_blurHeight, DXGI_FORMAT_R8G8B8A8_UNORM,
                                    D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        g_blurState[i] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    if (!g_scene || !g_blur[0] || !g_blur[1]) {
        log::Warn("No se pudieron crear las texturas del desenfoque.");
        DestroyBlurTargets();
        return false;
    }
    CreateSrv(g_scene.Get(), backBufferFormat, kSceneSlot);
    for (int i = 0; i < 2; ++i) {
        CreateSrv(g_blur[i].Get(), DXGI_FORMAT_R8G8B8A8_UNORM, kBlurSlots[i]);
        g_device->CreateRenderTargetView(g_blur[i].Get(), nullptr, BlurRtv(i));
    }
    g_blurReady = true;
    return true;
}

void DestroyBlurTargets() {
    g_blurReady = false;
    g_scene.Reset();
    g_blur[0].Reset();
    g_blur[1].Reset();
}

void RecordBlur(ID3D12GraphicsCommandList* list, ID3D12Resource* backBuffer, float strength,
                D3D12_RESOURCE_STATES before) {
    if (!g_blurReady) {
        Barrier(list, backBuffer, before, D3D12_RESOURCE_STATE_RENDER_TARGET);
        return;
    }
    // a. Copiar la imagen del juego.
    Barrier(list, backBuffer, before, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(list, g_scene.Get(), g_sceneState, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(g_scene.Get(), backBuffer);
    Barrier(list, g_scene.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    g_sceneState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Barrier(list, backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    // b. Desenfocar a 1/4 de resolución, alternando entre dos texturas.
    list->SetGraphicsRootSignature(g_rootSignature.Get());
    list->SetPipelineState(g_pipeline.Get());
    ID3D12DescriptorHeap* heaps[] = {g_srvHeap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(g_blurWidth),
                                  static_cast<float>(g_blurHeight), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(g_blurWidth), static_cast<LONG>(g_blurHeight)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);

    const float spread = 1.0f + strength * 2.2f;            // qué tan "abierta" es cada pasada
    const int iterations = 1 + static_cast<int>(strength * 2.0f + 0.5f);  // 1 a 3 rondas
    UINT sourceSlot = kSceneSlot;
    int target = 0;
    for (int pass = 0; pass < iterations * 2; ++pass) {
        const bool horizontal = (pass % 2) == 0;
        Barrier(list, g_blur[target].Get(), g_blurState[target], D3D12_RESOURCE_STATE_RENDER_TARGET);
        g_blurState[target] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = BlurRtv(target);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->SetGraphicsRootDescriptorTable(0, GpuSlot(sourceSlot));
        const float constants[4] = {1.0f / static_cast<float>(g_blurWidth), 1.0f / static_cast<float>(g_blurHeight),
                                    horizontal ? spread : 0.0f, horizontal ? 0.0f : spread};
        list->SetGraphicsRoot32BitConstants(1, 4, constants, 0);
        list->DrawInstanced(3, 1, 0, 0);
        Barrier(list, g_blur[target].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        g_blurState[target] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        sourceSlot = kBlurSlots[target];
        target = 1 - target;
    }
    // La última pasada escribió en la textura 1 (las pasadas son pares).
}

bool PostReady() {
    return g_blurReady && g_postPipeline;
}

void RecordPost(ID3D12GraphicsCommandList* list, ID3D12Resource* backBuffer, D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv,
                const PostParams& p) {
    if (!PostReady()) {
        Barrier(list, backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        return;
    }
    // a. Copiar el frame del juego (el shader lee de la copia y escribe en el original).
    Barrier(list, backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(list, g_scene.Get(), g_sceneState, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(g_scene.Get(), backBuffer);
    Barrier(list, g_scene.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    g_sceneState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Barrier(list, backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    // b. Un triángulo que cubre la pantalla, con el shader de color.
    list->SetGraphicsRootSignature(g_rootSignature.Get());
    list->SetPipelineState(g_postPipeline.Get());
    ID3D12DescriptorHeap* heaps[] = {g_srvHeap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(g_sceneWidth), static_cast<float>(g_sceneHeight), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(g_sceneWidth), static_cast<LONG>(g_sceneHeight)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &backBufferRtv, FALSE, nullptr);
    list->SetGraphicsRootDescriptorTable(0, GpuSlot(kSceneSlot));
    const float constants[16] = {1.0f / static_cast<float>(g_sceneWidth), 1.0f / static_cast<float>(g_sceneHeight), 0.0f, 0.0f,
                                 p.brightness, p.contrast, p.saturation, std::max(0.1f, p.gamma),
                                 p.temperature, p.tint, p.vibrance, p.vignette,
                                 p.sharpen, p.exposure, 0.0f, 0.0f};
    list->SetGraphicsRoot32BitConstants(1, 16, constants, 0);
    list->DrawInstanced(3, 1, 0, 0);
}

void SetBlurActive(bool active) {
    g_blurActive = active;
}

ImTextureID BlurTexture() {
    return g_blurReady && g_blurActive ? static_cast<ImTextureID>(GpuSlot(kBlurSlots[1]).ptr) : 0;
}

}  // namespace luma::render::gpu
