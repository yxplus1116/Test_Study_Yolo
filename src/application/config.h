#pragma once
#include "mouse.h"
#include <string>
struct AppOptions {
    PidSettings pid;
    TrackingSettings tracking;
    std::string config="config/default.yml",model="workspace/cf.onnx",cache=".local/engines";
    std::string capture="dxgi",image,video,output=".local/runlogs",calibration_file="config/calibration.yml";
    std::string tensor_dump,stop_file;
    int monitor=0,size=416,label=0,frames=0;
    double seconds=0;
    float nms=0.45f;
    bool headless=false,enable_input=false,rebuild=false,fp32=false,list_monitors=false;
    bool calibrate=false,help=false,calibrated=false,benchmark=false;
};
AppOptions parse_options(int argc,char** argv);
int run_application(const AppOptions& options);
void print_help();
