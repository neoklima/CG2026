#pragma once
#include "Common.h"

class Timer
{
public:
    Timer();

    void Reset();
    void Start();
    void Stop();
    void Tick();

    double TotalSeconds() const;   // время с Reset, без паузы
    double DeltaSeconds() const;   // время между кадрами

private:
    double mSecondsPerCount = 0.0;
    double mDeltaSeconds = 0.0;

    int64_t mBaseTime = 0;
    int64_t mPausedTime = 0;
    int64_t mStopTime = 0;
    int64_t mPrevTime = 0;
    int64_t mCurrTime = 0;

    bool mStopped = false;
};
