#include "App.h"

using namespace DirectX;

App::App(HINSTANCE hInst) : mHinst(hInst)
{
    WinWindow::Desc wd;
    wd.title = L"CG2.7 Post Processing";
    wd.width = 1280;
    wd.height = 720;
    wd.resizable = true;

    mWindow.Create(hInst, wd, &App::HandleMsgThunk, this);

    mRenderer.Initialize(mWindow.Hwnd(), mWindow.Width(), mWindow.Height());
}

int App::Run()
{
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
        if((lParam & (1LL << 30)) == 0)
        {
            if(wParam == 'N') mRenderer.ToggleNormalMapping();
            if(wParam == 'P') mRenderer.ToggleDisplacement();
            if(wParam == 'F') mRenderer.ToggleWireframe();
            if(wParam == 'C') mRenderer.ToggleFrustumCulling();
            if(wParam == 'O') mRenderer.ToggleOctreeCulling();
            if(wParam == 'H') mRenderer.ToggleShadows();
            if(wParam == 'L') mRenderer.ToggleLod();
            if(wParam == '1') mRenderer.SetPostEffect(RenderingSystem::PostEffect::None);
            if(wParam == '2') mRenderer.SetPostEffect(RenderingSystem::PostEffect::Vignette);
            if(wParam == '3') mRenderer.SetPostEffect(RenderingSystem::PostEffect::GaussianBlur);
            if(wParam == 'R')
            {
                mCameraPosition = XMFLOAT3(-9.0f, -4.0f, 0.0f);
                mCameraYaw = XM_PIDIV2;
                mCameraPitch = 0.0f;
            }
        }
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

        if((wParam & (MK_LBUTTON | MK_RBUTTON)) != 0)
        {
            const float dx = XMConvertToRadians(
                0.16f * static_cast<float>(x - mLastMouse.x));
            const float dy = XMConvertToRadians(
                0.16f * static_cast<float>(y - mLastMouse.y));
            mCameraYaw += dx;
            mCameraPitch = std::clamp(
                mCameraPitch - dy, -XM_PIDIV2 + 0.02f, XM_PIDIV2 - 0.02f);
        }

        mLastMouse = {x, y};
        return 1;
    }

    case WM_MOUSEWHEEL:
    {
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        mInput.OnMouseWheel(delta);
        mMoveSpeed += static_cast<float>(delta) /
            static_cast<float>(WHEEL_DELTA) * 5.0f;
        mMoveSpeed = std::clamp(mMoveSpeed, 5.0f, 120.0f);
        return 1;
    }
    }

    return 0;
}

void App::Update()
{
    const float dt = static_cast<float>(mTimer.DeltaSeconds());
    const float t = static_cast<float>(mTimer.TotalSeconds());

    const float cosPitch = cosf(mCameraPitch);
    const XMVECTOR forward = XMVector3Normalize(XMVectorSet(
        sinf(mCameraYaw) * cosPitch,
        sinf(mCameraPitch),
        cosf(mCameraYaw) * cosPitch,
        0.0f));
    const XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    const XMVECTOR right = XMVector3Normalize(XMVector3Cross(worldUp, forward));

    XMVECTOR movement = XMVectorZero();
    if(mInput.IsKeyDown('W')) movement += forward;
    if(mInput.IsKeyDown('S')) movement -= forward;
    if(mInput.IsKeyDown('D')) movement += right;
    if(mInput.IsKeyDown('A')) movement -= right;

    const float movementLengthSquared = XMVectorGetX(XMVector3LengthSq(movement));
    if(movementLengthSquared > 0.0001f)
    {
        const XMVECTOR position = XMLoadFloat3(&mCameraPosition) +
            XMVector3Normalize(movement) * (mMoveSpeed * dt);
        XMStoreFloat3(&mCameraPosition, position);
    }

    XMFLOAT3 forwardValue{};
    XMStoreFloat3(&forwardValue, forward);
    mRenderer.SetCamera(mCameraPosition, forwardValue);
    mRenderer.Update(dt, t);
}

void App::Draw()
{
    float t = static_cast<float>(mTimer.TotalSeconds());
    float r = 0.15f + 0.10f * sinf(t * 0.7f);
    float g = 0.18f + 0.10f * sinf(t * 0.9f + 1.0f);
    float b = 0.22f + 0.10f * sinf(t * 1.1f + 2.0f);

    mRenderer.Render(r, g, b);
}
