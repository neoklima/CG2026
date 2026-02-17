#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

// Prevent Windows headers from defining min/max macros that break std::min/std::max.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <windowsx.h>
#include <wrl.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>

#include <DirectXMath.h>

#include <array>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")

using Microsoft::WRL::ComPtr;

inline void ThrowIfFailed(HRESULT hr)
{
    if(FAILED(hr))
    {
        throw std::runtime_error("HRESULT failed: 0x" + std::to_string(static_cast<unsigned long>(hr)));
    }
}

inline std::wstring ToWString(const std::string& s)
{
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring ws(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, ws.data(), len);
    if (!ws.empty() && ws.back() == L'\0') ws.pop_back();
    return ws;
}

inline void DebugName(ID3D12Object* obj, const wchar_t* name)
{
#if defined(_DEBUG)
    if (obj) obj->SetName(name);
#else
    (void)obj; (void)name;
#endif
}
