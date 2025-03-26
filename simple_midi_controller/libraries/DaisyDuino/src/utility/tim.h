#pragma once
#ifndef DSY_TIM_H
#define DSY_TIM_H

#include <cstdint>

namespace daisy
{
/** Hardare timer peripheral support.
 ** 
 ** Supports general-function TIM peripherals:
 ** - TIM2, TIM3, TIM4, TIM5
 **
 ** DaisySeed, and many internal peripherals utilize TIM2 for timing/delay purposes.
 ** It is configured to be at the maximum frequency: typically 200MHz or 240MHz (boost) 
 ** for measuring/delaying for very short periods. 
 **
 ** The GetUs/GetMs functions are available for convenience (and backwards compatibility),
 ** but to avoid wrapping errors on math when doing time-delta calculations, using ticks is 
 ** recommended. The data can be converted to the final time-base after getting the difference in ticks.
 ** (Using GetFreq() can be used for these time-base calculations).
 **
 ** TODO:
 ** - Fix issues with realtime getters, and wrapping of the timer(s).
 **     - This very noticeable with default settings for the 16-bit counters.
 ** - Dispatch periodic callback(s)
 ** - Other General purpose timers
 ** - Non-internal clock sources
 ** - Use of the four-tim channels per tim
 **     - PWM, etc.
 **     - InputCapture/OutputCompare, etc.
 ** - HRTIM
 ** - Advanced timers (TIM1/TIM8)
 ** */
class TimerHandle
{
  public:
    struct Config
    {
        /** Hardwaare Timer to configure, and use. */
        enum class Peripheral
        {
            TIM_2 = 0, /**< 32-bit counter */
            TIM_3,     /**< 16-bit counter */
            TIM_4,     /**< 16-bit counter */
            TIM_5,     /**< 32-bit counter*/
        };

        /** Direction of the auto-reload counter. 
         ** TODO: Add support for the various  
         ** versions of Up/Down counters.
         ** */
        enum class CounterDir
        {
            UP = 0,
            DOWN,
        };

        Peripheral periph;
        CounterDir dir;
    };

    /** Return values for TIM funcitons. */
    enum class Result
    {
        OK,
        ERR,
    };

    TimerHandle() : pimpl_(nullptr) {}
    TimerHandle(const TimerHandle& other) = default;
    TimerHandle& operator=(const TimerHandle& other) = default;
    ~TimerHandle() {}

    /** Initializes the timer according to the configuration */
    Result Init(const Config& config);

    /** Returns a const reference to the Config struct */
    const Config& GetConfig() const;

    /** Sets the period of the Timer.
     ** This is the number of ticks it takes before it wraps back around.
     ** For self-managed timing, this can be left at the default. (0xffff for 16-bit
     ** and 0xffffffff for 32-bit timers). 
     ** This can be changed "on-the-fly" 
     ** */
    Result SetPeriod(uint32_t ticks);

    /** Sets the Prescalar applied to the TIM peripheral. 
     ** This can be any number up to 0xffff 
     ** This will adjust the rate of ticks:
     ** Calculated as APBN_Freq / prescalar per tick
     ** where APBN is APB1 for Most general purpose timers,
     ** and APB2 for HRTIM,a nd the advanced timers. 
     ** This can be changed "on-the-fly" 
     ** */
    Result SetPrescaler(uint32_t val);

    /** Starts the TIM peripheral specified by Config */
    Result Start();

    /** Stops the TIM peripheral specified by Config */
    Result Stop();

    /** Returns the frequency of each tick of the timer in Hz */
    uint32_t GetFreq();

    /** Returns the number of counter position. 
     ** This increments according to Config::CounterDir, 
     ** and wraps around at the specified period (maxing out 
     ** at 2^16 or 2^32 depending on the chosen TIM peripheral. */
    uint32_t GetTick();

    /** Returns the ticks scaled as milliseconds 
     **
     ** Use care when using for measurements and ensure that 
     ** the TIM period can handle the maximum desired measurement.
     ***/
    uint32_t GetMs();

    /** Returns the ticks scaled as microseconds 
     **
     ** Use care when using for measurements and ensure that 
     ** the TIM period can handle the maximum desired measurement.
     ***/
    uint32_t GetUs();

    /** Stay within this function for del ticks */
    void DelayTick(uint32_t del);

    /** Stay within this function for del milliseconds */
    void DelayMs(uint32_t del);

    /** Stay within this function for del microseconds */
    void DelayUs(uint32_t del);

    class Impl;

  private:
    Impl* pimpl_;
};

} // namespace daisy

#endif
