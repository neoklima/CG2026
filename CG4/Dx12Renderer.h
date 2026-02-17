#pragma once
#include "Common.h"
#include "Dx12Helpers.h"
#include "UploadBuffer.h"

class Dx12Renderer
{
public:
    struct Settings
    {
        bool enableMsaa4x = false;
        DXGI_FORMAT backBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        DXGI_FORMAT depthFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        int swapChainBufferCount = 2;
    };

    Dx12Renderer() = default;
    ~Dx12Renderer();

    Dx12Renderer(const Dx12Renderer&) = delete;
    Dx12Renderer& operator=(const Dx12Renderer&) = delete;

    void Initialize(HWND hwnd, int width, int height, const Settings& settings = {});
    void Resize(int width, int height);

    void Update(float dt, float totalTime);

    void SetOrbitCamera(float theta, float phi, float radius) { mTheta = theta; mPhi = phi; mRadius = radius; }
    void Render(float r, float g, float b);

private:
    void InitDevice();
    void CreateSwapChain();
    void CreateDescriptorHeaps();
    void CreateRenderTargets();
    void CreateDepthBuffer();

    void BuildRootSignature();
    void BuildShadersAndPSO();
    void BuildCubeGeometry();

    void BuildSponzaGeometry();

    struct Vertex
        {
            DirectX::XMFLOAT3 pos;
            DirectX::XMFLOAT3 normal;
        };

    bool LoadObjSimple(const std::wstring& path,
        std::vector<Vertex>& outVertices,
        std::vector<uint32_t>& outIndices,
        DirectX::XMFLOAT3& outMin,
        DirectX::XMFLOAT3& outMax);

    void Flush();
    void MoveToNextFrame();

    ID3D12Resource* CurrentBackBuffer() const;
    D3D12_CPU_DESCRIPTOR_HANDLE CurrentRTV() const;
    D3D12_CPU_DESCRIPTOR_HANDLE DSV() const;

private:


    struct SceneCB
    {
        DirectX::XMFLOAT4X4 world;
        DirectX::XMFLOAT4X4 viewProj;
        DirectX::XMFLOAT3 eyePos;
        float _pad0;
        DirectX::XMFLOAT3 lightDir;
        float _pad1;
        DirectX::XMFLOAT3 lightColor;
        float _pad2;
    };

private:
    HWND mHwnd = nullptr;
    int mWidth = 0;
    int mHeight = 0;
    Settings mSettings{};

    ComPtr<IDXGIFactory4> mFactory;
    ComPtr<ID3D12Device> mDevice;

    ComPtr<ID3D12CommandQueue> mQueue;
    ComPtr<ID3D12CommandAllocator> mCmdAlloc;
    ComPtr<ID3D12GraphicsCommandList> mCmdList;

    ComPtr<IDXGISwapChain3> mSwapChain;

    ComPtr<ID3D12DescriptorHeap> mRtvHeap;
    ComPtr<ID3D12DescriptorHeap> mDsvHeap;
    ComPtr<ID3D12DescriptorHeap> mCbvHeap;

    UINT mRtvInc = 0;
    UINT mDsvInc = 0;
    UINT mCbvInc = 0;

    std::vector<ComPtr<ID3D12Resource>> mBackBuffers;
    ComPtr<ID3D12Resource> mDepth;

    D3D12_VIEWPORT mViewport{};
    D3D12_RECT mScissor{};

    // sync
    ComPtr<ID3D12Fence> mFence;
    UINT64 mFenceValue = 0;
    HANDLE mFenceEvent = nullptr;

    UINT mFrameIndex = 0;

    // pipeline
    ComPtr<ID3D12RootSignature> mRootSig;
    ComPtr<ID3D12PipelineState> mPSO;
    ComPtr<ID3DBlob> mVS;
    ComPtr<ID3DBlob> mPS;

    
    ComPtr<ID3D12Resource> mVB;
    ComPtr<ID3D12Resource> mIB;
    ComPtr<ID3D12Resource> mVBUpload;
    ComPtr<ID3D12Resource> mIBUpload;

    D3D12_VERTEX_BUFFER_VIEW mVBV{};
    D3D12_INDEX_BUFFER_VIEW mIBV{};
    UINT mIndexCount = 0;


    std::unique_ptr<UploadBuffer<SceneCB>> mSceneCB;

    // камера
    float mTheta = 1.6f;
    float mPhi = 0.9f;
    float mRadius = 5.0f;
};
