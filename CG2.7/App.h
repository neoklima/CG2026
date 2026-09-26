#pragma once
#include "Common.h"
#include "WinWindow.h"
#include "Timer.h"
#include "InputDevice.h"
#include "RenderingSystem.h"

class App
{
public:
    explicit App(HINSTANCE hInst);

    int Run();

private:
    static LRESULT HandleMsgThunk(void* user, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMsg(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void Update();
    void Draw();

private:
    HINSTANCE mHinst = nullptr;

    WinWindow mWindow;
    Timer mTimer;
    InputDevice mInput;
    RenderingSystem mRenderer;

    bool mPaused = false;

    DirectX::XMFLOAT3 mCameraPosition = {-9.0f, -4.0f, 0.0f};
    float mCameraYaw = DirectX::XM_PIDIV2;
    float mCameraPitch = 0.0f;
    float mMoveSpeed = 25.0f;
    POINT mLastMouse{0,0};
};
