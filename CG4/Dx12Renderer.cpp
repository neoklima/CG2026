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

    // CPU → upload-heap
    void* mapped = nullptr;
    D3D12_RANGE range{0, 0};
    ThrowIfFailed(uploadBuffer->Map(0, &range, &mapped));
    memcpy(mapped, initData, byteSize);
    uploadBuffer->Unmap(0, nullptr);

    // копирование из upload-буфера в default-буфер
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

bool Dx12Renderer::Initialize(HWND hwnd, int width, int height)
{
    mHwnd = hwnd;
    mWidth = width;
    mHeight = height;

#if defined(_DEBUG)
    {
        ComPtr<ID3D12Debug> debug;
        if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            debug->EnableDebugLayer();
    }
#endif

    ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&mFactory)));

    ComPtr<IDXGIAdapter1> adapter;
    for(UINT i = 0; mFactory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);

        if(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;

        if(SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&mDevice))))
            break;
    }

    if(!mDevice)
    {
        ComPtr<IDXGIAdapter> warp;
        ThrowIfFailed(mFactory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
        ThrowIfFailed(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&mDevice)));
    }

    // очередь команд
    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qdesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    ThrowIfFailed(mDevice->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&mCmdQueue)));

    // список команд
    ThrowIfFailed(mDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&mCmdAlloc)));
    ThrowIfFailed(mDevice->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, mCmdAlloc.Get(), nullptr, IID_PPV_ARGS(&mCmdList)));
    ThrowIfFailed(mCmdList->Close());

    // Fence
    ThrowIfFailed(mDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mFence)));
    mFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    // SwapChain
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = mWidth;
    sd.Height = mHeight;
    sd.Format = mBackBufferFormat;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = SwapChainBufferCount;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    // удаляем полноэкранный режим
    ThrowIfFailed(mFactory->MakeWindowAssociation(mHwnd, DXGI_MWA_NO_ALT_ENTER));

    ComPtr<IDXGISwapChain1> swap1;
    ThrowIfFailed(mFactory->CreateSwapChainForHwnd(
        mCmdQueue.Get(),
        mHwnd,
        &sd,
        nullptr,
        nullptr,
        &swap1));

    ThrowIfFailed(swap1.As(&mSwapChain));
    mCurrBackBuffer = 0;

    // RTV-heap: цветовые цели
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.NumDescriptors = SwapChainBufferCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&mRtvHeap)));

    // DSV-heap: дескриптор для буфера глубины
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&mDsvHeap)));

    // heap для CBV: под константный буфер
    D3D12_DESCRIPTOR_HEAP_DESC cbvHeapDesc{};
    cbvHeapDesc.NumDescriptors = 1;
    cbvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    cbvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&cbvHeapDesc, IID_PPV_ARGS(&mCbvHeap)));

    mRtvDescriptorSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    mDsvDescriptorSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    mCbvDescriptorSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    BuildRootSignature();
    BuildShadersAndInputLayout();
    BuildGeometry();
    BuildPSO();

    OnResize(mWidth, mHeight);

    return true;
}

void Dx12Renderer::OnResize(int width, int height)
{
    mWidth = width;
    mHeight = height;

    if(!mDevice || !mSwapChain)
        return;

    Flush();

    // учитываем размера окна
    for(int i = 0; i < SwapChainBufferCount; ++i)
        mSwapChainBuffer[i].Reset();
    mDepthStencilBuffer.Reset();

    ThrowIfFailed(mSwapChain->ResizeBuffers(
        SwapChainBufferCount,
        mWidth,
        mHeight,
        mBackBufferFormat,
        DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH));

    mCurrBackBuffer = 0;

    // пересоздаём RTV для всех back buffer-ов
    auto rtvHandle = mRtvHeap->GetCPUDescriptorHandleForHeapStart();
    for(UINT i = 0; i < SwapChainBufferCount; ++i)
    {
        ThrowIfFailed(mSwapChain->GetBuffer(i, IID_PPV_ARGS(&mSwapChainBuffer[i])));
        mDevice->CreateRenderTargetView(mSwapChainBuffer[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += mRtvDescriptorSize;
    }

    // depth-stencil
    auto depthHeap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    auto depthDesc = Tex2DDesc(
        mDepthStencilFormat,
        mWidth,
        mHeight,
        1, 1,
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);

    D3D12_CLEAR_VALUE optClear{};
    optClear.Format = mDepthStencilFormat;
    optClear.DepthStencil.Depth = 1.0f;
    optClear.DepthStencil.Stencil = 0;

    ThrowIfFailed(mDevice->CreateCommittedResource(
        &depthHeap,
        D3D12_HEAP_FLAG_NONE,
        &depthDesc,
        D3D12_RESOURCE_STATE_COMMON,
        &optClear,
        IID_PPV_ARGS(&mDepthStencilBuffer)));

    // DSV
    mDevice->CreateDepthStencilView(mDepthStencilBuffer.Get(), nullptr, mDsvHeap->GetCPUDescriptorHandleForHeapStart());

    // throw для depth буфера
    ThrowIfFailed(mCmdAlloc->Reset());
    ThrowIfFailed(mCmdList->Reset(mCmdAlloc.Get(), nullptr));

    auto b = Transition(mDepthStencilBuffer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    mCmdList->ResourceBarrier(1, &b);

    ThrowIfFailed(mCmdList->Close());
    ID3D12CommandList* lists[] = { mCmdList.Get() };
    mCmdQueue->ExecuteCommandLists(1, lists);

    Flush();

    // viewport и scissor-область для окна
    mViewport.TopLeftX = 0.0f;
    mViewport.TopLeftY = 0.0f;
    mViewport.Width = static_cast<float>(mWidth);
    mViewport.Height = static_cast<float>(mHeight);
    mViewport.MinDepth = 0.0f;
    mViewport.MaxDepth = 1.0f;

    mScissorRect = {0, 0, mWidth, mHeight};

    BuildConstantBuffer();
}

void Dx12Renderer::Update(float dt)
{
    mTheta += dt * 0.5f;

    // камера
    XMVECTOR pos = XMVectorSet(
        mRadius * sinf(mPhi) * cosf(mTheta),
        mRadius * cosf(mPhi),
        mRadius * sinf(mPhi) * sinf(mTheta),
        1.0f);

    XMVECTOR target = XMVectorZero();
    XMVECTOR up = XMVectorSet(0, 1, 0, 0);

    XMMATRIX view = XMMatrixLookAtLH(pos, target, up);

    XMMATRIX proj = XMMatrixPerspectiveFovLH(0.25f * XM_PI, AspectRatio(), 0.1f, 100.0f);

    XMMATRIX world = XMMatrixIdentity();
    XMMATRIX wvp = world * view * proj;

    // матрицы + свет
    ObjectConstants obj{};
    XMStoreFloat4x4(&obj.WorldViewProj, XMMatrixTranspose(wvp));

    obj.LightDir = XMFLOAT3(0.577f, -0.577f, 0.577f);
    obj.LightColor = XMFLOAT3(1.0f, 1.0f, 1.0f);
    obj.Ambient = XMFLOAT3(0.2f, 0.2f, 0.2f);

    mObjectCB->CopyData(0, obj);
}

void Dx12Renderer::Draw()
{
    ThrowIfFailed(mCmdAlloc->Reset());
    ThrowIfFailed(mCmdList->Reset(mCmdAlloc.Get(), mPSO.Get()));

    mCmdList->RSSetViewports(1, &mViewport);
    mCmdList->RSSetScissorRects(1, &mScissorRect);

    // Переводим текущий back buffer в состояние "цель рендеринга".
    auto toRT = Transition(CurrentBackBuffer(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    mCmdList->ResourceBarrier(1, &toRT);

    
    auto rtv = CurrentBackBufferView();
    auto dsv = DepthStencilView();


    mCmdList->OMSetRenderTargets(1, &rtv, TRUE, &dsv);

    const float clearColor[] = { mClearColor.x, mClearColor.y, mClearColor.z, 1.0f };
    mCmdList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    mCmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    mCmdList->SetGraphicsRootSignature(mRootSignature.Get());

    ID3D12DescriptorHeap* heaps[] = { mCbvHeap.Get() };
    mCmdList->SetDescriptorHeaps(_countof(heaps), heaps);

    mCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    mCmdList->IASetVertexBuffers(0, 1, &mVBView);
    mCmdList->IASetIndexBuffer(&mIBView);

    // отрисовка куба
    // устанавливаем GPU-адрес константного буфера
    mCmdList->SetGraphicsRootConstantBufferView(0, mObjectCB->Resource()->GetGPUVirtualAddress());
    mCmdList->DrawIndexedInstanced(mIndexCount, 1, 0, 0, 0);

    // возвращаем back buffer в состояние показа на экран 
    auto toPresent = Transition(CurrentBackBuffer(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    mCmdList->ResourceBarrier(1, &toPresent);

    ThrowIfFailed(mCmdList->Close());
    ID3D12CommandList* lists[] = { mCmdList.Get() };
    mCmdQueue->ExecuteCommandLists(1, lists);

    ThrowIfFailed(mSwapChain->Present(1, 0));
    mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;


    Flush();
}

void Dx12Renderer::OnMouseDown(WPARAM btnState, int x, int y)
{
    mLastMousePos.x = x;
    mLastMousePos.y = y;

    SetCapture(mHwnd);
}

void Dx12Renderer::OnMouseUp(WPARAM btnState, int x, int y)
{
    ReleaseCapture();
}

void Dx12Renderer::OnMouseMove(WPARAM btnState, int x, int y)
{
    if((btnState & MK_LBUTTON) != 0)
    {
        float dx = XMConvertToRadians(0.25f * static_cast<float>(x - mLastMousePos.x));
        float dy = XMConvertToRadians(0.25f * static_cast<float>(y - mLastMousePos.y));

        mTheta += dx;
        mPhi += dy;

        mPhi = std::clamp(mPhi, 0.1f, XM_PI - 0.1f);
    }
    else if((btnState & MK_RBUTTON) != 0)
    {
        float dx = 0.2f * static_cast<float>(x - mLastMousePos.x);
        float dy = 0.2f * static_cast<float>(y - mLastMousePos.y);

        mRadius += dx - dy;
        mRadius = std::clamp(mRadius, 2.0f, 50.0f);
    }

    mLastMousePos.x = x;
    mLastMousePos.y = y;
}

float Dx12Renderer::AspectRatio() const
{
    return static_cast<float>(mWidth) / static_cast<float>(mHeight);
}

ID3D12Resource* Dx12Renderer::CurrentBackBuffer() const
{
    return mSwapChainBuffer[mCurrBackBuffer].Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE Dx12Renderer::CurrentBackBufferView() const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = mRtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += mCurrBackBuffer * mRtvDescriptorSize;
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE Dx12Renderer::DepthStencilView() const
{
    return mDsvHeap->GetCPUDescriptorHandleForHeapStart();
}

void Dx12Renderer::Flush()
{
    mCurrentFence++;

    ThrowIfFailed(mCmdQueue->Signal(mFence.Get(), mCurrentFence));

    if(mFence->GetCompletedValue() < mCurrentFence)
    {
        ThrowIfFailed(mFence->SetEventOnCompletion(mCurrentFence, mFenceEvent));
        WaitForSingleObject(mFenceEvent, INFINITE);
    }
}

void Dx12Renderer::BuildConstantBuffer()
{
    // выделяем константный буфер под один объект
    mObjectCB = std::make_unique<UploadBuffer<ObjectConstants>>(mDevice.Get(), 1, true);

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
    cbvDesc.BufferLocation = mObjectCB->Resource()->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = Align256(sizeof(ObjectConstants));

    auto cpuHandle = mCbvHeap->GetCPUDescriptorHandleForHeapStart();
    mDevice->CreateConstantBufferView(&cbvDesc, cpuHandle);
}

void Dx12Renderer::BuildRootSignature()
{
    // RootSignature: один параметр — CBV 
    D3D12_ROOT_PARAMETER slotRootParameter[1]{};
    slotRootParameter[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    slotRootParameter[0].Descriptor.ShaderRegister = 0;
    slotRootParameter[0].Descriptor.RegisterSpace = 0;
    slotRootParameter[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc{};
    rootSigDesc.NumParameters = 1;
    rootSigDesc.pParameters = slotRootParameter;
    rootSigDesc.NumStaticSamplers = 0;
    rootSigDesc.pStaticSamplers = nullptr;
    rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serializedRootSig = nullptr;
    ComPtr<ID3DBlob> errorBlob = nullptr;
    ThrowIfFailed(D3D12SerializeRootSignature(
        &rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf()));

    ThrowIfFailed(mDevice->CreateRootSignature(
        0,
        serializedRootSig->GetBufferPointer(),
        serializedRootSig->GetBufferSize(),
        IID_PPV_ARGS(&mRootSignature)));
}

void Dx12Renderer::BuildShadersAndInputLayout()
{
    mVS = CompileShader(L"Phong.hlsl", nullptr, "VSMain", "vs_5_1");
    mPS = CompileShader(L"Phong.hlsl", nullptr, "PSMain", "ps_5_1");

    mInputLayout =
    {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
}

void Dx12Renderer::BuildGeometry()
{
    // 24 вершины для нормалей
    Vertex vertices[] =
    {
        // Грань X
        {{+0.5f, -0.5f, -0.5f}, {+1.0f, 0.0f, 0.0f}},
        {{+0.5f, +0.5f, -0.5f}, {+1.0f, 0.0f, 0.0f}},
        {{+0.5f, +0.5f, +0.5f}, {+1.0f, 0.0f, 0.0f}},
        {{+0.5f, -0.5f, +0.5f}, {+1.0f, 0.0f, 0.0f}},

        // Грань -X
        {{-0.5f, -0.5f, +0.5f}, {-1.0f, 0.0f, 0.0f}},
        {{-0.5f, +0.5f, +0.5f}, {-1.0f, 0.0f, 0.0f}},
        {{-0.5f, +0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}},
        {{-0.5f, -0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}},

        // Грань Y
        {{-0.5f, +0.5f, -0.5f}, {0.0f, +1.0f, 0.0f}},
        {{-0.5f, +0.5f, +0.5f}, {0.0f, +1.0f, 0.0f}},
        {{+0.5f, +0.5f, +0.5f}, {0.0f, +1.0f, 0.0f}},
        {{+0.5f, +0.5f, -0.5f}, {0.0f, +1.0f, 0.0f}},

        // Грань -Y
        {{-0.5f, -0.5f, +0.5f}, {0.0f, -1.0f, 0.0f}},
        {{-0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}},
        {{+0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}},
        {{+0.5f, -0.5f, +0.5f}, {0.0f, -1.0f, 0.0f}},

        // Грань Z
        {{+0.5f, -0.5f, +0.5f}, {0.0f, 0.0f, +1.0f}},
        {{+0.5f, +0.5f, +0.5f}, {0.0f, 0.0f, +1.0f}},
        {{-0.5f, +0.5f, +0.5f}, {0.0f, 0.0f, +1.0f}},
        {{-0.5f, -0.5f, +0.5f}, {0.0f, 0.0f, +1.0f}},

        // Грань -Z
        {{-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}},
        {{-0.5f, +0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}},
        {{+0.5f, +0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}},
        {{+0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}},
    };

    std::uint16_t indices[] =
    {
        0, 1, 2, 0, 2, 3,
        4, 5, 6, 4, 6, 7,
        8, 9,10, 8,10,11,
       12,13,14,12,14,15,
       16,17,18,16,18,19,
       20,21,22,20,22,23
    };
    mIndexCount = static_cast<UINT>(_countof(indices));

    const UINT vbByteSize = static_cast<UINT>(sizeof(vertices));
    const UINT ibByteSize = static_cast<UINT>(sizeof(indices));

    ThrowIfFailed(mCmdAlloc->Reset());
    ThrowIfFailed(mCmdList->Reset(mCmdAlloc.Get(), nullptr));

    ComPtr<ID3D12Resource> vbUpload, ibUpload;
    mVertexBuffer = CreateDefaultBuffer(mDevice.Get(), mCmdList.Get(), vertices, vbByteSize, vbUpload);
    mIndexBuffer  = CreateDefaultBuffer(mDevice.Get(), mCmdList.Get(), indices,  ibByteSize, ibUpload);

    ThrowIfFailed(mCmdList->Close());
    ID3D12CommandList* lists[] = { mCmdList.Get() };
    mCmdQueue->ExecuteCommandLists(1, lists);

    Flush();

    mVBView.BufferLocation = mVertexBuffer->GetGPUVirtualAddress();
    mVBView.StrideInBytes = sizeof(Vertex);
    mVBView.SizeInBytes = vbByteSize;

    mIBView.BufferLocation = mIndexBuffer->GetGPUVirtualAddress();
    mIBView.Format = DXGI_FORMAT_R16_UINT;
    mIBView.SizeInBytes = ibByteSize;
}

void Dx12Renderer::BuildPSO()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.InputLayout = { mInputLayout.data(), (UINT)mInputLayout.size() };
    psoDesc.pRootSignature = mRootSignature.Get();
    psoDesc.VS =
    {
        reinterpret_cast<BYTE*>(mVS->GetBufferPointer()),
        mVS->GetBufferSize()
    };
    psoDesc.PS =
    {
        reinterpret_cast<BYTE*>(mPS->GetBufferPointer()),
        mPS->GetBufferSize()
    };
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = mBackBufferFormat;
    psoDesc.DSVFormat = mDepthStencilFormat;
    psoDesc.SampleDesc.Count = 1;

    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPSO)));
}
