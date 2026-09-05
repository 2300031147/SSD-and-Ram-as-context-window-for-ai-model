#pragma once

#include "llama.h"
#include "ggml.h"

#include <cstdint>
#include <cstddef>
#include <vector>
#include <cmath>

enum class llama_tq_mode : uint8_t {
    TURBO4 = 4, // 4-bit PolarQuant (16 centroids, ~3.8x compression, highest quality)
    TURBO3 = 3, // 3-bit PolarQuant (8 centroids, ~4.6x compression)
    TURBO2 = 2, // 2-bit PolarQuant (4 centroids, ~6.4x compression, ideal for V cache)
};

// TurboQuant+ PolarQuant encoder/decoder with Fast Walsh-Hadamard Transform (FWHT)
class llama_turboquant {
public:
    // Compute optimal Lloyd-Max Gaussian centroids scaled by 1/sqrt(d)
    static std::vector<float> get_centroids(llama_tq_mode mode, uint32_t d);

    // Fast Walsh-Hadamard Transform in-place: O(d log d)
    static void fwht(float * data, uint32_t n);

    // Compress a single attention head vector x in R^d
    // Returns number of bytes written to dst_packed
    static size_t quantize_head(
            const float * src,
            uint32_t d,
            llama_tq_mode mode,
            uint8_t * dst_packed,
            float & out_norm);

    // Decompress a single attention head vector back to float32
    static void dequantize_head(
            const uint8_t * src_packed,
            uint32_t d,
            llama_tq_mode mode,
            float norm,
            float * dst);

    // Calculate compressed byte size for an attention head of dimension d
    static size_t get_head_packed_bytes(uint32_t d, llama_tq_mode mode);

    // Compress a full KV block of multiple tokens across attention heads
    static size_t compress_block(
            const float * k_src,
            const float * v_src,
            uint32_t n_tokens,
            uint32_t n_embd_k,
            uint32_t n_embd_v,
            llama_tq_mode mode_k,
            llama_tq_mode mode_v,
            uint8_t * dst_buffer);

    // Decompress a full KV block
    static void decompress_block(
            const uint8_t * src_buffer,
            uint32_t n_tokens,
            uint32_t n_embd_k,
            uint32_t n_embd_v,
            llama_tq_mode mode_k,
            llama_tq_mode mode_v,
            float * k_dst,
            float * v_dst);
};
