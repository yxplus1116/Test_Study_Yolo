#pragma once
#include <opencv2/core.hpp>
#include <memory>
#include <string>
#include <vector>
#include "pid.h"

struct MonitorGeometry {
    int index=0, left=0, top=0, width=0, height=0;
    std::string name;
};
enum class CaptureStatus { Ok, NoFrame, Unavailable };
struct CaptureResult {
    cv::Mat image; // Owned BGR pixels, independent of Map/ReleaseFrame.
    CaptureStatus status=CaptureStatus::Unavailable;
    SteadyClock::time_point captured{};
    std::string error;
};
class ScreenCapture {
public:
    static std::vector<MonitorGeometry> monitors();
    ScreenCapture(int monitor, int size, bool gdi);
    ~ScreenCapture();
    CaptureResult grab();
    MonitorGeometry geometry() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
