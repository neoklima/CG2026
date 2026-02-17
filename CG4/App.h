#pragma once
#include "Common.h"
#include "WinWindow.h"
#include "Timer.h"
#include "InputDevice.h"
#include "Dx12Renderer.h"

// окно + цикл сообщений + таймер + рендер
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
    Dx12Renderer mRenderer;

    bool mPaused = false;

    // камера 
    float mTheta = 1.5f * DirectX::XM_PI;
    float mPhi = DirectX::XM_PIDIV4;
    float mRadius = 5.0f;
    POINT mLastMouse{0,0};
};
