#include "application/config.h"
#include "application/dxgi-cap.h"
#include "runtime/trt_detector.h"
#include "application/preview.h"
#include "application/input_diagnostics.h"
#include <Windows.h>
#include <opencv2/opencv.hpp>
#include <opencv2/highgui/highgui_c.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <stdexcept>
#include <thread>
namespace fs=std::filesystem;
namespace {
std::atomic<bool> stopping{false};
HANDLE stopped=nullptr;
BOOL WINAPI console_handler(DWORD event){
    if(event==CTRL_C_EVENT || event==CTRL_BREAK_EVENT || event==CTRL_CLOSE_EVENT ||
       event==CTRL_LOGOFF_EVENT || event==CTRL_SHUTDOWN_EVENT){
        stopping.store(true);
        if(event!=CTRL_C_EVENT && event!=CTRL_BREAK_EVENT && stopped)WaitForSingleObject(stopped,4000);
        return TRUE;
    }
    return FALSE;
}
struct ConsoleGuard {
    ConsoleGuard(){stopping=false;stopped=CreateEventW(nullptr,TRUE,FALSE,nullptr);SetConsoleCtrlHandler(console_handler,TRUE);}
    ~ConsoleGuard(){if(stopped)SetEvent(stopped);SetConsoleCtrlHandler(console_handler,FALSE);
        // Retain the event until process exit: an in-flight close callback may still be waiting.
    }
};
bool down(int key){return (GetAsyncKeyState(key)&0x8000)!=0;}
// This is called exclusively by ControlWorker output callbacks, including calibration.
bool dispatch_mouse(const Movement& movement,DWORD* error=nullptr){
    INPUT input{};input.type=INPUT_MOUSE;input.mi.dx=movement.x;input.mi.dy=movement.y;
    input.mi.dwFlags=MOUSEEVENTF_MOVE;
    SetLastError(ERROR_SUCCESS);
    auto inserted=SendInput(1,&input,sizeof(input));
    auto code=inserted==1?ERROR_SUCCESS:GetLastError();
    if(error)*error=code;
    if(inserted!=1)std::cerr<<"SendInput failed: inserted="<<inserted<<" requested=1 win32_error="<<code
        <<". A zero error does not rule out UIPI; the API cannot identify UIPI as the cause."<<std::endl;
    return inserted==1;
}
template<class T>void read_value(const cv::FileStorage& f,const char* key,T& value){if(!f[key].empty())f[key]>>value;}
void load_config(AppOptions& o){
    if(!fs::exists(o.config))throw std::runtime_error("Configuration not found: "+o.config);
    cv::FileStorage f(o.config,cv::FileStorage::READ);
    if(!f.isOpened())throw std::runtime_error("Cannot read configuration");
    read_value(f,"kp",o.pid.kp);read_value(f,"ki",o.pid.ki);read_value(f,"kd",o.pid.kd);
    read_value(f,"integral_limit",o.pid.integral_limit);read_value(f,"output_limit",o.pid.output_limit);
    read_value(f,"dead_zone",o.pid.dead_zone);read_value(f,"period_ms",o.pid.period_ms);
    read_value(f,"max_age_ms",o.pid.max_age_ms);read_value(f,"units_per_pixel_x",o.pid.units_per_pixel_x);
    read_value(f,"units_per_pixel_y",o.pid.units_per_pixel_y);
    read_value(f,"confidence",o.tracking.confidence);read_value(f,"minimum_iou",o.tracking.minimum_iou);
    read_value(f,"confidence_weight",o.tracking.confidence_weight);read_value(f,"body_y_fraction",o.tracking.body_y_fraction);
    read_value(f,"max_lost_frames",o.tracking.max_lost_frames);read_value(f,"nms",o.nms);
    read_value(f,"capture_size",o.size);read_value(f,"monitor",o.monitor);read_value(f,"label",o.label);
    read_value(f,"calibration_file",o.calibration_file);
}
void load_calibration(AppOptions& o){
    if(!fs::exists(o.calibration_file))return;
    cv::FileStorage f(o.calibration_file,cv::FileStorage::READ);
    if(!f.isOpened())throw std::runtime_error("Cannot read calibration file");
    int speed=0,mouse[3]{};SystemParametersInfoW(SPI_GETMOUSESPEED,0,&speed,0);SystemParametersInfoW(SPI_GETMOUSE,0,mouse,0);
    int expected_speed=-1,expected_accel=-1,expected_t1=-1,expected_t2=-1;
    read_value(f,"mouse_speed",expected_speed);read_value(f,"mouse_acceleration",expected_accel);
    read_value(f,"mouse_threshold1",expected_t1);read_value(f,"mouse_threshold2",expected_t2);
    if(speed!=expected_speed || mouse[2]!=expected_accel || mouse[0]!=expected_t1 || mouse[1]!=expected_t2){
        std::cerr<<"Mouse settings changed; calibration ignored. Run --calibrate again."<<std::endl;return;
    }
    double x=1,y=1;read_value(f,"units_per_pixel_x",x);read_value(f,"units_per_pixel_y",y);
    if(!std::isfinite(x)||!std::isfinite(y)||x<=0||y<=0)throw std::runtime_error("Invalid calibration");
    o.pid.units_per_pixel_x*=x;o.pid.units_per_pixel_y*=y;o.calibrated=true;
}
int calibrate(const AppOptions& o){
    auto canceled=[&]{return stopping.load() || down(VK_ESCAPE) || (!o.stop_file.empty() && fs::exists(o.stop_file));};
    std::cout<<"Calibration starts in 2 seconds; leave the pointer still. ESC cancels."<<std::endl;
    for(int i=0;i<100;++i){
        if(canceled())throw std::runtime_error("Calibration canceled");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    POINT origin{};if(!GetCursorPos(&origin))throw std::runtime_error("GetCursorPos failed");
    MONITORINFO info{};info.cbSize=sizeof(info);
    if(!GetMonitorInfoW(MonitorFromPoint(origin,MONITOR_DEFAULTTONEAREST),&info))throw std::runtime_error("GetMonitorInfo failed");
    const auto r=info.rcMonitor;
    if(origin.x-r.left<128 || r.right-origin.x<128 || origin.y-r.top<128 || r.bottom-origin.y<128)
        throw std::runtime_error("Calibration requires the pointer at least 128 physical pixels from monitor edges");
    PidSettings settings;settings.kp=1;settings.ki=0;settings.kd=0;settings.dead_zone=0;
    settings.output_limit=32;settings.units_per_pixel_x=settings.units_per_pixel_y=1;settings.max_age_ms=1000;
    std::atomic<int> completed{0};
    ControlWorker worker(settings,[&](const Movement& m){
        if(canceled())return false;
        bool ok=dispatch_mouse(m);++completed;return ok;
    });
    int sequence=0;
    auto move=[&](int x,int y){
        if(canceled())throw std::runtime_error("Calibration canceled");
        int expected=completed.load()+1;
        worker.publish({true,true,double(x),double(y),std::uint64_t(++sequence),SteadyClock::now()});
        auto deadline=SteadyClock::now()+std::chrono::seconds(1);
        while(completed.load()<expected && !canceled() && SteadyClock::now()<deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        if(completed.load()<expected || worker.output_failed())throw std::runtime_error("Calibration input failed");
    };
    std::cout<<"Measuring desktop pointer response with bounded +/-16 input units..."<<std::endl;
    POINT xpoint{},ypoint{},returned{};
    move(16,0);if(!GetCursorPos(&xpoint))throw std::runtime_error("GetCursorPos failed");
    move(-16,0);
    GetCursorPos(&returned);
    std::cout<<"X calibration: origin=("<<origin.x<<","<<origin.y<<"), forward=("<<xpoint.x<<","<<xpoint.y
        <<"), return=("<<returned.x<<","<<returned.y<<")"<<std::endl;
    if(!GetCursorPos(&returned) || std::abs(returned.x-origin.x)>2 || std::abs(returned.y-origin.y)>2)
        throw std::runtime_error("Non-repeatable pointer response (manual motion or acceleration); calibration discarded");
    move(0,16);if(!GetCursorPos(&ypoint))throw std::runtime_error("GetCursorPos failed");
    move(0,-16);worker.stop();
    if(!GetCursorPos(&returned) || std::abs(returned.x-origin.x)>2 || std::abs(returned.y-origin.y)>2 ||
       xpoint.x<=origin.x || ypoint.y<=origin.y || std::abs(xpoint.y-origin.y)>2 || std::abs(ypoint.x-origin.x)>2)
        throw std::runtime_error("Calibration was interrupted or not reversible; measurement discarded");
    int speed=0,mouse[3]{};
    if(!SystemParametersInfoW(SPI_GETMOUSESPEED,0,&speed,0) || !SystemParametersInfoW(SPI_GETMOUSE,0,mouse,0))
        throw std::runtime_error("Cannot read Windows mouse settings");
    auto parent=fs::path(o.calibration_file).parent_path();if(!parent.empty())fs::create_directories(parent);
    cv::FileStorage f(o.calibration_file,cv::FileStorage::WRITE);
    if(!f.isOpened())throw std::runtime_error("Cannot write calibration");
    double sx=16.0/(xpoint.x-origin.x),sy=16.0/(ypoint.y-origin.y);
    f<<"units_per_pixel_x"<<sx<<"units_per_pixel_y"<<sy<<"mouse_speed"<<speed
        <<"mouse_acceleration"<<mouse[2]<<"mouse_threshold1"<<mouse[0]<<"mouse_threshold2"<<mouse[1];
    f.release();
    std::cout<<"Calibration saved: "<<o.calibration_file<<"; units/pixel=("<<sx<<","<<sy<<"), pointer restored."<<std::endl;
    if(mouse[2])std::cout<<"Windows acceleration is enabled; this is a local estimate at 16 units, not a global linear mapping."<<std::endl;
    std::cout<<"Raw-input applications require their own sensitivity/FOV measurement; override config scales accordingly."<<std::endl;
    return 0;
}

}
void print_help(){
    std::cout <<
      "TestStudyYolo --model workspace/cf.onnx [options]\n"
      "  --capture dxgi|gdi --monitor N --size 416 --list-monitors\n"
      "  --label 0|1                  Select body (0) or head (1) candidates\n"
      "  --image FILE | --video FILE   Offline detection; input disabled\n"
      "  --headless --frames N --seconds N --output DIRECTORY\n"
      "  --enable-input               Allow relative mouse output while LMB/RMB held\n"
      "  --calibrate                  Measure and save desktop mouse scaling\n"
      "  --benchmark --frames 300     GPU-only warmed-up engine benchmark\n"
      "  --config FILE --calibration FILE --cache DIRECTORY --rebuild --fp32\n"
      "  --dump-tensors DIRECTORY     Export float tensors for offline validation\n"
      "  --stop-file FILE             Exit when this file exists (scripted shutdown)\n"
      "  ESC/Ctrl+C: exit; Left: pause; Right: resume; Up: class 1; Down: class 0.\n"
      "  Default: detection and movement logs only. Model classes must be verified.\n";
}
AppOptions parse_options(int argc,char** argv){
    AppOptions o;
    for(int i=1;i<argc;++i)if(std::string(argv[i])=="--config"){
        if(i+1>=argc)throw std::invalid_argument("Missing --config value");o.config=argv[++i];
    }
    for(int i=1;i<argc;++i)if(std::string(argv[i])=="--help" || std::string(argv[i])=="-h"){o.help=true;return o;}
    load_config(o);
    for(int i=1;i<argc;++i){
        std::string arg=argv[i];
        auto value=[&](){if(i+1>=argc)throw std::invalid_argument("Missing value for "+arg);return std::string(argv[++i]);};
        if(arg=="--config")o.config=value();
        else if(arg=="--model")o.model=value();else if(arg=="--cache")o.cache=value();
        else if(arg=="--capture")o.capture=value();else if(arg=="--monitor")o.monitor=std::stoi(value());
        else if(arg=="--size")o.size=std::stoi(value());else if(arg=="--label")o.label=std::stoi(value());
        else if(arg=="--frames")o.frames=std::stoi(value());else if(arg=="--seconds")o.seconds=std::stod(value());
        else if(arg=="--image")o.image=value();else if(arg=="--video")o.video=value();
        else if(arg=="--output")o.output=value();else if(arg=="--calibration")o.calibration_file=value();
        else if(arg=="--dump-tensors")o.tensor_dump=value();else if(arg=="--stop-file")o.stop_file=value();
        else if(arg=="--headless")o.headless=true;else if(arg=="--enable-input")o.enable_input=true;
        else if(arg=="--rebuild")o.rebuild=true;else if(arg=="--fp32")o.fp32=true;
        else if(arg=="--list-monitors")o.list_monitors=true;else if(arg=="--calibrate")o.calibrate=true;
        else if(arg=="--benchmark")o.benchmark=true;
        else throw std::invalid_argument("Unknown option: "+arg);
    }
    if(o.capture!="dxgi" && o.capture!="gdi")throw std::invalid_argument("Capture must be dxgi or gdi");
    if(!o.image.empty() && !o.video.empty())throw std::invalid_argument("Choose image or video");
    if(o.enable_input && (!o.image.empty() || !o.video.empty()))throw std::invalid_argument("Offline input injection is disabled");
    if(o.benchmark && (o.enable_input || o.calibrate || !o.image.empty() || !o.video.empty()))
        throw std::invalid_argument("GPU benchmark cannot be combined with input control, calibration or media");
    if(!o.tensor_dump.empty() && o.image.empty())throw std::invalid_argument("Tensor dumps require --image");
    if(o.monitor<0 || o.frames<0 || !std::isfinite(o.seconds) || o.seconds<0 || o.label<0 || o.size<32 || o.size>4096 ||
       !std::isfinite(o.nms) || o.nms<=0 || o.nms>1)throw std::invalid_argument("Invalid numeric options");
    load_calibration(o);
    PidController validate_pid(o.pid);TargetTracker validate_tracking(o.tracking);
    return o;
}
int run_application(const AppOptions& o){
    ConsoleGuard console;
    if(o.list_monitors){
        for(auto& m:ScreenCapture::monitors())std::cout<<m.index<<": "<<m.name<<" "<<m.width<<"x"<<m.height
            <<" origin=("<<m.left<<","<<m.top<<") physical pixels"<<std::endl;return 0;
    }
    if(o.calibrate)return calibrate(o);
    auto should_stop=[&]{return stopping.load() || down(VK_ESCAPE) || (!o.stop_file.empty() && fs::exists(o.stop_file));};
    Runtime::Detector detector(o.model,o.cache,o.rebuild,o.fp32,should_stop);
    std::cout<<"GPU: "<<detector.device_name()<<"; TensorRT "<<(o.fp32?"FP32":"FP16")
        <<"; image preprocessing / decode: CPU"<<std::endl;
    fs::create_directories(o.output);
    if(o.benchmark){
        auto result=detector.benchmark(30,o.frames?o.frames:300);
        std::ofstream summary(fs::path(o.output)/"summary.json");
        summary<<"{\"mode\":\"gpu_benchmark\",\"device\":"<<std::quoted(result.device)
            <<",\"warmup\":"<<result.warmup<<",\"iterations\":"<<result.iterations
            <<",\"gpu_mean_ms\":"<<result.gpu_mean_ms<<",\"gpu_fps\":"<<result.gpu_fps
            <<",\"input_enabled\":false}\n";
        if(!summary)throw std::runtime_error("Cannot save benchmark");
        std::cout<<"GPU-only benchmark: "<<result.gpu_mean_ms<<" ms/inference, "<<result.gpu_fps
            <<" FPS; "<<result.warmup<<" warmup + "<<result.iterations<<" measured runs."<<std::endl;
        std::cout<<"Excludes capture, CPU preprocessing/decode, frame transfers and display."<<std::endl;
        return 0;
    }
    if(o.label>=detector.classes())throw std::runtime_error("Selected label exceeds model class count");
    bool offline=!o.image.empty() || !o.video.empty();
    cv::Mat still;cv::VideoCapture video;
    std::unique_ptr<ScreenCapture> capture;
    if(!o.image.empty()){
        still=cv::imread(o.image);if(still.empty())throw std::runtime_error("Cannot read image: "+o.image);
    }else if(!o.video.empty()){
        if(!video.open(o.video))throw std::runtime_error("Cannot open video: "+o.video);
    }else capture=std::make_unique<ScreenCapture>(o.monitor,o.size,o.capture=="gdi");
    std::ofstream frame_log(fs::path(o.output)/"frames.csv"),movement_log(fs::path(o.output)/"movement.csv");
    std::ofstream context_log(fs::path(o.output)/"input-context.csv");
    if(!frame_log || !movement_log || !context_log)throw std::runtime_error("Cannot create run logs");
    frame_log<<"frame,total_ms,boxes,target_valid,dx,dy,lost_frames,candidate_count,capture_ms,preprocess_ms,"
        <<"upload_ms,gpu_inference_ms,download_ms,postprocess_ms,inference_total_ms,render_ms,gui_wait_ms,label,"
        <<"button_held,key_guard,sample_fresh,sample_age_ms\n";
    movement_log<<"frame,dx,dy,injected,attempted,win32_error,foreground_pid,elapsed_ms\n";
    context_log<<"elapsed_seconds,foreground_pid,foreground_exe,foreground_integrity,foreground_elevated,"
        <<"foreground_query_error,sender_pid,sender_integrity,sender_elevated,sender_query_error,"
        <<"foreground_is_preview,cursor_known,cursor_x,cursor_y,clip_known,clip_left,clip_top,clip_right,clip_bottom\n";
    auto sender=InputDiagnostics::process_info(GetCurrentProcessId());
    std::cout<<"Input sender: pid="<<sender.pid<<" integrity="<<InputDiagnostics::integrity_name(sender.integrity_rid)
        <<" elevated="<<(sender.elevation_known?(sender.elevated?"yes":"no"):"unknown")
        <<"; successful SendInput means Windows accepted the event, not target application response."<<std::endl;
    TargetTracker tracker(o.tracking);
    std::atomic<std::uint64_t> sent_count{0},attempt_count{0},send_failures{0};
    const auto diagnostics_started=SteadyClock::now();
    auto last_movement_flush=diagnostics_started;
    ControlWorker worker(o.pid,[&](const Movement& m){
        bool inject=o.enable_input && !should_stop() && !down(VK_LEFT) &&
            !down(VK_UP) && !down(VK_DOWN) && (down(VK_LBUTTON)||down(VK_RBUTTON));
        auto foreground=InputDiagnostics::foreground();
        DWORD input_error=ERROR_SUCCESS;
        if(inject)++attempt_count;
        bool ok=!inject || dispatch_mouse(m,&input_error);
        if(inject && ok)++sent_count;
        if(inject && !ok)++send_failures;
        auto recorded=SteadyClock::now();
        movement_log<<m.frame<<","<<m.x<<","<<m.y<<","<<(inject&&ok?1:0)<<","<<(inject?1:0)
            <<","<<input_error<<","<<foreground.pid<<","<<std::chrono::duration<double,std::milli>(recorded-diagnostics_started).count()<<"\n";
        if(!ok || recorded-last_movement_flush>=std::chrono::seconds(1)){
            movement_log.flush();last_movement_flush=recorded;
        }
        return ok && bool(movement_log);
    });
    std::cout<<"Started: "<<(o.enable_input?"INPUT ENABLED, hold LMB/RMB":"dry run (no mouse injection)")
        <<"; selected "<<class_name(o.label)<<"; ESC/Ctrl+C to stop."<<std::endl;
    if(o.enable_input && !o.calibrated)std::cout<<"Using configured sensitivity scales without a desktop calibration file."<<std::endl;
    bool active=true,had_input_gate=false;
    int label=o.label,empty_frames=0,errors=0;
    std::uint64_t frame=0,reported_frames=0;
    MonitorGeometry previous_geometry=capture?capture->geometry():MonitorGeometry{};
    auto started=SteadyClock::now(),last_report=started,last_idle_render=SteadyClock::time_point{};
    auto last_context_query=SteadyClock::time_point{};
    InputDiagnostics::ProcessInfo foreground_process;
    auto log_input_context=[&]{
        auto now=SteadyClock::now();
        auto foreground=InputDiagnostics::foreground();
        if(last_context_query==SteadyClock::time_point{} || foreground.pid!=foreground_process.pid ||
           now-last_context_query>=std::chrono::seconds(10)){
            foreground_process=InputDiagnostics::process_info(foreground.pid);last_context_query=now;
        }
        POINT cursor{};RECT clip{};
        bool cursor_known=GetCursorPos(&cursor)!=FALSE,clip_known=GetClipCursor(&clip)!=FALSE;
        context_log<<std::chrono::duration<double>(now-diagnostics_started).count()<<","<<foreground.pid
            <<","<<std::quoted(foreground_process.executable)<<","<<InputDiagnostics::integrity_name(foreground_process.integrity_rid)
            <<","<<(foreground_process.elevation_known?(foreground_process.elevated?1:0):-1)<<","<<foreground_process.query_error
            <<","<<sender.pid<<","<<InputDiagnostics::integrity_name(sender.integrity_rid)
            <<","<<(sender.elevation_known?(sender.elevated?1:0):-1)<<","<<sender.query_error
            <<","<<(foreground.pid==sender.pid?1:0)<<","<<cursor_known<<","<<cursor.x<<","<<cursor.y
            <<","<<clip_known<<","<<clip.left<<","<<clip.top<<","<<clip.right<<","<<clip.bottom<<"\n";
        context_log.flush();
        if(!context_log)throw std::runtime_error("Input context logging failed");
    };
    log_input_context();
    auto last_capture_error=SteadyClock::time_point{};
    Runtime::InferenceTimings sums;
    double capture_sum=0,render_sum=0,gui_sum=0,loop_sum=0;
    cv::Mat last_raw,last_rendered;
    ObjectDetector::BoxArray last_boxes;
    TargetUpdate last_target;
    PreviewInfo info;info.label=label;info.input_enabled=o.enable_input;info.device=detector.device_name();
    info.max_lost=o.tracking.max_lost_frames;info.capture=offline?(still.empty()?"video":"image"):o.capture;
    auto milliseconds=[](SteadyClock::time_point from,SteadyClock::time_point to){
        return std::chrono::duration<double,std::milli>(to-from).count();
    };
    auto update_info=[&]{
        info.label=label;info.frame=frame;info.candidates=tracker.aims_last_num();info.lost=tracker.lost_frames();
        info.selected_box=last_target.valid && info.frame_state==FrameState::Ready?tracker.selected_box():std::nullopt;
        info.button_held=down(VK_LBUTTON)||down(VK_RBUTTON);
        info.key_guard=down(VK_LEFT)||down(VK_UP)||down(VK_DOWN);
        info.sent=sent_count.load();
        auto now=SteadyClock::now();
        info.sample_fresh=last_target.valid && now>=last_target.captured &&
            now-last_target.captured<=std::chrono::milliseconds(o.pid.max_age_ms);
        if(info.frame_state!=FrameState::Ready){info.candidates=0;info.sample_fresh=false;info.timings={};info.capture_ms=0;}
        info.in_dead_zone=std::abs(last_target.dx)<=o.pid.dead_zone && std::abs(last_target.dy)<=o.pid.dead_zone;
        double elapsed=std::chrono::duration<double>(now-started).count();
        info.fps_average=elapsed>0?frame/elapsed:0;
    };
    auto report=[&]{
        auto now=SteadyClock::now();
        double interval=std::chrono::duration<double>(now-last_report).count();
        if(interval<1)return;
        update_info();
        info.fps_recent=(frame-reported_frames)/interval;reported_frames=frame;last_report=now;
        log_input_context();
        std::cout<<"frames="<<frame<<" fps="<<info.fps_average<<" fps_recent="<<info.fps_recent
            <<" boxes="<<last_boxes.size()<<" class="<<label<<" candidates="<<info.candidates
            <<" lost="<<info.lost<<" sent="<<info.sent<<" attempted="<<attempt_count<<" send_failures="<<send_failures
            <<" button_held="<<info.button_held<<" sample_fresh="<<info.sample_fresh
            <<" foreground_pid="<<foreground_process.pid<<" foreground_exe="<<std::quoted(foreground_process.executable)
            <<" foreground_integrity="<<InputDiagnostics::integrity_name(foreground_process.integrity_rid)
            <<" gpu_ms="<<info.timings.gpu_inference_ms<<" capture_ms="<<info.capture_ms
            <<" target="<<std::quoted(target_status_text(info,last_boxes,last_target))
            <<" output="<<std::quoted(output_status_text(info,last_target))<<std::endl;
    };
    auto cancel=[&]{worker.cancel();tracker.reset();had_input_gate=false;last_target={};};
    auto show_idle=[&](FrameState state){
        bool changed=info.frame_state!=state;
        info.frame_state=state;last_target={};last_boxes.clear();update_info();report();
        if(!o.headless && (changed || SteadyClock::now()-last_idle_render>std::chrono::milliseconds(100))){
            last_rendered=render_preview(last_raw,{},last_target,info);cv::imshow("TestStudyYolo",last_rendered);
            last_idle_render=SteadyClock::now();
        }
    };
    if(!o.headless){
        cv::namedWindow("TestStudyYolo",cv::WINDOW_AUTOSIZE);
        last_rendered=render_preview({}, {}, {},info);
        cv::imshow("TestStudyYolo",last_rendered);
        auto handle=static_cast<HWND>(cvGetWindowHandle("TestStudyYolo"));
        auto window=handle?GetAncestor(handle,GA_ROOT):nullptr;
        if(!window || !SetWindowPos(window,HWND_TOPMOST,0,0,0,0,
            SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW))
            throw std::runtime_error("Cannot keep the preview window on top; Windows error "+std::to_string(GetLastError()));
    }
    while(!should_stop()){
        auto began=SteadyClock::now();
        if((o.frames && frame>=std::uint64_t(o.frames)) ||
           (o.seconds>0 && std::chrono::duration<double>(began-started).count()>=o.seconds))break;
        double gui_ms=0;
        if(!o.headless){
            try{if(cv::waitKey(1)==27 || cv::getWindowProperty("TestStudyYolo",cv::WND_PROP_VISIBLE)<1){stopping=true;break;}}
            catch(const cv::Exception&){stopping=true;break;}
            gui_ms=milliseconds(began,SteadyClock::now());
        }
        if(worker.output_failed())throw std::runtime_error("Mouse output or movement logging failed");
        bool next_active=active;int next_label=label;
        if(down(VK_LEFT))next_active=false;if(down(VK_RIGHT))next_active=true;
        if(down(VK_UP) && detector.classes()>1)next_label=1;if(down(VK_DOWN))next_label=0;
        if(next_active!=active || next_label!=label){cancel();active=next_active;label=next_label;}
        bool input_gate=!o.enable_input || (down(VK_LBUTTON)||down(VK_RBUTTON));
        if(!input_gate && had_input_gate)cancel();
        had_input_gate=input_gate;
        if(!active){worker.cancel();show_idle(FrameState::Paused);std::this_thread::sleep_for(std::chrono::milliseconds(10));continue;}
        cv::Mat image;auto capture_begin=SteadyClock::now(),captured=capture_begin;
        if(!still.empty())image=still;
        else if(video.isOpened()){if(!video.read(image))break;}
        else{
            auto result=capture->grab();
            auto geometry=capture->geometry();
            if(geometry.left!=previous_geometry.left || geometry.top!=previous_geometry.top ||
               geometry.width!=previous_geometry.width || geometry.height!=previous_geometry.height){
                cancel();previous_geometry=geometry;
            }
            image=result.image;captured=result.captured;
            if(image.empty()){
                worker.cancel();++empty_frames;
                if(result.status==CaptureStatus::Unavailable){
                    tracker.reset();++errors;
                    auto now=SteadyClock::now();
                    if(last_capture_error==SteadyClock::time_point{} || now-last_capture_error>=std::chrono::seconds(1)){
                        std::cerr<<result.error<<std::endl;last_capture_error=now;
                    }
                }
                show_idle(result.status==CaptureStatus::Unavailable?FrameState::Unavailable:FrameState::Waiting);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));continue;
            }
        }
        double capture_ms=milliseconds(capture_begin,SteadyClock::now());
        ++frame;
        auto boxes=detector.infer(image,o.tracking.confidence,o.nms);
        auto timings=detector.last_timings();
        if(!o.tensor_dump.empty())detector.dump_tensors(o.tensor_dump);
        auto target=tracker.update_target(boxes,image.cols,image.rows,label,frame,captured);
        if(target.valid && input_gate && !should_stop())worker.publish(target);else worker.cancel();
        last_raw=image;last_boxes=boxes;last_target=target;
        info.frame_state=FrameState::Ready;info.timings=timings;info.capture_ms=capture_ms;update_info();
        auto render_begin=SteadyClock::now();
        if(!o.headless){last_rendered=render_preview(image,boxes,target,info);cv::imshow("TestStudyYolo",last_rendered);}
        double render_ms=milliseconds(render_begin,SteadyClock::now());
        if(offline && !o.headless)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        double total_ms=milliseconds(began,SteadyClock::now());
        capture_sum+=capture_ms;render_sum+=render_ms;gui_sum+=gui_ms;loop_sum+=total_ms;
        sums.preprocess_ms+=timings.preprocess_ms;sums.upload_ms+=timings.upload_ms;
        sums.gpu_inference_ms+=timings.gpu_inference_ms;sums.download_ms+=timings.download_ms;
        sums.postprocess_ms+=timings.postprocess_ms;sums.total_ms+=timings.total_ms;
        frame_log<<frame<<","<<total_ms<<","<<boxes.size()<<","<<target.valid<<","<<target.dx<<","<<target.dy
            <<","<<tracker.lost_frames()<<","<<tracker.aims_last_num()<<","<<capture_ms<<","<<timings.preprocess_ms
            <<","<<timings.upload_ms<<","<<timings.gpu_inference_ms<<","<<timings.download_ms<<","<<timings.postprocess_ms
            <<","<<timings.total_ms<<","<<render_ms<<","<<gui_ms<<","<<label
            <<","<<info.button_held<<","<<info.key_guard<<","<<info.sample_fresh
            <<","<<(target.valid?milliseconds(target.captured,SteadyClock::now()):-1)<<"\n";
        if(!frame_log)throw std::runtime_error("Frame logging failed");
        report();
        if(!still.empty() && o.frames==0 && o.seconds==0)break;
    }
    worker.cancel();worker.stop();frame_log.flush();movement_log.flush();context_log.flush();
    double elapsed=std::chrono::duration<double>(SteadyClock::now()-started).count();
    update_info();
    if(o.headless && !last_raw.empty())last_rendered=render_preview(last_raw,last_boxes,last_target,info);
    if(!last_rendered.empty() && !cv::imwrite((fs::path(o.output)/"last-frame.jpg").string(),last_rendered))
        throw std::runtime_error("Cannot save preview");
    if(!o.headless)cv::destroyAllWindows();
    auto avg=[&](double sum){return frame?sum/frame:0;};
    std::ofstream summary(fs::path(o.output)/"summary.json");
    summary<<"{\"mode\":"<<std::quoted(offline?"offline":"live")<<",\"capture\":"<<std::quoted(info.capture)
        <<",\"headless\":"<<(o.headless?"true":"false")<<",\"device\":"<<std::quoted(detector.device_name())
        <<",\"label\":"<<label<<",\"frames\":"<<frame<<",\"elapsed_seconds\":"<<elapsed<<",\"fps\":"<<(elapsed>0?frame/elapsed:0)
        <<",\"empty_captures\":"<<empty_frames<<",\"capture_errors\":"<<errors<<",\"sent_count\":"<<sent_count
        <<",\"send_attempts\":"<<attempt_count<<",\"send_failures\":"<<send_failures
        <<",\"input_enabled\":"<<(o.enable_input?"true":"false")
        <<",\"average_ms\":{\"capture\":"<<avg(capture_sum)<<",\"preprocess\":"<<avg(sums.preprocess_ms)
        <<",\"upload\":"<<avg(sums.upload_ms)<<",\"gpu_inference\":"<<avg(sums.gpu_inference_ms)
        <<",\"download\":"<<avg(sums.download_ms)<<",\"postprocess\":"<<avg(sums.postprocess_ms)
        <<",\"inference_total\":"<<avg(sums.total_ms)<<",\"render\":"<<avg(render_sum)<<",\"gui_wait\":"<<avg(gui_sum)
        <<",\"processed_loop\":"<<avg(loop_sum)<<"}}\n";
    std::cout<<"Stopped cleanly: "<<frame<<" frames, "<<elapsed<<" s; logs: "<<o.output<<std::endl;
    if(!summary || !frame_log || !movement_log || !context_log)throw std::runtime_error("Log finalization failed");
    return frame>0 || should_stop()?0:2;
}
