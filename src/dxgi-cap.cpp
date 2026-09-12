#include "application/dxgi-cap.h"
#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <sstream>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
namespace {
std::string failure(const char* operation,HRESULT hr) {
    std::ostringstream s;s<<operation<<" failed (0x"<<std::hex<<static_cast<unsigned long>(hr)<<")";return s.str();
}
void check(HRESULT hr,const char* operation){if(FAILED(hr))throw std::runtime_error(failure(operation,hr));}
struct Display { MonitorGeometry geometry; ComPtr<IDXGIAdapter1> adapter; ComPtr<IDXGIOutput> output; };
std::vector<Display> enumerate() {
    ComPtr<IDXGIFactory1> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"CreateDXGIFactory1");
    std::vector<Display> displays;
    for(UINT a=0;;++a){
        ComPtr<IDXGIAdapter1> adapter;
        HRESULT hr=factory->EnumAdapters1(a,&adapter);
        if(hr==DXGI_ERROR_NOT_FOUND)break;check(hr,"EnumAdapters1");
        for(UINT o=0;;++o){
            ComPtr<IDXGIOutput> output;hr=adapter->EnumOutputs(o,&output);
            if(hr==DXGI_ERROR_NOT_FOUND)break;check(hr,"EnumOutputs");
            DXGI_OUTPUT_DESC d{};check(output->GetDesc(&d),"GetDesc");
            if(!d.AttachedToDesktop)continue;
            auto r=d.DesktopCoordinates;
            char name[128]{};
            WideCharToMultiByte(CP_UTF8,0,d.DeviceName,-1,name,sizeof(name),nullptr,nullptr);
            displays.push_back({{int(displays.size()),r.left,r.top,r.right-r.left,r.bottom-r.top,name},adapter,output});
        }
    }
    return displays;
}
}
struct ScreenCapture::Impl {
    int index,size; bool gdi;
    MonitorGeometry geometry;
    std::string selected_name;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIOutputDuplication> duplication;
    ComPtr<ID3D11Texture2D> staging;
    DXGI_MODE_ROTATION rotation=DXGI_MODE_ROTATION_IDENTITY;
    SteadyClock::time_point retry{};
    Impl(int i,int s,bool g):index(i),size(s),gdi(g) {
        if(size<32 || size>4096)throw std::invalid_argument("Capture size must be 32..4096");
        auto d=enumerate();
        if(index<0 || index>=int(d.size()))throw std::invalid_argument("Monitor index is unavailable");
        geometry=d[index].geometry;
        selected_name=geometry.name;
    }
    void reset(){staging.Reset();duplication.Reset();context.Reset();device.Reset();}
    void initialize(){
        auto d=enumerate();
        auto selected=std::find_if(d.begin(),d.end(),[&](const auto& display){return display.geometry.name==selected_name;});
        if(selected==d.end())throw std::runtime_error("Selected monitor disconnected");
        index=int(selected-d.begin());
        geometry=d[index].geometry;
        check(D3D11CreateDevice(d[index].adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"D3D11CreateDevice");
        ComPtr<IDXGIOutput1> output;
        check(d[index].output.As(&output),"Query IDXGIOutput1");
        check(output->DuplicateOutput(device.Get(),&duplication),"DuplicateOutput");
        DXGI_OUTDUPL_DESC desc{};duplication->GetDesc(&desc);rotation=desc.Rotation;
    }
    CaptureResult grab_gdi(){
        auto all=enumerate();
        auto selected=std::find_if(all.begin(),all.end(),[&](const auto& display){return display.geometry.name==selected_name;});
        if(selected==all.end())throw std::runtime_error("Selected monitor disconnected");
        index=int(selected-all.begin());
        geometry=all[index].geometry;
        int w=std::min(size,geometry.width),h=std::min(size,geometry.height);
        if(w<=0 || h<=0)throw std::runtime_error("Invalid monitor geometry");
        struct Resources{
            HDC screen=nullptr,memory=nullptr;HBITMAP bitmap=nullptr;HGDIOBJ previous=nullptr;
            ~Resources(){if(previous && previous!=HGDI_ERROR)SelectObject(memory,previous);
                if(bitmap)DeleteObject(bitmap);if(memory)DeleteDC(memory);if(screen)ReleaseDC(nullptr,screen);}
        } r;
        r.screen=GetDC(nullptr);if(!r.screen)throw std::runtime_error("GetDC failed");
        r.memory=CreateCompatibleDC(r.screen);if(!r.memory)throw std::runtime_error("CreateCompatibleDC failed");
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=w;info.bmiHeader.biHeight=-h;info.bmiHeader.biPlanes=1;
        info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        void* pixels=nullptr;
        r.bitmap=CreateDIBSection(r.screen,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
        if(!r.bitmap || !pixels)throw std::runtime_error("CreateDIBSection failed");
        r.previous=SelectObject(r.memory,r.bitmap);
        if(!r.previous || r.previous==HGDI_ERROR)throw std::runtime_error("SelectObject failed");
        auto captured=SteadyClock::now();
        if(!BitBlt(r.memory,0,0,w,h,r.screen,geometry.left+(geometry.width-w)/2,
            geometry.top+(geometry.height-h)/2,SRCCOPY|CAPTUREBLT))throw std::runtime_error("BitBlt failed");
        if(!GdiFlush())throw std::runtime_error("GdiFlush failed");
        cv::Mat bgr;cv::cvtColor(cv::Mat(h,w,CV_8UC4,pixels,size_t(w)*4),bgr,cv::COLOR_BGRA2BGR);
        return {bgr,CaptureStatus::Ok,captured,{}};
    }
    CaptureResult grab_dxgi(){
        if(!duplication)initialize();
        DXGI_OUTDUPL_FRAME_INFO info{};ComPtr<IDXGIResource> resource;
        HRESULT hr=duplication->AcquireNextFrame(10,&info,&resource);
        if(hr==DXGI_ERROR_WAIT_TIMEOUT)return {{},CaptureStatus::NoFrame,{},{}};
        check(hr,"AcquireNextFrame");
        struct FrameGuard{
            IDXGIOutputDuplication* d;bool active=true;
            ~FrameGuard(){if(active)d->ReleaseFrame();}
        } guard{duplication.Get()};
        auto captured=SteadyClock::now();
        ComPtr<ID3D11Texture2D> texture;check(resource.As(&texture),"Query desktop texture");
        D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
        if(desc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM || !desc.Width || !desc.Height)
            throw std::runtime_error("Unsupported desktop texture format");
        D3D11_TEXTURE2D_DESC existing{};
        if(staging)staging->GetDesc(&existing);
        if(!staging || existing.Width!=desc.Width || existing.Height!=desc.Height){
            staging.Reset();desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;
            desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
            check(device->CreateTexture2D(&desc,nullptr,&staging),"Create staging texture");
        }
        context->CopyResource(staging.Get(),texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Map");
        struct MapGuard{
            ID3D11DeviceContext* c;ID3D11Texture2D* t;bool active=true;
            ~MapGuard(){if(active)c->Unmap(t,0);}
        } mapGuard{context.Get(),staging.Get()};
        if(!mapped.pData || mapped.RowPitch<size_t(desc.Width)*4)throw std::runtime_error("Invalid desktop RowPitch");
        cv::Mat view(int(desc.Height),int(desc.Width),CV_8UC4,mapped.pData,mapped.RowPitch);
        cv::Mat oriented;
        if(rotation==DXGI_MODE_ROTATION_ROTATE90)cv::rotate(view,oriented,cv::ROTATE_90_CLOCKWISE);
        else if(rotation==DXGI_MODE_ROTATION_ROTATE180)cv::rotate(view,oriented,cv::ROTATE_180);
        else if(rotation==DXGI_MODE_ROTATION_ROTATE270)cv::rotate(view,oriented,cv::ROTATE_90_COUNTERCLOCKWISE);
        else oriented=view;
        if(oriented.cols!=geometry.width || oriented.rows!=geometry.height)
            throw std::runtime_error("Desktop geometry changed; reacquiring");
        int w=std::min(size,oriented.cols),h=std::min(size,oriented.rows);
        cv::Mat bgr;
        cv::cvtColor(oriented(cv::Rect((oriented.cols-w)/2,(oriented.rows-h)/2,w,h)),bgr,cv::COLOR_BGRA2BGR);
        context->Unmap(staging.Get(),0);mapGuard.active=false;
        hr=duplication->ReleaseFrame();guard.active=false;check(hr,"ReleaseFrame");
        return {bgr,CaptureStatus::Ok,captured,{}};
    }
};
std::vector<MonitorGeometry> ScreenCapture::monitors(){
    std::vector<MonitorGeometry> result;for(auto& d:enumerate())result.push_back(d.geometry);return result;
}
ScreenCapture::ScreenCapture(int i,int s,bool g):impl_(std::make_unique<Impl>(i,s,g)){}
ScreenCapture::~ScreenCapture()=default;
MonitorGeometry ScreenCapture::geometry()const{return impl_->geometry;}
CaptureResult ScreenCapture::grab(){
    if(SteadyClock::now()<impl_->retry)return {{},CaptureStatus::Unavailable,{},"Waiting to reacquire desktop"};
    try{return impl_->gdi?impl_->grab_gdi():impl_->grab_dxgi();}
    catch(const std::exception& e){
        impl_->reset();impl_->retry=SteadyClock::now()+std::chrono::milliseconds(250);
        return {{},CaptureStatus::Unavailable,{},e.what()};
    }
}
