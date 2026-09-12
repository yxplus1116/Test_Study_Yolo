#pragma once
#include <opencv2/core.hpp>
#include "application/common/object_detector.hpp"
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <exception>
namespace Runtime {
class Canceled final:public std::exception {
public: const char* what()const noexcept override{return "GPU operation canceled";}
};
// CPU stages and total use steady_clock; upload/inference/download use CUDA
// events on the inference stream. A Detector is used by one calling thread.
struct InferenceTimings {
    double preprocess_ms=0,upload_ms=0,gpu_inference_ms=0,download_ms=0;
    double postprocess_ms=0,total_ms=0;
};
struct BenchmarkResult {
    int warmup=0,iterations=0;
    double gpu_mean_ms=0,gpu_fps=0;
    std::string device;
};
struct Letterbox { float scale=1,tx=0,ty=0; };
std::vector<float> preprocess(const cv::Mat& image,int side,Letterbox& transform);
ObjectDetector::BoxArray decode(const std::vector<float>& data,int rows,int attributes,
    const Letterbox& transform,int width,int height,float confidence,float nms);
class Detector {
public:
    Detector(const std::string& model,const std::string& cache_dir,bool rebuild,bool fp32=false,
        std::function<bool()> should_stop={});
    ~Detector();
    ObjectDetector::BoxArray infer(const cv::Mat& image,float confidence,float nms);
    int classes() const;
    std::string engine_path() const;
    std::string device_name() const;
    InferenceTimings last_timings() const;
    // Uploads a constant input once, warms up, then times repeated GPU engine
    // execution without capture, per-frame transfers, preprocessing or display.
    // Uses the constructor's cancellation callback and throws Canceled on stop.
    BenchmarkResult benchmark(int warmup=30,int iterations=300);
    void dump_tensors(const std::string& directory) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
