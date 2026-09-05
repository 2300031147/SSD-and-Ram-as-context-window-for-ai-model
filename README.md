# SSD as Secondary KV Cache for LLM Inference

> **This is a modified fork of [llama.cpp](https://github.com/ggml-org/llama.cpp).**
> The original project is developed and maintained by [ggml-org](https://github.com/ggml-org). All original code is licensed under the [MIT License](https://opensource.org/licenses/MIT).

## What This Project Does

This fork extends llama.cpp with a **tiered KV cache system** that uses an NVMe SSD as a secondary storage tier for the key-value cache. This allows running LLM inference with context lengths that exceed what fits in GPU VRAM or system RAM alone, by spilling cold KV cache blocks to fast SSD storage and fetching them back on demand.

### Why

On consumer hardware (e.g. a laptop with a 6 GB GPU), the KV cache for large context windows quickly exhausts available VRAM and RAM. By offloading cold (less recently accessed) KV cache blocks to an NVMe SSD and keeping hot blocks in RAM, we can serve longer conversations and more concurrent requests without upgrading hardware.

## Changes Made to llama.cpp

### New Files

| File | Description |
|---|---|
| `src/llama-kv-swap.h` | Header for the tiered KV cache manager (`llama_kv_tiered_manager`) |
| `src/llama-kv-swap.cpp` | Implementation of SSD-backed KV block eviction, fetching, and metadata persistence |

### Modified Files

| File | Change |
|---|---|
| `common/common.cpp` | Added auto-detection of `n_ctx` from KV swap capacity, capped to `n_ctx_train` to prevent OOM. Disabled GPU KV offload (`offload_kqv = false`) when SSD swap is active. Added SSD swap initialization after context creation. |
| `common/arg.cpp` | Added CLI arguments: `--kv-swap-size`, `--kv-swap-ram`, `--kv-swap-drive`, `--kv-swap-block-size`, `--kv-swap-engine`, `--turboquant-k`, `--turboquant-v` |
| `src/llama-kv-cache.cpp` | Integrated tiered manager hooks into KV cache find/evict paths |
| `src/llama-kv-cache.h` | Added tiered manager pointer and SSD swap enable/disable API |
| `src/llama-context.cpp` | Wired up `llama_memory_enable_ssd_swap()` API for runtime SSD swap toggling |
| `tools/server/server-context.cpp` | Added `/kv-swap/config` endpoint for dynamic KV swap configuration. Added `kv_swap_ram_size_gb` to `/props` response. Handle dynamic model reload with KV swap parameters. |
| `tools/server/server.cpp` | Routed `/kv-swap/config` POST/GET endpoints |
| `tools/server/server-task.h` | Added `kv_swap_action` struct for task-based KV swap configuration |
| `tools/server/server-task.cpp` | Parse KV swap config from JSON requests |
| `tools/ui/src/lib/constants/settings.constants.ts` | Added UI settings fields for KV swap (drive, SSD size, RAM size, block size, engine, turboquant) with dropdown selects |
| `tools/ui/src/lib/constants/settings-keys.constants.ts` | Added settings keys for all KV swap parameters |
| `tools/ui/src/lib/stores/settings/index.svelte.ts` | Added `syncKvSwapToServer` logic to persist KV swap config from UI to server |
| `tools/ui/src/lib/components/app/settings/SettingsChat/SettingsChat.svelte` | Added KV swap parameter passing during dynamic model reload via `extra_args` |

### Key Bug Fixes

- **OOM/SIGSEGV on context creation**: Auto-detected `n_ctx` from swap capacity could be 10x larger than the model's training context (e.g. 374k vs 32k tokens), causing GPU compute buffer allocation failures and crashes. Fixed by capping `n_ctx` to `n_ctx_train`.
- **GPU KV offload conflict**: When SSD swap is active, KV cache must stay on CPU RAM (not GPU VRAM) so the tiered manager can access it. Fixed by forcing `offload_kqv = false` when any swap path/drive/size is configured.

## Architecture

### Memory Layout (with SSD KV Swap active)

```
+--------------------------------------------+
|           GPU VRAM (~6 GB)                 |
|                                            |
|  +--------------------------------------+  |
|  |       Model Weights (~5.2 GB)        |  |
|  +--------------------------------------+  |
|  +--------------------------------------+  |
|  |     Compute Buffers (~0.15 GB)       |  |
|  +--------------------------------------+  |
|  |          Free (~0.65 GB)             |  |
|  +--------------------------------------+  |
+--------------------------------------------+
            |  attention ops only
            v
+--------------------------------------------+
|         System RAM (~15 GB)                |
|                                            |
|  +--------------------------------------+  |
|  |      OS + Applications (~5 GB)       |  |
|  +--------------------------------------+  |
|  +--------------------------------------+  |
|  |    KV Cache HOT Tier (in-RAM)        |  |
|  |    n_ctx = 32,768 tokens             |  |
|  |    ~1.8 GB (K + V, fp16)             |  |
|  +--------------------------------------+  |
|  |          Free (~8.5 GB)              |  |
|  +--------------------------------------+  |
+--------------------------------------------+
            |  evict cold / fetch hot
            v
+--------------------------------------------+
|        NVMe SSD (/mnt/data)                |
|                                            |
|  +--------------------------------------+  |
|  |    KV Cache COLD Tier (on-disk)      |  |
|  |    llama_kv_swap.bin (up to 20 GB)   |  |
|  +--------------------------------------+  |
|  +--------------------------------------+  |
|  |    Metadata: llama_kv_swap.meta      |  |
|  +--------------------------------------+  |
+--------------------------------------------+
```

### Data Flow During Inference

```
User Prompt
    |
    v
+-------------------+      +------------------+
| llama-server      |----->| GPU (Forward     |
| (decode request)  |      |  Pass/Attention) |
+-------------------+      +--------+---------+
                                    |
                              needs KV data
                                    |
                                    v
                      +-------------------------+
                      | KV Cache HOT Tier (RAM) |
                      |  32,768 token slots     |
                      +------------+------------+
                                   |
                     +-------------+-------------+
                     |                           |
               cache hit                   cache miss
                     |                           |
                     v                           v
              return KV data        +------------------------+
              to GPU for            | llama_kv_tiered_manager|
              attention             |                        |
                                    | 1. evict cold block    |
                                    |    RAM --> SSD          |
                                    | 2. fetch needed block  |
                                    |    SSD --> RAM          |
                                    +------------------------+
                                             |
                                             v
                                    +------------------+
                                    | NVMe SSD         |
                                    | llama_kv_swap.bin|
                                    +------------------+
```

### The OOM/SIGSEGV Crash (Before Fix)

```
n_ctx auto-detection (BROKEN):

  n_ctx = (kv_swap_size + kv_swap_ram) / bytes_per_token
        = (20 GB + 0.5 GB) / 57,344 bytes
        = 374,491 tokens    <-- FAR TOO LARGE

  Model n_ctx_train = 32,768 tokens

  Result:
    GPU tries to allocate compute buffers for 374k tokens
    --> cudaMalloc fails (only 0.8 GB free after model weights)
    --> SIGSEGV
```

### The Fix

```
n_ctx auto-detection (FIXED):

  n_ctx_auto = (20 GB + 0.5 GB) / 57,344 bytes = 374,491
  n_ctx_train = 32,768  (from model metadata)

  n_ctx_auto > n_ctx_train --> cap to 32,768 ✓

  Result:
    GPU compute buffers sized for 32k tokens (~0.15 GB) --> fits
    KV cache on CPU RAM (~1.8 GB) --> fits
    SSD provides overflow for concurrent slots, not larger context
    --> Server starts successfully ✓
```

---

*Below is the original llama.cpp README.*

---

# llama.cpp

![llama](https://raw.githubusercontent.com/ggml-org/llama.brand/refs/heads/master/cover/llama-cpp/cover-llama-cpp-dark.svg)

<div align="center">

<b>LLM inference in C/C++</b>

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](https://opensource.org/licenses/MIT)

</div>

## Quick start

A few options to get `llama.cpp` installed on your machine:

- Visit https://llama.app and follow the instructions
- Run with Docker - see our [Docker documentation](docs/docker.md)
- Download pre-built binaries from the [releases page](https://github.com/ggml-org/llama.cpp/releases)
- Build from source by cloning this repository - check out [our build guide](docs/build.md)

Once installed:

```sh
# Download and run a model directly from Hugging Face
llama cli -hf ggml-org/Qwen3.5-0.8B-GGUF

# Launch OpenAI-compatible API server
llama serve -hf ggml-org/Qwen3.5-0.8B-GGUF
```

<table align="center">
    <tr>
        <td align="center" width=50%>
            <img width="1310" height="888" alt="VLM session with `llama cli`" src="https://github.com/user-attachments/assets/88726b48-1713-48aa-a525-95a02e78afc4" />
            <i>VLM session with <b>llama cli</b></i>
        </td>
        <td align="center">
            <img width="1392" height="958" alt="Built-in web UI against `llama serve` running Qwen 3.6" src="https://github.com/user-attachments/assets/b402f972-2e32-4def-8771-8d849f08cf2e" />
            <i>Built-in web UI against <b>llama serve</b></i>
        </td>
    </tr>
<table>

## Description

The main goal of `llama.cpp` is to enable LLM (and VLM) inference with minimal setup and state-of-the-art performance on
a wide range of hardware - locally and in the cloud.

- Plain C/C++ implementation without any dependencies
- Apple silicon is a first-class citizen - optimized via ARM NEON, Accelerate and Metal frameworks
- AVX, AVX2, AVX512 and AMX support for x86 architectures
- RVV, ZVFH, ZFH, ZICBOP and ZIHINTPAUSE support for RISC-V architectures
- 1.5-bit, 2-bit, 3-bit, 4-bit, 5-bit, 6-bit, and 8-bit integer quantization for faster inference and reduced memory use
- Custom CUDA kernels for running LLMs on NVIDIA GPUs (support for AMD GPUs via HIP and Moore Threads GPUs via MUSA)
- Vulkan and SYCL backend support
- CPU+GPU hybrid inference to partially accelerate models larger than the total VRAM capacity

The `llama.cpp` project is build on top of the [ggml](https://github.com/ggml-org/ggml) library.

## Supported backends

| Backend | Target devices |
| --- | --- |
| [BLAS](docs/build.md#blas-build) | All |
| [BLIS](docs/backend/BLIS.md) | All |
| [CANN](docs/build.md#cann) | Ascend NPU |
| [CUDA](docs/build.md#cuda) | Nvidia GPU |
| [HIP](docs/build.md#hip) | AMD GPU |
| [Hexagon [In Progress]](docs/backend/snapdragon/README.md) | Snapdragon |
| [IBM zDNN](docs/backend/zDNN.md) | IBM Z & LinuxONE |
| [MUSA](docs/build.md#musa) | Moore Threads GPU |
| [Metal](docs/build.md#metal-build) | Apple Silicon |
| [OpenCL](docs/backend/OPENCL.md) | Adreno GPU |
| [OpenVINO [In Progress]](docs/backend/OPENVINO.md) | Intel CPUs, GPUs, and NPUs |
| [RPC](https://github.com/ggml-org/llama.cpp/tree/master/tools/rpc) | All |
| [SYCL](docs/backend/SYCL.md) | Intel GPU |
| [VirtGPU](docs/backend/VirtGPU.md) | VirtGPU APIR |
| [Vulkan](docs/build.md#vulkan) | GPU |
| [WebGPU](docs/build.md#webgpu) | All |
| [ZenDNN](docs/build.md#zendnn) | AMD CPU |

## Documentation

#### Tools

- [cli](tools/cli/README.md)
- [completion](tools/completion/README.md)
- [server](tools/server/README.md)
- [GBNF grammars](grammars/README.md)

#### Development

- [How to build](docs/build.md)
- [Running on Docker](docs/docker.md)
- [Build on Android](docs/android.md)
- [Multi-GPU usage](docs/multi-gpu.md)
- [Performance troubleshooting](docs/development/token_generation_performance_tips.md)
- [GGML tips & tricks](https://github.com/ggml-org/llama.cpp/wiki/GGML-Tips-&-Tricks)
- [XCFramework](docs/xcframework.md)
- [Completions](docs/completions.md)
- [Models](docs/models.md)
- [Release process](docs/release.md)

## Contributing

- Contributors can open PRs
- Collaborators will be invited based on contributions
- Maintainers can push to branches in the `llama.cpp` repo and merge PRs into the `master` branch
- Any help with managing issues, PRs and projects is very appreciated!
- Read the [CONTRIBUTING.md](CONTRIBUTING.md) for more information

## Acknowledgements

- [yhirose/cpp-httplib](https://github.com/yhirose/cpp-httplib) - Single-header HTTP server, used by `llama-server` - MIT license
- [nothings/stb](https://github.com/nothings/stb) - Single-header image format decoder, used by multimodal subsystem - Public domain
- [nlohmann/json](https://github.com/nlohmann/json) - Single-header JSON library, used by various tools/examples - MIT License
- [mackron/miniaudio](https://github.com/mackron/miniaudio) - Single-header audio format decoder, used by multimodal subsystem - Public domain
- [sheredom/subprocess.h](https://github.com/sheredom/subprocess.h) - Single-header process launching solution for C and C++ - Public domain
