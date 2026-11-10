// CudaKernel.cu — the NVIDIA render path (Windows, Linux). Compiles LensKernel.h as CUDA device
// code ahead of time and runs it on the stream Resolve hands the plugin.
//
// Same passes per frame as MetalKernel.mm:
//   1. linearize: decode the clip's transfer function once into a scratch buffer
//   2. mips:      build the weighted pyramid the bokeh and glow read from, one level per launch
//                 (skipped when nothing blurs)
//   3. lens:      the full simulation, reading already-linear pixels
//
// Scratch memory comes from a stream-ordered pool: each frame allocates and frees on its own
// stream, so frames Resolve renders concurrently never share a buffer, and the pool keeps the
// memory around so steady playback does not allocate. GPUs or drivers without memory pools
// fall back to plain cudaMalloc, synchronising the stream before freeing. Setting the environment
// variable LENSCHARACTER_CUDA_NOPOOL=1 forces that fallback (for diagnosing a host's CUDA setup).

#include <cuda_runtime.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "LensKernel.h"

namespace {

__global__ void LensLinearizeKernel(lc::LCParams P, const float* __restrict__ src, float* __restrict__ dst)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= P.width || y >= P.height) return;
    const int i = (y * P.width + x) * 4;
    dst[i + 0] = lc::decode(P.encoding, src[i + 0]);
    dst[i + 1] = lc::decode(P.encoding, src[i + 1]);
    dst[i + 2] = lc::decode(P.encoding, src[i + 2]);
    dst[i + 3] = src[i + 3];
}

__global__ void LensMipKernel(lc::LCParams P, const float* src, float* mips, int level)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    const int k = level;
    const int w = P.mipW[k - 1];
    if (x >= w || y >= P.mipH[k - 1]) return;
    const int i = P.mipOffset[k - 1] + (y * w + x) * 8;
    for (int set = 0; set < 2; set++)
    {
        lc::LCColor c = lc::mipBuildTexel(src, mips, P, k, x, y, set);
        mips[i + set * 4 + 0] = c.r;
        mips[i + set * 4 + 1] = c.g;
        mips[i + set * 4 + 2] = c.b;
        mips[i + set * 4 + 3] = c.a;
    }
}

__global__ void LensCharacterKernel(lc::LCParams P, const float* __restrict__ src, const float* __restrict__ mips,
                                    float* __restrict__ dst)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= P.width || y >= P.height) return;
    lc::LCColor c = lc::shade(P, src, mips, x, y);
    const int i = (y * P.width + x) * 4;
    dst[i + 0] = c.r;
    dst[i + 1] = c.g;
    dst[i + 2] = c.b;
    dst[i + 3] = c.a;
}

// Short, wide blocks keep neighbouring pixels (and their samples) together, as on Metal
const dim3 kBlock(32, 8, 1);

dim3 gridFor(int w, int h)
{
    return dim3((unsigned)((w + kBlock.x - 1) / kBlock.x), (unsigned)((h + kBlock.y - 1) / kBlock.y), 1);
}

// ---- scratch memory ----------------------------------------------------------------------------

const int kMaxDevices = 16;
const uint64_t kPoolKeepBytes = 1ull << 30;   // the pool returns memory above this to the driver

struct DevicePool
{
    bool checked = false;
    cudaMemPool_t pool = nullptr;   // null: no memory pools, use cudaMalloc
};

std::mutex s_PoolMutex;
DevicePool s_Pools[kMaxDevices];

cudaMemPool_t poolFor(int device)
{
    if (device < 0 || device >= kMaxDevices) return nullptr;
    std::lock_guard<std::mutex> lock(s_PoolMutex);
    DevicePool& d = s_Pools[device];
    if (d.checked) return d.pool;
    d.checked = true;
    if (std::getenv("LENSCHARACTER_CUDA_NOPOOL")) return nullptr;
    int supported = 0;
    if (cudaDeviceGetAttribute(&supported, cudaDevAttrMemoryPoolsSupported, device) != cudaSuccess || !supported)
    {
        cudaGetLastError();
        return nullptr;
    }
    // Our own pool rather than the device default, so its settings never touch Resolve's allocations
    cudaMemPoolProps props = {};
    props.allocType = cudaMemAllocationTypePinned;
    props.location.type = cudaMemLocationTypeDevice;
    props.location.id = device;
    cudaMemPool_t pool = nullptr;
    if (cudaMemPoolCreate(&pool, &props) != cudaSuccess)
    {
        cudaGetLastError();
        return nullptr;
    }
    uint64_t keep = kPoolKeepBytes;
    cudaMemPoolSetAttribute(pool, cudaMemPoolAttrReleaseThreshold, &keep);
    d.pool = pool;
    return pool;
}

// Buffers for one frame, released in stream order when it goes out of scope
struct FrameBuffers
{
    cudaStream_t stream;
    cudaMemPool_t pool;
    float* scratch = nullptr;
    float* mips = nullptr;

    FrameBuffers(cudaStream_t s, cudaMemPool_t p) : stream(s), pool(p) {}

    float* alloc(size_t floats)
    {
        void* ptr = nullptr;
        cudaError_t e = pool ? cudaMallocFromPoolAsync(&ptr, floats * sizeof(float), pool, stream)
                             : cudaMalloc(&ptr, floats * sizeof(float));
        if (e != cudaSuccess) { cudaGetLastError(); return nullptr; }
        return static_cast<float*>(ptr);
    }

    ~FrameBuffers()
    {
        if (pool)
        {
            if (scratch) cudaFreeAsync(scratch, stream);
            if (mips) cudaFreeAsync(mips, stream);
        }
        else if (scratch || mips)
        {
            cudaStreamSynchronize(stream);
            cudaFree(scratch);
            cudaFree(mips);
        }
    }
};

void logError(const char* what, cudaError_t e)
{
    fprintf(stderr, "LensCharacter: CUDA %s failed: %s\n", what, cudaGetErrorString(e));
}

} // namespace

// Renders one frame on `p_Stream` (device pointers, rows tightly packed). Asynchronous: returns
// once the work is queued. Returns 0 on success, else the CUDA error code.
int RunCudaKernel(void* p_Stream, const lc::LCParams& p_Params, const float* p_Input, float* p_Output)
{
    cudaStream_t stream = static_cast<cudaStream_t>(p_Stream);
    lc::LCParams P = p_Params;
    P.rowStride = P.width * 4;

    int device = 0;
    cudaGetDevice(&device);
    FrameBuffers fb(stream, poolFor(device));

    if (P.mipLevels > 0)
    {
        fb.mips = fb.alloc((size_t)P.mipFloats);
        if (!fb.mips) P.mipLevels = 0;   // out of memory: fall back to point samples
    }
    if (P.encoding != lc::ENC_LINEAR && P.decodeOnFetch == 0)
        fb.scratch = fb.alloc((size_t)P.width * P.height * 4);

    const float* src = p_Input;
    if (fb.scratch)
    {
        LensLinearizeKernel<<<gridFor(P.width, P.height), kBlock, 0, stream>>>(P, p_Input, fb.scratch);
        src = fb.scratch;
    }
    else
        P.decodeOnFetch = 1;

    // Launches on one stream run in order, so each level sees the one before it complete
    for (int k = 1; k <= P.mipLevels; k++)
        LensMipKernel<<<gridFor(P.mipW[k - 1], P.mipH[k - 1]), kBlock, 0, stream>>>(P, src, fb.mips, k);

    // mips is unread when P.mipLevels is 0; pass src so the pointer is never null
    LensCharacterKernel<<<gridFor(P.width, P.height), kBlock, 0, stream>>>(P, src, fb.mips ? fb.mips : src, p_Output);

    cudaError_t e = cudaGetLastError();
    if (e != cudaSuccess) { logError("launch", e); return (int)e; }
    return 0;
}

// Test hook for the self-test: run on device 0 with host buffers.
// Returns 0 on success, 2 when there is no usable CUDA device; p_GpuMs receives the GPU time.
int LensCharacterRunCudaTest(const lc::LCParams& p_Params, const float* p_In, float* p_Out, double* p_GpuMs)
{
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess || count == 0) { cudaGetLastError(); return 2; }
    static bool s_Printed = false;
    if (!s_Printed)
    {
        cudaDeviceProp prop;
        if (cudaGetDeviceProperties(&prop, 0) == cudaSuccess)
            printf("  CUDA device: %s (compute %d.%d)\n", prop.name, prop.major, prop.minor);
        s_Printed = true;
    }
    cudaSetDevice(0);

    const size_t bytes = (size_t)p_Params.width * p_Params.height * 4 * sizeof(float);
    float *in = nullptr, *out = nullptr;
    cudaStream_t stream = nullptr;
    cudaEvent_t start = nullptr, stop = nullptr;
    int rc = 1;
    if (cudaMalloc(&in, bytes) == cudaSuccess && cudaMalloc(&out, bytes) == cudaSuccess &&
        cudaStreamCreate(&stream) == cudaSuccess && cudaEventCreate(&start) == cudaSuccess &&
        cudaEventCreate(&stop) == cudaSuccess)
    {
        cudaMemcpy(in, p_In, bytes, cudaMemcpyHostToDevice);
        cudaEventRecord(start, stream);
        int err = RunCudaKernel(stream, p_Params, in, out);
        cudaEventRecord(stop, stream);
        cudaError_t e = cudaStreamSynchronize(stream);
        if (err == 0 && e == cudaSuccess)
        {
            float ms = 0.0f;
            cudaEventElapsedTime(&ms, start, stop);
            if (p_GpuMs) *p_GpuMs = ms;
            cudaMemcpy(p_Out, out, bytes, cudaMemcpyDeviceToHost);
            rc = 0;
        }
        else if (e != cudaSuccess)
            logError("render", e);
    }
    if (start) cudaEventDestroy(start);
    if (stop) cudaEventDestroy(stop);
    if (stream) cudaStreamDestroy(stream);
    cudaFree(in);
    cudaFree(out);
    return rc;
}
