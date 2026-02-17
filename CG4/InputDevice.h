#pragma once
#include "Common.h"

// рефакторинг InputDevice
// окно вызывает методы 
// а игра читает IsKeyDown/MouseDelta и т.д.
class InputDevice
{
public:
    void NewFrame();

    void OnKeyDown(uint8_t vk);
    void OnKeyUp(uint8_t vk);

    void OnMouseMove(int x, int y);
    void OnMouseButtonDown(uint8_t buttonVk, int x, int y);
    void OnMouseButtonUp(uint8_t buttonVk, int x, int y);
    void OnMouseWheel(int delta);

    bool IsKeyDown(uint8_t vk) const;
    bool WasKeyPressed(uint8_t vk) const; // нажатие в этом кадре

    POINT MousePos() const { return mMousePos; }
    POINT MouseDelta() const { return mMouseDelta; }
    int WheelDelta() const { return mWheelDelta; }

private:
    std::array<uint8_t, 256> mCurrKeys{};
    std::array<uint8_t, 256> mPrevKeys{};

    POINT mMousePos{0,0};
    POINT mPrevMousePos{0,0};
    POINT mMouseDelta{0,0};

    int mWheelDelta = 0;
};
