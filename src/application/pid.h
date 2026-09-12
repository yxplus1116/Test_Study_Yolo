#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

using SteadyClock = std::chrono::steady_clock;
struct TargetUpdate {
    bool valid=false, reset=false;
    double dx=0, dy=0;
    std::uint64_t frame=0;
    SteadyClock::time_point captured{};
};
struct PidSettings {
    double kp=0.3, ki=0.05, kd=0.002;
    double integral_limit=50, output_limit=12, dead_zone=0.75;
    double units_per_pixel_x=1, units_per_pixel_y=1;
    int period_ms=8, max_age_ms=100;
};
struct Movement { int x=0, y=0; std::uint64_t frame=0; };
class PidController {
public:
    explicit PidController(PidSettings settings={});
    Movement step(const TargetUpdate& target,double dt);
    void reset();
    double integral_x() const { return x_.integral; }
private:
    struct Axis { double integral=0,previous=0,residual=0; bool initialized=false; };
    int step_axis(Axis& axis,double error,double dt,double scale);
    PidSettings settings_;
    Axis x_,y_;
};
// Only the worker owns PID state and dispatches output. Measurements are
// consumed at most once; faster producers replace pending measurements.
class ControlWorker {
public:
    using Output=std::function<bool(const Movement&)>;
    ControlWorker(PidSettings settings,Output output);
    ~ControlWorker();
    ControlWorker(const ControlWorker&)=delete;
    ControlWorker& operator=(const ControlWorker&)=delete;
    void publish(TargetUpdate target);
    void cancel();
    void stop();
    bool output_failed() const;
private:
    void run();
    PidSettings settings_;
    Output output_;
    PidController pid_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    TargetUpdate latest_;
    std::uint64_t revision_=0;
    bool stopping_=false,failed_=false;
    std::thread worker_;
};
