#include "application/makcu_mouse.h"
#include <Windows.h>
#include <array>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {
std::wstring port_name(const std::string& port) {
    if (port.empty()) throw std::invalid_argument("MAKCU serial port is empty");
    return port.rfind("COM", 0) == 0 && port.size() > 4
        ? L"\\\\.\\" + std::wstring(port.begin(), port.end())
        : std::wstring(port.begin(), port.end());
}
HANDLE as_handle(void* value) { return reinterpret_cast<HANDLE>(value); }
}

MakcuMouse::MakcuMouse(std::string port, int baud, int ack_timeout_ms, std::string protocol)
    : port_(std::move(port)), protocol_(std::move(protocol)), baud_(baud), ack_timeout_ms_(ack_timeout_ms) {
    if (baud_ < 115200 || baud_ > 4000000) throw std::invalid_argument("MAKCU baud is out of range");
    if (ack_timeout_ms_ < 1 || ack_timeout_ms_ > 5000) throw std::invalid_argument("MAKCU ACK timeout is out of range");
    if (protocol_ != "ascii" && protocol_ != "binary") throw std::invalid_argument("MAKCU protocol must be ascii or binary");
}
MakcuMouse::~MakcuMouse() { release_all(); close(); }
void MakcuMouse::close() noexcept { if (handle_) { CloseHandle(as_handle(handle_)); handle_ = nullptr; } }

void MakcuMouse::connect() {
    close();
    HANDLE h=CreateFileW(port_name(port_).c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
    if(h==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open MAKCU serial port "+port_+": "+std::to_string(GetLastError()));
    DCB dcb{}; dcb.DCBlength=sizeof(dcb);
    if(!GetCommState(h,&dcb)){auto e=GetLastError();CloseHandle(h);throw std::runtime_error("Cannot read MAKCU serial settings: "+std::to_string(e));}
    dcb.BaudRate=static_cast<DWORD>(baud_);dcb.ByteSize=8;dcb.Parity=NOPARITY;dcb.StopBits=ONESTOPBIT;
    dcb.fBinary=TRUE;dcb.fParity=FALSE;dcb.fOutxCtsFlow=FALSE;dcb.fOutxDsrFlow=FALSE;
    dcb.fDtrControl=DTR_CONTROL_ENABLE;dcb.fRtsControl=RTS_CONTROL_ENABLE;
    if(!SetCommState(h,&dcb)){auto e=GetLastError();CloseHandle(h);throw std::runtime_error("Cannot configure MAKCU serial settings: "+std::to_string(e));}
    // Keep reads bounded but long enough for the documented `>>> ` prompt.
    // A zero multiplier prevents ReadFile from waiting for the whole buffer.
    COMMTIMEOUTS t{};t.ReadIntervalTimeout=5;t.ReadTotalTimeoutMultiplier=0;
    t.ReadTotalTimeoutConstant=static_cast<DWORD>(ack_timeout_ms_);
    t.WriteTotalTimeoutMultiplier=0;t.WriteTotalTimeoutConstant=static_cast<DWORD>(ack_timeout_ms_);
    if(!SetupComm(h,8192,8192)){auto e=GetLastError();CloseHandle(h);throw std::runtime_error("Cannot size MAKCU serial buffers: "+std::to_string(e));}
    if(!SetCommTimeouts(h,&t)){auto e=GetLastError();CloseHandle(h);throw std::runtime_error("Cannot configure MAKCU serial timeouts: "+std::to_string(e));}
    PurgeComm(h,PURGE_RXCLEAR|PURGE_TXCLEAR);handle_=h;
    // Opening USB CDC commonly toggles DTR and resets the ESP32. The reference
    // firmware waits about 1.1 s before installing its serial receive hook.
    // Sending the handshake immediately loses it and falsely reports an
    // apparently usable port as `version=unknown`.
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    std::string reply;
    // The reference client treats a successfully opened COM handle as the
    // connection test and does not wait for ASCII replies. Some firmware
    // builds expose no reply stream, so make the handshake best-effort.
    write_command("km.version()",false);
    read_reply(reply);
    version_=reply.empty()?"unknown":reply;
    // Suppress per-command ASCII echoes after the handshake. This keeps the
    // control path non-blocking while preventing the RX queue from growing.
    // The echo(0) command may intentionally suppress its own reply on some
    // firmware revisions, so treat a successful serial write as sufficient.
    if(!write_command("km.echo(0)",false)){close();throw std::runtime_error("MAKCU refused echo configuration");}
}
bool MakcuMouse::write_command(const std::string& command,bool wait_for_reply){
    if(!handle_)return false;std::string wire=command+"\r\n";
    if(!write_bytes(wire.data(),wire.size()))return false;
    if(!wait_for_reply)return true;std::string ignored;return read_reply(ignored);
}
bool MakcuMouse::write_bytes(const void* data,std::size_t size){
    if(!handle_ || !data || size==0)return false;
    const auto* bytes=static_cast<const unsigned char*>(data);std::size_t offset=0;
    while(offset<size){
        DWORD written=0;
        if(!WriteFile(as_handle(handle_),bytes+offset,static_cast<DWORD>(size-offset),&written,nullptr) || written==0)return false;
        offset+=written;
    }
    return true;
}
bool MakcuMouse::read_reply(std::string& reply){
    reply.clear();if(!handle_)return false;auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(ack_timeout_ms_);std::array<char,256> b{};
    while(std::chrono::steady_clock::now()<deadline){DWORD n=0;if(!ReadFile(as_handle(handle_),b.data(),static_cast<DWORD>(b.size()-1),&n,nullptr))return false;if(n){reply.append(b.data(),n);if(reply.find(">>>")!=std::string::npos)return true;}else std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    return !reply.empty();
}
bool MakcuMouse::move(const Movement& movement){
    if(!handle_)return false;
    if(protocol_=="ascii") return write_command("km.move("+std::to_string(movement.x)+","+std::to_string(movement.y)+")",false);
    std::array<unsigned char,11> frame{0x50,0x0D,7,0,
        static_cast<unsigned char>(movement.x&0xff),static_cast<unsigned char>((movement.x>>8)&0xff),
        static_cast<unsigned char>(movement.y&0xff),static_cast<unsigned char>((movement.y>>8)&0xff),1,0,0};
    return write_bytes(frame.data(),frame.size());
}
void MakcuMouse::release_all() noexcept {if(!handle_)return;for(const char* c:{"km.left(0)","km.right(0)","km.middle(0)","km.side1(0)","km.side2(0)"})write_command(c,false);}
