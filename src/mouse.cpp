#include "application/mouse.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

TargetTracker::TargetTracker(TrackingSettings s):settings_(s) {
    if(!std::isfinite(s.confidence) || !std::isfinite(s.minimum_iou) ||
       !std::isfinite(s.confidence_weight) || !std::isfinite(s.body_y_fraction) ||
       s.confidence<0 || s.confidence>1 || s.minimum_iou<=0 || s.minimum_iou>1 ||
       s.confidence_weight<0 || s.body_y_fraction<0 || s.body_y_fraction>1 || s.max_lost_frames<1)
        throw std::invalid_argument("Invalid tracking settings");
}
void TargetTracker::reset(){target_={};selected_box_.reset();tracking_=false;label_=-1;lost_=count_=0;}
float TargetTracker::iou(const ObjectDetector::Box& a,const ObjectDetector::Box& b) {
    float aa=std::max(0.f,a.right-a.left)*std::max(0.f,a.bottom-a.top);
    float ba=std::max(0.f,b.right-b.left)*std::max(0.f,b.bottom-b.top);
    float inter=std::max(0.f,std::min(a.right,b.right)-std::max(a.left,b.left))*
        std::max(0.f,std::min(a.bottom,b.bottom)-std::max(a.top,b.top));
    float total=aa+ba-inter;
    return total>0 && std::isfinite(total)? inter/total:0;
}
TargetUpdate TargetTracker::update_target(const ObjectDetector::BoxArray& boxes,int width,int height,
    int label,std::uint64_t frame,SteadyClock::time_point captured) {
    selected_box_.reset();
    bool changed=label_!=label;
    if(changed) reset();
    label_=label;
    TargetUpdate result{false,changed,0,0,frame,captured};
    if(width<=0 || height<=0 || label<0){reset();result.reset=true;return result;}
    ObjectDetector::BoxArray candidates;
    for(auto box:boxes) {
        if(box.class_label!=label || !std::isfinite(box.confidence) || box.confidence<settings_.confidence ||
           !std::isfinite(box.left) || !std::isfinite(box.top) || !std::isfinite(box.right) || !std::isfinite(box.bottom)) continue;
        box.left=std::clamp(box.left,0.f,float(width));box.right=std::clamp(box.right,0.f,float(width));
        box.top=std::clamp(box.top,0.f,float(height));box.bottom=std::clamp(box.bottom,0.f,float(height));
        if(box.right>box.left && box.bottom>box.top) candidates.push_back(box);
    }
    count_=static_cast<int>(candidates.size()); // Telemetry, never an identity test.
    auto score=[&](const ObjectDetector::Box& b) {
        double dx=(b.left+b.right)/2.0-width/2.0,dy=(b.top+b.bottom)/2.0-height/2.0;
        return std::hypot(dx,dy)/std::hypot(width/2.0,height/2.0)-settings_.confidence_weight*b.confidence;
    };
    std::sort(candidates.begin(),candidates.end(),[&](const auto& a,const auto& b) {
        if(score(a)!=score(b)) return score(a)<score(b);
        if(a.confidence!=b.confidence) return a.confidence>b.confidence;
        if(a.left!=b.left) return a.left<b.left;
        return a.top<b.top;
    });
    int selected=-1;
    if(tracking_) {
        float best=settings_.minimum_iou;
        for(int i=0;i<int(candidates.size());++i) {
            float overlap=iou(target_,candidates[i]);
            if(overlap>=best && (selected<0 || overlap>best)){best=overlap;selected=i;}
        }
    } else if(!candidates.empty()){selected=0;result.reset=true;}
    if(selected<0) {
        if(lost_<settings_.max_lost_frames)++lost_;
        result.reset=true;
        if(lost_>=settings_.max_lost_frames){tracking_=false;target_={};}
        return result; // Identity retention never authorizes stale motion.
    }
    target_=candidates[selected];tracking_=true;lost_=0;
    selected_box_=target_;
    result.valid=true;
    result.dx=(target_.left+target_.right)/2.0-width/2.0;
    result.dy=target_.top+(target_.bottom-target_.top)*(label==1?0.5:settings_.body_y_fraction)-height/2.0;
    return result;
}
