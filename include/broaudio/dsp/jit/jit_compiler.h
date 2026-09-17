#pragma once

#include "broaudio/dsp/jit/jit_types.h"
#include "broaudio/dsp/jit/jit_bus_pipeline.h"

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>

namespace brass::codegen {
class KernelFunction;
}

namespace broaudio {

// Background JIT compilation service and kernel cache.
// Compiles JitTopology requests to machine code off the audio thread.
class JitCompiler {
public:
    JitCompiler();
    ~JitCompiler();

    void shutdown();

    JitCompiler(const JitCompiler&) = delete;
    JitCompiler& operator=(const JitCompiler&) = delete;

    // Synchronously compile or fetch from cache (used in tests or offline render)
    std::shared_ptr<JitBusPipeline> compileSync(const JitTopology& topology);

    // Asynchronously compile or fetch from cache, invoking callback on completion
    void compileAsync(const JitTopology& topology,
                      std::function<void(std::shared_ptr<JitBusPipeline>)> onComplete);

    // Clear cached compiled kernels
    void clearCache();

    // Number of compiled topologies currently cached
    size_t cacheSize() const;

    // Check if a topology is already compiled in the cache
    bool isCached(const JitTopology& topology) const;

private:
    struct CompileTask {
        JitTopology topology;
        std::function<void(std::shared_ptr<JitBusPipeline>)> callback;
    };

    void workerLoop();
    std::shared_ptr<brass::codegen::KernelFunction> compileKernel(const JitTopology& topology);

    mutable std::mutex cacheMutex_;
    std::unordered_map<JitTopology, std::shared_ptr<brass::codegen::KernelFunction>, JitTopologyHash> cache_;

    std::mutex queueMutex_;
    std::condition_variable cv_;
    std::queue<CompileTask> queue_;
    std::thread workerThread_;
    std::atomic<bool> stopWorker_{false};
};

} // namespace broaudio
