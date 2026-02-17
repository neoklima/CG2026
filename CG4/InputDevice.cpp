#include "InputDevice.h"

void InputDevice::NewFrame()
{
    mPrevKeys = mCurrKeys;

    mMouseDelta.x = mMousePos.x - mPrevMousePos.x;
    mMouseDelta.y = mMousePos.y - mPrevMousePos.y;
    mPrevMousePos = mMousePos;

    mWheelDelta = 0; // сбрасываем каждую рамку
}

void InputDevice::OnKeyDown(uint8_t vk)
{
    mCurrKeys[vk] = 1;
}

void InputDevice::OnKeyUp(uint8_t vk)
{
    mCurrKeys[vk] = 0;
}

void InputDevice::OnMouseMove(int x, int y)
{
    mMousePos = {x, y};
}

void InputDevice::OnMouseButtonDown(uint8_t buttonVk, int x, int y)
{
    OnKeyDown(buttonVk);
    OnMouseMove(x, y);
}

void InputDevice::OnMouseButtonUp(uint8_t buttonVk, int x, int y)
{
    OnKeyUp(buttonVk);
    OnMouseMove(x, y);
}

void InputDevice::OnMouseWheel(int delta)
{
    mWheelDelta += delta;
}

bool InputDevice::IsKeyDown(uint8_t vk) const
{
    return mCurrKeys[vk] != 0;
}

bool InputDevice::WasKeyPressed(uint8_t vk) const
{
    return (mCurrKeys[vk] != 0) && (mPrevKeys[vk] == 0);
}
