#include "application/pid.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

PidController::PidController(PidSettings s) : settings_(s) {
    for(double v:{s.kp,s.ki,s.kd,s.integral_limit,s.output_limit,s.dead_zone,s.units_per_pixel_x,s.units_per_pixel_y})
        if(!std::isfinite(v)) throw std::invalid_argument("PID values must be finite");
    if(s.kp<0 || s.ki<0 || s.kd<0 || s.integral_limit<0 || s.dead_zone<0 ||
       s.output_limit<1 || s.output_limit>1000 || s.period_ms<1 ||
       s.max_age_ms<s.period_ms || s.units_per_pixel_x<=0 || s.units_per_pixel_y<=0)
        throw std::invalid_argument("Invalid PID limits, period or calibration");
}
void PidController::reset() { x_={}; y_={}; }
int PidController::step_axis(Axis& a,double error,double dt,double scale) {
    if(std::abs(error)<=settings_.dead_zone) { a={}; return 0; }
    double derivative=a.initialized ? (error-a.previous)/dt : 0;
    double candidate=std::clamp(a.integral+error*dt,-settings_.integral_limit,settings_.integral_limit);
    auto output=[&](double integral){return scale*(settings_.kp*error+settings_.ki*integral+settings_.kd*derivative);};
    double tentative=output(candidate);
    // Conditional integration: do not wind up further into saturation.
    if(std::abs(tentative)<=settings_.output_limit || tentative*error<0) a.integral=candidate;
    a.previous=error; a.initialized=true;
    double bounded=std::clamp(output(a.integral),-settings_.output_limit,settings_.output_limit);
    if(!std::isfinite(bounded)){a={};return 0;}
    int limit=static_cast<int>(settings_.output_limit);
    int integer=std::clamp(static_cast<int>(std::round(bounded+a.residual)),-limit,limit);
    a.residual=std::clamp(bounded+a.residual-integer,-0.5,0.5);
    return integer;
}
Movement PidController::step(const TargetUpdate& t,double dt) {
    if(t.reset) reset();
    if(!t.valid || !std::isfinite(t.dx) || !std::isfinite(t.dy) || !std::isfinite(dt) || dt<=0 || dt>0.5)
        {reset();return {};}
    dt=std::max(dt,0.001);
    return {step_axis(x_,t.dx,dt,settings_.units_per_pixel_x),
            step_axis(y_,t.dy,dt,settings_.units_per_pixel_y),t.frame};
}
ControlWorker::ControlWorker(PidSettings settings,Output output)
    :settings_(settings),output_(std::move(output)),pid_(settings) {
    if(!output_) throw std::invalid_argument("Control output is required");
    worker_=std::thread(&ControlWorker::run,this);
}
ControlWorker::~ControlWorker(){stop();}
void ControlWorker::publish(TargetUpdate target) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(stopping_) return;
    target.reset=target.reset || latest_.reset;
    latest_=target; ++revision_; changed_.notify_one();
}
void ControlWorker::cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    latest_={}; latest_.reset=true; ++revision_; changed_.notify_one();
    // Dispatch also holds mutex_: no old dispatch can begin after this returns.
}
void ControlWorker::stop() {
    {std::lock_guard<std::mutex> lock(mutex_);stopping_=true;latest_={};}
    changed_.notify_all();
    if(worker_.joinable()) worker_.join();
}
bool ControlWorker::output_failed() const {std::lock_guard<std::mutex> lock(mutex_);return failed_;}
void ControlWorker::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    std::uint64_t seen=0;
    auto next=SteadyClock::now();
    SteadyClock::time_point previous{};
    while(!stopping_) {
        changed_.wait(lock,[&]{return stopping_ || revision_!=seen;});
        if(stopping_) break;
        if(!latest_.valid){seen=revision_;pid_.reset();previous={};latest_.reset=false;continue;}
        changed_.wait_until(lock,next,[&]{return stopping_ || !latest_.valid;});
        if(stopping_) break;
        if(!latest_.valid) continue;
        TargetUpdate sample=latest_; seen=revision_;latest_.reset=false;
        auto now=SteadyClock::now();
        if(sample.captured>now || now-sample.captured>std::chrono::milliseconds(settings_.max_age_ms))
            {pid_.reset();previous={};continue;}
        if(sample.reset)previous={};
        double dt=previous==SteadyClock::time_point{} ? settings_.period_ms/1000.0 :
            std::chrono::duration<double>(sample.captured-previous).count();
        previous=sample.captured;
        Movement move=pid_.step(sample,dt);
        next=now+std::chrono::milliseconds(settings_.period_ms);
        if(move.x || move.y) {
            try {if(!output_(move)){failed_=true;stopping_=true;}}
            catch(...){failed_=true;stopping_=true;}
        }
    }
    pid_.reset();
}
