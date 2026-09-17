#include "broaudio/dsp/jit/jit_compiler.h"
#include "jit_bus_builder.h"
#include <brass/codegen/kernel_jit.hpp>

namespace broaudio {

JitCompiler::JitCompiler()
{
    workerThread_ = std::thread(&JitCompiler::workerLoop, this);
}

void JitCompiler::shutdown()
{
    stopWorker_.store(true, std::memory_order_release);
    cv_.notify_all();
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
}

JitCompiler::~JitCompiler()
{
    shutdown();
}

std::shared_ptr<brass::codegen::KernelFunction> JitCompiler::compileKernel(const JitTopology& topology)
{
    return buildBusKernel(topology);
}

std::shared_ptr<JitBusPipeline> JitCompiler::compileSync(const JitTopology& topology)
{
    std::shared_ptr<brass::codegen::KernelFunction> kernel;
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        auto it = cache_.find(topology);
        if (it != cache_.end()) {
            kernel = it->second;
        }
    }

    if (!kernel) {
        kernel = compileKernel(topology);
        {
            std::lock_guard<std::mutex> lock(cacheMutex_);
            cache_[topology] = kernel;
        }
    }

    auto fnPtr = kernel ? kernel->as<BusJitFn>() : nullptr;
    return std::make_shared<JitBusPipeline>(topology, kernel, fnPtr);
}

void JitCompiler::compileAsync(const JitTopology& topology,
                              std::function<void(std::shared_ptr<JitBusPipeline>)> onComplete)
{
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        auto it = cache_.find(topology);
        if (it != cache_.end()) {
            auto kernel = it->second;
            auto fnPtr = kernel ? kernel->as<BusJitFn>() : nullptr;
            if (onComplete) {
                onComplete(std::make_shared<JitBusPipeline>(topology, kernel, fnPtr));
            }
            return;
        }
    }

    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        queue_.push(CompileTask{topology, std::move(onComplete)});
    }
    cv_.notify_one();
}

void JitCompiler::workerLoop()
{
    while (!stopWorker_.load(std::memory_order_acquire)) {
        CompileTask task;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            cv_.wait(lock, [&]() {
                return stopWorker_.load(std::memory_order_acquire) || !queue_.empty();
            });

            if (stopWorker_.load(std::memory_order_acquire)) {
                break;
            }

            task = std::move(queue_.front());
            queue_.pop();
        }

        std::shared_ptr<brass::codegen::KernelFunction> kernel;
        {
            std::lock_guard<std::mutex> lock(cacheMutex_);
            auto it = cache_.find(task.topology);
            if (it != cache_.end()) {
                kernel = it->second;
            }
        }

        if (!kernel) {
            try {
                kernel = compileKernel(task.topology);
                {
                    std::lock_guard<std::mutex> lock(cacheMutex_);
                    cache_[task.topology] = kernel;
                }
            } catch (...) {
                kernel = nullptr;
            }
        }

        if (task.callback) {
            auto fnPtr = kernel ? kernel->as<BusJitFn>() : nullptr;
            auto pipeline = std::make_shared<JitBusPipeline>(task.topology, kernel, fnPtr);
            task.callback(pipeline);
        }
    }
}

void JitCompiler::clearCache()
{
    std::lock_guard<std::mutex> lock(cacheMutex_);
    cache_.clear();
}

size_t JitCompiler::cacheSize() const
{
    std::lock_guard<std::mutex> lock(cacheMutex_);
    return cache_.size();
}

bool JitCompiler::isCached(const JitTopology& topology) const
{
    std::lock_guard<std::mutex> lock(cacheMutex_);
    return cache_.find(topology) != cache_.end();
}

} // namespace broaudio
