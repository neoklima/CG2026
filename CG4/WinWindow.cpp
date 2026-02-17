#include "WinWindow.h"

WinWindow::~WinWindow()
{
    if(mHwnd)
        DestroyWindow(mHwnd);

    if(!mClassName.empty() && mHinst)
        UnregisterClassW(mClassName.c_str(), mHinst);
}

void WinWindow::Create(HINSTANCE hInst, const Desc& desc, MsgHandler handler, void* user)
{
    mHinst = hInst;
    mHandler = handler;
    mUser = user;
    mWidth = desc.width;
    mHeight = desc.height;

    // класса окна
    mClassName = L"CG4Unique_WindowClass_" + std::to_wstring(reinterpret_cast<uintptr_t>(this));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &WinWindow::StaticWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = mClassName.c_str();

    if(!RegisterClassExW(&wc))
        throw std::runtime_error("RegisterClassEx failed");

    DWORD style = WS_OVERLAPPEDWINDOW;
    if(!desc.resizable)
        style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);

    RECT r{0, 0, desc.width, desc.height};
    AdjustWindowRect(&r, style, FALSE);

    mHwnd = CreateWindowExW(
        0, mClassName.c_str(), desc.title.c_str(), style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top,
        nullptr, nullptr, hInst, this);

    if(!mHwnd)
        throw std::runtime_error("CreateWindowEx failed");

    ShowWindow(mHwnd, SW_SHOW);
    UpdateWindow(mHwnd);
}

void WinWindow::SetTitle(const std::wstring& title)
{
    if(mHwnd) SetWindowTextW(mHwnd, title.c_str());
}

LRESULT CALLBACK WinWindow::StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    WinWindow* self = nullptr;

    if(msg == WM_NCCREATE)
    {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<WinWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->mHwnd = hwnd;
    }
    else
    {
        self = reinterpret_cast<WinWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if(self)
        return self->InstanceWndProc(hwnd, msg, wParam, lParam);

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT WinWindow::InstanceWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // сообщение приложению 
    if(mHandler)
    {
        if(LRESULT r = mHandler(mUser, hwnd, msg, wParam, lParam); r != 0)
            return r;
    }

    switch(msg)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}
