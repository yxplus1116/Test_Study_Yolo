#pragma once
#include "mouse.h"
#include "runtime/trt_detector.h"
#include <opencv2/core.hpp>
#include <string>
enum class FrameState { Ready, Waiting, Unavailable, Paused };
enum class TargetState { Selected, NoDetections, NoSelectedClass, AssociationPending, Inactive };
struct PreviewInfo {
    FrameState frame_state=FrameState::Waiting;
    int label=0,candidates=0,lost=0,max_lost=5;
    bool input_enabled=false,button_held=false,sample_fresh=false,in_dead_zone=false,key_guard=false;
    double fps_recent=0,fps_average=0,capture_ms=0;
    std::uint64_t frame=0,sent=0;
    Runtime::InferenceTimings timings;
    std::optional<ObjectDetector::Box> selected_box;
    std::string device,capture="gdi",mouse_backend="windows";
};
std::string class_name(int label);
TargetState preview_target_state(const PreviewInfo& info,const ObjectDetector::BoxArray& boxes,const TargetUpdate& target);
std::string target_status_text(const PreviewInfo& info,const ObjectDetector::BoxArray& boxes,const TargetUpdate& target);
std::string output_status_text(const PreviewInfo& info,const TargetUpdate& target);
cv::Mat render_preview(const cv::Mat& raw,const ObjectDetector::BoxArray& boxes,const TargetUpdate& target,const PreviewInfo& info);
