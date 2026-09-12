#include "trt_detector.h"
#include <NvInfer.h>
#include <NvOnnxParser.h>
#include <cuda_runtime_api.h>
#include <Windows.h>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <chrono>
#include <cmath>
namespace fs=std::filesystem;
namespace Runtime {
namespace {
void cuda_check(cudaError_t e,const char* operation){
    if(e!=cudaSuccess)throw std::runtime_error(std::string(operation)+": "+cudaGetErrorString(e));
}
struct Logger final:nvinfer1::ILogger{
    void log(Severity severity,const char* message)noexcept override{
        if(severity<=Severity::kWARNING)std::cerr<<"[TensorRT] "<<message<<std::endl;
    }
};
struct CancelMonitor final:nvinfer1::IProgressMonitor{
    std::function<bool()> should_stop;
    explicit CancelMonitor(std::function<bool()> callback):should_stop(std::move(callback)){}
    bool canceled()const noexcept{try{return should_stop && should_stop();}catch(...){return true;}}
    void phaseStart(const char*,const char*,int32_t)noexcept override{}
    void phaseFinish(const char*)noexcept override{}
    bool stepComplete(const char*,int32_t)noexcept override{return !canceled();}
};
std::vector<char> read_file(const fs::path& path){
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file)throw std::runtime_error("Cannot open "+path.string());
    auto n=file.tellg();if(n<=0 || n>std::streamoff(1024ULL*1024*1024))throw std::runtime_error("Invalid file size");
    std::vector<char> data(static_cast<size_t>(n));
    file.seekg(0);if(!file.read(data.data(),data.size()))throw std::runtime_error("Incomplete file read");
    return data;
}
std::string digest(const std::vector<char>& model,const std::string& identity){
    struct Hash{
        BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
        ~Hash(){if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);}
    } h;
    auto ok=[](NTSTATUS s){if(s<0)throw std::runtime_error("SHA256 failed");};
    ok(BCryptOpenAlgorithmProvider(&h.algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0));
    ok(BCryptCreateHash(h.algorithm,&h.hash,nullptr,0,nullptr,0,0));
    ok(BCryptHashData(h.hash,reinterpret_cast<PUCHAR>(const_cast<char*>(model.data())),ULONG(model.size()),0));
    ok(BCryptHashData(h.hash,reinterpret_cast<PUCHAR>(const_cast<char*>(identity.data())),ULONG(identity.size()),0));
    unsigned char bytes[32];ok(BCryptFinishHash(h.hash,bytes,sizeof(bytes),0));
    std::ostringstream out;out<<std::hex<<std::setfill('0');
    for(auto b:bytes)out<<std::setw(2)<<int(b);return out.str();
}
struct DeviceMemory {
    void* pointer=nullptr;
    ~DeviceMemory(){if(pointer)cudaFree(pointer);}
    void allocate(size_t bytes){cuda_check(cudaMalloc(&pointer,bytes),"cudaMalloc");}
};
struct Stream {
    cudaStream_t handle=nullptr;
    Stream(){cuda_check(cudaStreamCreateWithFlags(&handle,cudaStreamNonBlocking),"cudaStreamCreate");}
    ~Stream(){if(handle){cudaStreamSynchronize(handle);cudaStreamDestroy(handle);}}
};
struct Event {
    cudaEvent_t handle=nullptr;
    Event(){cuda_check(cudaEventCreate(&handle),"cudaEventCreate");}
    ~Event(){if(handle)cudaEventDestroy(handle);}
    Event(const Event&)=delete;
    Event& operator=(const Event&)=delete;
    void record(cudaStream_t stream){cuda_check(cudaEventRecord(handle,stream),"cudaEventRecord");}
    double elapsed_to(const Event& end)const{
        float ms=0;
        cuda_check(cudaEventElapsedTime(&ms,handle,end.handle),"cudaEventElapsedTime");
        return ms;
    }
};
using TimingClock=std::chrono::steady_clock;
double elapsed_ms(TimingClock::time_point begin,TimingClock::time_point end){
    return std::chrono::duration<double,std::milli>(end-begin).count();
}
}
struct Detector::Impl {
    Logger logger;
    CancelMonitor monitor;
    std::unique_ptr<nvinfer1::IRuntime> runtime;
    std::unique_ptr<nvinfer1::ICudaEngine> engine;
    std::unique_ptr<nvinfer1::IExecutionContext> context;
    DeviceMemory input,output;
    Stream stream; // Destroy/synchronize before freeing buffers and context.
    Event upload_start,upload_end,inference_end,download_end;
    std::vector<float> host_output,host_input;
    std::string path,device;
    InferenceTimings timings;
    int side=640,rows=0,attrs=0;
    Impl(const std::string& model,const std::string& cache,bool rebuild,bool fp32,std::function<bool()> stop)
        :monitor(std::move(stop)){
        if(monitor.canceled())throw Canceled();
        cuda_check(cudaSetDevice(0),"cudaSetDevice");
        cudaDeviceProp gpu{};cuda_check(cudaGetDeviceProperties(&gpu,0),"cudaGetDeviceProperties");
        device=gpu.name;
        int driver=0;cuda_check(cudaDriverGetVersion(&driver),"cudaDriverGetVersion");
        if(getInferLibVersion()!=NV_TENSORRT_VERSION)throw std::runtime_error("TensorRT header/runtime version mismatch");
        auto bytes=read_file(model);
        std::ostringstream identity;
        identity<<"app-v2-trt"<<NV_TENSORRT_VERSION<<"-cuda"<<CUDART_VERSION<<"-driver"<<driver
            <<"-gpu"<<gpu.name<<"-"<<gpu.major<<gpu.minor<<"-batch1-640-"<<(fp32?"fp32":"fp16");
        fs::create_directories(cache);
        path=(fs::path(cache)/(fs::path(model).stem().string()+"-"+digest(bytes,identity.str())+".engine")).string();
        runtime.reset(nvinfer1::createInferRuntime(logger));
        if(!runtime)throw std::runtime_error("createInferRuntime failed");
        if(!rebuild && fs::exists(path)){
            try{auto plan=read_file(path);engine.reset(runtime->deserializeCudaEngine(plan.data(),plan.size()));}
            catch(const std::exception& e){std::cerr<<"Ignoring invalid engine cache: "<<e.what()<<std::endl;}
        }
        if(!engine){
            std::cout<<"Building TensorRT engine on "<<gpu.name<<" ("<<(fp32?"FP32":"FP16")<<")..."<<std::endl;
            std::unique_ptr<nvinfer1::IBuilder> builder(nvinfer1::createInferBuilder(logger));
            if(!builder)throw std::runtime_error("createInferBuilder failed");
            std::unique_ptr<nvinfer1::INetworkDefinition> network(builder->createNetworkV2(0));
            if(!network)throw std::runtime_error("createNetworkV2 failed");
            std::unique_ptr<nvonnxparser::IParser> parser(nvonnxparser::createParser(*network,logger));
            if(!parser || !parser->parse(bytes.data(),bytes.size(),model.c_str()))throw std::runtime_error("ONNX parsing failed");
            if(network->getNbInputs()!=1 || std::string(network->getInput(0)->getName())!="images")
                throw std::runtime_error("Model must expose one input named images");
            auto dims=network->getInput(0)->getDimensions();
            if(dims.nbDims!=4 || (dims.d[0]!=-1 && dims.d[0]!=1) || dims.d[1]!=3 || dims.d[2]!=side || dims.d[3]!=side ||
               network->getInput(0)->getType()!=nvinfer1::DataType::kFLOAT)
                throw std::runtime_error("Expected float images [batch,3,640,640]");
            bool found=false;
            for(int i=network->getNbOutputs()-1;i>=0;--i){
                auto tensor=network->getOutput(i);
                if(std::string(tensor->getName())=="output")found=true;else network->unmarkOutput(*tensor);
            }
            if(!found)throw std::runtime_error("Missing decoded YOLOv5 output tensor named output");
            network->getOutput(0)->setType(nvinfer1::DataType::kFLOAT);
            std::unique_ptr<nvinfer1::IBuilderConfig> config(builder->createBuilderConfig());
            if(!config)throw std::runtime_error("createBuilderConfig failed");
            config->setProgressMonitor(&monitor);
            config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE,512ULL*1024*1024);
            config->setBuilderOptimizationLevel(3);
            if(!fp32)config->setFlag(nvinfer1::BuilderFlag::kFP16);
            if(dims.d[0]==-1){
                auto profile=builder->createOptimizationProfile();
                if(!profile)throw std::runtime_error("createOptimizationProfile failed");
                auto shape=nvinfer1::Dims4{1,3,side,side};
                if(!profile->setDimensions("images",nvinfer1::OptProfileSelector::kMIN,shape) ||
                   !profile->setDimensions("images",nvinfer1::OptProfileSelector::kOPT,shape) ||
                   !profile->setDimensions("images",nvinfer1::OptProfileSelector::kMAX,shape) ||
                   config->addOptimizationProfile(profile)<0)throw std::runtime_error("Optimization profile failed");
            }
            std::unique_ptr<nvinfer1::IHostMemory> plan(builder->buildSerializedNetwork(*network,*config));
            if(monitor.canceled())throw Canceled();
            if(!plan)throw std::runtime_error("Engine build failed");
            engine.reset(runtime->deserializeCudaEngine(plan->data(),plan->size()));
            if(!engine)throw std::runtime_error("Built engine cannot deserialize");
            auto temporary=path+".tmp";
            std::ofstream f(temporary,std::ios::binary|std::ios::trunc);
            f.write(static_cast<const char*>(plan->data()),plan->size());f.close();
            if(!f)throw std::runtime_error("Cannot write engine cache");
            if(!MoveFileExW(fs::path(temporary).c_str(),fs::path(path).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("Cannot finalize engine cache");
        }
        if(engine->getNbIOTensors()!=2 || engine->getTensorIOMode("images")!=nvinfer1::TensorIOMode::kINPUT ||
           engine->getTensorIOMode("output")!=nvinfer1::TensorIOMode::kOUTPUT ||
           engine->getTensorDataType("images")!=nvinfer1::DataType::kFLOAT ||
           engine->getTensorDataType("output")!=nvinfer1::DataType::kFLOAT)
            throw std::runtime_error("Unsupported engine I/O; rebuild required");
        context.reset(engine->createExecutionContext());
        if(!context || !context->setInputShape("images",nvinfer1::Dims4{1,3,side,side}))
            throw std::runtime_error("Cannot initialize inference context");
        auto shape=context->getTensorShape("output");
        if(shape.nbDims!=3 || shape.d[0]!=1 || shape.d[1]<=0 || shape.d[1]>100000 || shape.d[2]<6 || shape.d[2]>1000)
            throw std::runtime_error("Expected YOLOv5 output [1,anchors,5+classes]");
        rows=int(shape.d[1]);attrs=int(shape.d[2]);
        input.allocate(size_t(3)*side*side*sizeof(float));
        output.allocate(size_t(rows)*attrs*sizeof(float));host_output.resize(size_t(rows)*attrs);
        if(!context->setTensorAddress("images",input.pointer) || !context->setTensorAddress("output",output.pointer))
            throw std::runtime_error("setTensorAddress failed");
        std::cout<<"Engine ready: "<<path<<"; output [1,"<<rows<<","<<attrs<<"]"<<std::endl;
        if(monitor.canceled())throw Canceled();
    }
    ObjectDetector::BoxArray infer(const cv::Mat& image,float confidence,float nms){
        const auto total_start=TimingClock::now();
        InferenceTimings measured;
        Letterbox t;
        host_input=preprocess(image,side,t);
        measured.preprocess_ms=elapsed_ms(total_start,TimingClock::now());
        upload_start.record(stream.handle);
        cuda_check(cudaMemcpyAsync(input.pointer,host_input.data(),host_input.size()*sizeof(float),cudaMemcpyHostToDevice,stream.handle),"Upload");
        upload_end.record(stream.handle);
        if(!context->enqueueV3(stream.handle))throw std::runtime_error("TensorRT enqueueV3 failed");
        inference_end.record(stream.handle);
        cuda_check(cudaMemcpyAsync(host_output.data(),output.pointer,host_output.size()*sizeof(float),cudaMemcpyDeviceToHost,stream.handle),"Download");
        download_end.record(stream.handle);
        cuda_check(cudaStreamSynchronize(stream.handle),"Inference synchronization");
        measured.upload_ms=upload_start.elapsed_to(upload_end);
        measured.gpu_inference_ms=upload_end.elapsed_to(inference_end);
        measured.download_ms=inference_end.elapsed_to(download_end);
        const auto postprocess_start=TimingClock::now();
        auto result=decode(host_output,rows,attrs,t,image.cols,image.rows,confidence,nms);
        const auto total_end=TimingClock::now();
        measured.postprocess_ms=elapsed_ms(postprocess_start,total_end);
        measured.total_ms=elapsed_ms(total_start,total_end);
        timings=measured;
        return result;
    }
    BenchmarkResult benchmark(int warmup,int iterations){
        if(warmup<0 || warmup>100000 || iterations<=0 || iterations>100000)
            throw std::invalid_argument("Benchmark requires 0..100000 warmup and 1..100000 iterations");
        const auto check_canceled=[&]{if(monitor.canceled())throw Canceled();};
        check_canceled();
        // The float input is fully initialized. Keep this separate from the
        // most recent inference's host buffers so tensor dumps remain paired.
        const std::vector<float> constant_input(size_t(3)*side*side,114.0f/255.0f);
        struct FinishPendingWork {
            cudaStream_t stream;
            ~FinishPendingWork(){cudaStreamSynchronize(stream);}
        } finish{stream.handle}; // Also protect input lifetime on cancel/error.
        cuda_check(cudaMemcpyAsync(input.pointer,constant_input.data(),constant_input.size()*sizeof(float),
            cudaMemcpyHostToDevice,stream.handle),"Benchmark upload");
        for(int i=0;i<warmup;++i){
            check_canceled();
            if(!context->enqueueV3(stream.handle))throw std::runtime_error("Benchmark warmup enqueueV3 failed");
        }
        cuda_check(cudaStreamSynchronize(stream.handle),"Benchmark warmup synchronization");
        check_canceled();
        Event begin,end;
        begin.record(stream.handle);
        for(int i=0;i<iterations;++i){
            check_canceled();
            if(!context->enqueueV3(stream.handle))throw std::runtime_error("Benchmark enqueueV3 failed");
        }
        end.record(stream.handle);
        cuda_check(cudaEventSynchronize(end.handle),"Benchmark synchronization");
        check_canceled();
        const double total=begin.elapsed_to(end);
        if(!std::isfinite(total) || total<=0)throw std::runtime_error("Invalid GPU benchmark timing");
        BenchmarkResult result;
        result.warmup=warmup;result.iterations=iterations;result.device=device;
        result.gpu_mean_ms=total/iterations;result.gpu_fps=1000.0/result.gpu_mean_ms;
        return result;
    }
};
Detector::Detector(const std::string& m,const std::string& c,bool r,bool f,std::function<bool()> stop)
    :impl_(std::make_unique<Impl>(m,c,r,f,std::move(stop))){}
Detector::~Detector()=default;
ObjectDetector::BoxArray Detector::infer(const cv::Mat& image,float confidence,float nms){return impl_->infer(image,confidence,nms);}
int Detector::classes()const{return impl_->attrs-5;}
std::string Detector::engine_path()const{return impl_->path;}
std::string Detector::device_name()const{return impl_->device;}
InferenceTimings Detector::last_timings()const{return impl_->timings;}
BenchmarkResult Detector::benchmark(int warmup,int iterations){return impl_->benchmark(warmup,iterations);}
void Detector::dump_tensors(const std::string& directory)const{
    if(impl_->host_input.empty())throw std::runtime_error("Run inference before dumping tensors");
    fs::create_directories(directory);
    auto write=[&](const char* name,const std::vector<float>& data){
        std::ofstream f(fs::path(directory)/name,std::ios::binary);
        f.write(reinterpret_cast<const char*>(data.data()),data.size()*sizeof(float));
        if(!f)throw std::runtime_error("Cannot write tensor dump");
    };
    write("input.f32",impl_->host_input);write("output.f32",impl_->host_output);
}
}
