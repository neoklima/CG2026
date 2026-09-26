#pragma once

#include "Common.h"

class GBuffer
{
public:
    static constexpr UINT TargetCount = 3;

    enum Target : UINT
    {
        AlbedoSpecular = 0,
        NormalShininess = 1,
        WorldPosition = 2
    };

    void Initialize(
        ID3D12Device* device,
        int width,
        int height,
        ID3D12DescriptorHeap* shaderVisibleSrvHeap,
        UINT srvBaseIndex);
    void Resize(int width, int height);

    void BeginGeometryPass(
        ID3D12GraphicsCommandList* commandList,
        D3D12_CPU_DESCRIPTOR_HANDLE depthStencilView);
    void EndGeometryPass(ID3D12GraphicsCommandList* commandList);

    D3D12_GPU_DESCRIPTOR_HANDLE SrvGpuStart() const;
    static DXGI_FORMAT Format(Target target);

private:
    void CreateResources();

private:
    ComPtr<ID3D12Device> mDevice;
    ComPtr<ID3D12DescriptorHeap> mRtvHeap;
    ComPtr<ID3D12DescriptorHeap> mSrvHeap;
    std::array<ComPtr<ID3D12Resource>, TargetCount> mTargets;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, TargetCount> mRtvs{};

    UINT mRtvIncrement = 0;
    UINT mSrvIncrement = 0;
    UINT mSrvBaseIndex = 0;
    int mWidth = 1;
    int mHeight = 1;
};
