#include "GBuffer.h"
#include "Dx12Helpers.h"

namespace
{
    constexpr std::array<DXGI_FORMAT, GBuffer::TargetCount> GBufferFormats = {
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R16G16B16A16_FLOAT
    };

    constexpr std::array<const wchar_t*, GBuffer::TargetCount> GBufferNames = {
        L"GBuffer Albedo + Specular",
        L"GBuffer Normal + Shininess",
        L"GBuffer World Position"
    };
}

void GBuffer::Initialize(
    ID3D12Device* device,
    int width,
    int height,
    ID3D12DescriptorHeap* shaderVisibleSrvHeap,
    UINT srvBaseIndex)
{
    mDevice = device;
    mSrvHeap = shaderVisibleSrvHeap;
    mSrvBaseIndex = srvBaseIndex;
    mWidth = std::max(1, width);
    mHeight = std::max(1, height);

    mRtvIncrement = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    mSrvIncrement = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.NumDescriptors = TargetCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&mRtvHeap)));

    CreateResources();
}

void GBuffer::Resize(int width, int height)
{
    const int newWidth = std::max(1, width);
    const int newHeight = std::max(1, height);
    if(!mDevice || (newWidth == mWidth && newHeight == mHeight))
        return;

    mWidth = newWidth;
    mHeight = newHeight;
    CreateResources();
}

void GBuffer::CreateResources()
{
    for(auto& target : mTargets)
        target.Reset();

    auto rtv = mRtvHeap->GetCPUDescriptorHandleForHeapStart();
    auto srv = mSrvHeap->GetCPUDescriptorHandleForHeapStart();
    srv.ptr += static_cast<SIZE_T>(mSrvBaseIndex) * mSrvIncrement;

    for(UINT i = 0; i < TargetCount; ++i)
    {
        D3D12_RESOURCE_DESC textureDesc{};
        textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        textureDesc.Width = static_cast<UINT64>(mWidth);
        textureDesc.Height = static_cast<UINT>(mHeight);
        textureDesc.DepthOrArraySize = 1;
        textureDesc.MipLevels = 1;
        textureDesc.Format = GBufferFormats[i];
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = GBufferFormats[i];

        const auto heap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
        ThrowIfFailed(mDevice->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &textureDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clearValue,
            IID_PPV_ARGS(&mTargets[i])));

        DebugName(mTargets[i].Get(), GBufferNames[i]);

        mRtvs[i] = rtv;
        mDevice->CreateRenderTargetView(mTargets[i].Get(), nullptr, rtv);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = GBufferFormats[i];
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        mDevice->CreateShaderResourceView(mTargets[i].Get(), &srvDesc, srv);

        rtv.ptr += mRtvIncrement;
        srv.ptr += mSrvIncrement;
    }
}

void GBuffer::BeginGeometryPass(
    ID3D12GraphicsCommandList* commandList,
    D3D12_CPU_DESCRIPTOR_HANDLE depthStencilView)
{
    std::array<D3D12_RESOURCE_BARRIER, TargetCount> barriers{};
    for(UINT i = 0; i < TargetCount; ++i)
    {
        barriers[i] = Transition(
            mTargets[i].Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    commandList->ResourceBarrier(TargetCount, barriers.data());

    constexpr float clear[] = {0.0f, 0.0f, 0.0f, 0.0f};
    for(const auto& rtv : mRtvs)
        commandList->ClearRenderTargetView(rtv, clear, 0, nullptr);

    commandList->OMSetRenderTargets(TargetCount, mRtvs.data(), FALSE, &depthStencilView);
}

void GBuffer::EndGeometryPass(ID3D12GraphicsCommandList* commandList)
{
    commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

    std::array<D3D12_RESOURCE_BARRIER, TargetCount> barriers{};
    for(UINT i = 0; i < TargetCount; ++i)
    {
        barriers[i] = Transition(
            mTargets[i].Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    commandList->ResourceBarrier(TargetCount, barriers.data());
}

D3D12_GPU_DESCRIPTOR_HANDLE GBuffer::SrvGpuStart() const
{
    auto handle = mSrvHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(mSrvBaseIndex) * mSrvIncrement;
    return handle;
}

DXGI_FORMAT GBuffer::Format(Target target)
{
    return GBufferFormats[static_cast<UINT>(target)];
}
