# SSD as Secondary KV Cache for LLM Inference

> **This is a modified fork of [llama.cpp](https://github.com/ggml-org/llama.cpp).**
> The original project is developed and maintained by [ggml-org](https://github.com/ggml-org). All original code is licensed under the [MIT License](https://opensource.org/licenses/MIT).

## What This Project Does

This fork adds an experimental **tiered KV cache** to llama.cpp. It can move older key-value cache blocks from the active cache into a secondary RAM tier and then to SSD storage, and can restore eligible blocks when space is available. The usable context and performance depend on the model, configuration, and workload; SSD storage does not guarantee that all earlier context will be retained.

### Why

On consumer hardware, a large KV cache can consume substantial memory. The secondary tiers are intended to make additional cache storage available without keeping every block in the active cache. Measure response quality, latency, memory use, and disk activity on your own hardware before relying on this feature.

## Project Guide for Readers and AI Assistants

This repository is a C/C++ llama.cpp fork with a Svelte/TypeScript server UI and a bundled Python TurboQuant reference project. Its fork-specific goal is SSD-backed secondary storage for inference KV cache blocks. This is not a Python-only application or an operating-system swap manager; the KV swap mechanism does not offload model weights.

Start with [AGENTS.md](AGENTS.md) and [CONTRIBUTING.md](CONTRIBUTING.md). This README is a map of the current implementation, not a substitute for reading the code. When behavior and documentation disagree, check the source and reproduce the behavior before making claims or changes. The upstream llama.cpp README is preserved below the fork-specific sections.

| Task | Start here |
|---|---|
| Understand block storage, tiering, matching, and metadata | [`src/llama-kv-swap.h`](src/llama-kv-swap.h), [`src/llama-kv-swap.cpp`](src/llama-kv-swap.cpp) |
| Follow cache slot selection, eviction, and recall | [`src/llama-kv-cache.cpp`](src/llama-kv-cache.cpp), [`src/llama-kv-cache.h`](src/llama-kv-cache.h) |
| Follow cache enable/disable and memory types | [`src/llama-context.cpp`](src/llama-context.cpp), [`include/llama.h`](include/llama.h), [`src/llama-memory-recurrent.cpp`](src/llama-memory-recurrent.cpp) |
| Check CLI options, startup, and memory limits | [`common/arg.cpp`](common/arg.cpp), [`common/common.h`](common/common.h), [`common/common.cpp`](common/common.cpp) |
| Follow HTTP routes and model reload | [`tools/server/server.cpp`](tools/server/server.cpp), [`tools/server/server-context.cpp`](tools/server/server-context.cpp), [`tools/server/server-task.h`](tools/server/server-task.h) |
| Follow UI settings and requests | [`tools/ui/src/lib/constants/settings.constants.ts`](tools/ui/src/lib/constants/settings.constants.ts), [`tools/ui/src/lib/stores/settings/index.svelte.ts`](tools/ui/src/lib/stores/settings/index.svelte.ts) |
| Understand the context gauge | [`tools/ui/src/lib/stores/chat/context-stats.svelte.ts`](tools/ui/src/lib/stores/chat/context-stats.svelte.ts), [`tools/ui/src/lib/components/app/chat/ChatForm/ChatFormContextGauge/`](tools/ui/src/lib/components/app/chat/ChatForm/ChatFormContextGauge/) |
| Inspect swap-block compression | [`src/llama-turboquant.h`](src/llama-turboquant.h), [`src/llama-turboquant.cpp`](src/llama-turboquant.cpp), [`turboquant_plus-main/README.md`](turboquant_plus-main/README.md) |
| Build, use, or test the upstream server | [Build guide](docs/build.md), [server usage](tools/server/README.md), [server development](tools/server/README-dev.md), [`tests/`](tests/) |

The root-level `patch_*.py`, `fix_*.py`, and `recovered_llama-*` files are helper or recovery artifacts. The current implementation is under `src/`, `common/`, and `tools/`. In particular, `test_swap.py` only prints a message and is not an SSD swap regression test.

## Getting Started

Build the project using the [build guide](docs/build.md). For a basic CPU build:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
```

Choose a writable directory on an SSD with enough free space. Replace the example paths below with your model and SSD paths:

```sh
mkdir -p /path/to/ssd/llama-cache

./build/bin/llama-server \
    --model /path/to/model.gguf \
    --host 127.0.0.1 --port 8080 \
    --ctx-size 2048 \
    --kv-swap-path /path/to/ssd/llama-cache/swap.bin \
    --kv-swap-size 1G \
    --kv-swap-ram 256M
```

Open `http://127.0.0.1:8080` in a browser. The example uses a swap file path; alternatively, use `--kv-swap-drive /path/to/ssd` to select a directory and let the server resolve the swap file location. `--kv-swap-size` sets the maximum SSD swap size, and `--kv-swap-ram` sets the secondary RAM tier limit. Both accept suffixes such as `M` and `G`. A GPU-enabled build and GPU layer offload are optional; see the [build guide](docs/build.md).

The server logs `SSD secondary KV cache enabled` when initialization succeeds. Check for errors as well: if initialization fails, the server can continue without SSD swap. For a running server, inspect its reported configuration with:

```sh
curl http://127.0.0.1:8080/props
```

Look for `kv_swap_enabled`, `kv_swap_path`, `kv_swap_size_gb`, and `kv_swap_ram_size_gb`. These fields report configuration, not proof that a block was written to or read from the SSD.

### CLI options

| Option | Purpose | Environment variable |
|---|---|---|
| `--kv-swap-path PATH` | Use a specific swap file path. Empty by default. | `LLAMA_ARG_KV_SWAP_PATH` |
| `--kv-swap-drive DIR` | Validate a directory and use `DIR/llama_kv_cache/llama_kv_swap.bin`. | `LLAMA_ARG_KV_SWAP_DRIVE` |
| `--kv-swap-size SIZE` | Set the maximum SSD swap size. Default is zero. | `LLAMA_ARG_KV_SWAP_SIZE` |
| `--kv-swap-ram SIZE` | Set the maximum secondary RAM tier. The default is auto. | `LLAMA_ARG_KV_SWAP_RAM` |
| `--turboquant-k LEVEL` | Set swap-block key compression to `turbo2`, `turbo3`, `turbo4`, or `none`. | `LLAMA_ARG_TURBOQUANT_K` |
| `--turboquant-v LEVEL` | Set swap-block value compression to `turbo2`, `turbo3`, `turbo4`, or `none`. | `LLAMA_ARG_TURBOQUANT_V` |
| `--ctx-size N` | Set the active context size. `0` uses the model default; this fork also handles `-1` as a capacity-based estimate. | `LLAMA_ARG_CTX_SIZE` |

The size parser accepts `K`, `M`, and `G` suffixes, optionally followed by `B`; an unsuffixed value is interpreted as MiB. When both a drive and a path are set, the drive takes precedence during startup. The automatic RAM limit is capped using a system-memory estimate; the actual value can be lower than the requested one. See `./build/bin/llama-server --help` and `common/common.cpp` for the current behavior.

For the standard KV swap manager, the current default compression settings resolve to TurboQuant `turbo4` for keys and `turbo2` for values. Setting either compression option to `none` disables TurboQuant for the swap manager. The implementation uses both settings together; verify output quality on the target model and workload rather than assuming a given compression ratio or accuracy. The similarly named upstream cache-type options and the bundled Python reference project are separate configuration surfaces.

### Web UI

1. Open **Settings > KV Cache & SSD Storage**.
2. Enable **SSD Secondary Cache Offloading** and select a writable SSD storage drive.
3. Choose the secondary RAM and SSD cache capacities and select **Save settings**.
4. Check the server logs and `/props` after applying settings. Changing the configuration can reload the model.

The RAM capacity control may be hidden when the server reports no discrete GPU. If the desired drive is not listed, check that it is mounted and writable, or start the server with `--kv-swap-path` or `--kv-swap-drive`.

The UI stores settings in browser local storage and synchronizes swap settings with the server. In this checkout, UI initialization or saving settings can apply the browser's swap configuration and reload the model, including disabling swap if the UI setting is off. Check `/props` and server logs after opening the UI or saving settings; a CLI setting alone is not proof that the same configuration is still active later.

The UI also displays an SSD swap block-size field, but the current `/kv-swap/config` request does not send that value and the server task uses a block size of 32. Do not assume changing that UI field changes the running swap manager without checking the implementation and active configuration.

### Server API

The server provides `GET /health`, `GET /props`, `GET /drives` (also `/v1/drives`), and `POST /kv-swap/config` (also `/v1/kv-swap/config`). The server's OpenAI-compatible chat endpoints are documented in the [server README](tools/server/README.md).

To configure swap on a running local server, replace `/path/to/ssd` with a writable directory:

```sh
curl --request POST http://127.0.0.1:8080/kv-swap/config \
    --header 'Content-Type: application/json' \
    --data '{"enabled":true,"drive":"/path/to/ssd","size_gb":1,"ram_mb":256,"turboquant_k":"none","turboquant_v":"none"}'
```

`size_gb` uses GiB and `ram_mb` uses MiB. The server validates the directory, available space when it can be checked, and the requested values. A disabled request uses `{"enabled":false}`. Applying either request can reload a single-model server, so do not assume an in-progress conversation will survive a configuration change. Check the response, `/props`, and server logs; successful configuration is not a retrieval or performance test.

### How the tiers work

- When the active cache needs space, the manager can evict an eligible block into the secondary RAM tier.
- When that tier is full, an older RAM block can be written to SSD. If a block cannot fit in the RAM tier, the implementation has a direct-to-SSD fallback.
- The manager can restore eligible blocks from RAM or SSD when there is safe capacity. Recall is limited to avoid repeatedly evicting blocks from the same active sequence.

The active KV cache is configured for CPU memory when SSD swap is enabled; model layers and other computation may still use the GPU. With `--ctx-size -1`, this fork estimates context size from the configured swap and RAM capacities and caps the estimate to the model's training context when available. A larger swap allocation is not a guarantee of a proportionally larger usable context.

### Checking a run

In a local test, the server was started with a 512-token context, a 32 MB RAM tier, and a 64 MB SSD swap limit. The UI displayed 710 / 512 while the model was still generating, and 741 / 512 after it completed a response of 684 generated tokens. The UI counter combines prompt and output usage, so a value above 100% does not prove that the full conversation is simultaneously in the active cache or that an SSD read occurred. The test did not establish retrieval accuracy or measure SSD I/O.

For your own test, compare runs with the same model, prompt, and settings. Check server logs for initialization and errors, observe the response and timing, and use a suitable disk I/O monitor if you need evidence of SSD activity. Cache misses and disk access can increase latency.

## Changes Made to llama.cpp

### New Files

| File | Description |
|---|---|
| `src/llama-kv-swap.h` | Header for the tiered KV cache manager (`llama_kv_tiered_manager`) |
| `src/llama-kv-swap.cpp` | Implementation of SSD-backed KV block eviction, fetching, and metadata persistence |

### Modified Files

| File | Change |
|---|---|
| `common/common.cpp` | Estimates `n_ctx` from swap capacity and caps it to `n_ctx_train` when available. Disables GPU KV offload when swap is configured and initializes swap after context creation. |
| `common/arg.cpp` | Added CLI arguments for the swap path, drive, SSD size, RAM limit, and TurboQuant levels |
| `src/llama-kv-cache.cpp` | Integrated tiered manager hooks into KV cache find/evict paths |
| `src/llama-kv-cache.h` | Added tiered manager pointer and SSD swap enable/disable API |
| `src/llama-context.cpp` | Wired up `llama_memory_enable_ssd_swap()` API for runtime SSD swap toggling |
| `tools/server/server-context.cpp` | Added `/kv-swap/config` endpoint for dynamic KV swap configuration. Added `kv_swap_ram_size_gb` to `/props` response. Handle dynamic model reload with KV swap parameters. |
| `tools/server/server.cpp` | Routed the `/kv-swap/config` POST endpoint and drive-listing endpoints |
| `tools/server/server-task.h` | Added `kv_swap_action` struct for task-based KV swap configuration |
| `tools/server/server-task.cpp` | Serializes KV swap configuration results |
| `tools/ui/src/lib/constants/settings.constants.ts` | Added UI settings fields for KV swap, storage capacities, block size, and TurboQuant |
| `tools/ui/src/lib/constants/settings-keys.constants.ts` | Added settings keys for all KV swap parameters |
| `tools/ui/src/lib/stores/settings/index.svelte.ts` | Added `syncKvSwapToServer` logic to persist KV swap config from UI to server |
| `tools/ui/src/lib/components/app/settings/SettingsChat/SettingsChat.svelte` | Added KV swap parameter passing during dynamic model reload via `extra_args` |

### Context and Memory Safeguards

- The auto-detected `n_ctx` is capped to the model's training context when that value is available. This avoids one source of oversized context allocations but does not guarantee that a configuration fits available memory.
- The current startup code sets `offload_kqv = false` when a swap path, drive, or size is configured. Inspect the actual backend and memory configuration before making claims about physical placement or transfer speed.

## Architecture

The diagrams describe the standard attention KV cache path in this fork. They show intended control and data flow, not a guarantee that every request moves data through every tier. The recurrent and hybrid memory paths use additional logic described below.

### System overview

~~~mermaid
flowchart LR
    User["User / API client"] --> UI["Web UI\nSvelte / TypeScript"]
    User --> API["HTTP API\nllama-server"]
    UI -->|"settings, prompts"| API
    API --> Tasks["Server task queue\nconfiguration and generation"]
    Tasks --> Context["llama context and memory"]
    Context --> Model["Model inference\nCPU and optional GPU"]
    Context --> Cache["Active KV cache"]
    Cache --> Manager["Tiered KV manager"]
    Manager --> Warm["Secondary RAM\nWARM_RAM"]
    Manager --> Store["Swap store\nSSD file"]
    Store --> Meta["Metadata and token signatures\n.meta file"]
~~~

The CLI uses common/arg.cpp and common/common.cpp to configure the same memory API without going through the UI. The llama_memory_enable_ssd_swap() function in src/llama-context.cpp selects the relevant attention, recurrent, or hybrid memory implementation.

### Cache block lifecycle

~~~mermaid
flowchart TD
    A["Prompt and generation\nregister active KV blocks"] --> B{"Active cache\nneeds space?"}
    B -->|No| C["Continue inference"]
    B -->|Yes| D["Select an eligible LRU block"]
    D --> E{"Secondary RAM\nhas room?"}
    E -->|Yes| F["Store block in RAM"]
    E -->|"No, RAM has older blocks"| G["Write an older RAM block to SSD\nthen place the new block in RAM"]
    E -->|"No block fits in RAM"| H["Write the new block directly to SSD"]
    F --> I{"Eligible block selected\nfor recall?"}
    G --> I
    H --> I
    I -->|No| C
    I -->|Yes| J["Check safe active-cache capacity\nand find cells"]
    J --> K["Read from RAM or SSD\nand restore KV data"]
    K --> C
~~~

Blocks are identified by sequence and position and carry location, slot, token, and access metadata. The default block size is 32 tokens. Eviction uses LRU tracking, avoids shared cells when cell information is available, and uses fallback choices for protected or current-sequence blocks. Recall is bounded by estimated available capacity. These are implementation rules, not a promise that every past block will be recalled.

The enum name HOT_VRAM means the active tier in this implementation. It must not be taken as proof of physical GPU residency: common/common.cpp disables GPU KV offload when swap is configured. Model weights and compute can still use the GPU. Likewise, the presence of transfer-engine enum names is not proof of direct GPU-to-SSD transfer: the current direct read/write methods fall back to ordinary file I/O when a usable direct handle is unavailable.

### Settings and server control

~~~mermaid
sequenceDiagram
    participant U as User
    participant W as Web UI
    participant S as llama-server
    participant M as Memory / KV manager
    U->>W: Choose drive and cache capacities
    W->>S: POST /kv-swap/config
    S->>S: Validate path and sizes
    S->>M: Queue enable or disable task
    S->>S: Reload single-model context when applicable
    U->>S: GET /props
    S-->>U: Report current configuration
~~~

The UI maintains its own settings in browser local storage and sends configuration to the server. The /props response reports configured state; it is not a per-block activity trace. A single-model configuration change can reload the model and recalculate context size. Treat UI state, CLI arguments, and active server state as separate things to verify.

### Matching, persistence, and restart

~~~mermaid
flowchart LR
    Tokens["Block token IDs"] --> Signature["Token signature\nBloom filter + token list"]
    Signature --> Match["Candidate matching\nfor eligible queries"]
    RAM["Warm RAM blocks"] -->|"save / shutdown attempt"| Disk["Swap data file"]
    Disk --> Metadata["Metadata file\nslots, block records, signatures"]
    Metadata -->|"compatible state accepted"| Reload["Reload as untrusted\nprevious-session blocks"]
    Reload --> Match
~~~

The block index uses token overlap and a Bloom filter; it is not an embedding-based semantic search system. Same-sequence and cross-sequence candidates follow different paths in get_recallable_blocks(). The code marks blocks loaded from a previous session as untrusted for positional recall; content matching and capacity checks still apply.

A swap file ending in .bin uses a sibling .meta file; another filename gets .meta appended. During normal manager shutdown or state saving, the implementation attempts to write warm blocks to disk and save metadata. The data and metadata files are retained. The loader checks conditions including metadata magic, block size, slot count, and whether the data file is newer than the metadata. It does not establish full model identity or prove restored KV correctness. Use a separate swap path for each model and incompatible configuration, do not share it between running instances, and protect the files as they can contain cache data and token IDs. An interrupted shutdown or write failure may prevent safe reuse.

Recurrent memory has its own state save and load path in src/llama-memory-recurrent.cpp, using a `.recurrent.meta` file; hybrid models can use both recurrent and attention memory. Do not assume the attention-block lifecycle above fully describes every model architecture.

### Compression

~~~mermaid
flowchart LR
    KV["KV block from active cache"] --> Check{"TurboQuant enabled?"}
    Check -->|Yes| Encode["Quantize keys and values\nselected K and V modes"]
    Check -->|No| Raw["Store block without TurboQuant"]
    Encode --> Tier["Secondary RAM or SSD"]
    Raw --> Tier
    Tier --> Restore["Read block and restore\nwith decompression if enabled"]
~~~

The standard swap manager calls the C++ implementation in src/llama-turboquant.cpp when compression is enabled. turboquant_plus-main/ contains a separate Python reference and evaluation project with its own dependencies, benchmarks, and documentation. Results in that directory are not automatically results for this fork, machine, or configuration. Compression can change retrieval quality; evaluate it on the intended workload.

### Context sizing and observed limits

When --ctx-size -1 is used, common/common.cpp estimates bytes per token from model metadata and computes an initial context estimate from the configured SSD and RAM capacities. It caps that estimate to the model's training context when available. Backends and server slot setup can impose their own sizing and rounding. This is an allocation estimate, not proof of a supported effective context length or a guarantee against out-of-memory errors.

The UI gauge in tools/ui/src/lib/stores/chat/context-stats.svelte.ts derives usage from prompt and output token counts. A full or over-100% gauge is not a disk activity meter and does not prove the model retained every earlier token. Likewise, a swap-enabled log, a .bin file, or saved metadata alone is not evidence that a particular prompt caused an SSD read.

## Testing and Troubleshooting

Start with the existing [build guide](docs/build.md) and [server development guide](tools/server/README-dev.md). Inspect available tests before running a suitable subset:

~~~sh
ctest --test-dir build -N
ctest --test-dir build --output-on-failure
~~~

The second command can run the full configured test suite. test_swap.py is a placeholder, and no dedicated SSD-swap test was found under tests/ in this checkout. General llama.cpp tests or tests from turboquant_plus-main/ do not by themselves validate this fork's swap, persistence, or recall behavior.

For a meaningful feature test, record the model, build options, hardware, context size, swap and RAM limits, compression modes, and prompt. Compare with swap disabled using the same workload. Include cases for RAM eviction, SSD write and read, context recall accuracy, save/restart, insufficient disk space, configuration reload, and multi-sequence behavior as relevant. Measure latency, memory, disk I/O, and output quality separately. Do not infer a successful SSD read from a running model or a gauge alone.

| Symptom | What to check |
|---|---|
| Server starts but swap is not active | Check startup errors and /props; initialization can fail and continue without swap. |
| A CLI-enabled cache changes after opening the UI | The UI can synchronize its saved settings and reload the model. Check browser settings, logs, and /props. |
| Drive is missing or rejected | Check the mount, directory permissions, and free space. --kv-swap-path accepts a file path; --kv-swap-drive expects a directory. |
| Memory use or context size is unexpected | Check the active context, RAM cap, model size, GPU offload, --ctx-size, and model reload logs. |
| The gauge exceeds 100% | It is based on prompt and output counts. Check actual cache behavior and retrieval separately. |
| Repeated tool labels appear instead of an answer | Check whether built-in or other tools are enabled for the conversation; the UI can render tool calls as labels. Disable unnecessary tools for a plain-text test. |
| A saved cache is not loaded | Check file paths, metadata compatibility and timestamps, then use a fresh path for the model/configuration if needed. |

For local development, binding to 127.0.0.1 limits the server to the local machine. If exposing the server to a network, follow the authentication, deployment, and security guidance in the [server documentation](tools/server/README.md).

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
