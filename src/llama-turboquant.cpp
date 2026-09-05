#include "llama-turboquant.h"
#include <algorithm>
#include <cstring>
#include <cassert>

static uint32_t next_power_of_2(uint32_t n) {
    if (n == 0) return 1;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return n + 1;
}

std::vector<float> llama_turboquant::get_centroids(llama_tq_mode mode, uint32_t d) {
    const float scale = 1.0f / std::sqrt((float) d);

    if (mode == llama_tq_mode::TURBO2) {
        // 2-bit Lloyd-Max optimal Gaussian centroids
        return {
            -1.510f * scale,
            -0.453f * scale,
             0.453f * scale,
             1.510f * scale
        };
    } else if (mode == llama_tq_mode::TURBO3) {
        // 3-bit Lloyd-Max optimal Gaussian centroids
        return {
            -2.152f * scale,
            -1.344f * scale,
            -0.756f * scale,
            -0.245f * scale,
             0.245f * scale,
             0.756f * scale,
             1.344f * scale,
             2.152f * scale
        };
    } else {
        // 4-bit Lloyd-Max optimal Gaussian centroids (Turbo4)
        return {
            -2.733f * scale,
            -2.069f * scale,
            -1.618f * scale,
            -1.256f * scale,
            -0.942f * scale,
            -0.656f * scale,
            -0.388f * scale,
            -0.128f * scale,
             0.128f * scale,
             0.388f * scale,
             0.656f * scale,
             0.942f * scale,
             1.256f * scale,
             1.618f * scale,
             2.069f * scale,
             2.733f * scale
        };
    }
}

void llama_turboquant::fwht(float * data, uint32_t n) {
    // Fast Walsh-Hadamard Transform: in-place O(n log n)
    for (uint32_t len = 1; len < n; len <<= 1) {
        for (uint32_t i = 0; i < n; i += (len << 1)) {
            for (uint32_t j = 0; j < len; ++j) {
                const float u = data[i + j];
                const float v = data[i + len + j];
                data[i + j] = u + v;
                data[i + len + j] = u - v;
            }
        }
    }
    const float scale = 1.0f / std::sqrt((float) n);
    for (uint32_t i = 0; i < n; ++i) {
        data[i] *= scale;
    }
}

size_t llama_turboquant::get_head_packed_bytes(uint32_t d, llama_tq_mode mode) {
    const uint32_t pad_d = next_power_of_2(d);
    const uint32_t bits_per_val = (uint32_t) mode;
    return (pad_d * bits_per_val + 7) / 8;
}

size_t llama_turboquant::quantize_head(
        const float * src,
        uint32_t d,
        llama_tq_mode mode,
        uint8_t * dst_packed,
        float & out_norm) {

    // 1. Calculate L2 norm
    float sum_sq = 0.0f;
    for (uint32_t i = 0; i < d; ++i) {
        sum_sq += src[i] * src[i];
    }
    out_norm = std::sqrt(sum_sq);

    const uint32_t pad_d = next_power_of_2(d);
    const size_t packed_bytes = get_head_packed_bytes(d, mode);

    if (out_norm < 1e-12f) {
        std::memset(dst_packed, 0, packed_bytes);
        return packed_bytes;
    }

    const float inv_norm = 1.0f / out_norm;

    std::vector<float> temp(pad_d, 0.0f);
    for (uint32_t i = 0; i < d; ++i) {
        temp[i] = src[i] * inv_norm;
    }

    // 2. Rotate with Fast Walsh-Hadamard Transform
    fwht(temp.data(), pad_d);

    // 3. Scalar quantization with Lloyd-Max centroids
    const auto centroids = get_centroids(mode, pad_d);
    const size_t n_centroids = centroids.size();

    std::memset(dst_packed, 0, packed_bytes);

    if (mode == llama_tq_mode::TURBO4) {
        // 4-bit: 2 values per byte
        for (uint32_t i = 0; i < pad_d; ++i) {
            const float val = temp[i];
            uint8_t best_idx = 0;
            float min_dist = std::abs(val - centroids[0]);

            for (size_t c = 1; c < n_centroids; ++c) {
                const float dist = std::abs(val - centroids[c]);
                if (dist < min_dist) {
                    min_dist = dist;
                    best_idx = (uint8_t) c;
                }
            }

            if (i % 2 == 0) {
                dst_packed[i / 2] = (best_idx & 0x0F);
            } else {
                dst_packed[i / 2] |= ((best_idx & 0x0F) << 4);
            }
        }
    } else if (mode == llama_tq_mode::TURBO2) {
        // 2-bit: 4 values per byte
        for (uint32_t i = 0; i < pad_d; ++i) {
            const float val = temp[i];
            uint8_t best_idx = 0;
            float min_dist = std::abs(val - centroids[0]);

            for (size_t c = 1; c < n_centroids; ++c) {
                const float dist = std::abs(val - centroids[c]);
                if (dist < min_dist) {
                    min_dist = dist;
                    best_idx = (uint8_t) c;
                }
            }

            const uint32_t shift = (i % 4) * 2;
            dst_packed[i / 4] |= ((best_idx & 0x03) << shift);
        }
    } else {
        // 3-bit: pack sequentially
        uint32_t bit_pos = 0;
        for (uint32_t i = 0; i < pad_d; ++i) {
            const float val = temp[i];
            uint8_t best_idx = 0;
            float min_dist = std::abs(val - centroids[0]);

            for (size_t c = 1; c < n_centroids; ++c) {
                const float dist = std::abs(val - centroids[c]);
                if (dist < min_dist) {
                    min_dist = dist;
                    best_idx = (uint8_t) c;
                }
            }

            for (uint32_t b = 0; b < 3; ++b) {
                if ((best_idx >> b) & 1) {
                    dst_packed[(bit_pos + b) / 8] |= (1 << ((bit_pos + b) % 8));
                }
            }
            bit_pos += 3;
        }
    }

    return packed_bytes;
}

void llama_turboquant::dequantize_head(
        const uint8_t * src_packed,
        uint32_t d,
        llama_tq_mode mode,
        float norm,
        float * dst) {

    if (norm < 1e-12f) {
        std::memset(dst, 0, d * sizeof(float));
        return;
    }

    const uint32_t pad_d = next_power_of_2(d);
    const auto centroids = get_centroids(mode, pad_d);

    std::vector<float> temp(pad_d, 0.0f);

    if (mode == llama_tq_mode::TURBO4) {
        for (uint32_t i = 0; i < pad_d; ++i) {
            uint8_t idx = 0;
            if (i % 2 == 0) {
                idx = src_packed[i / 2] & 0x0F;
            } else {
                idx = (src_packed[i / 2] >> 4) & 0x0F;
            }
            temp[i] = centroids[idx];
        }
    } else if (mode == llama_tq_mode::TURBO2) {
        for (uint32_t i = 0; i < pad_d; ++i) {
            const uint32_t shift = (i % 4) * 2;
            const uint8_t idx = (src_packed[i / 4] >> shift) & 0x03;
            temp[i] = centroids[idx];
        }
    } else {
        uint32_t bit_pos = 0;
        for (uint32_t i = 0; i < pad_d; ++i) {
            uint8_t idx = 0;
            for (uint32_t b = 0; b < 3; ++b) {
                if ((src_packed[(bit_pos + b) / 8] >> ((bit_pos + b) % 8)) & 1) {
                    idx |= (1 << b);
                }
            }
            temp[i] = centroids[idx];
            bit_pos += 3;
        }
    }

    // Unit norm correction before inverse FWHT (PolarQuant norm recovery)
    float y_norm = 0.0f;
    for (uint32_t i = 0; i < pad_d; ++i) {
        y_norm += temp[i] * temp[i];
    }
    y_norm = std::sqrt(y_norm);
    if (y_norm > 1e-10f) {
        const float inv_y = 1.0f / y_norm;
        for (uint32_t i = 0; i < pad_d; ++i) {
            temp[i] *= inv_y;
        }
    }

    // Inverse Fast Walsh-Hadamard Transform (FWHT is self-inverse when normalized)
    fwht(temp.data(), pad_d);

    // Rescale by original L2 norm
    for (uint32_t i = 0; i < d; ++i) {
        dst[i] = temp[i] * norm;
    }
}

size_t llama_turboquant::compress_block(
        const float * k_src,
        const float * v_src,
        uint32_t n_tokens,
        uint32_t n_embd_k,
        uint32_t n_embd_v,
        llama_tq_mode mode_k,
        llama_tq_mode mode_v,
        uint8_t * dst_buffer) {

    size_t offset = 0;

    const size_t k_packed_len = get_head_packed_bytes(n_embd_k, mode_k);
    const size_t v_packed_len = get_head_packed_bytes(n_embd_v, mode_v);

    for (uint32_t t = 0; t < n_tokens; ++t) {
        float k_norm = 0.0f;
        float v_norm = 0.0f;

        // Write K norm
        std::memcpy(dst_buffer + offset, &k_norm, sizeof(k_norm)); // placeholder
        size_t k_norm_pos = offset;
        offset += sizeof(float);

        // Write K packed indices
        quantize_head(k_src + t * n_embd_k, n_embd_k, mode_k, dst_buffer + offset, k_norm);
        std::memcpy(dst_buffer + k_norm_pos, &k_norm, sizeof(k_norm));
        offset += k_packed_len;

        // Write V norm
        std::memcpy(dst_buffer + offset, &v_norm, sizeof(v_norm)); // placeholder
        size_t v_norm_pos = offset;
        offset += sizeof(float);

        // Write V packed indices
        quantize_head(v_src + t * n_embd_v, n_embd_v, mode_v, dst_buffer + offset, v_norm);
        std::memcpy(dst_buffer + v_norm_pos, &v_norm, sizeof(v_norm));
        offset += v_packed_len;
    }

    return offset;
}

void llama_turboquant::decompress_block(
        const uint8_t * src_buffer,
        uint32_t n_tokens,
        uint32_t n_embd_k,
        uint32_t n_embd_v,
        llama_tq_mode mode_k,
        llama_tq_mode mode_v,
        float * k_dst,
        float * v_dst) {

    size_t offset = 0;

    const size_t k_packed_len = get_head_packed_bytes(n_embd_k, mode_k);
    const size_t v_packed_len = get_head_packed_bytes(n_embd_v, mode_v);

    for (uint32_t t = 0; t < n_tokens; ++t) {
        float k_norm = 0.0f;
        std::memcpy(&k_norm, src_buffer + offset, sizeof(float));
        offset += sizeof(float);

        dequantize_head(src_buffer + offset, n_embd_k, mode_k, k_norm, k_dst + t * n_embd_k);
        offset += k_packed_len;

        float v_norm = 0.0f;
        std::memcpy(&v_norm, src_buffer + offset, sizeof(float));
        offset += sizeof(float);

        dequantize_head(src_buffer + offset, n_embd_v, mode_v, v_norm, v_dst + t * n_embd_v);
        offset += v_packed_len;
    }
}
