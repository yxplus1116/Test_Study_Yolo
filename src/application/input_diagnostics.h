#pragma once

#include <cstdint>
#include <string>

namespace InputDiagnostics {

struct ProcessInfo {
    unsigned long pid=0;
    std::string executable="unknown";
    int integrity_rid=-1;
    bool elevated=false;
    bool elevation_known=false;
    // First Windows query error, or zero when all requested information is known.
    unsigned long query_error=0;
};

ProcessInfo process_info(unsigned long pid);

struct ForegroundInfo {
    unsigned long pid=0;
    uintptr_t window=0;
};

ForegroundInfo foreground();
std::string integrity_name(int rid);

}
