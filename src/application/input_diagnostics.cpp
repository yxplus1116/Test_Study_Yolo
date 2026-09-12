#include "input_diagnostics.h"

#include <Windows.h>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace InputDiagnostics {
namespace {

struct Handle {
    HANDLE value=nullptr;
    explicit Handle(HANDLE handle=nullptr):value(handle){}
    ~Handle(){if(value && value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
};

void record_error(ProcessInfo& info,DWORD error) {
    if(!info.query_error)info.query_error=error ? error : ERROR_GEN_FAILURE;
}

void query_executable(HANDLE process,ProcessInfo& info) {
    std::vector<wchar_t> path(32768);
    DWORD length=static_cast<DWORD>(path.size());
    if(!QueryFullProcessImageNameW(process,0,path.data(),&length)){
        record_error(info,GetLastError());return;
    }
    size_t first=0;
    for(size_t i=0;i<length;++i)if(path[i]==L'\\' || path[i]==L'/')first=i+1;
    const int count=static_cast<int>(length-first);
    if(count<=0){record_error(info,ERROR_INVALID_DATA);return;}
    const int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,path.data()+first,
        count,nullptr,0,nullptr,nullptr);
    if(!bytes){record_error(info,GetLastError());return;}
    std::string basename(static_cast<size_t>(bytes),'\0');
    if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,path.data()+first,count,
        basename.data(),bytes,nullptr,nullptr)){
        record_error(info,GetLastError());return;
    }
    info.executable=std::move(basename);
}

void query_integrity(HANDLE token,ProcessInfo& info) {
    DWORD bytes=0;
    if(!GetTokenInformation(token,TokenIntegrityLevel,nullptr,0,&bytes)){
        const DWORD error=GetLastError();
        if(error!=ERROR_INSUFFICIENT_BUFFER){record_error(info,error);return;}
    }
    if(bytes<sizeof(TOKEN_MANDATORY_LABEL)){
        record_error(info,ERROR_INVALID_DATA);return;
    }
    std::vector<unsigned char> storage(bytes);
    if(!GetTokenInformation(token,TokenIntegrityLevel,storage.data(),bytes,&bytes)){
        record_error(info,GetLastError());return;
    }
    const auto label=reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(storage.data());
    if(!label->Label.Sid || !IsValidSid(label->Label.Sid)){
        record_error(info,ERROR_INVALID_SID);return;
    }
    const auto count=*GetSidSubAuthorityCount(label->Label.Sid);
    if(!count){record_error(info,ERROR_INVALID_SID);return;}
    const DWORD rid=*GetSidSubAuthority(label->Label.Sid,static_cast<DWORD>(count-1));
    if(rid>static_cast<DWORD>((std::numeric_limits<int>::max)())){
        record_error(info,ERROR_INVALID_DATA);return;
    }
    info.integrity_rid=static_cast<int>(rid);
}

}

ProcessInfo process_info(unsigned long pid) {
    ProcessInfo info;info.pid=pid;
    // Query failures are diagnostic data, not application failures. In particular,
    // an inaccessible token must never be interpreted as a normal integrity token.
    try {
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));
        if(!process.value){record_error(info,GetLastError());return info;}
        query_executable(process.value,info);
        HANDLE token_value=nullptr;
        if(!OpenProcessToken(process.value,TOKEN_QUERY,&token_value)){
            record_error(info,GetLastError());return info;
        }
        Handle token(token_value);
        query_integrity(token.value,info);
        TOKEN_ELEVATION elevation{};
        DWORD bytes=0;
        if(GetTokenInformation(token.value,TokenElevation,&elevation,sizeof(elevation),&bytes)){
            info.elevated=elevation.TokenIsElevated!=0;
            info.elevation_known=true;
        }else record_error(info,GetLastError());
    }catch(const std::bad_alloc&){
        record_error(info,ERROR_NOT_ENOUGH_MEMORY);
    }catch(...){
        record_error(info,ERROR_GEN_FAILURE);
    }
    return info;
}

ForegroundInfo foreground() {
    ForegroundInfo result;
    const HWND window=GetForegroundWindow();
    result.window=reinterpret_cast<uintptr_t>(window);
    if(window){
        DWORD pid=0;
        GetWindowThreadProcessId(window,&pid);
        result.pid=pid;
    }
    return result;
}

std::string integrity_name(int rid) {
    if(rid<0)return "unknown";
    if(rid>=SECURITY_MANDATORY_PROTECTED_PROCESS_RID)return "protected";
    if(rid>=SECURITY_MANDATORY_SYSTEM_RID)return "system";
    if(rid>=SECURITY_MANDATORY_HIGH_RID)return "high";
    if(rid>=SECURITY_MANDATORY_MEDIUM_PLUS_RID)return "medium-plus";
    if(rid>=SECURITY_MANDATORY_MEDIUM_RID)return "medium";
    if(rid>=SECURITY_MANDATORY_LOW_RID)return "low";
    return "untrusted";
}

}
