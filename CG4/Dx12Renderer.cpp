#include "Dx12Renderer.h"

using namespace DirectX;

static ComPtr<ID3D12Resource> CreateDefaultBuffer(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    const void* initData,
    UINT64 byteSize,
    ComPtr<ID3D12Resource>& uploadBuffer)
{
    ComPtr<ID3D12Resource> defaultBuffer;

    auto defaultHeap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    auto uploadHeap  = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    auto defaultDesc = BufferDesc(byteSize);
    auto uploadDesc  = BufferDesc(byteSize);

    ThrowIfFailed(device->CreateCommittedResource(
        &defaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &defaultDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&defaultBuffer)));

    ThrowIfFailed(device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&uploadBuffer)));

    void* mapped = nullptr;
    D3D12_RANGE range{0, 0};
    ThrowIfFailed(uploadBuffer->Map(0, &range, &mapped));
    memcpy(mapped, initData, byteSize);
    uploadBuffer->Unmap(0, nullptr);

    auto b0 = Transition(defaultBuffer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->ResourceBarrier(1, &b0);
    cmdList->CopyBufferRegion(defaultBuffer.Get(), 0, uploadBuffer.Get(), 0, byteSize);
    auto b1 = Transition(defaultBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ);
    cmdList->ResourceBarrier(1, &b1);

    return defaultBuffer;
}

Dx12Renderer::~Dx12Renderer()
{
    if(mDevice)
        Flush();

    if(mFenceEvent)
        CloseHandle(mFenceEvent);
}

void Dx12Renderer::Initialize(HWND hwnd, int width, int height, const Settings& settings)
{
    mHwnd = hwnd;
    mWidth = width;
    mHeight = height;
    mSettings = settings;

    InitDevice();

    ThrowIfFailed(mDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&mCmdAlloc)));
    ThrowIfFailed(mDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, mCmdAlloc.Get(), nullptr, IID_PPV_ARGS(&mCmdList)));
    ThrowIfFailed(mCmdList->Close());

    ThrowIfFailed(mDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mFence)));
    mFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    CreateSwapChain();
    CreateDescriptorHeaps();
    CreateRenderTargets();
    CreateDepthBuffer();

    BuildRootSignature();
    BuildShadersAndPSO();
    BuildCubeGeometry();

    mSceneCB = std::make_unique<UploadBuffer<SceneCB>>(mDevice.Get(), 1, true);

    D3D12_DESCRIPTOR_HEAP_DESC cbvDesc{};
    cbvDesc.NumDescriptors = 1;
    cbvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    cbvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&cbvDesc, IID_PPV_ARGS(&mCbvHeap)));

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
    cbv.BufferLocation = mSceneCB->Resource()->GetGPUVirtualAddress();
    cbv.SizeInBytes = Align256(sizeof(SceneCB));
    mDevice->CreateConstantBufferView(&cbv, mCbvHeap->GetCPUDescriptorHandleForHeapStart());

    Resize(width, height);
}

void Dx12Renderer::InitDevice()
{
#if defined(_DEBUG)
    {
        ComPtr<ID3D12Debug> dbg;
        if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))))
            dbg->EnableDebugLayer();
    }
#endif

    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&mFactory)));

    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&mDevice));
    if(FAILED(hr))
    {
        ComPtr<IDXGIAdapter> warp;
        ThrowIfFailed(mFactory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
        ThrowIfFailed(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&mDevice)));
    }

    D3D12_COMMAND_QUEUE_DESC q{};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    q.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    ThrowIfFailed(mDevice->CreateCommandQueue(&q, IID_PPV_ARGS(&mQueue)));

    mRtvInc = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    mDsvInc = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    mCbvInc = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void Dx12Renderer::CreateSwapChain()
{
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.BufferCount = mSettings.swapChainBufferCount;
    sd.Width = mWidth;
    sd.Height = mHeight;
    sd.Format = mSettings.backBufferFormat;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> sc1;
    ThrowIfFailed(mFactory->CreateSwapChainForHwnd(
        mQueue.Get(), mHwnd, &sd, nullptr, nullptr, &sc1));

    ThrowIfFailed(sc1.As(&mSwapChain));
    mFrameIndex = mSwapChain->GetCurrentBackBufferIndex();

    mFactory->MakeWindowAssociation(mHwnd, DXGI_MWA_NO_ALT_ENTER);
}

void Dx12Renderer::CreateDescriptorHeaps()
{
    D3D12_DESCRIPTOR_HEAP_DESC rtv{};
    rtv.NumDescriptors = mSettings.swapChainBufferCount;
    rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&mRtvHeap)));


    D3D12_DESCRIPTOR_HEAP_DESC dsv{};
    dsv.NumDescriptors = 1;
    dsv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsv.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&dsv, IID_PPV_ARGS(&mDsvHeap)));
}

void Dx12Renderer::CreateRenderTargets()
{
    mBackBuffers.resize(mSettings.swapChainBufferCount);

    auto handle = mRtvHeap->GetCPUDescriptorHandleForHeapStart();
    for(int i = 0; i < mSettings.swapChainBufferCount; ++i)
    {
        ThrowIfFailed(mSwapChain->GetBuffer(i, IID_PPV_ARGS(&mBackBuffers[i])));
        mDevice->CreateRenderTargetView(mBackBuffers[i].Get(), nullptr, handle);
        handle.ptr += mRtvInc;
    }
}

void Dx12Renderer::CreateDepthBuffer()
{
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Alignment = 0;
    d.Width = mWidth;
    d.Height = mHeight;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = mSettings.depthFormat;
    d.SampleDesc.Count = 1;
    d.SampleDesc.Quality = 0;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = mSettings.depthFormat;
    clear.DepthStencil.Depth = 1.0f;
    clear.DepthStencil.Stencil = 0;

    auto depthHeap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(mDevice->CreateCommittedResource(
        &depthHeap,
        D3D12_HEAP_FLAG_NONE,
        &d,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &clear,
        IID_PPV_ARGS(&mDepth)));

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = mSettings.depthFormat;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsv.Flags = D3D12_DSV_FLAG_NONE;

    mDevice->CreateDepthStencilView(mDepth.Get(), &dsv, mDsvHeap->GetCPUDescriptorHandleForHeapStart());
}

void Dx12Renderer::Resize(int width, int height)
{
    if(!mDevice) return;

    mWidth = std::max(1, width);
    mHeight = std::max(1, height);

    Flush();

    for(auto& bb : mBackBuffers) bb.Reset();
    mDepth.Reset();

    ThrowIfFailed(mSwapChain->ResizeBuffers(
        mSettings.swapChainBufferCount,
        mWidth,
        mHeight,
        mSettings.backBufferFormat,
        0));

    mFrameIndex = mSwapChain->GetCurrentBackBufferIndex();

    CreateRenderTargets();
    CreateDepthBuffer();

    mViewport = {0.0f, 0.0f, static_cast<float>(mWidth), static_cast<float>(mHeight), 0.0f, 1.0f};
    mScissor = {0, 0, mWidth, mHeight};
}

void Dx12Renderer::BuildRootSignature()
{
    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    param.Descriptor.ShaderRegister = 0;
    param.Descriptor.RegisterSpace = 0;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 1;
    desc.pParameters = &param;
    desc.NumStaticSamplers = 0;
    desc.pStaticSamplers = nullptr;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sig;
    ComPtr<ID3DBlob> err;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if(FAILED(hr))
    {
        std::string msg = err ? std::string((const char*)err->GetBufferPointer(), err->GetBufferSize()) : "Root signature error";
        throw std::runtime_error(msg);
    }

    ThrowIfFailed(mDevice->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&mRootSig)));
}

void Dx12Renderer::BuildShadersAndPSO()
{
    UINT flags = 0;
#if defined(_DEBUG)
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> errors;
    ThrowIfFailed(D3DCompileFromFile(L"Phong.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "VSMain", "vs_5_0", flags, 0, &mVS, &errors));

    errors.Reset();
    ThrowIfFailed(D3DCompileFromFile(L"Phong.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        "PSMain", "ps_5_0", flags, 0, &mPS, &errors));

    D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = mRootSig.Get();
    pso.VS = {mVS->GetBufferPointer(), mVS->GetBufferSize()};
    pso.PS = {mPS->GetBufferPointer(), mPS->GetBufferSize()};
    D3D12_BLEND_DESC blend{};
    blend.AlphaToCoverageEnable = FALSE;
    blend.IndependentBlendEnable = FALSE;
    blend.RenderTarget[0].BlendEnable = FALSE;
    blend.RenderTarget[0].LogicOpEnable = FALSE;
    blend.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_RASTERIZER_DESC rast{};
    rast.FillMode = D3D12_FILL_MODE_SOLID;
    rast.CullMode = D3D12_CULL_MODE_BACK;
    rast.FrontCounterClockwise = FALSE;
    rast.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    rast.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    rast.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    rast.DepthClipEnable = TRUE;
    rast.MultisampleEnable = FALSE;
    rast.AntialiasedLineEnable = FALSE;
    rast.ForcedSampleCount = 0;
    rast.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    D3D12_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    ds.StencilEnable = TRUE;
    ds.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    ds.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    D3D12_DEPTH_STENCILOP_DESC sop{};
    sop.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    sop.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    sop.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    sop.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    ds.FrontFace = sop;
    ds.BackFace = sop;

    pso.BlendState = blend;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState = rast;
    pso.DepthStencilState = ds;
    pso.InputLayout = {layout, _countof(layout)};
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = mSettings.backBufferFormat;
    pso.DSVFormat = mSettings.depthFormat;
    pso.SampleDesc.Count = 1;

    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&mPSO)));
}

void Dx12Renderer::BuildCubeGeometry()
{

    const float s = 1.0f;
    std::vector<Vertex> v = {
        // +X
        {{ s,-s,-s},{ 1,0,0}}, {{ s, s,-s},{ 1,0,0}}, {{ s, s, s},{ 1,0,0}}, {{ s,-s, s},{ 1,0,0}},
        // -X
        {{-s,-s, s},{-1,0,0}}, {{-s, s, s},{-1,0,0}}, {{-s, s,-s},{-1,0,0}}, {{-s,-s,-s},{-1,0,0}},
        // +Y
        {{-s, s,-s},{0, 1,0}}, {{-s, s, s},{0, 1,0}}, {{ s, s, s},{0, 1,0}}, {{ s, s,-s},{0, 1,0}},
        // -Y
        {{-s,-s, s},{0,-1,0}}, {{-s,-s,-s},{0,-1,0}}, {{ s,-s,-s},{0,-1,0}}, {{ s,-s, s},{0,-1,0}},
        // +Z
        {{-s,-s, s},{0,0, 1}}, {{ s,-s, s},{0,0, 1}}, {{ s, s, s},{0,0, 1}}, {{-s, s, s},{0,0, 1}},
        // -Z
        {{ s,-s,-s},{0,0,-1}}, {{-s,-s,-s},{0,0,-1}}, {{-s, s,-s},{0,0,-1}}, {{ s, s,-s},{0,0,-1}},
    };

    std::vector<uint16_t> i;
    i.reserve(36);
    for(uint16_t face = 0; face < 6; ++face)
    {
        uint16_t base = face * 4;
        i.push_back(base + 0); i.push_back(base + 1); i.push_back(base + 2);
        i.push_back(base + 0); i.push_back(base + 2); i.push_back(base + 3);
    }

    mIndexCount = static_cast<UINT>(i.size());

    ThrowIfFailed(mCmdAlloc->Reset());
    ThrowIfFailed(mCmdList->Reset(mCmdAlloc.Get(), nullptr));

    mVB = CreateDefaultBuffer(mDevice.Get(), mCmdList.Get(), v.data(), sizeof(Vertex) * v.size(), mVBUpload);
    mIB = CreateDefaultBuffer(mDevice.Get(), mCmdList.Get(), i.data(), sizeof(uint16_t) * i.size(), mIBUpload);

    ThrowIfFailed(mCmdList->Close());
    ID3D12CommandList* lists[] = {mCmdList.Get()};
    mQueue->ExecuteCommandLists(1, lists);
    Flush();

    mVBV.BufferLocation = mVB->GetGPUVirtualAddress();
    mVBV.StrideInBytes = sizeof(Vertex);
    mVBV.SizeInBytes = static_cast<UINT>(sizeof(Vertex) * v.size());

    mIBV.BufferLocation = mIB->GetGPUVirtualAddress();
    mIBV.Format = DXGI_FORMAT_R16_UINT;
    mIBV.SizeInBytes = static_cast<UINT>(sizeof(uint16_t) * i.size());
}

void Dx12Renderer::Update(float dt, float totalTime)
{
    (void)dt;


    float x = mRadius * sinf(mPhi) * cosf(mTheta);
    float z = mRadius * sinf(mPhi) * sinf(mTheta);
    float y = mRadius * cosf(mPhi);

    XMVECTOR eye = XMVectorSet(x, y, z, 1.0f);
    XMVECTOR at = XMVectorZero();
    XMVECTOR up = XMVectorSet(0, 1, 0, 0);

    XMMATRIX view = XMMatrixLookAtLH(eye, at, up);
    XMMATRIX proj = XMMatrixPerspectiveFovLH(0.25f * XM_PI, (float)mWidth / (float)mHeight, 0.1f, 1000.0f);

    XMMATRIX world = XMMatrixRotationY(0.6f * totalTime) * XMMatrixRotationX(0.25f * totalTime);

    SceneCB cb{};
    XMStoreFloat4x4(&cb.world, XMMatrixTranspose(world));
    XMStoreFloat4x4(&cb.viewProj, XMMatrixTranspose(view * proj));

    cb.eyePos = XMFLOAT3(x, y, z);
    cb.lightDir = XMFLOAT3(0.577f, -0.577f, 0.577f);
    cb.lightColor = XMFLOAT3(1.0f, 1.0f, 1.0f);

    mSceneCB->CopyData(0, cb);
}

void Dx12Renderer::Render(float r, float g, float b)
{
    ThrowIfFailed(mCmdAlloc->Reset());
    ThrowIfFailed(mCmdList->Reset(mCmdAlloc.Get(), mPSO.Get()));

    mCmdList->RSSetViewports(1, &mViewport);
    mCmdList->RSSetScissorRects(1, &mScissor);

 
    auto barrier = Transition(CurrentBackBuffer(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    mCmdList->ResourceBarrier(1, &barrier);

    const float clearColor[] = {r, g, b, 1.0f};
    mCmdList->ClearRenderTargetView(CurrentRTV(), clearColor, 0, nullptr);
    mCmdList->ClearDepthStencilView(DSV(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    auto rtv = CurrentRTV();
    auto dsv = DSV();
    mCmdList->OMSetRenderTargets(1, &rtv, TRUE, &dsv);


    ID3D12DescriptorHeap* heaps[] = {mCbvHeap.Get()};
    mCmdList->SetDescriptorHeaps(1, heaps);

    mCmdList->SetGraphicsRootSignature(mRootSig.Get());
 
    mCmdList->SetGraphicsRootConstantBufferView(0, mSceneCB->Resource()->GetGPUVirtualAddress());

    mCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    mCmdList->IASetVertexBuffers(0, 1, &mVBV);
    mCmdList->IASetIndexBuffer(&mIBV);

    mCmdList->DrawIndexedInstanced(mIndexCount, 1, 0, 0, 0);


    barrier = Transition(CurrentBackBuffer(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    mCmdList->ResourceBarrier(1, &barrier);

    ThrowIfFailed(mCmdList->Close());
    ID3D12CommandList* lists[] = {mCmdList.Get()};
    mQueue->ExecuteCommandLists(1, lists);

    ThrowIfFailed(mSwapChain->Present(1, 0));

    MoveToNextFrame();
}

ID3D12Resource* Dx12Renderer::CurrentBackBuffer() const
{
    return mBackBuffers[mFrameIndex].Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE Dx12Renderer::CurrentRTV() const
{
    auto h = mRtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(mFrameIndex) * mRtvInc;
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE Dx12Renderer::DSV() const
{
    return mDsvHeap->GetCPUDescriptorHandleForHeapStart();
}

void Dx12Renderer::Flush()
{
    const UINT64 fenceToWaitFor = ++mFenceValue;
    ThrowIfFailed(mQueue->Signal(mFence.Get(), fenceToWaitFor));

    if(mFence->GetCompletedValue() < fenceToWaitFor)
    {
        ThrowIfFailed(mFence->SetEventOnCompletion(fenceToWaitFor, mFenceEvent));
        WaitForSingleObject(mFenceEvent, INFINITE);
    }
}

void Dx12Renderer::MoveToNextFrame()
{
    Flush();
    mFrameIndex = mSwapChain->GetCurrentBackBufferIndex();
}
