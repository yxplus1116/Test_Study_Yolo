#include "application/mouse.h"
#include "runtime/trt_detector.h"
#include "application/preview.h"
#include <opencv2/core.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(__func__)+":"+std::to_string(__LINE__)+" " #x);}while(0)
using namespace std::chrono_literals;
using ObjectDetector::Box;
TargetUpdate target(double x,double y,std::uint64_t frame=1){return {true,false,x,y,frame,SteadyClock::now()};}
template<class F>bool wait_for(F predicate,int ms=500){
    auto end=SteadyClock::now()+std::chrono::milliseconds(ms);
    while(!predicate() && SteadyClock::now()<end)std::this_thread::sleep_for(1ms);
    return predicate();
}
void tracking(){
    CHECK(TargetTracker::iou(Box(0,0,10,10,1,0),Box(10,0,20,10,1,0))==0);
    CHECK(std::abs(TargetTracker::iou(Box(0,0,10,10,1,0),Box(5,0,15,10,1,0))-1.f/3)<1e-5);
    TrackingSettings s;s.max_lost_frames=2;s.body_y_fraction=.5f;
    TargetTracker tracker(s);auto now=SteadyClock::now();
    ObjectDetector::BoxArray boxes{{0,0,10,10,.9f,0},{40,40,60,60,.8f,0}};
    auto a=tracker.update_target(boxes,100,100,0,1,now);
    CHECK(a.valid && a.reset && a.dx==0 && a.dy==0 && tracker.aims_last_num()==2);
    std::reverse(boxes.begin(),boxes.end());TargetTracker reversed(s);
    auto b=reversed.update_target(boxes,100,100,0,2,now);CHECK(a.dx==b.dx && a.dy==b.dy);
    a=tracker.update_target({{70,70,90,90,.99f,0},{0,0,10,10,.9f,0}},100,100,0,3,now);
    CHECK(!a.valid && a.reset && tracker.lost_frames()==1 && tracker.aims_last_num()==2);
    a=tracker.update_target({{41,41,61,61,.8f,0}},100,100,0,4,now);
    CHECK(a.valid && tracker.lost_frames()==0 && tracker.aims_last_num()==1);
    a=tracker.update_target({},100,100,0,5,now);CHECK(!a.valid && tracker.aims_last_num()==0);
    tracker.update_target({},100,100,0,6,now);
    a=tracker.update_target({{70,70,90,90,.99f,0}},100,100,0,7,now);
    CHECK(a.valid && a.reset && tracker.lost_frames()==0);
    a=tracker.update_target({{70,70,90,90,.99f,0}},100,100,1,8,now);
    CHECK(!a.valid && a.reset && tracker.aims_last_num()==0);
    auto nan=std::numeric_limits<float>::quiet_NaN();
    a=tracker.update_target({{nan,0,10,10,1,1},{2,2,1,1,1,1}},100,100,1,9,now);
    CHECK(!a.valid && tracker.aims_last_num()==0);
}
void selection_metadata(){
    TargetTracker tracker;auto now=SteadyClock::now();
    // A closer 60% head wins acquisition; a 99% body is not a head candidate.
    ObjectDetector::BoxArray boxes{{30,30,70,70,.79f,1},{188,188,228,228,.60f,1},{178,178,238,300,.99f,0}};
    auto a=tracker.update_target(boxes,416,416,1,1,now);
    CHECK(a.valid && a.reset && a.dx==0 && a.dy==0 && tracker.aims_last_num()==2);
    CHECK(tracker.selected_box() && tracker.selected_box()->class_label==1 && tracker.selected_box()->confidence==.60f);
    // The 99% head would win new acquisition, but the old head has greater IoU.
    boxes={{164,188,204,228,.99f,1},{194,188,234,228,.60f,1}};
    TargetTracker fresh;auto new_pick=fresh.update_target(boxes,416,416,1,2,now);
    CHECK(new_pick.valid && fresh.selected_box()->confidence==.99f);
    a=tracker.update_target(boxes,416,416,1,2,now);
    CHECK(a.valid && !a.reset && a.dx==6 && tracker.selected_box()->confidence==.60f);
    a=tracker.update_target({{300,300,340,340,.99f,1}},416,416,1,3,now);
    CHECK(!a.valid && !tracker.selected_box() && tracker.lost_frames()==1);
    // Lost-frame metadata is cleared while the retained identity can still match.
    a=tracker.update_target({{166,188,206,228,.99f,1},{196,190,236,230,.58f,1}},416,416,1,4,now);
    CHECK(a.valid && !a.reset && tracker.selected_box()->confidence==.58f && tracker.lost_frames()==0);
    tracker.reset();CHECK(!tracker.selected_box());
    a=tracker.update_target(boxes,416,416,1,5,now);CHECK(a.valid && tracker.selected_box());
    a=tracker.update_target(boxes,416,416,0,6,now);CHECK(!a.valid && !tracker.selected_box());
}
void pid(){
    PidSettings settings;settings.kp=.5;settings.ki=1;settings.kd=.001;
    settings.output_limit=7;settings.integral_limit=2;settings.dead_zone=.5;
    PidController pid(settings);
    for(int i=0;i<1000;++i){
        auto m=pid.step(target(1000,-1000),.008);
        CHECK(std::abs(m.x)<=7 && std::abs(m.y)<=7 && std::abs(pid.integral_x())<=2);
    }
    CHECK(pid.integral_x()==0);
    auto m=pid.step(target(.1,-.1),.008);CHECK(m.x==0 && m.y==0 && pid.integral_x()==0);
    m=pid.step(target(10,10),0);CHECK(m.x==0 && m.y==0);
    m=pid.step(target(std::numeric_limits<double>::quiet_NaN(),0),.01);CHECK(m.x==0 && m.y==0);
    settings.kp=0;settings.kd=0;settings.ki=1;settings.output_limit=100;settings.dead_zone=0;
    PidController p1(settings),p2(settings);
    for(int i=0;i<100;++i)p1.step(target(1,0),.01);
    for(int i=0;i<50;++i)p2.step(target(1,0),.02);
    CHECK(std::abs(p1.integral_x()-p2.integral_x())<1e-10);
    settings.kp=.1;settings.ki=0;PidController fractional(settings);int sum=0;
    for(int i=0;i<100;++i)sum+=fractional.step(target(1,0),.01).x;
    CHECK(sum==10);
    bool rejected=false;try{settings.period_ms=0;PidController invalid(settings);}catch(const std::invalid_argument&){rejected=true;}CHECK(rejected);
}
void worker(){
    PidSettings s;s.kp=1;s.ki=0;s.kd=0;s.period_ms=2;s.max_age_ms=50;
    std::atomic<int> count{0},bad{0};auto caller=std::this_thread::get_id();
    ControlWorker worker(s,[&](const Movement& m){if(m.x!=-m.y || std::this_thread::get_id()==caller)++bad;++count;return true;});
    worker.publish(target(4,-4));CHECK(wait_for([&]{return count==1;}));
    std::this_thread::sleep_for(20ms);CHECK(count==1);
    worker.cancel();int previous=count;std::this_thread::sleep_for(10ms);CHECK(count==previous);
    auto stale=target(4,-4);stale.captured-=1s;worker.publish(stale);
    std::this_thread::sleep_for(10ms);CHECK(count==previous);
    std::thread p1([&]{for(int i=0;i<300;++i)worker.publish(target(3,-3,i+10));});
    std::thread p2([&]{for(int i=0;i<300;++i)worker.publish(target(-7,7,i+400));});
    p1.join();p2.join();CHECK(wait_for([&]{return count>previous;}));worker.cancel();previous=count;
    std::this_thread::sleep_for(10ms);CHECK(count==previous && bad==0);worker.stop();
    worker.publish(target(4,-4));std::this_thread::sleep_for(5ms);CHECK(count==previous);
    std::promise<void> entered,release;auto released=release.get_future().share();
    ControlWorker blocking(s,[&](const Movement&){entered.set_value();released.wait();return true;});
    blocking.publish(target(2,0));CHECK(entered.get_future().wait_for(500ms)==std::future_status::ready);
    auto canceled=std::async(std::launch::async,[&]{blocking.cancel();});
    CHECK(canceled.wait_for(10ms)==std::future_status::timeout);
    release.set_value();CHECK(canceled.wait_for(500ms)==std::future_status::ready);blocking.stop();
    ControlWorker failing(s,[](const Movement&){return false;});
    failing.publish(target(2,0));CHECK(wait_for([&]{return failing.output_failed();}));failing.stop();
    auto began=SteadyClock::now();{ControlWorker idle(s,[](const Movement&){return true;});}
    CHECK(SteadyClock::now()-began<500ms);
}
void vision(){
    cv::Mat full(43,71,CV_8UC3);cv::randu(full,0,255);
    auto roi=full(cv::Rect(3,5,29,31));CHECK(!roi.isContinuous());
    Runtime::Letterbox a,b;
    auto x=Runtime::preprocess(roi,64,a),y=Runtime::preprocess(roi.clone(),64,b);
    CHECK(x==y && x.size()==64*64*3);
    CHECK(*std::min_element(x.begin(),x.end())>=0 && *std::max_element(x.begin(),x.end())<=1);
    bool rejected=false;try{Runtime::preprocess(cv::Mat(),64,a);}catch(const std::invalid_argument&){rejected=true;}CHECK(rejected);
    Runtime::Letterbox identity{1,0,0};
    std::vector<float> data{10,10,10,10,1,.9f,14,10,10,10,1,.8f,18,10,10,10,1,.7f};
    auto boxes=Runtime::decode(data,3,6,identity,100,100,.5f,.3f);
    CHECK(boxes.size()==2 && boxes[0].confidence==.9f && boxes[1].confidence==.7f);
    data[0]=std::numeric_limits<float>::quiet_NaN();
    boxes=Runtime::decode(data,3,6,identity,100,100,.5f,.3f);CHECK(boxes.size()==1);
    Runtime::Letterbox t;Runtime::preprocess(roi,640,t);
    float cx=10*t.scale+t.tx,cy=12*t.scale+t.ty;
    boxes=Runtime::decode({cx,cy,8*t.scale,10*t.scale,1,.9f},1,6,t,29,31,.5f,.3f);
    CHECK(boxes.size()==1 && std::abs(boxes[0].left-6)<1e-4 && std::abs(boxes[0].top-7)<1e-4);
}
void preview_status(){
    auto now=SteadyClock::now();
    TargetTracker tracker;ObjectDetector::BoxArray heads{{100,100,150,150,.78f,1}};
    PreviewInfo info;info.frame_state=FrameState::Ready;info.label=0;
    auto selected=tracker.update_target(heads,416,416,0,1,now);info.candidates=tracker.aims_last_num();
    CHECK(!selected.valid && preview_target_state(info,heads,selected)==TargetState::NoSelectedClass);
    CHECK(target_status_text(info,heads,selected).find("BODY C0")!=std::string::npos);
    info.label=1;selected=tracker.update_target(heads,416,416,1,2,now);info.candidates=tracker.aims_last_num();
    info.selected_box=tracker.selected_box();
    CHECK(selected.valid && preview_target_state(info,heads,selected)==TargetState::Selected);
    CHECK(output_status_text(info,selected).find("DRY RUN")!=std::string::npos);
    info.input_enabled=true;info.sample_fresh=true;
    CHECK(output_status_text(info,selected).find("hold")!=std::string::npos);
    info.button_held=true;info.sample_fresh=false;
    CHECK(output_status_text(info,selected).find("older")!=std::string::npos);
    auto red_pixels=[](const cv::Mat& frame){
        int count=0;for(int y=0;y<frame.rows;++y)for(int x=0;x<frame.cols;++x){
            auto p=frame.at<cv::Vec3b>(y,x);if(p[2]>220 && p[0]<40 && p[1]<40)++count;
        }return count;
    };
    cv::Mat black(416,416,CV_8UC3,cv::Scalar(0,0,0));
    CHECK(red_pixels(render_preview(black,heads,selected,info))>0);
    info.frame_state=FrameState::Paused;
    CHECK(preview_target_state(info,heads,selected)==TargetState::Inactive);
    CHECK(red_pixels(render_preview(black,heads,selected,info))==0);
    info.frame_state=FrameState::Waiting;
    CHECK(red_pixels(render_preview(black,heads,selected,info))==0);
    info.frame_state=FrameState::Unavailable;
    CHECK(red_pixels(render_preview(black,heads,selected,info))==0);
    info.frame_state=FrameState::Ready;info.sample_fresh=true;info.in_dead_zone=true;
    CHECK(output_status_text(info,selected).find("dead zone")!=std::string::npos);
    selected=tracker.update_target({{250,250,300,300,.9f,1}},416,416,1,3,now);info.candidates=tracker.aims_last_num();
    CHECK(!selected.valid && preview_target_state(info,heads,selected)==TargetState::AssociationPending);

    tracker.reset();ObjectDetector::BoxArray mixed{{40,40,80,80,.79f,1},{188,188,228,228,.60f,1},{184,174,240,310,.93f,0}};
    selected=tracker.update_target(mixed,416,416,1,4,now);info.selected_box=tracker.selected_box();
    info.candidates=tracker.aims_last_num();
    const auto status=target_status_text(info,mixed,selected);
    CHECK(status.find("SELECTED HEAD C1 60%")!=std::string::npos);
    CHECK(status.find("BODY")==std::string::npos && status.find("93%")==std::string::npos && status.find("79%")==std::string::npos);
    auto annotation_pixels=[&](const cv::Mat& view,const cv::Vec3b& color){
        int count=0;auto frame=view(cv::Rect((view.cols-black.cols)/2,166,black.cols,black.rows));
        for(int y=0;y<frame.rows;++y)for(int x=0;x<frame.cols;++x)if(frame.at<cv::Vec3b>(y,x)==color)++count;
        return count;
    };
    auto rendered=render_preview(black,mixed,selected,info);
    CHECK(annotation_pixels(rendered,cv::Vec3b(0,220,255))>0);
    CHECK(annotation_pixels(rendered,cv::Vec3b(60,230,80))>0);
    CHECK(annotation_pixels(rendered,cv::Vec3b(140,140,140))>0);
    info.frame_state=FrameState::Paused;
    CHECK(target_status_text(info,mixed,selected).find("SELECTED")==std::string::npos);
    rendered=render_preview(black,mixed,selected,info);
    CHECK(annotation_pixels(rendered,cv::Vec3b(0,220,255))==0 && red_pixels(rendered)==0);
}
int main(){
    try{tracking();selection_metadata();std::cout<<"tracking / selected metadata passed\n";pid();std::cout<<"PID passed\n";
        worker();std::cout<<"worker lifecycle / concurrency passed\n";vision();std::cout<<"vision / NMS passed\n";
        preview_status();std::cout<<"preview state / stale-marker clearing passed\n";return 0;}
    catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}
}
