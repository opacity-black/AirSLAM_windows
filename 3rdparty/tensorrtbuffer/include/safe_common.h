/*
 * SPDX-FileCopyrightText: Copyright (c) 1993-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef TENSORRT_SAFE_COMMON_H
#define TENSORRT_SAFE_COMMON_H

#include "cuda_runtime.h"
#include "NvInferRuntimeCommon.h"
#include <cstdlib>
#include <iostream>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>

// For safeLoadLibrary
#ifdef _MSC_VER
// Needed so that the max/min definitions in windows.h do not conflict with std::max/min.
#define NOMINMAX
#include <windows.h>
#undef NOMINMAX
#else
#include <dlfcn.h>
#endif

#undef CHECK
#define CHECK(status)                                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        auto ret = (status);                                                                                           \
        if (ret != 0)                                                                                                  \
        {                                                                                                              \
            std::cerr << "Cuda failure: " << ret << std::endl;                                                         \
            abort();                                                                                                   \
        }                                                                                                              \
    } while (0)

#undef SAFE_ASSERT
#define SAFE_ASSERT(condition)                                                   \
    do                                                                      \
    {                                                                       \
        if (!(condition))                                                   \
        {                                                                   \
            std::cerr << "Assertion failure: " << #condition << std::endl;  \
            abort();                                                        \
        }                                                                   \
    } while (0)

namespace tensorrt_buffer
{
    template <typename T>
    inline std::shared_ptr<T> infer_object(T* obj)
    {
        if (!obj)
        {
            throw std::runtime_error("Failed to create object");
        }
        return std::shared_ptr<T>(obj);
    }

    inline uint32_t elementSize(nvinfer1::DataType t)
    {
        switch (t)
        {
            case nvinfer1::DataType::kINT32:
            case nvinfer1::DataType::kFLOAT: return 4;
            case nvinfer1::DataType::kHALF: return 2;
            case nvinfer1::DataType::kINT8: return 1;
            case nvinfer1::DataType::kUINT8: return 1;
            case nvinfer1::DataType::kBOOL: return 1;
            case nvinfer1::DataType::kFP8: return 1;
        }
        return 0;
    }

    template <typename A, typename B>
    inline A divUp(A x, B n)
    {
        return (x + n - 1) / n;
    }

    inline int64_t volume(nvinfer1::Dims const& d)
    {
        return std::accumulate(d.d, d.d + d.nbDims, int64_t{1}, std::multiplies<int64_t>{});
    }

// Return m rounded up to nearest multiple of n
    template <typename T>
    inline T roundUp(T m, T n)
    {
        return ((m + n - 1) / n) * n;
    }

//! comps is the number of components in a vector. Ignored if vecDim < 0.
    inline int64_t volume(nvinfer1::Dims dims, int32_t vecDim, int32_t comps, int32_t batch)
    {
        if (vecDim >= 0)
        {
            dims.d[vecDim] = roundUp(dims.d[vecDim], comps);
        }
        return tensorrt_buffer::volume(dims) * std::max(batch, 1);
    }

//!
//! \class TrtCudaGraphSafe
//! \brief Managed CUDA graph
//!
    class TrtCudaGraphSafe
    {
    public:
        explicit TrtCudaGraphSafe() = default;

        TrtCudaGraphSafe(const TrtCudaGraphSafe&) = delete;

        TrtCudaGraphSafe& operator=(const TrtCudaGraphSafe&) = delete;

        TrtCudaGraphSafe(TrtCudaGraphSafe&&) = delete;

        TrtCudaGraphSafe& operator=(TrtCudaGraphSafe&&) = delete;

        ~TrtCudaGraphSafe()
        {
            if (mGraphExec)
            {
                cudaGraphExecDestroy(mGraphExec);
            }
        }

        void beginCapture(cudaStream_t& stream)
        {
            // cudaStreamCaptureModeGlobal is the only allowed mode in SAFE CUDA
            CHECK(cudaStreamBeginCapture(stream, cudaStreamCaptureModeGlobal));
        }

        bool launch(cudaStream_t& stream)
        {
            return cudaGraphLaunch(mGraphExec, stream) == cudaSuccess;
        }

        void endCapture(cudaStream_t& stream)
        {
            CHECK(cudaStreamEndCapture(stream, &mGraph));
            CHECK(cudaGraphInstantiate(&mGraphExec, mGraph, nullptr, nullptr, 0));
            CHECK(cudaGraphDestroy(mGraph));
        }

        void endCaptureOnError(cudaStream_t& stream)
        {
            // There are two possibilities why stream capture would fail:
            // (1) stream is in cudaErrorStreamCaptureInvalidated state.
            // (2) TRT reports a failure.
            // In case (1), the returning mGraph should be nullptr.
            // In case (2), the returning mGraph is not nullptr, but it should not be used.
            const auto ret = cudaStreamEndCapture(stream, &mGraph);
            if (ret == cudaErrorStreamCaptureInvalidated)
            {
                SAFE_ASSERT(mGraph == nullptr);
            }
            else
            {
                SAFE_ASSERT(ret == cudaSuccess);
                SAFE_ASSERT(mGraph != nullptr);
                CHECK(cudaGraphDestroy(mGraph));
                mGraph = nullptr;
            }
            // Clean up any CUDA error.
            cudaGetLastError();
            tensorrt_log::gLogError << "The CUDA graph capture on the stream has failed." << std::endl;
        }

    private:
        cudaGraph_t mGraph{};
        cudaGraphExec_t mGraphExec{};
    };

    inline void safeLoadLibrary(const std::string& path)
    {
#ifdef _MSC_VER
        void* handle = LoadLibrary(path.c_str());
#else
        int32_t flags{RTLD_LAZY};
        void* handle = dlopen(path.c_str(), flags);
#endif
        if (handle == nullptr)
        {
#ifdef _MSC_VER
            tensorrt_log::gLogError << "Could not load plugin library: " << path << std::endl;
#else
            tensorrt_log::gLogError << "Could not load plugin library: " << path << ", due to: " << dlerror() << std::endl;
#endif
        }
    }

    inline std::vector<std::string> safeSplitString(std::string str, char delimiter = ',')
    {
        std::vector<std::string> splitVect;
        std::stringstream ss(str);
        std::string substr;

        while (ss.good())
        {
            getline(ss, substr, delimiter);
            splitVect.emplace_back(std::move(substr));
        }
        return splitVect;
    }

} // namespace tensorrt_buffer

#endif // TENSORRT_SAFE_COMMON_H