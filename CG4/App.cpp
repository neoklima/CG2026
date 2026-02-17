#include "App.h"

using namespace DirectX;

App::App(HINSTANCE hInst) : mHinst(hInst)
{
    WinWindow::Desc wd;
    wd.title = L"CG4Unique - DX12 (clear + Phong cube)";
    wd.width = 1280;
    wd.height = 720;
    wd.resizable = true;

    mWindow.Create(hInst, wd, &App::HandleMsgThunk, this);

    mRenderer.Initialize(mWindow.Hwnd(), mWindow.Width(), mWindow.Height());
}

int App::Run()
{
    // цикл обработки сообщений
    MSG msg{};
    mTimer.Reset();

    while(msg.message != WM_QUIT)
    {
        if(PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        else
        {
            mTimer.Tick();
            mInput.NewFrame();

            if(!mPaused)
            {
                Update();
                Draw();
            }
            else
            {
                Sleep(20);
            }
        }
    }

    return static_cast<int>(msg.wParam);
}

LRESULT App::HandleMsgThunk(void* user, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    return reinterpret_cast<App*>(user)->HandleMsg(hwnd, msg, wParam, lParam);
}

LRESULT App::HandleMsg(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch(msg)
    {
    case WM_ACTIVATE:
        if(LOWORD(wParam) == WA_INACTIVE) { mPaused = true; mTimer.Stop(); }
        else { mPaused = false; mTimer.Start(); }
        return 1;

    case WM_SIZE:
        if(wParam != SIZE_MINIMIZED)
        {
            int w = LOWORD(lParam);
            int h = HIWORD(lParam);
            mRenderer.Resize(w, h);
        }
        return 1;

    case WM_KEYDOWN:
        mInput.OnKeyDown(static_cast<uint8_t>(wParam));
        if(wParam == VK_ESCAPE)
            DestroyWindow(hwnd);
        return 1;

    case WM_KEYUP:
        mInput.OnKeyUp(static_cast<uint8_t>(wParam));
        return 1;

    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        mInput.OnMouseButtonDown(VK_LBUTTON, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        mLastMouse = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        return 1;

    case WM_LBUTTONUP:
        ReleaseCapture();
        mInput.OnMouseButtonUp(VK_LBUTTON, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 1;

    case WM_RBUTTONDOWN:
        SetCapture(hwnd);
        mInput.OnMouseButtonDown(VK_RBUTTON, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        mLastMouse = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        return 1;

    case WM_RBUTTONUP:
        ReleaseCapture();
        mInput.OnMouseButtonUp(VK_RBUTTON, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 1;

    case WM_MOUSEMOVE:
    {
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);
        mInput.OnMouseMove(x, y);

        if(mInput.IsKeyDown(VK_LBUTTON))
        {
            float dx = XMConvertToRadians(0.25f * static_cast<float>(x - mLastMouse.x));
            float dy = XMConvertToRadians(0.25f * static_cast<float>(y - mLastMouse.y));
            mTheta += dx;
            mPhi += dy;
            mPhi = std::clamp(mPhi, 0.1f, XM_PI - 0.1f);
        }
        else if(mInput.IsKeyDown(VK_RBUTTON))
        {
            float dx = 0.005f * static_cast<float>(x - mLastMouse.x);
            float dy = 0.005f * static_cast<float>(y - mLastMouse.y);
            mRadius += dx - dy;
            mRadius = std::clamp(mRadius, 2.0f, 20.0f);
        }

        mLastMouse = {x, y};
        return 1;
    }

    case WM_MOUSEWHEEL:
        mInput.OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        return 1;
    }

    return 0; // 0 => WinWindow вызовет DefWindowProc
}

void App::Update()
{
    const float dt = static_cast<float>(mTimer.DeltaSeconds());
    const float t = static_cast<float>(mTimer.TotalSeconds());

    // передаём камеру в рендер.
    mRenderer.SetOrbitCamera(mTheta, mPhi, mRadius);
    mRenderer.Update(dt, t);
}

void App::Draw()
{
    // очистка бэк-буфера цветом.
    float t = static_cast<float>(mTimer.TotalSeconds());
    float r = 0.15f + 0.10f * sinf(t * 0.7f);
    float g = 0.18f + 0.10f * sinf(t * 0.9f + 1.0f);
    float b = 0.22f + 0.10f * sinf(t * 1.1f + 2.0f);

    mRenderer.Render(r, g, b);
}
