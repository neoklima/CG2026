#include "Timer.h"

Timer::Timer()
{
    int64_t countsPerSec = 0;
    QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER*>(&countsPerSec));
    mSecondsPerCount = 1.0 / static_cast<double>(countsPerSec);

    Reset();
}

void Timer::Reset()
{
    int64_t t = 0;
    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&t));

    mBaseTime = t;
    mPrevTime = t;
    mStopTime = 0;
    mStopped = false;
    mPausedTime = 0;
}

void Timer::Start()
{
    if(!mStopped) return;

    int64_t startTime = 0;
    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&startTime));

    // время паузы
    mPausedTime += (startTime - mStopTime);

    mPrevTime = startTime;
    mStopTime = 0;
    mStopped = false;
}

void Timer::Stop()
{
    if(mStopped) return;

    int64_t t = 0;
    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&t));

    mStopTime = t;
    mStopped = true;
}

void Timer::Tick()
{
    if(mStopped)
    {
        mDeltaSeconds = 0.0;
        return;
    }

    int64_t t = 0;
    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&t));
    mCurrTime = t;

    mDeltaSeconds = (mCurrTime - mPrevTime) * mSecondsPerCount;
    mPrevTime = mCurrTime;

    if(mDeltaSeconds < 0.0)
        mDeltaSeconds = 0.0;
}

double Timer::TotalSeconds() const
{
    int64_t endTime = mStopped ? mStopTime : mCurrTime;
    return ((endTime - mPausedTime) - mBaseTime) * mSecondsPerCount;
}

double Timer::DeltaSeconds() const
{
    return mDeltaSeconds;
}
