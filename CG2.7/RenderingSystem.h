#pragma once

#include "Common.h"
#include "Dx12Helpers.h"
#include "GBuffer.h"
#include "SpatialCulling.h"
#include "UploadBuffer.h"
#include <deque>

class RenderingSystem
{
public:
    struct Settings
    {
        bool enableMsaa4x = false;
        DXGI_FORMAT backBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        DXGI_FORMAT depthFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        int swapChainBufferCount = 2;
    };

    enum class LightType : uint32_t
    {
        Directional = 0,
        Point = 1,
        Spot = 2
    };

    enum class PostEffect : uint32_t
    {
        None = 0,
        Vignette = 1,
        GaussianBlur = 2
    };

    struct Light
    {
        LightType type = LightType::Point;
        DirectX::XMFLOAT3 position = {0.0f, 0.0f, 0.0f};
        float range = 10.0f;
        DirectX::XMFLOAT3 direction = {0.0f, -1.0f, 0.0f};
        float spotCosOuter = 0.75f;
        DirectX::XMFLOAT3 color = {1.0f, 1.0f, 1.0f};
        float intensity = 1.0f;
        float spotCosInner = 0.9f;
    };

    RenderingSystem() = default;
    ~RenderingSystem();

    RenderingSystem(const RenderingSystem&) = delete;
    RenderingSystem& operator=(const RenderingSystem&) = delete;

    void Initialize(HWND hwnd, int width, int height, const Settings& settings = {});
    void Resize(int width, int height);
    void Update(float dt, float totalTime);
    void SetCamera(
        const DirectX::XMFLOAT3& position,
        const DirectX::XMFLOAT3& forward)
    {
        mCameraPosition = position;
        mCameraForward = forward;
    }
    void SetLights(std::vector<Light> lights);
    const std::vector<Light>& Lights() const { return mLights; }
    void ToggleNormalMapping() { mNormalMappingEnabled = !mNormalMappingEnabled; mNextTitleUpdate = 0.0f; }
    void ToggleDisplacement() { mDisplacementEnabled = !mDisplacementEnabled; mNextTitleUpdate = 0.0f; }
    void ToggleWireframe() { mWireframeEnabled = !mWireframeEnabled; mNextTitleUpdate = 0.0f; }
    void ToggleFrustumCulling();
    void ToggleOctreeCulling();
    void ToggleShadows() { mShadowsEnabled = !mShadowsEnabled; mNextTitleUpdate = 0.0f; }
    void ToggleLod() { mLodEnabled = !mLodEnabled; mNextTitleUpdate = 0.0f; }
    void SetPostEffect(PostEffect effect) { mPostEffect = effect; mNextTitleUpdate = 0.0f; }
    void QueueParticleBurst(const DirectX::XMFLOAT3& position);
    void Render(float r, float g, float b);

private:
    static constexpr UINT MaxLights = 32;
    static constexpr UINT RockObjectCount = 600;
    static constexpr UINT SceneObjectCount = RockObjectCount + 1;
    static constexpr UINT RockLodCount = 3;
    static constexpr UINT ShadowCascadeCount = 3;
    static constexpr UINT ShadowMapSize = 1024;
    static constexpr UINT FountainParticleCount = 128;
    static constexpr UINT BurstParticleCount = 128;
    static constexpr UINT ParticleCount = FountainParticleCount + BurstParticleCount;

    struct Vertex
    {
        DirectX::XMFLOAT3 pos;
        DirectX::XMFLOAT3 normal;
        DirectX::XMFLOAT2 texCoord;
        DirectX::XMFLOAT4 tangent = {1.0f, 0.0f, 0.0f, 1.0f};
    };

    struct ParsedSubset
    {
        std::string materialName;
        UINT indexStart = 0;
        UINT indexCount = 0;
    };

    struct Material
    {
        std::string name;
        DirectX::XMFLOAT4 diffuse = {1.0f, 1.0f, 1.0f, 1.0f};
        DirectX::XMFLOAT3 specular = {0.25f, 0.25f, 0.25f};
        float shininess = 32.0f;
        std::wstring diffuseMap;
        UINT textureIndex = 0;
    };

    struct DrawSubset
    {
        UINT indexStart = 0;
        UINT indexCount = 0;
        UINT materialIndex = 0;
    };

    struct ObjectDraw
    {
        UINT firstSubset = 0;
        UINT subsetCount = 0;
    };

    struct SceneCB
    {
        DirectX::XMFLOAT4X4 world;
        DirectX::XMFLOAT4X4 viewProj;
        DirectX::XMFLOAT2 textureOffset;
        DirectX::XMFLOAT2 textureTiling;
        DirectX::XMFLOAT3 eyePosition;
        uint32_t colorizeTiles = 0;
        uint32_t enableNormalMapping = 1;
        uint32_t enableDisplacement = 1;
        DirectX::XMFLOAT2 togglePadding = {0.0f, 0.0f};
    };

    struct MaterialCB
    {
        DirectX::XMFLOAT4 diffuse;
        DirectX::XMFLOAT3 specular;
        float shininess;
        float displacementScale;
        float displacementBias;
        float minTessFactor;
        float maxTessFactor;
        float tessNearDistance;
        float tessFarDistance;
        DirectX::XMFLOAT2 padding;
    };

    struct GpuLight
    {
        DirectX::XMFLOAT3 position;
        float range;
        DirectX::XMFLOAT3 direction;
        float spotCosOuter;
        DirectX::XMFLOAT3 color;
        float intensity;
        float spotCosInner;
        uint32_t type;
        DirectX::XMFLOAT2 padding;
    };

    struct LightingCB
    {
        DirectX::XMFLOAT3 eyePosition;
        float ambientIntensity;
        DirectX::XMFLOAT3 ambientColor;
        uint32_t lightCount;
        DirectX::XMFLOAT3 backgroundColor;
        float padding;
        std::array<GpuLight, MaxLights> lights;
        std::array<DirectX::XMFLOAT4X4, ShadowCascadeCount> shadowViewProj;
        DirectX::XMFLOAT4 cascadeSplits;
        DirectX::XMFLOAT4 cameraForwardAndShadow;
    };

    struct ShadowCB
    {
        DirectX::XMFLOAT4X4 worldViewProj;
    };

    struct Particle
    {
        DirectX::XMFLOAT3 position;
        float age;
        DirectX::XMFLOAT3 velocity;
        float lifetime;
        uint32_t kind;
    };

    struct ParticleFrameCB
    {
        DirectX::XMFLOAT4X4 viewProj;
        DirectX::XMFLOAT4 cameraRight;
        DirectX::XMFLOAT4 cameraUp;
        DirectX::XMFLOAT4 emitterAndDelta;
        DirectX::XMFLOAT4 burstPositionAndActive;
    };

    void InitDevice();
    void CreateSwapChain();
    void CreateDescriptorHeaps();
    void CreateRenderTargets();
    void CreateSceneColorTarget();
    void CreateDepthBuffer();
    void BuildRootSignatures();
    void BuildShadersAndPSOs();
    void BuildCubeGeometry();
    void BuildSponzaGeometry();
    void BuildObjectScene();
    void BuildMaterialResources();
    void BuildDefaultLights();
    void BuildShadowResources();
    void UpdateShadows(const DirectX::XMMATRIX& view);
    void RenderShadows();
    void BuildParticleResources();
    void UpdateParticleConstants(const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& viewProj, float dt);
    void SimulateParticles();
    void RenderParticles(D3D12_CPU_DESCRIPTOR_HANDLE rtv);
    void RenderPostProcess();

    void UpdateVisibleObjects(const DirectX::BoundingFrustum& worldFrustum);
    void UpdateObjectLods();
    void UpdateObjectConstants(
        const DirectX::XMMATRIX& viewProjection,
        const DirectX::XMFLOAT3& eyePosition,
        float totalTime);
    void UpdateWindowTitle(float totalTime);

    bool LoadObjSimple(
        const std::wstring& path,
        std::vector<Vertex>& outVertices,
        std::vector<uint32_t>& outIndices,
        std::vector<ParsedSubset>& outSubsets,
        std::wstring& outMaterialLibrary,
        DirectX::XMFLOAT3& outMin,
        DirectX::XMFLOAT3& outMax);

    bool LoadMaterialLibrary(const std::wstring& path, std::vector<Material>& outMaterials);

    void Flush();
    void MoveToNextFrame();

    ID3D12Resource* CurrentBackBuffer() const;
    D3D12_CPU_DESCRIPTOR_HANDLE CurrentRTV() const;
    D3D12_CPU_DESCRIPTOR_HANDLE DSV() const;

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
    ComPtr<ID3D12DescriptorHeap> mSrvHeap;

    UINT mRtvInc = 0;
    UINT mDsvInc = 0;
    UINT mSrvInc = 0;
    UINT mGBufferSrvOffset = 0;
    UINT mParticleSrvOffset = 0;
    UINT mParticleUavOffset = 0;
    UINT mSceneColorSrvOffset = 0;

    std::vector<ComPtr<ID3D12Resource>> mBackBuffers;
    ComPtr<ID3D12Resource> mDepth;
    ComPtr<ID3D12Resource> mShadowMaps;
    ComPtr<ID3D12Resource> mSceneColor;
    D3D12_CPU_DESCRIPTOR_HANDLE mSceneColorRtv{};
    D3D12_GPU_DESCRIPTOR_HANDLE mSceneColorSrv{};
    GBuffer mGBuffer;

    D3D12_VIEWPORT mViewport{};
    D3D12_RECT mScissor{};

    ComPtr<ID3D12Fence> mFence;
    UINT64 mFenceValue = 0;
    HANDLE mFenceEvent = nullptr;
    UINT mFrameIndex = 0;

    ComPtr<ID3D12RootSignature> mGeometryRootSig;
    ComPtr<ID3D12RootSignature> mLightingRootSig;
    ComPtr<ID3D12RootSignature> mShadowRootSig;
    ComPtr<ID3D12RootSignature> mParticleComputeRootSig;
    ComPtr<ID3D12RootSignature> mParticleGraphicsRootSig;
    ComPtr<ID3D12RootSignature> mPostRootSig;
    ComPtr<ID3D12PipelineState> mGeometryPSO;
    ComPtr<ID3D12PipelineState> mGeometryWireframePSO;
    ComPtr<ID3D12PipelineState> mLightingPSO;
    ComPtr<ID3D12PipelineState> mShadowPSO;
    ComPtr<ID3D12PipelineState> mParticleComputePSO;
    ComPtr<ID3D12PipelineState> mParticleGraphicsPSO;
    ComPtr<ID3D12PipelineState> mPostCopyPSO;
    ComPtr<ID3D12PipelineState> mPostVignettePSO;
    ComPtr<ID3D12PipelineState> mPostBlurPSO;
    ComPtr<ID3DBlob> mGeometryVS;
    ComPtr<ID3DBlob> mGeometryHS;
    ComPtr<ID3DBlob> mGeometryDS;
    ComPtr<ID3DBlob> mGeometryPS;
    ComPtr<ID3DBlob> mFullscreenVS;
    ComPtr<ID3DBlob> mLightingPS;
    ComPtr<ID3DBlob> mShadowVS;
    ComPtr<ID3DBlob> mParticleCS;
    ComPtr<ID3DBlob> mParticleVS;
    ComPtr<ID3DBlob> mParticleGS;
    ComPtr<ID3DBlob> mParticlePS;
    ComPtr<ID3DBlob> mPostVS;
    ComPtr<ID3DBlob> mPostCopyPS;
    ComPtr<ID3DBlob> mPostVignettePS;
    ComPtr<ID3DBlob> mPostBlurPS;

    ComPtr<ID3D12Resource> mVB;
    ComPtr<ID3D12Resource> mIB;
    ComPtr<ID3D12Resource> mVBUpload;
    ComPtr<ID3D12Resource> mIBUpload;
    D3D12_VERTEX_BUFFER_VIEW mVBV{};
    D3D12_INDEX_BUFFER_VIEW mIBV{};
    UINT mIndexCount = 0;
    UINT mSponzaSubsetStart = 0;
    UINT mSponzaSubsetCount = 0;
    UINT mRockSubsetIndex = 0;
    DirectX::BoundingBox mSponzaLocalBounds;
    DirectX::BoundingBox mRockLocalBounds;

    std::vector<Material> mMaterials;
    std::vector<DrawSubset> mDrawSubsets;
    std::vector<ComPtr<ID3D12Resource>> mTextures;
    std::vector<Light> mLights;
    std::vector<DirectX::XMFLOAT4X4> mObjectWorlds;
    std::vector<DirectX::BoundingBox> mObjectBounds;
    std::vector<ObjectDraw> mObjectDraws;
    std::vector<uint32_t> mVisibleObjects;
    std::vector<uint8_t> mObjectLods;
    std::array<UINT, RockLodCount> mVisibleRockLodCounts{};
    SpatialCulling::Octree mOctree;
    SpatialCulling::Stats mCullingStats{};
    std::unique_ptr<UploadBuffer<SceneCB>> mSceneCB;
    std::unique_ptr<UploadBuffer<MaterialCB>> mMaterialCB;
    std::unique_ptr<UploadBuffer<LightingCB>> mLightingCB;
    std::unique_ptr<UploadBuffer<ShadowCB>> mShadowCB;
    LightingCB mLightingData{};
    std::array<DirectX::XMFLOAT4X4, ShadowCascadeCount> mShadowViewProj{};
    DirectX::XMFLOAT4 mCascadeSplits{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, ShadowCascadeCount> mShadowDsvs{};
    std::array<ComPtr<ID3D12Resource>, 2> mParticleBuffers;
    std::array<ComPtr<ID3D12Resource>, 2> mParticleCounters;
    std::array<D3D12_RESOURCE_STATES, 2> mParticleBufferStates{
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    std::unique_ptr<UploadBuffer<ParticleFrameCB>> mParticleFrameCB;
    ComPtr<ID3D12Resource> mParticleZeroUpload;
    UINT mParticleReadIndex = 0;
    std::deque<DirectX::XMFLOAT3> mPendingBursts;
    uint32_t mBurstCount = 0;

    DirectX::XMFLOAT3 mCameraPosition = {-9.0f, -4.0f, 0.0f};
    DirectX::XMFLOAT3 mCameraForward = {1.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT3 mParticleEmitter = {0.0f, -8.7f, 0.0f};
    float mNextTitleUpdate = 0.0f;
    float mSmoothedFrameMilliseconds = 16.67f;
    float mCullingMicroseconds = 0.0f;
    bool mNormalMappingEnabled = true;
    bool mDisplacementEnabled = true;
    bool mWireframeEnabled = false;
    bool mFrustumCullingEnabled = true;
    bool mOctreeCullingEnabled = true;
    bool mShadowsEnabled = true;
    bool mLodEnabled = true;
    PostEffect mPostEffect = PostEffect::None;
};
