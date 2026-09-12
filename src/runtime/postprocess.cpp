#include "trt_detector.h"
#include "application/mouse.h"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace Runtime {
std::vector<float> preprocess(const cv::Mat& image,int side,Letterbox& t){
    if(image.empty() || image.type()!=CV_8UC3 || side<=0)throw std::invalid_argument("Expected nonempty BGR uint8 image");
    t.scale=std::min(float(side)/image.cols,float(side)/image.rows);
    t.tx=(-t.scale*image.cols+side+t.scale-1)*0.5f;
    t.ty=(-t.scale*image.rows+side+t.scale-1)*0.5f;
    cv::Mat letterbox;
    cv::warpAffine(image,letterbox,cv::Matx23f(t.scale,0,t.tx,0,t.scale,t.ty),
        cv::Size(side,side),cv::INTER_LINEAR,cv::BORDER_CONSTANT,cv::Scalar(114,114,114));
    cv::cvtColor(letterbox,letterbox,cv::COLOR_BGR2RGB);
    letterbox.convertTo(letterbox,CV_32FC3,1.0/255);
    std::vector<float> data(size_t(side)*side*3);
    std::vector<cv::Mat> planes;
    for(int c=0;c<3;++c)planes.emplace_back(side,side,CV_32FC1,data.data()+size_t(c)*side*side);
    cv::split(letterbox,planes);
    return data;
}
ObjectDetector::BoxArray decode(const std::vector<float>& data,int rows,int attrs,const Letterbox& t,
    int width,int height,float confidence,float nms){
    if(rows<0 || attrs<6 || data.size()!=size_t(rows)*attrs || width<=0 || height<=0 ||
       !std::isfinite(t.scale) || t.scale<=0 || !std::isfinite(t.tx) || !std::isfinite(t.ty) ||
       !std::isfinite(confidence) || confidence<0 || confidence>1 || !std::isfinite(nms) || nms<=0 || nms>1)
        throw std::invalid_argument("Invalid detector output or thresholds");
    ObjectDetector::BoxArray boxes;
    for(int i=0;i<rows;++i){
        const float* p=data.data()+size_t(i)*attrs;
        if(!std::all_of(p,p+attrs,[](float x){return std::isfinite(x);}) || p[2]<=0 || p[3]<=0 || p[4]<confidence)continue;
        int label=int(std::max_element(p+5,p+attrs)-(p+5));
        float score=p[4]*p[5+label];if(score<confidence || score>1)continue;
        float left=std::clamp((p[0]-p[2]*0.5f-t.tx)/t.scale,0.f,float(width));
        float right=std::clamp((p[0]+p[2]*0.5f-t.tx)/t.scale,0.f,float(width));
        float top=std::clamp((p[1]-p[3]*0.5f-t.ty)/t.scale,0.f,float(height));
        float bottom=std::clamp((p[1]+p[3]*0.5f-t.ty)/t.scale,0.f,float(height));
        if(right>left && bottom>top)boxes.emplace_back(left,top,right,bottom,score,label);
    }
    std::stable_sort(boxes.begin(),boxes.end(),[](const auto& a,const auto& b){return a.confidence>b.confidence;});
    ObjectDetector::BoxArray kept;
    for(const auto& candidate:boxes){
        bool suppressed=false;
        for(const auto& accepted:kept)
            if(candidate.class_label==accepted.class_label && TargetTracker::iou(candidate,accepted)>nms){suppressed=true;break;}
        if(!suppressed)kept.push_back(candidate);
        if(kept.size()>=1024)break;
    }
    return kept;
}
}
