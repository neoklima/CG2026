#include "RenderingSystem.h"

#include <algorithm>
#include <cfloat>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <unordered_map>

using namespace DirectX;

static std::wstring FindAssetPath(const std::wstring& relative)
{
    namespace fs = std::filesystem;

    auto exists = [](const fs::path& p) {
        std::error_code ec;
        return fs::exists(p, ec) && fs::is_regular_file(p, ec);
    };

    fs::path rel(relative);

    fs::path p1 = fs::current_path() / rel;
    if(exists(p1))
        return p1.wstring();

    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    fs::path exeDir = fs::path(exePath).parent_path();
    fs::path p2 = exeDir / rel;
    if(exists(p2))
        return p2.wstring();

    fs::path p3 = exeDir / L".." / L".." / rel;
    if(exists(p3))
        return fs::weakly_canonical(p3).wstring();

    return relative;
}

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

struct ImageData
{
    UINT width = 0;
    UINT height = 0;
    std::vector<uint8_t> rgba;
};

static std::string Trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if(first == std::string::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

static bool ReadPpmToken(std::istream& stream, std::string& token)
{
    token.clear();
    char c = 0;
    while(stream.get(c))
    {
        if(c == '#')
        {
            std::string ignored;
            std::getline(stream, ignored);
            continue;
        }
        if(!std::isspace(static_cast<unsigned char>(c)))
        {
            token.push_back(c);
            break;
        }
    }

    while(stream.get(c))
    {
        if(std::isspace(static_cast<unsigned char>(c)))
            break;
        token.push_back(c);
    }
    return !token.empty();
}

static bool LoadPpmImage(const std::wstring& path, ImageData& outImage)
{
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if(!file)
        return false;

    std::string magic, token;
    if(!ReadPpmToken(file, magic) || (magic != "P3" && magic != "P6"))
        return false;
    if(!ReadPpmToken(file, token)) return false;
    const int width = std::stoi(token);
    if(!ReadPpmToken(file, token)) return false;
    const int height = std::stoi(token);
    if(!ReadPpmToken(file, token)) return false;
    const int maxValue = std::stoi(token);
    if(width <= 0 || height <= 0 || maxValue <= 0 || maxValue > 65535)
        return false;

    outImage.width = static_cast<UINT>(width);
    outImage.height = static_cast<UINT>(height);
    outImage.rgba.resize(static_cast<size_t>(width) * height * 4u);

    if(magic == "P3")
    {
        for(size_t pixel = 0; pixel < static_cast<size_t>(width) * height; ++pixel)
        {
            for(int channel = 0; channel < 3; ++channel)
            {
                if(!ReadPpmToken(file, token)) return false;
                const int value = std::clamp(std::stoi(token), 0, maxValue);
                outImage.rgba[pixel * 4 + channel] = static_cast<uint8_t>((value * 255) / maxValue);
            }
            outImage.rgba[pixel * 4 + 3] = 255;
        }
    }
    else
    {
        if(maxValue > 255)
            return false;
        std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3u);
        file.read(reinterpret_cast<char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
        if(file.gcount() != static_cast<std::streamsize>(rgb.size()))
            return false;
        for(size_t pixel = 0; pixel < static_cast<size_t>(width) * height; ++pixel)
        {
            outImage.rgba[pixel * 4 + 0] = static_cast<uint8_t>((rgb[pixel * 3 + 0] * 255) / maxValue);
            outImage.rgba[pixel * 4 + 1] = static_cast<uint8_t>((rgb[pixel * 3 + 1] * 255) / maxValue);
            outImage.rgba[pixel * 4 + 2] = static_cast<uint8_t>((rgb[pixel * 3 + 2] * 255) / maxValue);
            outImage.rgba[pixel * 4 + 3] = 255;
        }
    }
    return true;
}

static bool LoadWicImage(const std::wstring& path, ImageData& outImage)
{
    struct ComScope
    {
        bool ownsInitialization = false;
        ComScope()
        {
            const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            ownsInitialization = SUCCEEDED(hr);
        }
        ~ComScope() { if(ownsInitialization) CoUninitialize(); }
    } comScope;

    ComPtr<IWICImagingFactory> factory;
    if(FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return false;

    ComPtr<IWICBitmapDecoder> decoder;
    if(FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder)))
        return false;

    ComPtr<IWICBitmapFrameDecode> frame;
    if(FAILED(decoder->GetFrame(0, &frame)))
        return false;

    UINT width = 0, height = 0;
    if(FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0)
        return false;

    ComPtr<IWICFormatConverter> converter;
    if(FAILED(factory->CreateFormatConverter(&converter)))
        return false;
    if(FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return false;

    outImage.width = width;
    outImage.height = height;
    outImage.rgba.resize(static_cast<size_t>(width) * height * 4u);
    return SUCCEEDED(converter->CopyPixels(nullptr, width * 4,
        static_cast<UINT>(outImage.rgba.size()), outImage.rgba.data()));
}

static bool LoadImage(const std::wstring& path, ImageData& outImage)
{
    std::wstring extension = std::filesystem::path(path).extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
    return extension == L".ppm" ? LoadPpmImage(path, outImage) : LoadWicImage(path, outImage);
}

static ImageData MakeFallbackTexture()
{
    ImageData image;
    image.width = 8;
    image.height = 8;
    image.rgba.resize(8u * 8u * 4u);
    for(UINT y = 0; y < image.height; ++y)
    {
        for(UINT x = 0; x < image.width; ++x)
        {
            const bool light = ((x / 2) + (y / 2)) % 2 == 0;
            const size_t p = (static_cast<size_t>(y) * image.width + x) * 4u;
            image.rgba[p + 0] = light ? 230 : 62;
            image.rgba[p + 1] = light ? 220 : 72;
            image.rgba[p + 2] = light ? 190 : 92;
            image.rgba[p + 3] = 255;
        }
    }
    return image;
}

static ImageData MakeSolidTexture(uint8_t r, uint8_t g, uint8_t b)
{
    ImageData image;
    image.width = 2;
    image.height = 2;
    image.rgba.resize(2u * 2u * 4u);
    for(size_t pixel = 0; pixel < 4; ++pixel)
    {
        image.rgba[pixel * 4 + 0] = r;
        image.rgba[pixel * 4 + 1] = g;
        image.rgba[pixel * 4 + 2] = b;
        image.rgba[pixel * 4 + 3] = 255;
    }
    return image;
}

template<typename TVertex, typename TIndex>
static void CalculateTangents(std::vector<TVertex>& vertices,
    const std::vector<TIndex>& indices)
{
    std::vector<XMFLOAT3> tangents(vertices.size(), XMFLOAT3(0.0f, 0.0f, 0.0f));
    std::vector<XMFLOAT3> bitangents(vertices.size(), XMFLOAT3(0.0f, 0.0f, 0.0f));

    for(size_t triangle = 0; triangle + 2 < indices.size(); triangle += 3)
    {
        const uint32_t i0 = static_cast<uint32_t>(indices[triangle + 0]);
        const uint32_t i1 = static_cast<uint32_t>(indices[triangle + 1]);
        const uint32_t i2 = static_cast<uint32_t>(indices[triangle + 2]);
        if(i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size())
            continue;

        const XMFLOAT3& p0 = vertices[i0].pos;
        const XMFLOAT3& p1 = vertices[i1].pos;
        const XMFLOAT3& p2 = vertices[i2].pos;
        const XMFLOAT2& uv0 = vertices[i0].texCoord;
        const XMFLOAT2& uv1 = vertices[i1].texCoord;
        const XMFLOAT2& uv2 = vertices[i2].texCoord;

        const float x1 = p1.x - p0.x;
        const float y1 = p1.y - p0.y;
        const float z1 = p1.z - p0.z;
        const float x2 = p2.x - p0.x;
        const float y2 = p2.y - p0.y;
        const float z2 = p2.z - p0.z;
        const float s1 = uv1.x - uv0.x;
        const float t1 = uv1.y - uv0.y;
        const float s2 = uv2.x - uv0.x;
        const float t2 = uv2.y - uv0.y;
        const float determinant = s1 * t2 - s2 * t1;
        if(std::abs(determinant) < 1.0e-8f)
            continue;

        const float reciprocal = 1.0f / determinant;
        const XMFLOAT3 tangent{
            (x1 * t2 - x2 * t1) * reciprocal,
            (y1 * t2 - y2 * t1) * reciprocal,
            (z1 * t2 - z2 * t1) * reciprocal};
        const XMFLOAT3 bitangent{
            (x2 * s1 - x1 * s2) * reciprocal,
            (y2 * s1 - y1 * s2) * reciprocal,
            (z2 * s1 - z1 * s2) * reciprocal};

        for(uint32_t index : {i0, i1, i2})
        {
            tangents[index].x += tangent.x;
            tangents[index].y += tangent.y;
            tangents[index].z += tangent.z;
            bitangents[index].x += bitangent.x;
            bitangents[index].y += bitangent.y;
            bitangents[index].z += bitangent.z;
        }
    }

    for(size_t index = 0; index < vertices.size(); ++index)
    {
        XMVECTOR normal = XMVector3Normalize(XMLoadFloat3(&vertices[index].normal));
        XMVECTOR tangent = XMLoadFloat3(&tangents[index]);
        tangent = XMVectorSubtract(tangent, XMVectorScale(normal,
            XMVectorGetX(XMVector3Dot(normal, tangent))));

        if(XMVectorGetX(XMVector3LengthSq(tangent)) < 1.0e-8f)
        {
            const XMVECTOR axis = std::abs(XMVectorGetY(normal)) < 0.999f
                ? XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)
                : XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
            tangent = XMVector3Cross(axis, normal);
        }
        tangent = XMVector3Normalize(tangent);

        const XMVECTOR bitangent = XMLoadFloat3(&bitangents[index]);
        const float handedness = XMVectorGetX(XMVector3Dot(
            XMVector3Cross(normal, tangent), bitangent)) < 0.0f ? -1.0f : 1.0f;

        XMFLOAT3 tangentValue{};
        XMStoreFloat3(&tangentValue, tangent);
        vertices[index].tangent = XMFLOAT4(
            tangentValue.x, tangentValue.y, tangentValue.z, handedness);
    }
}

RenderingSystem::~RenderingSystem()
{
    if(mDevice)
        Flush();

    if(mFenceEvent)
        CloseHandle(mFenceEvent);
}

void RenderingSystem::Initialize(HWND hwnd, int width, int height, const Settings& settings)
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
    BuildSponzaGeometry();
    BuildObjectScene();

    mSceneCB = std::make_unique<UploadBuffer<SceneCB>>(
        mDevice.Get(), static_cast<UINT>(mObjectWorlds.size()), true);
    BuildMaterialResources();
    mGBuffer.Initialize(mDevice.Get(), width, height, mSrvHeap.Get(), mGBufferSrvOffset);
    mLightingCB = std::make_unique<UploadBuffer<LightingCB>>(mDevice.Get(), 1, true);
    BuildDefaultLights();
    BuildShadowResources();
    BuildParticleResources();
    CreateSceneColorTarget();

    BuildRootSignatures();
    BuildShadersAndPSOs();

    mViewport = {0.0f, 0.0f, static_cast<float>(mWidth), static_cast<float>(mHeight), 0.0f, 1.0f};
    mScissor = {0, 0, mWidth, mHeight};
}

bool RenderingSystem::LoadObjSimple(const std::wstring& path,
    std::vector<Vertex>& outVertices,
    std::vector<uint32_t>& outIndices,
    std::vector<ParsedSubset>& outSubsets,
    std::wstring& outMaterialLibrary,
    XMFLOAT3& outMin,
    XMFLOAT3& outMax)
{
    namespace fs = std::filesystem;

    std::ifstream file(path);
    if(!file.is_open())
        return false;

    std::vector<XMFLOAT3> positions;
    std::vector<XMFLOAT3> normals;
    std::vector<XMFLOAT2> texCoords;

    positions.reserve(100000);
    normals.reserve(100000);
    texCoords.reserve(100000);

    outVertices.clear();
    outIndices.clear();
    outSubsets.clear();
    outMaterialLibrary.clear();
    outVertices.reserve(200000);
    outIndices.reserve(400000);

    outMin = XMFLOAT3(+FLT_MAX, +FLT_MAX, +FLT_MAX);
    outMax = XMFLOAT3(-FLT_MAX, -FLT_MAX, -FLT_MAX);

    struct Key
    {
        int v = 0;
        int t = 0;
        int n = 0;
        bool operator==(const Key& o) const noexcept { return v == o.v && t == o.t && n == o.n; }
    };

    struct KeyHash
    {
        size_t operator()(const Key& k) const noexcept
        {
            return (static_cast<size_t>(k.v) * 73856093u) ^
                (static_cast<size_t>(k.t) * 83492791u) ^
                (static_cast<size_t>(k.n) * 19349663u);
        }
    };

    std::unordered_map<Key, uint32_t, KeyHash> remap;
    remap.reserve(200000);

    auto fixIndex = [](int idx, int count) -> int {
        if(idx > 0) return idx - 1;
        if(idx < 0) return count + idx;
        return -1;
    };

    auto parseFaceVertex = [](const std::string& token, int& outV, int& outT, int& outN)
    {
        outV = 0;
        outT = 0;
        outN = 0;

        int v = 0, vt = 0, vn = 0;

        size_t s1 = token.find('/');
        if(s1 == std::string::npos)
        {
            v = std::stoi(token);
        }
        else
        {
            std::string a = token.substr(0, s1);
            v = a.empty() ? 0 : std::stoi(a);

            size_t s2 = token.find('/', s1 + 1);
            if(s2 == std::string::npos)
            {
                std::string b = token.substr(s1 + 1);
                vt = b.empty() ? 0 : std::stoi(b);
            }
            else
            {
                std::string b = token.substr(s1 + 1, s2 - (s1 + 1));
                std::string c = token.substr(s2 + 1);
                vt = b.empty() ? 0 : std::stoi(b);
                vn = c.empty() ? 0 : std::stoi(c);
            }
        }

        outV = v;
        outT = vt;
        outN = vn;
    };

    std::string currentMaterial = "default";
    UINT currentSubsetStart = 0;
    auto finishSubset = [&]()
    {
        const UINT end = static_cast<UINT>(outIndices.size());
        if(end > currentSubsetStart)
            outSubsets.push_back({currentMaterial, currentSubsetStart, end - currentSubsetStart});
        currentSubsetStart = end;
    };

    std::string line;
    while(std::getline(file, line))
    {
        if(line.empty())
            continue;


        if(line[0] == '#')
            continue;

        std::istringstream iss(line);
        std::string tag;
        iss >> tag;

        if(tag == "mtllib")
        {
            std::string fileName;
            std::getline(iss, fileName);
            fileName = Trim(fileName);
            if(!fileName.empty())
                outMaterialLibrary = (std::filesystem::path(path).parent_path() / ToWString(fileName)).wstring();
        }
        else if(tag == "usemtl")
        {
            finishSubset();
            std::getline(iss, currentMaterial);
            currentMaterial = Trim(currentMaterial);
            if(currentMaterial.empty())
                currentMaterial = "default";
        }
        else if(tag == "v")
        {
            XMFLOAT3 p{};
            iss >> p.x >> p.y >> p.z;
            positions.push_back(p);
        }
        else if(tag == "vt")
        {
            XMFLOAT2 uv{};
            iss >> uv.x >> uv.y;
            texCoords.push_back(uv);
        }
        else if(tag == "vn")
        {
            XMFLOAT3 n{};
            iss >> n.x >> n.y >> n.z;
            normals.push_back(n);
        }
        else if(tag == "f")
        {
            std::vector<std::string> tokens;
            tokens.reserve(8);

            std::string tok;
            while(iss >> tok)
                tokens.push_back(tok);

            if(tokens.size() < 3)
                continue;

            auto emit = [&](const std::string& t) -> uint32_t
            {
                int vRaw = 0, tRaw = 0, nRaw = 0;
                parseFaceVertex(t, vRaw, tRaw, nRaw);

                int vi = fixIndex(vRaw, (int)positions.size());
                int ti = fixIndex(tRaw, (int)texCoords.size());
                int ni = fixIndex(nRaw, (int)normals.size());

                if(vi < 0 || vi >= (int)positions.size())
                    return 0;

                Key key{vi, ti, ni};
                auto it = remap.find(key);
                if(it != remap.end())
                    return it->second;

                Vertex vx{};
                vx.pos = positions[vi];
                if(ni >= 0 && ni < (int)normals.size())
                    vx.normal = normals[ni];
                else
                    vx.normal = XMFLOAT3(0, 1, 0);
                if(ti >= 0 && ti < (int)texCoords.size())
                    vx.texCoord = XMFLOAT2(texCoords[ti].x, 1.0f - texCoords[ti].y);
                else
                    vx.texCoord = XMFLOAT2(0.0f, 0.0f);

                outMin.x = std::min(outMin.x, vx.pos.x);
                outMin.y = std::min(outMin.y, vx.pos.y);
                outMin.z = std::min(outMin.z, vx.pos.z);
                outMax.x = std::max(outMax.x, vx.pos.x);
                outMax.y = std::max(outMax.y, vx.pos.y);
                outMax.z = std::max(outMax.z, vx.pos.z);

                uint32_t newIndex = (uint32_t)outVertices.size();
                outVertices.push_back(vx);
                remap.emplace(key, newIndex);
                return newIndex;
            };

            for(size_t k = 1; k + 1 < tokens.size(); ++k)
            {
                uint32_t i0 = emit(tokens[0]);
                uint32_t i1 = emit(tokens[k]);
                uint32_t i2 = emit(tokens[k + 1]);
                outIndices.push_back(i0);
                outIndices.push_back(i1);
                outIndices.push_back(i2);
            }
        }
    }

    finishSubset();
    return !outVertices.empty() && !outIndices.empty();
}

bool RenderingSystem::LoadMaterialLibrary(const std::wstring& path, std::vector<Material>& outMaterials)
{
    std::ifstream file{std::filesystem::path(path)};
    if(!file)
        return false;

    outMaterials.clear();
    Material* current = nullptr;
    std::string line;
    while(std::getline(file, line))
    {
        line = Trim(line);
        if(line.empty() || line[0] == '#')
            continue;

        std::istringstream iss(line);
        std::string tag;
        iss >> tag;
        if(tag == "newmtl")
        {
            std::string name;
            std::getline(iss, name);
            outMaterials.push_back({});
            current = &outMaterials.back();
            current->name = Trim(name);
        }
        else if(current && tag == "Kd")
        {
            iss >> current->diffuse.x >> current->diffuse.y >> current->diffuse.z;
        }
        else if(current && tag == "Ks")
        {
            iss >> current->specular.x >> current->specular.y >> current->specular.z;
        }
        else if(current && tag == "Ns")
        {
            iss >> current->shininess;
            current->shininess = std::clamp(current->shininess, 1.0f, 256.0f);
        }
        else if(current && tag == "d")
        {
            iss >> current->diffuse.w;
        }
        else if(current && tag == "Tr")
        {
            float transparency = 0.0f;
            iss >> transparency;
            current->diffuse.w = 1.0f - transparency;
        }
        else if(current && tag == "map_Kd")
        {
            std::string mapLine;
            std::getline(iss, mapLine);
            mapLine = Trim(mapLine);
            if(!mapLine.empty())
            {
                std::istringstream mapStream(mapLine);
                std::string token;
                while(mapStream >> token)
                    mapLine = token;
                if(mapLine.size() >= 2 && mapLine.front() == '"' && mapLine.back() == '"')
                    mapLine = mapLine.substr(1, mapLine.size() - 2);
                current->diffuseMap = (std::filesystem::path(path).parent_path() / ToWString(mapLine)).wstring();
            }
        }
    }
    return !outMaterials.empty();
}

void RenderingSystem::BuildSponzaGeometry()
{
    const std::wstring rel = L"Assets\\sponza.obj";
    const std::wstring objPath = FindAssetPath(rel);

    std::vector<Vertex> verts;
    std::vector<uint32_t> inds;
    std::vector<ParsedSubset> parsedSubsets;
    std::wstring materialLibrary;
    XMFLOAT3 bmin{}, bmax{};

    if(!LoadObjSimple(objPath, verts, inds, parsedSubsets, materialLibrary, bmin, bmax))
        throw std::runtime_error("Assets\\sponza.obj is required for the CG2.4 scene");

    XMFLOAT3 center{ (bmin.x + bmax.x) * 0.5f, (bmin.y + bmax.y) * 0.5f, (bmin.z + bmax.z) * 0.5f };
    XMFLOAT3 ext{ (bmax.x - bmin.x), (bmax.y - bmin.y), (bmax.z - bmin.z) };
    float maxExtent = std::max(ext.x, std::max(ext.y, ext.z));
    float inv = (maxExtent > 0.00001f) ? (1.0f / maxExtent) : 1.0f;
    float scale = inv * 50.0f; 

    XMFLOAT3 scaledMin(+FLT_MAX, +FLT_MAX, +FLT_MAX);
    XMFLOAT3 scaledMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    for(auto& v : verts)
    {
        v.pos.x = (v.pos.x - center.x) * scale;
        v.pos.y = (v.pos.y - center.y) * scale;
        v.pos.z = (v.pos.z - center.z) * scale;
        scaledMin.x = std::min(scaledMin.x, v.pos.x);
        scaledMin.y = std::min(scaledMin.y, v.pos.y);
        scaledMin.z = std::min(scaledMin.z, v.pos.z);
        scaledMax.x = std::max(scaledMax.x, v.pos.x);
        scaledMax.y = std::max(scaledMax.y, v.pos.y);
        scaledMax.z = std::max(scaledMax.z, v.pos.z);
    }
    mSponzaLocalBounds = BoundingBox(
        XMFLOAT3(
            (scaledMin.x + scaledMax.x) * 0.5f,
            (scaledMin.y + scaledMax.y) * 0.5f,
            (scaledMin.z + scaledMax.z) * 0.5f),
        XMFLOAT3(
            (scaledMax.x - scaledMin.x) * 0.5f,
            (scaledMax.y - scaledMin.y) * 0.5f,
            (scaledMax.z - scaledMin.z) * 0.5f));

    std::vector<Material> declaredMaterials;
    LoadMaterialLibrary(materialLibrary, declaredMaterials);

    std::unordered_map<std::string, Material> declarations;
    for(const Material& material : declaredMaterials)
        declarations.emplace(material.name, material);

    mMaterials.clear();
    mDrawSubsets.clear();
    mSponzaSubsetStart = 0;
    std::unordered_map<std::string, UINT> materialIndices;
    for(const ParsedSubset& parsed : parsedSubsets)
    {
        UINT materialIndex = 0;
        const auto existing = materialIndices.find(parsed.materialName);
        if(existing != materialIndices.end())
        {
            materialIndex = existing->second;
        }
        else
        {
            Material material;
            const auto declaration = declarations.find(parsed.materialName);
            if(declaration != declarations.end())
            {
                material = declaration->second;
            }
            else
            {
                material.name = parsed.materialName;
                const size_t hash = std::hash<std::string>{}(parsed.materialName);
                material.diffuse = XMFLOAT4(
                    0.45f + 0.35f * static_cast<float>((hash >> 0) & 0xff) / 255.0f,
                    0.45f + 0.35f * static_cast<float>((hash >> 8) & 0xff) / 255.0f,
                    0.45f + 0.35f * static_cast<float>((hash >> 16) & 0xff) / 255.0f,
                    1.0f);
            }
            materialIndex = static_cast<UINT>(mMaterials.size());
            materialIndices.emplace(parsed.materialName, materialIndex);
            mMaterials.push_back(material);
        }
        mDrawSubsets.push_back({parsed.indexStart, parsed.indexCount, materialIndex});
    }

    if(mMaterials.empty())
        mMaterials.push_back({"default"});
    if(mDrawSubsets.empty())
        mDrawSubsets.push_back({0, static_cast<UINT>(inds.size()), 0});

    mSponzaSubsetCount = static_cast<UINT>(mDrawSubsets.size());

    constexpr std::array<uint32_t, RockLodCount> rockSlices = {12, 8, 6};
    constexpr std::array<uint32_t, RockLodCount> rockStacks = {7, 5, 4};
    std::array<DrawSubset, RockLodCount> rockSubsets{};
    XMFLOAT3 rockMin(+FLT_MAX, +FLT_MAX, +FLT_MAX);
    XMFLOAT3 rockMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);

    for(UINT lod = 0; lod < RockLodCount; ++lod)
    {
        const uint32_t slices = rockSlices[lod];
        const uint32_t stacks = rockStacks[lod];
        const uint32_t rockVertexStart = static_cast<uint32_t>(verts.size());
        const uint32_t rockIndexStart = static_cast<uint32_t>(inds.size());

        for(uint32_t stack = 0; stack <= stacks; ++stack)
        {
            const float v = static_cast<float>(stack) / static_cast<float>(stacks);
            const float phi = v * XM_PI;
            for(uint32_t slice = 0; slice <= slices; ++slice)
            {
                const float u = static_cast<float>(slice) / static_cast<float>(slices);
                const float theta = u * XM_2PI;
                const XMFLOAT3 direction(
                    sinf(phi) * cosf(theta),
                    cosf(phi),
                    sinf(phi) * sinf(theta));
                const float radius = 1.0f +
                    0.18f * sinf(theta * 3.0f + phi * 2.0f) +
                    0.10f * sinf(theta * 5.0f - phi) +
                    0.07f * cosf(phi * 4.0f + theta);

                Vertex vertex{};
                vertex.pos = XMFLOAT3(
                    direction.x * radius,
                    direction.y * radius,
                    direction.z * radius);
                vertex.normal = direction;
                vertex.texCoord = XMFLOAT2(u, v);
                vertex.tangent = XMFLOAT4(-sinf(theta), 0.0f, cosf(theta), 1.0f);
                verts.push_back(vertex);

                rockMin.x = std::min(rockMin.x, vertex.pos.x);
                rockMin.y = std::min(rockMin.y, vertex.pos.y);
                rockMin.z = std::min(rockMin.z, vertex.pos.z);
                rockMax.x = std::max(rockMax.x, vertex.pos.x);
                rockMax.y = std::max(rockMax.y, vertex.pos.y);
                rockMax.z = std::max(rockMax.z, vertex.pos.z);
            }
        }

        for(uint32_t stack = 0; stack < stacks; ++stack)
        {
            for(uint32_t slice = 0; slice < slices; ++slice)
            {
                const uint32_t row = slices + 1;
                const uint32_t a = rockVertexStart + stack * row + slice;
                const uint32_t b = a + row;
                const uint32_t c = b + 1;
                const uint32_t d = a + 1;
                if(stack != 0)
                {
                    inds.push_back(a);
                    inds.push_back(b);
                    inds.push_back(d);
                }
                if(stack + 1 != stacks)
                {
                    inds.push_back(d);
                    inds.push_back(b);
                    inds.push_back(c);
                }
            }
        }

        rockSubsets[lod].indexStart = rockIndexStart;
        rockSubsets[lod].indexCount = static_cast<UINT>(inds.size()) - rockIndexStart;
    }

    mRockLocalBounds = BoundingBox(
        XMFLOAT3(
            (rockMin.x + rockMax.x) * 0.5f,
            (rockMin.y + rockMax.y) * 0.5f,
            (rockMin.z + rockMax.z) * 0.5f),
        XMFLOAT3(
            (rockMax.x - rockMin.x) * 0.5f,
            (rockMax.y - rockMin.y) * 0.5f,
            (rockMax.z - rockMin.z) * 0.5f));

    Material rockMaterial;
    rockMaterial.name = "procedural-rock";
    rockMaterial.diffuse = XMFLOAT4(0.78f, 0.84f, 0.92f, 1.0f);
    rockMaterial.specular = XMFLOAT3(0.28f, 0.28f, 0.28f);
    rockMaterial.shininess = 40.0f;
    const UINT rockMaterialIndex = static_cast<UINT>(mMaterials.size());
    mMaterials.push_back(rockMaterial);
    mRockSubsetIndex = static_cast<UINT>(mDrawSubsets.size());
    for(DrawSubset& subset : rockSubsets)
    {
        subset.materialIndex = rockMaterialIndex;
        mDrawSubsets.push_back(subset);
    }

    CalculateTangents(verts, inds);

    mIndexCount = (UINT)inds.size();

    ThrowIfFailed(mCmdAlloc->Reset());
    ThrowIfFailed(mCmdList->Reset(mCmdAlloc.Get(), nullptr));

    mVB = CreateDefaultBuffer(mDevice.Get(), mCmdList.Get(), verts.data(), sizeof(Vertex) * verts.size(), mVBUpload);
    mIB = CreateDefaultBuffer(mDevice.Get(), mCmdList.Get(), inds.data(), sizeof(uint32_t) * inds.size(), mIBUpload);

    ThrowIfFailed(mCmdList->Close());
    ID3D12CommandList* lists[] = { mCmdList.Get() };
    mQueue->ExecuteCommandLists(1, lists);
    Flush();

    mVBV.BufferLocation = mVB->GetGPUVirtualAddress();
    mVBV.StrideInBytes = sizeof(Vertex);
    mVBV.SizeInBytes = (UINT)(sizeof(Vertex) * verts.size());

    mIBV.BufferLocation = mIB->GetGPUVirtualAddress();
    mIBV.Format = DXGI_FORMAT_R32_UINT;
    mIBV.SizeInBytes = (UINT)(sizeof(uint32_t) * inds.size());

    OutputDebugStringW((L"[CG] Sponza + procedural rock: verts=" +
        std::to_wstring(verts.size()) + L" inds=" + std::to_wstring(inds.size()) +
        L" materials=" + std::to_wstring(mMaterials.size()) + L" subsets=" +
        std::to_wstring(mDrawSubsets.size()) + L"\n").c_str());
}

void RenderingSystem::BuildMaterialResources()
{
    std::array<ImageData, 4> images;
    const std::array<std::wstring, 4> paths = {
        FindAssetPath(L"Assets\\rock_01_diff_1k.jpg"),
        FindAssetPath(L"Assets\\rock_01_nor_dx_1k.jpg"),
        FindAssetPath(L"Assets\\rock_01_disp_1k.png"),
        FindAssetPath(L"Assets\\sponza_tiles.ppm")
    };

    if(!LoadImage(paths[0], images[0]))
    {
        OutputDebugStringW(L"[CG] Diffuse map missing; using checker fallback.\n");
        images[0] = MakeFallbackTexture();
    }
    if(!LoadImage(paths[1], images[1]))
    {
        OutputDebugStringW(L"[CG] Normal map missing; using a flat tangent-space normal.\n");
        images[1] = MakeSolidTexture(128, 128, 255);
    }
    if(!LoadImage(paths[2], images[2]))
    {
        OutputDebugStringW(L"[CG] Displacement map missing; using neutral height.\n");
        images[2] = MakeSolidTexture(128, 128, 128);
    }
    if(!LoadImage(paths[3], images[3]))
    {
        OutputDebugStringW(L"[CG] Sponza tile texture missing; using checker fallback.\n");
        images[3] = MakeFallbackTexture();
    }

    for(Material& material : mMaterials)
        material.textureIndex = 3;
    mMaterials[mDrawSubsets[mRockSubsetIndex].materialIndex].textureIndex = 0;

    mGBufferSrvOffset = static_cast<UINT>(images.size()) + 2;
    mParticleSrvOffset = mGBufferSrvOffset + GBuffer::TargetCount + 1;
    mParticleUavOffset = mParticleSrvOffset + 2;
    mSceneColorSrvOffset = mParticleUavOffset + 2;

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = mSceneColorSrvOffset + 1;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&mSrvHeap)));

    ThrowIfFailed(mCmdAlloc->Reset());
    ThrowIfFailed(mCmdList->Reset(mCmdAlloc.Get(), nullptr));

    mTextures.resize(images.size());
    std::vector<ComPtr<ID3D12Resource>> uploads(images.size());
    auto srvHandle = mSrvHeap->GetCPUDescriptorHandleForHeapStart();

    for(size_t textureIndex = 0; textureIndex < images.size(); ++textureIndex)
    {
        const ImageData& image = images[textureIndex];
        D3D12_RESOURCE_DESC textureDesc{};
        textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        textureDesc.Width = image.width;
        textureDesc.Height = image.height;
        textureDesc.DepthOrArraySize = 1;
        textureDesc.MipLevels = 1;
        textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        auto defaultHeap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
        ThrowIfFailed(mDevice->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&mTextures[textureIndex])));

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rowCount = 0;
        UINT64 rowSize = 0;
        UINT64 uploadSize = 0;
        mDevice->GetCopyableFootprints(&textureDesc, 0, 1, 0,
            &footprint, &rowCount, &rowSize, &uploadSize);

        auto uploadHeap = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
        auto uploadDesc = BufferDesc(uploadSize);
        ThrowIfFailed(mDevice->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&uploads[textureIndex])));

        uint8_t* mapped = nullptr;
        D3D12_RANGE noRead{0, 0};
        ThrowIfFailed(uploads[textureIndex]->Map(0, &noRead, reinterpret_cast<void**>(&mapped)));
        const size_t sourcePitch = static_cast<size_t>(image.width) * 4u;
        for(UINT row = 0; row < image.height; ++row)
        {
            memcpy(mapped + footprint.Offset + static_cast<size_t>(row) * footprint.Footprint.RowPitch,
                image.rgba.data() + static_cast<size_t>(row) * sourcePitch,
                sourcePitch);
        }
        uploads[textureIndex]->Unmap(0, nullptr);

        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = uploads[textureIndex].Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprint;

        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = mTextures[textureIndex].Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = 0;
        mCmdList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

        auto barrier = Transition(mTextures[textureIndex].Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        mCmdList->ResourceBarrier(1, &barrier);

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = textureDesc.Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        mDevice->CreateShaderResourceView(mTextures[textureIndex].Get(), &srv, srvHandle);
        srvHandle.ptr += mSrvInc;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC sharedMapSrv{};
    sharedMapSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sharedMapSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sharedMapSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sharedMapSrv.Texture2D.MipLevels = 1;
    for(UINT textureIndex = 1; textureIndex <= 2; ++textureIndex)
    {
        mDevice->CreateShaderResourceView(mTextures[textureIndex].Get(), &sharedMapSrv, srvHandle);
        srvHandle.ptr += mSrvInc;
    }

    ThrowIfFailed(mCmdList->Close());
    ID3D12CommandList* lists[] = {mCmdList.Get()};
    mQueue->ExecuteCommandLists(1, lists);
    Flush();

    mMaterialCB = std::make_unique<UploadBuffer<MaterialCB>>(
        mDevice.Get(), static_cast<UINT>(mMaterials.size()), true);
    for(UINT i = 0; i < static_cast<UINT>(mMaterials.size()); ++i)
    {
        MaterialCB constants{};
        constants.diffuse = mMaterials[i].diffuse;
        constants.specular = mMaterials[i].specular;
        constants.shininess = mMaterials[i].shininess;
        constants.displacementScale = 0.36f;
        constants.displacementBias = -0.18f;
        constants.minTessFactor = 1.0f;
        constants.maxTessFactor = 16.0f;
        constants.tessNearDistance = 4.0f;
        constants.tessFarDistance = 45.0f;
        mMaterialCB->CopyData(i, constants);
    }

    OutputDebugStringW(L"[CG] Poly Haven Rock 01: diffuse + normal DX + displacement loaded.\n");
}

void RenderingSystem::InitDevice()
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
    mSrvInc = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void RenderingSystem::CreateSwapChain()
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

void RenderingSystem::CreateDescriptorHeaps()
{
    D3D12_DESCRIPTOR_HEAP_DESC rtv{};
    rtv.NumDescriptors = mSettings.swapChainBufferCount + 1;
    rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&mRtvHeap)));


    D3D12_DESCRIPTOR_HEAP_DESC dsv{};
    dsv.NumDescriptors = 1 + ShadowCascadeCount;
    dsv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsv.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&dsv, IID_PPV_ARGS(&mDsvHeap)));
}

void RenderingSystem::CreateRenderTargets()
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

void RenderingSystem::CreateSceneColorTarget()
{
    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = static_cast<UINT64>(mWidth);
    texture.Height = static_cast<UINT>(mHeight);
    texture.DepthOrArraySize = 1;
    texture.MipLevels = 1;
    texture.Format = mSettings.backBufferFormat;
    texture.SampleDesc.Count = 1;
    texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = mSettings.backBufferFormat;
    clear.Color[3] = 1.0f;
    const auto heap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(mDevice->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &texture,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
        IID_PPV_ARGS(&mSceneColor)));

    mSceneColorRtv = mRtvHeap->GetCPUDescriptorHandleForHeapStart();
    mSceneColorRtv.ptr += static_cast<SIZE_T>(mSettings.swapChainBufferCount) * mRtvInc;
    mDevice->CreateRenderTargetView(mSceneColor.Get(), nullptr, mSceneColorRtv);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = mSettings.backBufferFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    auto cpuHandle = mSrvHeap->GetCPUDescriptorHandleForHeapStart();
    cpuHandle.ptr += static_cast<SIZE_T>(mSceneColorSrvOffset) * mSrvInc;
    mDevice->CreateShaderResourceView(mSceneColor.Get(), &srv, cpuHandle);
    mSceneColorSrv = mSrvHeap->GetGPUDescriptorHandleForHeapStart();
    mSceneColorSrv.ptr += static_cast<UINT64>(mSceneColorSrvOffset) * mSrvInc;
}

void RenderingSystem::CreateDepthBuffer()
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

void RenderingSystem::BuildShadowResources()
{
    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = ShadowMapSize;
    texture.Height = ShadowMapSize;
    texture.DepthOrArraySize = ShadowCascadeCount;
    texture.MipLevels = 1;
    texture.Format = DXGI_FORMAT_R32_TYPELESS;
    texture.SampleDesc.Count = 1;
    texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1.0f;
    const auto heap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(mDevice->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &texture,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
        IID_PPV_ARGS(&mShadowMaps)));

    auto dsv = mDsvHeap->GetCPUDescriptorHandleForHeapStart();
    dsv.ptr += mDsvInc;
    for(UINT cascade = 0; cascade < ShadowCascadeCount; ++cascade)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_D32_FLOAT;
        view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.FirstArraySlice = cascade;
        view.Texture2DArray.ArraySize = 1;
        view.Texture2DArray.MipSlice = 0;
        mShadowDsvs[cascade] = dsv;
        mDevice->CreateDepthStencilView(mShadowMaps.Get(), &view, dsv);
        dsv.ptr += mDsvInc;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srv.Texture2DArray.ArraySize = ShadowCascadeCount;
    srv.Texture2DArray.MipLevels = 1;
    auto srvHandle = mSrvHeap->GetCPUDescriptorHandleForHeapStart();
    srvHandle.ptr += static_cast<SIZE_T>(mGBufferSrvOffset + GBuffer::TargetCount) * mSrvInc;
    mDevice->CreateShaderResourceView(mShadowMaps.Get(), &srv, srvHandle);

    mShadowCB = std::make_unique<UploadBuffer<ShadowCB>>(
        mDevice.Get(), ShadowCascadeCount * SceneObjectCount, true);
}

void RenderingSystem::BuildParticleResources()
{
    const auto defaultHeap = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    const auto uploadHeap = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    const UINT64 particleBytes = static_cast<UINT64>(ParticleCount) * sizeof(Particle);
    D3D12_RESOURCE_DESC particleDesc = BufferDesc(particleBytes);
    particleDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_RESOURCE_DESC counterDesc = BufferDesc(D3D12_UAV_COUNTER_PLACEMENT_ALIGNMENT);
    counterDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    for(UINT index = 0; index < 2; ++index)
    {
        ThrowIfFailed(mDevice->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &particleDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&mParticleBuffers[index])));
        ThrowIfFailed(mDevice->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &counterDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&mParticleCounters[index])));

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_UNKNOWN;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Buffer.NumElements = ParticleCount;
        srv.Buffer.StructureByteStride = sizeof(Particle);
        auto srvHandle = mSrvHeap->GetCPUDescriptorHandleForHeapStart();
        srvHandle.ptr += static_cast<SIZE_T>(mParticleSrvOffset + index) * mSrvInc;
        mDevice->CreateShaderResourceView(mParticleBuffers[index].Get(), &srv, srvHandle);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_UNKNOWN;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = ParticleCount;
        uav.Buffer.StructureByteStride = sizeof(Particle);
        uav.Buffer.CounterOffsetInBytes = 0;
        auto uavHandle = mSrvHeap->GetCPUDescriptorHandleForHeapStart();
        uavHandle.ptr += static_cast<SIZE_T>(mParticleUavOffset + index) * mSrvInc;
        mDevice->CreateUnorderedAccessView(
            mParticleBuffers[index].Get(), mParticleCounters[index].Get(), &uav, uavHandle);
    }

    std::vector<Particle> initial(ParticleCount);
    for(UINT index = 0; index < ParticleCount; ++index)
    {
        Particle& particle = initial[index];
        if(index >= FountainParticleCount)
        {
            particle.position = mParticleEmitter;
            particle.age = 0.0f;
            particle.lifetime = 0.0f;
            particle.kind = 1;
            continue;
        }

        const float phase = static_cast<float>(index) / FountainParticleCount;
        const float angle = phase * XM_2PI * 11.0f;
        particle.kind = 0;
        particle.lifetime = 1.7f + 0.7f * fmodf(index * 0.381f, 1.0f);
        particle.age = phase * particle.lifetime;
        particle.velocity = XMFLOAT3(cosf(angle) * 0.38f,
            2.2f + 0.8f * fmodf(index * 0.279f, 1.0f), sinf(angle) * 0.38f);
        const float radial = 0.12f + 0.38f * fmodf(index * 0.617f, 1.0f);
        particle.position = XMFLOAT3(
            mParticleEmitter.x + cosf(angle) * radial + particle.velocity.x * particle.age,
            mParticleEmitter.y + particle.velocity.y * particle.age - 0.4f * particle.age * particle.age,
            mParticleEmitter.z + sinf(angle) * radial + particle.velocity.z * particle.age);
    }

    ComPtr<ID3D12Resource> initialUpload;
    const auto initialDesc = BufferDesc(particleBytes);
    ThrowIfFailed(mDevice->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &initialDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&initialUpload)));
    const auto counterUploadDesc = BufferDesc(2 * sizeof(uint32_t));
    ThrowIfFailed(mDevice->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &counterUploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mParticleZeroUpload)));

    void* mapped = nullptr;
    D3D12_RANGE noRead{0, 0};
    ThrowIfFailed(initialUpload->Map(0, &noRead, &mapped));
    memcpy(mapped, initial.data(), static_cast<size_t>(particleBytes));
    initialUpload->Unmap(0, nullptr);
    ThrowIfFailed(mParticleZeroUpload->Map(0, &noRead, &mapped));
    const uint32_t initialCounts[2] = {ParticleCount, 0};
    memcpy(mapped, initialCounts, sizeof(initialCounts));
    mParticleZeroUpload->Unmap(0, nullptr);

    ThrowIfFailed(mCmdAlloc->Reset());
    ThrowIfFailed(mCmdList->Reset(mCmdAlloc.Get(), nullptr));
    mCmdList->CopyBufferRegion(mParticleBuffers[0].Get(), 0, initialUpload.Get(), 0, particleBytes);
    for(UINT index = 0; index < 2; ++index)
    {
        mCmdList->CopyBufferRegion(mParticleCounters[index].Get(), 0,
            mParticleZeroUpload.Get(), index * sizeof(uint32_t), sizeof(uint32_t));
        const D3D12_RESOURCE_BARRIER barriers[] = {
            Transition(mParticleBuffers[index].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            Transition(mParticleCounters[index].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        };
        mCmdList->ResourceBarrier(_countof(barriers), barriers);
    }
    ThrowIfFailed(mCmdList->Close());
    ID3D12CommandList* lists[] = {mCmdList.Get()};
    mQueue->ExecuteCommandLists(1, lists);
    Flush();

    mParticleFrameCB = std::make_unique<UploadBuffer<ParticleFrameCB>>(mDevice.Get(), 1, true);
}

void RenderingSystem::Resize(int width, int height)
{
    if(!mDevice) return;

    mWidth = std::max(1, width);
    mHeight = std::max(1, height);

    Flush();

    for(auto& bb : mBackBuffers) bb.Reset();
    mDepth.Reset();
    mSceneColor.Reset();

    ThrowIfFailed(mSwapChain->ResizeBuffers(
        mSettings.swapChainBufferCount,
        mWidth,
        mHeight,
        mSettings.backBufferFormat,
        0));

    mFrameIndex = mSwapChain->GetCurrentBackBufferIndex();

    CreateRenderTargets();
    CreateDepthBuffer();
    mGBuffer.Resize(mWidth, mHeight);
    CreateSceneColorTarget();

    mViewport = {0.0f, 0.0f, static_cast<float>(mWidth), static_cast<float>(mHeight), 0.0f, 1.0f};
    mScissor = {0, 0, mWidth, mHeight};
}

void RenderingSystem::BuildRootSignatures()
{
    auto createRootSignature = [this](
        const D3D12_ROOT_SIGNATURE_DESC& description,
        ComPtr<ID3D12RootSignature>& destination)
    {
        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> errors;
        const HRESULT hr = D3D12SerializeRootSignature(
            &description, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
        if(FAILED(hr))
        {
            const std::string message = errors
                ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize())
                : "Root signature serialization failed";
            throw std::runtime_error(message);
        }
        ThrowIfFailed(mDevice->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&destination)));
    };

    D3D12_DESCRIPTOR_RANGE materialSrvRange{};
    materialSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    materialSrvRange.NumDescriptors = 3;
    materialSrvRange.BaseShaderRegister = 0;
    materialSrvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER geometryParams[3]{};
    geometryParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    geometryParams[0].Descriptor.ShaderRegister = 0;
    geometryParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    geometryParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    geometryParams[1].Descriptor.ShaderRegister = 1;
    geometryParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    geometryParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    geometryParams[2].DescriptorTable.NumDescriptorRanges = 1;
    geometryParams[2].DescriptorTable.pDescriptorRanges = &materialSrvRange;
    geometryParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC geometryDesc{};
    geometryDesc.NumParameters = _countof(geometryParams);
    geometryDesc.pParameters = geometryParams;
    geometryDesc.NumStaticSamplers = 1;
    geometryDesc.pStaticSamplers = &sampler;
    geometryDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    createRootSignature(geometryDesc, mGeometryRootSig);

    D3D12_DESCRIPTOR_RANGE gbufferRange{};
    gbufferRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    gbufferRange.NumDescriptors = GBuffer::TargetCount + 1;
    gbufferRange.BaseShaderRegister = 0;
    gbufferRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER lightingParams[2]{};
    lightingParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    lightingParams[0].Descriptor.ShaderRegister = 0;
    lightingParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    lightingParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    lightingParams[1].DescriptorTable.NumDescriptorRanges = 1;
    lightingParams[1].DescriptorTable.pDescriptorRanges = &gbufferRange;
    lightingParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC lightingDesc{};
    lightingDesc.NumParameters = _countof(lightingParams);
    lightingDesc.pParameters = lightingParams;
    D3D12_STATIC_SAMPLER_DESC shadowSampler{};
    shadowSampler.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    shadowSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    shadowSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    shadowSampler.MinLOD = 0.0f;
    shadowSampler.MaxLOD = D3D12_FLOAT32_MAX;
    shadowSampler.ShaderRegister = 0;
    shadowSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    lightingDesc.NumStaticSamplers = 1;
    lightingDesc.pStaticSamplers = &shadowSampler;
    lightingDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    createRootSignature(lightingDesc, mLightingRootSig);

    D3D12_ROOT_PARAMETER shadowParam{};
    shadowParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    shadowParam.Descriptor.ShaderRegister = 0;
    shadowParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC shadowDesc{};
    shadowDesc.NumParameters = 1;
    shadowDesc.pParameters = &shadowParam;
    shadowDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    createRootSignature(shadowDesc, mShadowRootSig);

    D3D12_DESCRIPTOR_RANGE computeRanges[2]{};
    for(UINT index = 0; index < 2; ++index)
    {
        computeRanges[index].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        computeRanges[index].NumDescriptors = 1;
        computeRanges[index].BaseShaderRegister = index;
        computeRanges[index].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    }
    D3D12_ROOT_PARAMETER computeParams[3]{};
    computeParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    computeParams[0].Descriptor.ShaderRegister = 0;
    for(UINT index = 0; index < 2; ++index)
    {
        computeParams[index + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        computeParams[index + 1].DescriptorTable.NumDescriptorRanges = 1;
        computeParams[index + 1].DescriptorTable.pDescriptorRanges = &computeRanges[index];
    }
    D3D12_ROOT_SIGNATURE_DESC computeDesc{};
    computeDesc.NumParameters = _countof(computeParams);
    computeDesc.pParameters = computeParams;
    createRootSignature(computeDesc, mParticleComputeRootSig);

    D3D12_DESCRIPTOR_RANGE particleSrvRange{};
    particleSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    particleSrvRange.NumDescriptors = 1;
    particleSrvRange.BaseShaderRegister = 0;
    particleSrvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER particleParams[2]{};
    particleParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    particleParams[0].Descriptor.ShaderRegister = 0;
    particleParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    particleParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    particleParams[1].DescriptorTable.NumDescriptorRanges = 1;
    particleParams[1].DescriptorTable.pDescriptorRanges = &particleSrvRange;
    particleParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC particleDesc{};
    particleDesc.NumParameters = _countof(particleParams);
    particleDesc.pParameters = particleParams;
    particleDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    createRootSignature(particleDesc, mParticleGraphicsRootSig);

    D3D12_DESCRIPTOR_RANGE postRange{};
    postRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    postRange.NumDescriptors = 1;
    postRange.BaseShaderRegister = 0;
    postRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER postParam{};
    postParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    postParam.DescriptorTable.NumDescriptorRanges = 1;
    postParam.DescriptorTable.pDescriptorRanges = &postRange;
    postParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC postSampler{};
    postSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    postSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    postSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    postSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    postSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    postSampler.MinLOD = 0.0f;
    postSampler.MaxLOD = D3D12_FLOAT32_MAX;
    postSampler.ShaderRegister = 0;
    postSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC postDesc{};
    postDesc.NumParameters = 1;
    postDesc.pParameters = &postParam;
    postDesc.NumStaticSamplers = 1;
    postDesc.pStaticSamplers = &postSampler;
    postDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    createRootSignature(postDesc, mPostRootSig);

}

void RenderingSystem::BuildShadersAndPSOs()
{
    UINT flags = 0;
#if defined(_DEBUG)
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    auto compileShader = [flags](
        const std::wstring& path,
        const char* entryPoint,
        const char* target,
        ComPtr<ID3DBlob>& destination,
        const D3D_SHADER_MACRO* defines = nullptr)
    {
        ComPtr<ID3DBlob> errors;
        const HRESULT hr = D3DCompileFromFile(
            path.c_str(), defines, D3D_COMPILE_STANDARD_FILE_INCLUDE,
            entryPoint, target, flags, 0, &destination, &errors);
        if(FAILED(hr))
        {
            const std::string message = errors
                ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize())
                : "Shader compilation failed";
            throw std::runtime_error(message);
        }
    };

    const std::wstring gbufferShader = FindAssetPath(L"GBuffer.hlsl");
    const std::wstring lightingShader = FindAssetPath(L"DeferredLighting.hlsl");
    const std::wstring shadowShader = FindAssetPath(L"ShadowDepth.hlsl");
    const std::wstring particleShader = FindAssetPath(L"Particles.hlsl");
    const std::wstring postShader = FindAssetPath(L"PostProcess.hlsl");
    const std::string cascadeCount = std::to_string(NUM_CASCADES);
    const D3D_SHADER_MACRO shadowDefines[] = {
        {"NUM_CASCADES", cascadeCount.c_str()},
        {nullptr, nullptr}
    };
    compileShader(gbufferShader, "VSGeometry", "vs_5_0", mGeometryVS);
    compileShader(gbufferShader, "HSGeometry", "hs_5_0", mGeometryHS);
    compileShader(gbufferShader, "DSGeometry", "ds_5_0", mGeometryDS);
    compileShader(gbufferShader, "PSGeometry", "ps_5_0", mGeometryPS);
    compileShader(lightingShader, "VSFullscreen", "vs_5_0", mFullscreenVS, shadowDefines);
    compileShader(lightingShader, "PSLighting", "ps_5_0", mLightingPS, shadowDefines);
    compileShader(shadowShader, "VSShadow", "vs_5_0", mShadowVS);
    compileShader(particleShader, "CSUpdate", "cs_5_0", mParticleCS);
    compileShader(particleShader, "VSParticle", "vs_5_0", mParticleVS);
    compileShader(particleShader, "GSParticle", "gs_5_0", mParticleGS);
    compileShader(particleShader, "PSParticle", "ps_5_0", mParticlePS);
    compileShader(postShader, "VSPostProcess", "vs_5_0", mPostVS);
    compileShader(postShader, "PSCopy", "ps_5_0", mPostCopyPS);
    compileShader(postShader, "PSVignette", "ps_5_0", mPostVignettePS);
    compileShader(postShader, "PSGaussianBlur3x3", "ps_5_0", mPostBlurPS);

    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode = D3D12_CULL_MODE_BACK;
    rasterizer.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    rasterizer.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    rasterizer.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    rasterizer.DepthClipEnable = TRUE;

    D3D12_DEPTH_STENCIL_DESC geometryDepth{};
    geometryDepth.DepthEnable = TRUE;
    geometryDepth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    geometryDepth.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC geometryPso{};
    geometryPso.pRootSignature = mGeometryRootSig.Get();
    geometryPso.VS = {mGeometryVS->GetBufferPointer(), mGeometryVS->GetBufferSize()};
    geometryPso.HS = {mGeometryHS->GetBufferPointer(), mGeometryHS->GetBufferSize()};
    geometryPso.DS = {mGeometryDS->GetBufferPointer(), mGeometryDS->GetBufferSize()};
    geometryPso.PS = {mGeometryPS->GetBufferPointer(), mGeometryPS->GetBufferSize()};
    geometryPso.BlendState = blend;
    geometryPso.SampleMask = UINT_MAX;
    geometryPso.RasterizerState = rasterizer;
    geometryPso.DepthStencilState = geometryDepth;
    geometryPso.InputLayout = {layout, _countof(layout)};
    geometryPso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    geometryPso.NumRenderTargets = GBuffer::TargetCount;
    geometryPso.RTVFormats[0] = GBuffer::Format(GBuffer::AlbedoSpecular);
    geometryPso.RTVFormats[1] = GBuffer::Format(GBuffer::NormalShininess);
    geometryPso.RTVFormats[2] = GBuffer::Format(GBuffer::WorldPosition);
    geometryPso.DSVFormat = mSettings.depthFormat;
    geometryPso.SampleDesc.Count = 1;
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&geometryPso, IID_PPV_ARGS(&mGeometryPSO)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC wireframePso = geometryPso;
    wireframePso.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(
        &wireframePso, IID_PPV_ARGS(&mGeometryWireframePSO)));

    D3D12_DEPTH_STENCIL_DESC lightingDepth{};
    lightingDepth.DepthEnable = FALSE;
    lightingDepth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    lightingDepth.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;

    rasterizer.CullMode = D3D12_CULL_MODE_NONE;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC lightingPso{};
    lightingPso.pRootSignature = mLightingRootSig.Get();
    lightingPso.VS = {mFullscreenVS->GetBufferPointer(), mFullscreenVS->GetBufferSize()};
    lightingPso.PS = {mLightingPS->GetBufferPointer(), mLightingPS->GetBufferSize()};
    lightingPso.BlendState = blend;
    lightingPso.SampleMask = UINT_MAX;
    lightingPso.RasterizerState = rasterizer;
    lightingPso.DepthStencilState = lightingDepth;
    lightingPso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    lightingPso.NumRenderTargets = 1;
    lightingPso.RTVFormats[0] = mSettings.backBufferFormat;
    lightingPso.SampleDesc.Count = 1;
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&lightingPso, IID_PPV_ARGS(&mLightingPSO)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC shadowPso{};
    shadowPso.pRootSignature = mShadowRootSig.Get();
    shadowPso.VS = {mShadowVS->GetBufferPointer(), mShadowVS->GetBufferSize()};
    shadowPso.BlendState = blend;
    shadowPso.SampleMask = UINT_MAX;
    shadowPso.RasterizerState = rasterizer;
    shadowPso.RasterizerState.DepthBias = 600;
    shadowPso.RasterizerState.SlopeScaledDepthBias = 1.5f;
    shadowPso.DepthStencilState = geometryDepth;
    shadowPso.InputLayout = {layout, _countof(layout)};
    shadowPso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    shadowPso.NumRenderTargets = 0;
    shadowPso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    shadowPso.SampleDesc.Count = 1;
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&shadowPso, IID_PPV_ARGS(&mShadowPSO)));

    D3D12_COMPUTE_PIPELINE_STATE_DESC computePso{};
    computePso.pRootSignature = mParticleComputeRootSig.Get();
    computePso.CS = {mParticleCS->GetBufferPointer(), mParticleCS->GetBufferSize()};
    ThrowIfFailed(mDevice->CreateComputePipelineState(
        &computePso, IID_PPV_ARGS(&mParticleComputePSO)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC particlePso{};
    particlePso.pRootSignature = mParticleGraphicsRootSig.Get();
    particlePso.VS = {mParticleVS->GetBufferPointer(), mParticleVS->GetBufferSize()};
    particlePso.GS = {mParticleGS->GetBufferPointer(), mParticleGS->GetBufferSize()};
    particlePso.PS = {mParticlePS->GetBufferPointer(), mParticlePS->GetBufferSize()};
    particlePso.BlendState = blend;
    particlePso.SampleMask = UINT_MAX;
    particlePso.RasterizerState = rasterizer;
    particlePso.DepthStencilState = geometryDepth;
    particlePso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    particlePso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    particlePso.NumRenderTargets = 1;
    particlePso.RTVFormats[0] = mSettings.backBufferFormat;
    particlePso.DSVFormat = mSettings.depthFormat;
    particlePso.SampleDesc.Count = 1;
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(
        &particlePso, IID_PPV_ARGS(&mParticleGraphicsPSO)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC postPso{};
    postPso.pRootSignature = mPostRootSig.Get();
    postPso.VS = {mPostVS->GetBufferPointer(), mPostVS->GetBufferSize()};
    postPso.BlendState = blend;
    postPso.SampleMask = UINT_MAX;
    postPso.RasterizerState = rasterizer;
    postPso.DepthStencilState = lightingDepth;
    postPso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    postPso.NumRenderTargets = 1;
    postPso.RTVFormats[0] = mSettings.backBufferFormat;
    postPso.SampleDesc.Count = 1;

    postPso.PS = {mPostCopyPS->GetBufferPointer(), mPostCopyPS->GetBufferSize()};
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(
        &postPso, IID_PPV_ARGS(&mPostCopyPSO)));
    postPso.PS = {mPostVignettePS->GetBufferPointer(), mPostVignettePS->GetBufferSize()};
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(
        &postPso, IID_PPV_ARGS(&mPostVignettePSO)));
    postPso.PS = {mPostBlurPS->GetBufferPointer(), mPostBlurPS->GetBufferSize()};
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(
        &postPso, IID_PPV_ARGS(&mPostBlurPSO)));

}

void RenderingSystem::BuildCubeGeometry()
{

    const float s = 1.0f;
    std::vector<Vertex> v = {
        {{ s,-s,-s},{ 1,0,0},{0,1}}, {{ s, s,-s},{ 1,0,0},{0,0}}, {{ s, s, s},{ 1,0,0},{1,0}}, {{ s,-s, s},{ 1,0,0},{1,1}},
        {{-s,-s, s},{-1,0,0},{0,1}}, {{-s, s, s},{-1,0,0},{0,0}}, {{-s, s,-s},{-1,0,0},{1,0}}, {{-s,-s,-s},{-1,0,0},{1,1}},
        {{-s, s,-s},{0, 1,0},{0,1}}, {{-s, s, s},{0, 1,0},{0,0}}, {{ s, s, s},{0, 1,0},{1,0}}, {{ s, s,-s},{0, 1,0},{1,1}},
        {{-s,-s, s},{0,-1,0},{0,1}}, {{-s,-s,-s},{0,-1,0},{0,0}}, {{ s,-s,-s},{0,-1,0},{1,0}}, {{ s,-s, s},{0,-1,0},{1,1}},
        {{-s,-s, s},{0,0, 1},{0,1}}, {{ s,-s, s},{0,0, 1},{1,1}}, {{ s, s, s},{0,0, 1},{1,0}}, {{-s, s, s},{0,0, 1},{0,0}},
        {{ s,-s,-s},{0,0,-1},{0,1}}, {{-s,-s,-s},{0,0,-1},{1,1}}, {{-s, s,-s},{0,0,-1},{1,0}}, {{ s, s,-s},{0,0,-1},{0,0}},
    };

    std::vector<uint16_t> i;
    i.reserve(36);
    for(uint16_t face = 0; face < 6; ++face)
    {
        uint16_t base = face * 4;
        i.push_back(base + 0); i.push_back(base + 1); i.push_back(base + 2);
        i.push_back(base + 0); i.push_back(base + 2); i.push_back(base + 3);
    }
    CalculateTangents(v, i);

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

    mMaterials.clear();
    Material material;
    material.name = "culling-object";
    material.diffuse = XMFLOAT4(0.72f, 0.82f, 0.94f, 1.0f);
    material.specular = XMFLOAT3(0.32f, 0.32f, 0.32f);
    material.shininess = 48.0f;
    mMaterials.push_back(material);
    mDrawSubsets = {{0, mIndexCount, 0}};
}

void RenderingSystem::BuildObjectScene()
{
    std::mt19937 random(0xC0111DEu);
    std::uniform_real_distribution<float> unitDistribution(0.0f, 1.0f);
    std::normal_distribution<float> normalDistribution(0.0f, 1.0f);
    std::uniform_real_distribution<float> scaleDistribution(0.85f, 2.25f);
    std::uniform_real_distribution<float> stretchDistribution(0.72f, 1.45f);
    std::uniform_real_distribution<float> angleDistribution(0.0f, XM_2PI);

    mObjectWorlds.clear();
    mObjectBounds.clear();
    mObjectDraws.clear();
    mObjectWorlds.reserve(SceneObjectCount);
    mObjectBounds.reserve(SceneObjectCount);
    mObjectDraws.reserve(SceneObjectCount);

    XMFLOAT4X4 identity{};
    XMStoreFloat4x4(&identity, XMMatrixIdentity());
    mObjectWorlds.push_back(identity);
    BoundingBox sponzaBounds = mSponzaLocalBounds;
    sponzaBounds.Extents.x += 0.5f;
    sponzaBounds.Extents.y += 0.5f;
    sponzaBounds.Extents.z += 0.5f;
    mObjectBounds.push_back(sponzaBounds);
    mObjectDraws.push_back({mSponzaSubsetStart, mSponzaSubsetCount});

    constexpr uint32_t ArmCount = 5;
    for(uint32_t objectIndex = 0; objectIndex < RockObjectCount; ++objectIndex)
    {
        float baseScale = scaleDistribution(random);
        if(objectIndex % 97 == 0)
            baseScale *= 2.25f;
        const float scaleX = baseScale * stretchDistribution(random);
        const float scaleY = baseScale * stretchDistribution(random);
        const float scaleZ = baseScale * stretchDistribution(random);

        const float radialDistance =
            58.0f + sqrtf(unitDistribution(random)) * 215.0f;
        const uint32_t arm = objectIndex % ArmCount;
        const float angle =
            static_cast<float>(arm) * XM_2PI / static_cast<float>(ArmCount) +
            radialDistance * 0.032f + normalDistribution(random) * 0.28f;
        const float noisyRadius = radialDistance + normalDistribution(random) * 8.0f;
        const float positionX =
            cosf(angle) * noisyRadius + normalDistribution(random) * 4.0f;
        const float positionZ =
            sinf(angle) * noisyRadius + normalDistribution(random) * 4.0f;
        const float positionY =
            sinf(angle * 2.35f) * 9.0f + normalDistribution(random) * 13.0f;

        const XMMATRIX world =
            XMMatrixScaling(scaleX, scaleY, scaleZ) *
            XMMatrixRotationRollPitchYaw(
                angleDistribution(random),
                angleDistribution(random),
                angleDistribution(random)) *
            XMMatrixTranslation(positionX, positionY, positionZ);

        XMFLOAT4X4 worldValue{};
        XMStoreFloat4x4(&worldValue, world);
        mObjectWorlds.push_back(worldValue);

        BoundingBox worldBounds;
        mRockLocalBounds.Transform(worldBounds, world);
        const float displacementPadding =
            0.40f * std::max(scaleX, std::max(scaleY, scaleZ));
        worldBounds.Extents.x += displacementPadding;
        worldBounds.Extents.y += displacementPadding;
        worldBounds.Extents.z += displacementPadding;
        mObjectBounds.push_back(worldBounds);
        mObjectDraws.push_back({mRockSubsetIndex, 1});
    }

    mVisibleObjects.resize(mObjectWorlds.size());
    for(uint32_t index = 0; index < static_cast<uint32_t>(mVisibleObjects.size()); ++index)
        mVisibleObjects[index] = index;
    mObjectLods.assign(mObjectWorlds.size(), 0);
    mVisibleRockLodCounts.fill(0);

    mOctree.Build(mObjectBounds, 7, 18);
    OutputDebugStringW((L"[CG] Culling scene: Sponza + " +
        std::to_wstring(RockObjectCount) + L" procedural rocks; octree built.\n").c_str());
}

void RenderingSystem::BuildDefaultLights()
{
    auto directionFromTo = [](const XMFLOAT3& from, const XMFLOAT3& to)
    {
        XMFLOAT3 direction{};
        XMStoreFloat3(&direction, XMVector3Normalize(XMLoadFloat3(&to) - XMLoadFloat3(&from)));
        return direction;
    };

    std::vector<Light> lights;

    Light sun;
    sun.type = LightType::Directional;
    sun.direction = XMFLOAT3(0.35f, -0.82f, 0.45f);
    sun.color = XMFLOAT3(1.0f, 0.94f, 0.82f);
    sun.intensity = 0.80f;
    lights.push_back(sun);

    auto addPoint = [&lights](XMFLOAT3 position, XMFLOAT3 color, float range, float intensity)
    {
        Light light;
        light.type = LightType::Point;
        light.position = position;
        light.color = color;
        light.range = range;
        light.intensity = intensity;
        lights.push_back(light);
    };

    addPoint(XMFLOAT3(-14.0f, 4.0f, -7.0f), XMFLOAT3(1.0f, 0.18f, 0.08f), 17.0f, 1.25f);
    addPoint(XMFLOAT3(-7.0f, 6.0f, 8.0f), XMFLOAT3(0.10f, 0.45f, 1.0f), 15.0f, 1.1f);
    addPoint(XMFLOAT3(0.0f, 8.0f, 0.0f), XMFLOAT3(1.0f, 0.72f, 0.24f), 18.0f, 1.3f);
    addPoint(XMFLOAT3(8.0f, 5.0f, -8.0f), XMFLOAT3(0.25f, 1.0f, 0.38f), 15.0f, 1.1f);
    addPoint(XMFLOAT3(15.0f, 4.0f, 7.0f), XMFLOAT3(0.82f, 0.18f, 1.0f), 17.0f, 1.25f);

    auto addSpot = [&lights, &directionFromTo](
        XMFLOAT3 position, XMFLOAT3 target, XMFLOAT3 color)
    {
        Light light;
        light.type = LightType::Spot;
        light.position = position;
        light.direction = directionFromTo(position, target);
        light.color = color;
        light.range = 28.0f;
        light.intensity = 2.0f;
        light.spotCosInner = cosf(XMConvertToRadians(16.0f));
        light.spotCosOuter = cosf(XMConvertToRadians(27.0f));
        lights.push_back(light);
    };

    addSpot(XMFLOAT3(-12.0f, 13.0f, 0.0f), XMFLOAT3(-3.0f, 0.0f, 0.0f), XMFLOAT3(1.0f, 0.78f, 0.55f));
    addSpot(XMFLOAT3(12.0f, 13.0f, 0.0f), XMFLOAT3(3.0f, 0.0f, 0.0f), XMFLOAT3(0.55f, 0.78f, 1.0f));

    SetLights(std::move(lights));
}

void RenderingSystem::SetLights(std::vector<Light> lights)
{
    if(lights.size() > MaxLights)
        lights.resize(MaxLights);
    mLights = std::move(lights);
}

void RenderingSystem::QueueParticleBurst(const XMFLOAT3& position)
{
    mPendingBursts.push_back(position);
}

void RenderingSystem::ToggleFrustumCulling()
{
    mFrustumCullingEnabled = !mFrustumCullingEnabled;
    if(!mFrustumCullingEnabled)
    {
        mOctreeCullingEnabled = false;
    }
    mNextTitleUpdate = 0.0f;
}

void RenderingSystem::ToggleOctreeCulling()
{
    if(mOctreeCullingEnabled)
    {
        mOctreeCullingEnabled = false;
    }
    else
    {
        mFrustumCullingEnabled = true;
        mOctreeCullingEnabled = true;
    }
    mNextTitleUpdate = 0.0f;
}

void RenderingSystem::UpdateVisibleObjects(const BoundingFrustum& worldFrustum)
{
    if(!mFrustumCullingEnabled)
    {
        mCullingStats = {};
        mVisibleObjects.resize(mObjectBounds.size());
        for(uint32_t index = 0; index < static_cast<uint32_t>(mVisibleObjects.size()); ++index)
            mVisibleObjects[index] = index;
        return;
    }

    if(mOctreeCullingEnabled && !mOctree.Empty())
    {
        mOctree.Query(
            mObjectBounds, worldFrustum, mVisibleObjects,
            mCullingStats);
    }
    else
    {
        SpatialCulling::CullLinear(mObjectBounds, worldFrustum, mVisibleObjects, mCullingStats);
    }
}

void RenderingSystem::UpdateObjectLods()
{
    mVisibleRockLodCounts.fill(0);
    for(uint32_t objectIndex = 1; objectIndex < static_cast<uint32_t>(mObjectBounds.size()); ++objectIndex)
    {
        UINT lod = 0;
        if(mLodEnabled)
        {
            const BoundingBox& bounds = mObjectBounds[objectIndex];
            const float dx = bounds.Center.x - mCameraPosition.x;
            const float dy = bounds.Center.y - mCameraPosition.y;
            const float dz = bounds.Center.z - mCameraPosition.z;
            const float distanceSquared = dx * dx + dy * dy + dz * dz;
            const float radiusSquared = std::max(
                bounds.Extents.x * bounds.Extents.x +
                bounds.Extents.y * bounds.Extents.y +
                bounds.Extents.z * bounds.Extents.z, 0.0001f);
            lod = distanceSquared < radiusSquared * 20.0f * 20.0f
                ? 0
                : (distanceSquared < radiusSquared * 45.0f * 45.0f ? 1 : 2);
        }
        mObjectLods[objectIndex] = static_cast<uint8_t>(lod);
    }

    for(const uint32_t objectIndex : mVisibleObjects)
    {
        if(objectIndex != 0)
            ++mVisibleRockLodCounts[mObjectLods[objectIndex]];
    }
}

void RenderingSystem::UpdateObjectConstants(
    const XMMATRIX& viewProjection,
    const XMFLOAT3& eyePosition,
    float totalTime)
{
    for(uint32_t visibleSlot = 0;
        visibleSlot < static_cast<uint32_t>(mVisibleObjects.size());
        ++visibleSlot)
    {
        const uint32_t objectIndex = mVisibleObjects[visibleSlot];
        SceneCB scene{};
        XMStoreFloat4x4(
            &scene.world,
            XMMatrixTranspose(XMLoadFloat4x4(&mObjectWorlds[objectIndex])));
        XMStoreFloat4x4(&scene.viewProj, XMMatrixTranspose(viewProjection));
        if(objectIndex == 0)
        {
            scene.textureOffset = XMFLOAT2(
                totalTime * 0.08f,
                totalTime * 0.035f);
            scene.textureTiling = XMFLOAT2(2.0f, 2.0f);
        }
        else
        {
            scene.textureOffset = XMFLOAT2(0.0f, 0.0f);
            scene.textureTiling = XMFLOAT2(1.0f, 1.0f);
        }
        scene.eyePosition = eyePosition;
        scene.colorizeTiles = objectIndex == 0 ? 1u : 0u;
        scene.enableNormalMapping = mNormalMappingEnabled ? 1u : 0u;
        scene.enableDisplacement = mDisplacementEnabled ? 1u : 0u;
        mSceneCB->CopyData(static_cast<int>(visibleSlot), scene);
    }
}

void RenderingSystem::UpdateShadows(const XMMATRIX& view)
{
    constexpr float nearPlane = 0.1f;
    constexpr float farPlane = 450.0f;
    constexpr float splitWeight = 0.75f;
    const float aspect = static_cast<float>(mWidth) / static_cast<float>(mHeight);
    const XMMATRIX inverseView = XMMatrixInverse(nullptr, view);
    XMVECTOR lightDirection = XMVector3Normalize(XMLoadFloat3(&mLights.front().direction));
    float previousSplit = nearPlane;
    float* splits = mCascadeSplits.data();

    for(UINT cascade = 0; cascade < ShadowCascadeCount; ++cascade)
    {
        const float portion = static_cast<float>(cascade + 1) / ShadowCascadeCount;
        const float logarithmic = nearPlane * powf(farPlane / nearPlane, portion);
        const float uniform = nearPlane + (farPlane - nearPlane) * portion;
        const float cascadeFar = splitWeight * logarithmic + (1.0f - splitWeight) * uniform;
        splits[cascade] = cascadeFar;

        const XMMATRIX segmentProjection = XMMatrixPerspectiveFovLH(
            0.25f * XM_PI, aspect, previousSplit, cascadeFar);
        BoundingFrustum segmentView;
        BoundingFrustum::CreateFromMatrix(segmentView, segmentProjection);
        BoundingFrustum segmentWorld;
        segmentView.Transform(segmentWorld, inverseView);
        XMFLOAT3 corners[BoundingFrustum::CORNER_COUNT]{};
        segmentWorld.GetCorners(corners);

        XMVECTOR center = XMVectorZero();
        for(const XMFLOAT3& corner : corners)
            center += XMLoadFloat3(&corner);
        center /= static_cast<float>(BoundingFrustum::CORNER_COUNT);

        float radius = 0.0f;
        for(const XMFLOAT3& corner : corners)
            radius = std::max(radius, XMVectorGetX(XMVector3Length(XMLoadFloat3(&corner) - center)));
        radius = std::ceil(radius + 4.0f);

        const XMVECTOR lightEye = center - lightDirection * 650.0f;
        const XMMATRIX lightView = XMMatrixLookToLH(
            lightEye, lightDirection, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
        const XMVECTOR centerLight = XMVector3TransformCoord(center, lightView);
        const float texel = 2.0f * radius / ShadowMapSize;
        const float centerX = std::floor(XMVectorGetX(centerLight) / texel) * texel;
        const float centerY = std::floor(XMVectorGetY(centerLight) / texel) * texel;

        float minZ = FLT_MAX;
        float maxZ = -FLT_MAX;
        for(const BoundingBox& bounds : mObjectBounds)
        {
            XMFLOAT3 boxCorners[BoundingBox::CORNER_COUNT]{};
            bounds.GetCorners(boxCorners);
            for(const XMFLOAT3& corner : boxCorners)
            {
                const float z = XMVectorGetZ(XMVector3TransformCoord(XMLoadFloat3(&corner), lightView));
                minZ = std::min(minZ, z);
                maxZ = std::max(maxZ, z);
            }
        }

        const XMMATRIX lightProjection = XMMatrixOrthographicOffCenterLH(
            centerX - radius, centerX + radius,
            centerY - radius, centerY + radius,
            std::max(0.01f, minZ - 10.0f), maxZ + 10.0f);
        const XMMATRIX shadowMatrix = lightView * lightProjection;
        XMStoreFloat4x4(&mShadowViewProj[cascade], shadowMatrix);

        for(UINT objectIndex = 0; objectIndex < SceneObjectCount; ++objectIndex)
        {
            ShadowCB constants{};
            const XMMATRIX world = XMLoadFloat4x4(&mObjectWorlds[objectIndex]);
            XMStoreFloat4x4(&constants.worldViewProj, XMMatrixTranspose(world * shadowMatrix));
            mShadowCB->CopyData(static_cast<int>(cascade * SceneObjectCount + objectIndex), constants);
        }
        previousSplit = cascadeFar;
    }

    for(UINT cascade = 0; cascade < ShadowCascadeCount; ++cascade)
        XMStoreFloat4x4(&mLightingData.shadowViewProj[cascade],
            XMMatrixTranspose(XMLoadFloat4x4(&mShadowViewProj[cascade])));
    mLightingData.cascadeSplits = mCascadeSplits;
    mLightingData.cameraForwardAndShadow = XMFLOAT4(
        mCameraForward.x, mCameraForward.y, mCameraForward.z,
        mShadowsEnabled ? 1.0f : 0.0f);
}

void RenderingSystem::RenderShadows()
{
    if(!mShadowsEnabled || mLights.empty() || mLights.front().type != LightType::Directional)
        return;

    auto barrier = Transition(mShadowMaps.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    mCmdList->ResourceBarrier(1, &barrier);

    const D3D12_VIEWPORT viewport{
        0.0f, 0.0f, static_cast<float>(ShadowMapSize), static_cast<float>(ShadowMapSize), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(ShadowMapSize), static_cast<LONG>(ShadowMapSize)};
    mCmdList->RSSetViewports(1, &viewport);
    mCmdList->RSSetScissorRects(1, &scissor);
    mCmdList->SetPipelineState(mShadowPSO.Get());
    mCmdList->SetGraphicsRootSignature(mShadowRootSig.Get());
    mCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    mCmdList->IASetVertexBuffers(0, 1, &mVBV);
    mCmdList->IASetIndexBuffer(&mIBV);

    const D3D12_GPU_VIRTUAL_ADDRESS bufferStart = mShadowCB->Resource()->GetGPUVirtualAddress();
    const UINT64 stride = Align256(sizeof(ShadowCB));
    for(UINT cascade = 0; cascade < ShadowCascadeCount; ++cascade)
    {
        const auto dsv = mShadowDsvs[cascade];
        mCmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        mCmdList->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        for(UINT objectIndex = 0; objectIndex < SceneObjectCount; ++objectIndex)
        {
            mCmdList->SetGraphicsRootConstantBufferView(
                0, bufferStart + static_cast<UINT64>(cascade * SceneObjectCount + objectIndex) * stride);
            const ObjectDraw& objectDraw = mObjectDraws[objectIndex];
            for(UINT subsetIndex = 0; subsetIndex < objectDraw.subsetCount; ++subsetIndex)
            {
                const UINT drawSubsetIndex = objectIndex == 0
                    ? objectDraw.firstSubset + subsetIndex
                    : mRockSubsetIndex + mObjectLods[objectIndex];
                const DrawSubset& subset = mDrawSubsets[drawSubsetIndex];
                mCmdList->DrawIndexedInstanced(subset.indexCount, 1, subset.indexStart, 0, 0);
            }
        }
    }

    barrier = Transition(mShadowMaps.Get(),
        D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    mCmdList->ResourceBarrier(1, &barrier);
}

void RenderingSystem::UpdateParticleConstants(
    const XMMATRIX& view, const XMMATRIX& viewProj, float dt)
{
    ParticleFrameCB constants{};
    XMStoreFloat4x4(&constants.viewProj, XMMatrixTranspose(viewProj));
    const XMMATRIX inverseView = XMMatrixInverse(nullptr, view);
    XMFLOAT3 right{};
    XMFLOAT3 up{};
    XMStoreFloat3(&right, XMVector3Normalize(inverseView.r[0]));
    XMStoreFloat3(&up, XMVector3Normalize(inverseView.r[1]));
    constants.cameraRight = XMFLOAT4(right.x, right.y, right.z, 0.0f);
    constants.cameraUp = XMFLOAT4(up.x, up.y, up.z, 0.0f);
    constants.emitterAndDelta = XMFLOAT4(
        mParticleEmitter.x, mParticleEmitter.y, mParticleEmitter.z,
        std::clamp(dt, 0.0f, 0.05f));
    if(!mPendingBursts.empty())
    {
        const XMFLOAT3 position = mPendingBursts.front();
        mPendingBursts.pop_front();
        constants.burstPositionAndActive = XMFLOAT4(
            position.x, position.y, position.z, 1.0f);
        ++mBurstCount;
        mNextTitleUpdate = 0.0f;
    }
    mParticleFrameCB->CopyData(0, constants);
}

void RenderingSystem::SimulateParticles()
{
    const UINT inputIndex = mParticleReadIndex;
    const UINT outputIndex = 1 - inputIndex;
    for(UINT index : {inputIndex, outputIndex})
    {
        if(mParticleBufferStates[index] != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            const auto barrier = Transition(mParticleBuffers[index].Get(),
                mParticleBufferStates[index], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            mCmdList->ResourceBarrier(1, &barrier);
            mParticleBufferStates[index] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
    }

    auto barrier = Transition(mParticleCounters[outputIndex].Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    mCmdList->ResourceBarrier(1, &barrier);
    mCmdList->CopyBufferRegion(mParticleCounters[outputIndex].Get(), 0,
        mParticleZeroUpload.Get(), sizeof(uint32_t), sizeof(uint32_t));
    barrier = Transition(mParticleCounters[outputIndex].Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    mCmdList->ResourceBarrier(1, &barrier);

    mCmdList->SetPipelineState(mParticleComputePSO.Get());
    mCmdList->SetComputeRootSignature(mParticleComputeRootSig.Get());
    mCmdList->SetComputeRootConstantBufferView(
        0, mParticleFrameCB->Resource()->GetGPUVirtualAddress());
    auto inputHandle = mSrvHeap->GetGPUDescriptorHandleForHeapStart();
    inputHandle.ptr += static_cast<UINT64>(mParticleUavOffset + inputIndex) * mSrvInc;
    auto outputHandle = mSrvHeap->GetGPUDescriptorHandleForHeapStart();
    outputHandle.ptr += static_cast<UINT64>(mParticleUavOffset + outputIndex) * mSrvInc;
    mCmdList->SetComputeRootDescriptorTable(1, inputHandle);
    mCmdList->SetComputeRootDescriptorTable(2, outputHandle);
    mCmdList->Dispatch((ParticleCount + 63) / 64, 1, 1);

    D3D12_RESOURCE_BARRIER uavBarriers[2]{};
    uavBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarriers[0].UAV.pResource = mParticleBuffers[outputIndex].Get();
    uavBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarriers[1].UAV.pResource = mParticleCounters[outputIndex].Get();
    mCmdList->ResourceBarrier(_countof(uavBarriers), uavBarriers);
    barrier = Transition(mParticleBuffers[outputIndex].Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    mCmdList->ResourceBarrier(1, &barrier);
    mParticleBufferStates[outputIndex] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    mParticleReadIndex = outputIndex;
}

void RenderingSystem::RenderParticles(D3D12_CPU_DESCRIPTOR_HANDLE rtv)
{
    const auto dsv = DSV();
    mCmdList->OMSetRenderTargets(1, &rtv, TRUE, &dsv);
    mCmdList->SetPipelineState(mParticleGraphicsPSO.Get());
    mCmdList->SetGraphicsRootSignature(mParticleGraphicsRootSig.Get());
    mCmdList->SetGraphicsRootConstantBufferView(
        0, mParticleFrameCB->Resource()->GetGPUVirtualAddress());
    auto particleHandle = mSrvHeap->GetGPUDescriptorHandleForHeapStart();
    particleHandle.ptr += static_cast<UINT64>(mParticleSrvOffset + mParticleReadIndex) * mSrvInc;
    mCmdList->SetGraphicsRootDescriptorTable(1, particleHandle);
    mCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    mCmdList->IASetVertexBuffers(0, 0, nullptr);
    mCmdList->IASetIndexBuffer(nullptr);
    mCmdList->DrawInstanced(ParticleCount, 1, 0, 0);
}

void RenderingSystem::RenderPostProcess()
{
    mCmdList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
    D3D12_RESOURCE_BARRIER barriers[2] = {
        Transition(mSceneColor.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        Transition(CurrentBackBuffer(), D3D12_RESOURCE_STATE_PRESENT,
            D3D12_RESOURCE_STATE_RENDER_TARGET)
    };
    mCmdList->ResourceBarrier(_countof(barriers), barriers);

    const auto backBufferRtv = CurrentRTV();
    const float clearColor[] = {0.0f, 0.0f, 0.0f, 1.0f};
    mCmdList->ClearRenderTargetView(backBufferRtv, clearColor, 0, nullptr);
    mCmdList->OMSetRenderTargets(1, &backBufferRtv, TRUE, nullptr);
    mCmdList->RSSetViewports(1, &mViewport);
    mCmdList->RSSetScissorRects(1, &mScissor);

    ID3D12PipelineState* postPipeline = mPostCopyPSO.Get();
    if(mPostEffect == PostEffect::Vignette)
        postPipeline = mPostVignettePSO.Get();
    else if(mPostEffect == PostEffect::GaussianBlur)
        postPipeline = mPostBlurPSO.Get();
    mCmdList->SetPipelineState(postPipeline);
    mCmdList->SetGraphicsRootSignature(mPostRootSig.Get());
    mCmdList->SetGraphicsRootDescriptorTable(0, mSceneColorSrv);
    mCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    mCmdList->IASetVertexBuffers(0, 0, nullptr);
    mCmdList->IASetIndexBuffer(nullptr);
    mCmdList->DrawInstanced(3, 1, 0, 0);

    mCmdList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
    barriers[0] = Transition(CurrentBackBuffer(), D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT);
    barriers[1] = Transition(mSceneColor.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    mCmdList->ResourceBarrier(_countof(barriers), barriers);
}

void RenderingSystem::UpdateWindowTitle(float totalTime)
{
    if(totalTime < mNextTitleUpdate)
        return;

    const wchar_t* mode = !mFrustumCullingEnabled
        ? L"OFF"
        : (mOctreeCullingEnabled ? L"OCTREE" : L"FRUSTUM");
    const int framesPerSecond = static_cast<int>(std::round(
        1000.0f / std::max(mSmoothedFrameMilliseconds, 0.01f)));

    std::wstring title = L"CG2.7 | culling " + std::wstring(mode) +
        L" | visible " + std::to_wstring(mVisibleObjects.size()) + L"/" +
        std::to_wstring(mObjectBounds.size()) +
        L" | LOD " + std::wstring(mLodEnabled ? L"on " : L"off ") +
        std::to_wstring(mVisibleRockLodCounts[0]) + L"/" +
        std::to_wstring(mVisibleRockLodCounts[1]) + L"/" +
        std::to_wstring(mVisibleRockLodCounts[2]) +
        L" | tests " + std::to_wstring(mCullingStats.testedObjects) + L" obj";
    if(mOctreeCullingEnabled)
    {
        title += L" + " + std::to_wstring(mCullingStats.testedNodes) + L" nodes";
    }
    title += L" | " + std::to_wstring(framesPerSecond) + L" FPS";
    title += L" | cull " +
        std::to_wstring(static_cast<int>(std::round(mCullingMicroseconds))) + L" us";
    title += L" | shadows " + std::wstring(mShadowsEnabled ? L"on" : L"off") +
        L" (" + std::to_wstring(ShadowCascadeCount) +
        (ShadowCascadeCount == 1 ? L" cascade)" : L" cascades)");
    title += L" | particles " + std::to_wstring(FountainParticleCount) +
        L" + burst " + std::to_wstring(BurstParticleCount) +
        L" | clicks " + std::to_wstring(mBurstCount);
    const wchar_t* postMode = mPostEffect == PostEffect::Vignette
        ? L"vignette"
        : (mPostEffect == PostEffect::GaussianBlur ? L"blur 3x3" : L"off");
    title += L" | post " + std::wstring(postMode);
    title += L" | 1/2/3 post | C/O culling | L LOD | H shadows | N/P/F legacy";
    SetWindowTextW(mHwnd, title.c_str());
    mNextTitleUpdate = totalTime + 0.25f;
}

void RenderingSystem::Update(float dt, float totalTime)
{
    if(dt > 0.0f && dt < 1.0f)
    {
        const float frameMilliseconds = dt * 1000.0f;
        mSmoothedFrameMilliseconds =
            mSmoothedFrameMilliseconds * 0.92f + frameMilliseconds * 0.08f;
    }

    const XMVECTOR eye = XMVectorSet(
        mCameraPosition.x, mCameraPosition.y, mCameraPosition.z, 1.0f);
    const XMVECTOR forward = XMVector3Normalize(XMLoadFloat3(&mCameraForward));
    const XMVECTOR up = XMVectorSet(0, 1, 0, 0);

    const XMMATRIX view = XMMatrixLookToLH(eye, forward, up);
    const XMMATRIX proj = XMMatrixPerspectiveFovLH(
        0.25f * XM_PI, static_cast<float>(mWidth) / static_cast<float>(mHeight), 0.1f, 450.0f);

    BoundingFrustum viewFrustum;
    BoundingFrustum::CreateFromMatrix(viewFrustum, proj);
    BoundingFrustum worldFrustum;
    viewFrustum.Transform(worldFrustum, XMMatrixInverse(nullptr, view));

    const auto cullingStart = std::chrono::steady_clock::now();
    UpdateVisibleObjects(worldFrustum);
    const auto cullingEnd = std::chrono::steady_clock::now();
    mCullingMicroseconds = std::chrono::duration<float, std::micro>(
        cullingEnd - cullingStart).count();
    UpdateObjectLods();
    UpdateObjectConstants(view * proj, mCameraPosition, totalTime);
    UpdateParticleConstants(view, view * proj, dt);
    UpdateWindowTitle(totalTime);

    mLightingData = {};
    mLightingData.eyePosition = mCameraPosition;
    mLightingData.ambientColor = XMFLOAT3(0.58f, 0.65f, 0.78f);
    mLightingData.ambientIntensity = 0.20f;
    mLightingData.lightCount = static_cast<uint32_t>(std::min<size_t>(mLights.size(), MaxLights));

    for(UINT i = 0; i < mLightingData.lightCount; ++i)
    {
        const Light& source = mLights[i];
        GpuLight& destination = mLightingData.lights[i];
        destination.position = source.position;
        destination.range = source.range;
        destination.direction = source.direction;
        destination.spotCosOuter = source.spotCosOuter;
        destination.color = source.color;
        destination.intensity = source.intensity;
        destination.spotCosInner = source.spotCosInner;
        destination.type = static_cast<uint32_t>(source.type);
    }
    if(!mLights.empty() && mLights.front().type == LightType::Directional)
        UpdateShadows(view);
}

void RenderingSystem::Render(float r, float g, float b)
{
    mLightingData.backgroundColor = XMFLOAT3(
        0.01f + r * 0.08f,
        0.01f + g * 0.08f,
        0.015f + b * 0.08f);
    mLightingCB->CopyData(0, mLightingData);

    ThrowIfFailed(mCmdAlloc->Reset());
    ID3D12PipelineState* geometryPipeline = mWireframeEnabled
        ? mGeometryWireframePSO.Get()
        : mGeometryPSO.Get();
    ThrowIfFailed(mCmdList->Reset(mCmdAlloc.Get(), geometryPipeline));

    ID3D12DescriptorHeap* heaps[] = {mSrvHeap.Get()};
    mCmdList->SetDescriptorHeaps(1, heaps);

    SimulateParticles();
    RenderShadows();
    mCmdList->SetPipelineState(geometryPipeline);
    mCmdList->RSSetViewports(1, &mViewport);
    mCmdList->RSSetScissorRects(1, &mScissor);

    mCmdList->ClearDepthStencilView(
        DSV(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
    mGBuffer.BeginGeometryPass(mCmdList.Get(), DSV());

    mCmdList->SetGraphicsRootSignature(mGeometryRootSig.Get());
    mCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    mCmdList->IASetVertexBuffers(0, 1, &mVBV);
    mCmdList->IASetIndexBuffer(&mIBV);

    const D3D12_GPU_VIRTUAL_ADDRESS sceneBufferStart =
        mSceneCB->Resource()->GetGPUVirtualAddress();
    const UINT64 sceneStride = Align256(sizeof(SceneCB));

    for(uint32_t visibleSlot = 0;
        visibleSlot < static_cast<uint32_t>(mVisibleObjects.size());
        ++visibleSlot)
    {
        mCmdList->SetGraphicsRootConstantBufferView(
            0, sceneBufferStart + static_cast<UINT64>(visibleSlot) * sceneStride);

        const uint32_t objectIndex = mVisibleObjects[visibleSlot];
        const ObjectDraw& objectDraw = mObjectDraws[objectIndex];
        for(UINT localSubset = 0; localSubset < objectDraw.subsetCount; ++localSubset)
        {
            const UINT drawSubsetIndex = objectIndex == 0
                ? objectDraw.firstSubset + localSubset
                : mRockSubsetIndex + mObjectLods[objectIndex];
            const DrawSubset& subset = mDrawSubsets[drawSubsetIndex];
            const UINT materialIndex = std::min(
                subset.materialIndex, static_cast<UINT>(mMaterials.size() - 1));
            const Material& material = mMaterials[materialIndex];

            const D3D12_GPU_VIRTUAL_ADDRESS materialAddress =
                mMaterialCB->Resource()->GetGPUVirtualAddress() +
                static_cast<UINT64>(materialIndex) * Align256(sizeof(MaterialCB));
            mCmdList->SetGraphicsRootConstantBufferView(1, materialAddress);

            auto textureHandle = mSrvHeap->GetGPUDescriptorHandleForHeapStart();
            textureHandle.ptr += static_cast<UINT64>(material.textureIndex) * mSrvInc;
            mCmdList->SetGraphicsRootDescriptorTable(2, textureHandle);

            mCmdList->DrawIndexedInstanced(subset.indexCount, 1, subset.indexStart, 0, 0);
        }
    }

    mGBuffer.EndGeometryPass(mCmdList.Get());

    const float clearColor[] = {0.0f, 0.0f, 0.0f, 1.0f};
    const auto rtv = mSceneColorRtv;
    mCmdList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    mCmdList->OMSetRenderTargets(1, &rtv, TRUE, nullptr);

    mCmdList->SetPipelineState(mLightingPSO.Get());
    mCmdList->SetGraphicsRootSignature(mLightingRootSig.Get());
    mCmdList->SetGraphicsRootConstantBufferView(0, mLightingCB->Resource()->GetGPUVirtualAddress());
    mCmdList->SetGraphicsRootDescriptorTable(1, mGBuffer.SrvGpuStart());
    mCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    mCmdList->IASetVertexBuffers(0, 0, nullptr);
    mCmdList->IASetIndexBuffer(nullptr);
    mCmdList->DrawInstanced(3, 1, 0, 0);

    RenderParticles(rtv);
    RenderPostProcess();

    ThrowIfFailed(mCmdList->Close());
    ID3D12CommandList* lists[] = {mCmdList.Get()};
    mQueue->ExecuteCommandLists(1, lists);

    ThrowIfFailed(mSwapChain->Present(1, 0));
    MoveToNextFrame();
}

ID3D12Resource* RenderingSystem::CurrentBackBuffer() const
{
    return mBackBuffers[mFrameIndex].Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE RenderingSystem::CurrentRTV() const
{
    auto h = mRtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(mFrameIndex) * mRtvInc;
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE RenderingSystem::DSV() const
{
    return mDsvHeap->GetCPUDescriptorHandleForHeapStart();
}

void RenderingSystem::Flush()
{
    const UINT64 fenceToWaitFor = ++mFenceValue;
    ThrowIfFailed(mQueue->Signal(mFence.Get(), fenceToWaitFor));

    if(mFence->GetCompletedValue() < fenceToWaitFor)
    {
        ThrowIfFailed(mFence->SetEventOnCompletion(fenceToWaitFor, mFenceEvent));
        WaitForSingleObject(mFenceEvent, INFINITE);
    }
}

void RenderingSystem::MoveToNextFrame()
{
    Flush();
    mFrameIndex = mSwapChain->GetCurrentBackBufferIndex();
}
