#pragma once
#include "common/object_detector.hpp"
#include "pid.h"
#include <optional>
struct TrackingSettings {
    float confidence=0.5f,minimum_iou=0.2f;
    float confidence_weight=0.25f,body_y_fraction=0.35f;
    int max_lost_frames=5;
};
class TargetTracker {
public:
    explicit TargetTracker(TrackingSettings settings={});
    TargetUpdate update_target(const ObjectDetector::BoxArray& boxes,int width,int height,
        int label,std::uint64_t frame,SteadyClock::time_point captured);
    void reset();
    int lost_frames() const{return lost_;}
    int aims_last_num() const{return count_;}
    const std::optional<ObjectDetector::Box>& selected_box() const{return selected_box_;}
    static float iou(const ObjectDetector::Box& a,const ObjectDetector::Box& b);
private:
    TrackingSettings settings_;
    ObjectDetector::Box target_{};
    // Current-frame presentation metadata; retained tracking identity stays in target_.
    std::optional<ObjectDetector::Box> selected_box_;
    bool tracking_=false;
    int label_=-1,lost_=0,count_=0;
};
