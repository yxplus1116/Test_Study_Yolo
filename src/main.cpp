#include "application/config.h"
#include "runtime/trt_detector.h"
#include <Windows.h>
#include <iostream>
int main(int argc,char** argv){
    SetConsoleOutputCP(CP_UTF8);
    // Capture geometry and cursor positions share physical desktop pixels.
    if(!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) && GetLastError()!=ERROR_ACCESS_DENIED){
        std::cerr<<"Cannot enable per-monitor DPI awareness"<<std::endl;return 1;
    }
    if(GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext())!=DPI_AWARENESS_PER_MONITOR_AWARE){
        std::cerr<<"The process must use per-monitor DPI awareness"<<std::endl;return 1;
    }
    try{
        auto options=parse_options(argc,argv);
        if(options.help){print_help();return 0;}
        return run_application(options);
    }catch(const Runtime::Canceled& e){std::cout<<e.what()<<std::endl;return 0;}
    catch(const std::exception& e){std::cerr<<"Fatal: "<<e.what()<<std::endl;return 1;}
}
