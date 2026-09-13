#pragma once
#include "mouse.h"
#include <string>
struct AppOptions {
    PidSettings pid;
    TrackingSettings tracking;
    std::string config="config/default.yml",model="workspace/cf.onnx",cache=".local/engines";
    std::string capture="dxgi",image,video,output=".local/runlogs",calibration_file="config/calibration.yml";
    std::string mouse_backend="windows",makcu_port="COM3",makcu_protocol="ascii";
    // MAKCU terminates replies with a prompt and documents a 500 ms read
    // window. 30 ms is too short while the board is booting or re-enumerating.
    int makcu_baud=115200,makcu_ack_timeout_ms=500;
    std::string tensor_dump,stop_file;
    int monitor=0,size=416,label=0,frames=0;
    double seconds=0;
    float nms=0.45f;
    bool headless=false,enable_input=false,rebuild=false,fp32=false,list_monitors=false;
    bool calibrate=false,help=false,calibrated=false,benchmark=false,makcu_test=false;
};
AppOptions parse_options(int argc,char** argv);
int run_application(const AppOptions& options);
void print_help();
