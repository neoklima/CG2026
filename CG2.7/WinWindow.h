#pragma once
#include "Common.h"

class WinWindow
{
public:
    struct Desc
    {
        int width = 1280;
        int height = 720;
        std::wstring title = L"CG4Unique - DX12";
        bool resizable = true;
    };

    using MsgHandler = LRESULT(*)(void* user, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

public:
    WinWindow() = default;
    ~WinWindow();

    WinWindow(const WinWindow&) = delete;
    WinWindow& operator=(const WinWindow&) = delete;

    void Create(HINSTANCE hInst, const Desc& desc, MsgHandler handler, void* user);

    HWND Hwnd() const { return mHwnd; }
    int Width() const { return mWidth; }
    int Height() const { return mHeight; }

    void SetTitle(const std::wstring& title);

private:
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT InstanceWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    HINSTANCE mHinst = nullptr;
    HWND mHwnd = nullptr;
    std::wstring mClassName;

    int mWidth = 0;
    int mHeight = 0;

    MsgHandler mHandler = nullptr;
    void* mUser = nullptr;
};
