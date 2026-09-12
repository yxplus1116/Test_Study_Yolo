#include "preview.h"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <vector>
namespace {
std::string box_label(const ObjectDetector::Box& box){
    return class_name(box.class_label)+" "+std::to_string(int(std::round(box.confidence*100)))+"%";
}
const cv::Scalar candidate_color(60,230,80),other_color(140,140,140),selected_color(0,220,255);
}
std::string class_name(int label){return label==0?"BODY C0":label==1?"HEAD C1":"CLASS "+std::to_string(label);}
TargetState preview_target_state(const PreviewInfo& info,const ObjectDetector::BoxArray& boxes,const TargetUpdate& target){
    if(info.frame_state!=FrameState::Ready)return TargetState::Inactive;
    if(target.valid)return TargetState::Selected;
    if(boxes.empty())return TargetState::NoDetections;
    if(info.candidates==0)return TargetState::NoSelectedClass;
    return TargetState::AssociationPending;
}
std::string target_status_text(const PreviewInfo& info,const ObjectDetector::BoxArray& boxes,const TargetUpdate& target){
    if(info.frame_state==FrameState::Paused)return "PAUSED - press RIGHT to resume";
    if(info.frame_state==FrameState::Waiting)return "Waiting for a NEW frame - no target";
    if(info.frame_state==FrameState::Unavailable)return "Capture unavailable - no target";
    switch(preview_target_state(info,boxes,target)){
        case TargetState::Selected:
            return info.selected_box?"SELECTED "+box_label(*info.selected_box)+" - yellow box / red control point":
                "SELECTED - red circle marks the control point";
        case TargetState::NoDetections:return "No detections above confidence threshold";
        case TargetState::NoSelectedClass:return "No "+class_name(info.label)+" candidate (other classes detected)";
        case TargetState::AssociationPending:return "IoU match pending / reacquiring - output stopped";
        default:return "Inactive";
    }
}
std::string output_status_text(const PreviewInfo& info,const TargetUpdate& target){
    if(!info.input_enabled)return "DRY RUN - mouse output OFF";
    if(info.frame_state==FrameState::Paused)return "OFF - recognition paused";
    if(info.frame_state!=FrameState::Ready || !target.valid)return "WAIT - no valid target";
    if(!info.button_held)return "WAIT - hold LEFT or RIGHT mouse button";
    if(info.key_guard)return "WAIT - release the direction key";
    if(!info.sample_fresh)return "OFF - measurement older than allowed age";
    if(info.in_dead_zone)return "IDLE - target is in the center dead zone";
    return "ELIGIBLE - actual sent count is shown below";
}
cv::Mat render_preview(const cv::Mat& raw,const ObjectDetector::BoxArray& boxes,const TargetUpdate& target,const PreviewInfo& info){
    cv::Mat picture=raw.empty()?cv::Mat(416,416,CV_8UC3,cv::Scalar(30,30,30)):raw.clone();
    bool ready=info.frame_state==FrameState::Ready;
    if(ready){
        struct Annotation {ObjectDetector::Box box;int priority;};
        std::vector<Annotation> annotations;
        const auto* selected=preview_target_state(info,boxes,target)==TargetState::Selected && info.selected_box?
            &*info.selected_box:nullptr;
        if(selected)annotations.push_back({*selected,0});
        auto same_selected=[&](const ObjectDetector::Box& b){
            return selected && b.class_label==selected->class_label && b.confidence==selected->confidence &&
                std::clamp(b.left,0.f,float(picture.cols))==selected->left &&
                std::clamp(b.right,0.f,float(picture.cols))==selected->right &&
                std::clamp(b.top,0.f,float(picture.rows))==selected->top &&
                std::clamp(b.bottom,0.f,float(picture.rows))==selected->bottom;
        };
        for(const auto& b:boxes){
            if(!same_selected(b))annotations.push_back({b,b.class_label==info.label?1:2});
        }
        std::stable_sort(annotations.begin(),annotations.end(),[](const auto& a,const auto& b){return a.priority<b.priority;});
        auto color=[](int priority){return priority==0?selected_color:priority==1?candidate_color:other_color;};
        // Paint other classes first so the current candidates remain distinguishable.
        for(auto it=annotations.rbegin();it!=annotations.rend();++it){
            const auto& b=it->box;
            cv::rectangle(picture,cv::Point(int(b.left),int(b.top)),cv::Point(int(b.right),int(b.bottom)),
                color(it->priority),it->priority==0?2:1);
        }
        std::vector<cv::Rect> occupied;
        for(const auto& item:annotations){
            const auto& b=item.box;
            std::string label=(item.priority==0?"SELECTED ":"")+box_label(b);
            int baseline=0;auto extent=cv::getTextSize(label,cv::FONT_HERSHEY_SIMPLEX,.44,1,&baseline);
            int width=extent.width+6,height=extent.height+baseline+6;
            // Never truncate a caption into a misleading class or confidence value.
            // The selected caption is also available in the fixed header.
            if(width>picture.cols || height>picture.rows)continue;
            bool placed=false;cv::Rect caption;
            for(int y:{int(b.top)-height-2,int(b.bottom)+2,int(b.top)+2,int(b.bottom)-height-2}){
                for(int x:{int(b.left),int(b.right)-width,int((b.left+b.right-width)/2)}){
                    cv::Rect proposal(std::clamp(x,0,picture.cols-width),std::clamp(y,0,picture.rows-height),width,height);
                    bool overlaps=std::any_of(occupied.begin(),occupied.end(),[&](const auto& r){
                        return (proposal & cv::Rect(r.x-2,r.y-2,r.width+4,r.height+4)).area()>0;
                    });
                    if(!overlaps){caption=proposal;placed=true;break;}
                }
                if(placed)break;
            }
            if(!placed)continue;
            occupied.push_back(caption);
            cv::rectangle(picture,caption,cv::Scalar(22,22,22),cv::FILLED);
            cv::putText(picture,label,cv::Point(caption.x+3,caption.y+extent.height+2),cv::FONT_HERSHEY_SIMPLEX,.44,
                color(item.priority),1,cv::LINE_AA);
        }
        if(selected)cv::rectangle(picture,cv::Point(int(selected->left),int(selected->top)),
            cv::Point(int(selected->right),int(selected->bottom)),selected_color,2);
        cv::drawMarker(picture,cv::Point(picture.cols/2,picture.rows/2),cv::Scalar(255,180,0),cv::MARKER_CROSS,12,1);
        if(preview_target_state(info,boxes,target)==TargetState::Selected)
            cv::circle(picture,cv::Point(int(target.dx+picture.cols/2.0),int(target.dy+picture.rows/2.0)),5,cv::Scalar(0,0,255),2,cv::LINE_AA);
    }
    if(picture.cols>960 || picture.rows>960){
        double scale=960.0/std::max(picture.cols,picture.rows);
        cv::resize(picture,picture,{},scale,scale,cv::INTER_AREA);
    }
    constexpr int header=166,footer=30;
    int width=std::max(680,picture.cols);
    cv::Mat view(header+picture.rows+footer,width,CV_8UC3,cv::Scalar(24,27,31));
    picture.copyTo(view(cv::Rect((width-picture.cols)/2,header,picture.cols,picture.rows)));
    auto line=[&](const std::string& text,int y,const cv::Scalar& color=cv::Scalar(220,225,230)){
        cv::putText(view,text,cv::Point(12,y),cv::FONT_HERSHEY_SIMPLEX,.48,color,1,cv::LINE_AA);
    };
    line("TestStudyYolo  |  "+class_name(info.label)+"  |  "+(info.input_enabled?"INPUT ENABLED":"DETECTION ONLY"),23,
        info.input_enabled?cv::Scalar(80,190,255):cv::Scalar(150,225,170));
    line("GPU: "+info.device+"  |  capture: "+info.capture,47);
    line(target_status_text(info,boxes,target),71,
        preview_target_state(info,boxes,target)==TargetState::Selected?cv::Scalar(80,230,130):cv::Scalar(100,195,255));
    line(output_status_text(info,target),95);
    line(cv::format("Boxes: %d   candidates: %d   lost: %d/%d   sent: %llu",int(boxes.size()),info.candidates,
        info.lost,info.max_lost,static_cast<unsigned long long>(info.sent)),119);
    line(ready?cv::format("Loop FPS: %.1f  avg: %.1f | GPU: %.2f ms | capture: %.2f ms",
        info.fps_recent,info.fps_average,info.timings.gpu_inference_ms,info.capture_ms):
        cv::format("Loop FPS: %.1f  avg: %.1f | GPU / capture: no new measurement",info.fps_recent,info.fps_average),143);
    line("UP=head  DOWN=body  LEFT=pause  RIGHT=resume  ESC=exit",view.rows-10);
    return view;
}
