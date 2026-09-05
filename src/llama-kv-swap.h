#pragma once

#include "llama.h"
#include "llama-cparams.h"
#include "llama-hparams.h"
#include "llama-mmap.h"
#include "llama-turboquant.h"
#include "ggml.h"

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <list>
#include <mutex>

// Block size in tokens for secondary cache swapping
constexpr uint32_t LLAMA_KV_SWAP_DEFAULT_BLOCK_SIZE = 32;

// Alignment for NVMe Direct I/O
constexpr size_t LLAMA_KV_SWAP_PAGE_ALIGNMENT = 4096;

enum class llama_kv_block_loc : uint8_t {
    HOT_VRAM = 0,
    WARM_RAM = 1,
    COLD_SSD = 2,
};

// I/O transfer engine type
enum class llama_kv_swap_engine : uint8_t {
    POSIX_ALIGNED = 0, // 4KB aligned direct file I/O
    PINNED_DMA    = 1, // Pinned host memory with async GPU DMA
    GPU_DIRECT    = 2, // Direct NVMe to VRAM DMA (GPUDirect Storage)
};

// Information about a mounted storage drive / device
struct llama_drive_info {
    std::string device_name; // Hardware model/device name (e.g. "BG6 KIOXIA 1024GB", "Samsung SSD 980 PRO")
    std::string device_node; // Block device node (e.g. "/dev/nvme0n1p2")
    std::string path;        // Mount path or drive root (e.g. "/mnt/nvme", "D:\\")
    std::string fs_type;     // Filesystem type (e.g. "ext4", "btrfs", "ntfs")
    uint64_t    total_bytes = 0; // Total storage in bytes
    uint64_t    free_bytes  = 0; // Available free storage in bytes
    bool        is_ssd      = true; // True if solid-state / NVMe drive
};

// Discover and list mounted storage drives on the system
std::vector<llama_drive_info> llama_kv_get_available_drives();

// Validate that a selected drive exists, is writable, and has enough free space
bool llama_kv_validate_drive(const std::string & drive_path, size_t required_bytes, std::string & out_resolved_path);

struct llama_kv_block_id {
    llama_seq_id seq_id = -1;
    llama_pos    pos_start = -1;
    uint32_t     n_tokens = 0;

    bool operator==(const llama_kv_block_id & other) const {
        return seq_id == other.seq_id && pos_start == other.pos_start;
    }
};

struct llama_kv_block_id_hash {
    size_t operator()(const llama_kv_block_id & k) const {
        return (std::hash<int32_t>()(k.seq_id) ^ (std::hash<int64_t>()(k.pos_start) << 1));
    }
};

// Compact token signature for on-demand conditional retrieval
struct llama_kv_block_signature {
    uint64_t bloom_filter = 0; // 64-bit Bloom filter of tokens in the block
    std::vector<llama_token> tokens; // Token IDs in this block (for exact matching)

    void add_token(llama_token token) {
        tokens.push_back(token);
        // Hash token into bloom filter
        const uint64_t h = std::hash<int32_t>()(token);
        bloom_filter |= (1ULL << (h % 64));
        bloom_filter |= (1ULL << ((h >> 6) % 64));
    }

    bool may_contain(llama_token token) const {
        const uint64_t h = std::hash<int32_t>()(token);
        const uint64_t mask = (1ULL << (h % 64)) | (1ULL << ((h >> 6) % 64));
        return (bloom_filter & mask) == mask;
    }

    float match_score(const llama_token * query, size_t n_query) const {
        if (tokens.empty() || n_query == 0) {
            return 0.0f;
        }
        // Focus match on the active query prompt tail (up to last 128 tokens)
        const size_t q_start = (n_query > 128) ? (n_query - 128) : 0;
        const size_t q_len   = n_query - q_start;

        // Build set of block tokens for O(1) lookup and unique matching
        std::unordered_set<llama_token> block_set(tokens.begin(), tokens.end());

        size_t matches = 0;
        for (size_t i = q_start; i < n_query; ++i) {
            if (may_contain(query[i])) {
                auto it = block_set.find(query[i]);
                if (it != block_set.end()) {
                    matches++;
                    block_set.erase(it);
                }
            }
        }
        return (float) matches / (float) std::max<size_t>(1, std::min<size_t>(tokens.size(), q_len));
    }
};

struct llama_kv_block_meta {
    llama_kv_block_id   id;
    llama_kv_block_loc  loc = llama_kv_block_loc::HOT_VRAM;
    uint32_t            cell_start = 0; // cell index in llama_kv_cells
    uint64_t            swap_slot = 0;  // slot index in swap file
    uint64_t            access_ts = 0;  // timestamp for LRU
    uint32_t            stream_id = 0;  // stream/device association
    void *              ram_ptr = nullptr;
    bool                dirty = false;
    llama_kv_block_signature signature;
};

// Storage backend for swapping blocks to disk
class llama_kv_swap_store {
public:
    llama_kv_swap_store(
            const std::string & path,
            size_t max_swap_size,
            size_t block_bytes,
            llama_kv_swap_engine engine = llama_kv_swap_engine::POSIX_ALIGNED);
    ~llama_kv_swap_store();

    bool write_block(uint64_t slot, const void * src, size_t size);
    bool read_block (uint64_t slot, void * dst, size_t size);

    bool write_block_direct(uint64_t slot, const void * dev_ptr, size_t size);
    bool read_block_direct (uint64_t slot, void * dev_ptr, size_t size);

    int64_t alloc_slot();
    void    free_slot(uint64_t slot);
    void    flush();

    size_t get_block_bytes() const { return block_bytes; }
    size_t get_total_slots() const { return n_slots; }
    size_t get_used_slots() const { return used_slots; }
    llama_kv_swap_engine get_engine() const { return engine; }
    const std::string & get_path() const { return path; }
    
    const std::vector<bool>& get_slot_bitmap() const { return slot_bitmap; }
    void set_slot_bitmap(const std::vector<bool>& bitmap, size_t used) {
        slot_bitmap = bitmap;
        used_slots = used;
    }


private:
    std::string path;
    size_t block_bytes = 0;
    size_t n_slots = 0;
    size_t used_slots = 0;
    llama_kv_swap_engine engine = llama_kv_swap_engine::POSIX_ALIGNED;

    FILE * fp = nullptr;
    int fd = -1;

    void * gds_handle = nullptr;

    std::vector<bool> slot_bitmap;

    void init_gds_if_available();
    void cleanup_gds();
};

// In-memory lightweight index for fast conditional recall without disk access
class llama_kv_block_index {
public:
    void index_block(const llama_kv_block_id & id, const llama_token * tokens, size_t n_tokens);
    void append_block_tokens(const llama_kv_block_id & id, const llama_token * tokens, size_t n_tokens);
    void remove_block(const llama_kv_block_id & id);
    std::vector<llama_kv_block_id> find_matching_blocks(
            llama_seq_id seq_id,
            const llama_token * query_tokens,
            size_t n_query,
            float score_threshold = 0.25f) const;

    const std::unordered_map<llama_kv_block_id, llama_kv_block_signature, llama_kv_block_id_hash>& get_signatures() const { return signatures; }
    void set_signatures(std::unordered_map<llama_kv_block_id, llama_kv_block_signature, llama_kv_block_id_hash> sigs) { signatures = std::move(sigs); }

    void remove_seq(llama_seq_id seq_id, llama_pos p0 = 0, llama_pos p1 = -1);
    void shift_seq(llama_seq_id seq_id, llama_pos p0, llama_pos p1, llama_pos delta);
    void div_seq(llama_seq_id seq_id, llama_pos p0, llama_pos p1, int d);
    void cp_seq(llama_seq_id seq_id_src, llama_seq_id seq_id_dst, llama_pos p0, llama_pos p1);

private:
    std::unordered_map<llama_kv_block_id, llama_kv_block_signature, llama_kv_block_id_hash> signatures;
};

struct llama_kv_cells;

// Manager for tiered KV cache eviction and on-demand swapping
class llama_kv_tiered_manager {
public:
    llama_kv_tiered_manager(
            const std::string & swap_path,
            size_t max_swap_size,
            size_t max_ram_size,
            uint32_t block_size,
            uint32_t kv_size,
            const llama_hparams & hparams,
            uint32_t n_layers,
            ggml_type type_k,
            ggml_type type_v,
            bool v_trans,
            llama_kv_swap_engine engine = llama_kv_swap_engine::PINNED_DMA,
            llama_tq_mode tq_mode_k = llama_tq_mode::TURBO4,
            llama_tq_mode tq_mode_v = llama_tq_mode::TURBO2,
            bool use_turboquant = true);

    ~llama_kv_tiered_manager();

    uint32_t get_block_size() const { return block_size; }
    size_t   get_block_bytes() const { return block_bytes; }
    llama_kv_swap_engine get_engine() const { return store ? store->get_engine() : engine; }
    bool is_turboquant_active() const { return use_turboquant; }
    llama_tq_mode get_tq_mode_k() const { return tq_mode_k; }
    llama_tq_mode get_tq_mode_v() const { return tq_mode_v; }

    // Register active block in VRAM/RAM with token content for indexing
    void register_block_tokens(
            llama_seq_id seq_id,
            llama_pos pos_start,
            const llama_token * tokens,
            uint32_t n_tokens,
            uint32_t cell_start,
            uint32_t stream_id);

    void touch_block(llama_seq_id seq_id, llama_pos pos_start);
    void touch_seq(llama_seq_id seq_id);

    // Evict least-recently used block to SSD (write once)
    bool evict_lru_block(
            std::vector<ggml_tensor *> & k_tensors,
            std::vector<ggml_tensor *> & v_tensors,
            uint32_t stream_id,
            llama_kv_block_meta & out_evicted,
            llama_seq_id keep_seq = -1,
            const llama_kv_cells * cells = nullptr);

    // Check if query needs cold SSD blocks.
    // If not needed, 0 disk operations occur (saves SSD IOPS and wear).
    std::vector<llama_kv_block_id> get_recallable_blocks(
            llama_seq_id seq_id,
            const llama_token * query_tokens,
            size_t n_query);

    // Get VRAM cell start for HOT_VRAM blocks (for zero-copy semantic RAG sharing)
    int32_t get_block_cell_start(const llama_kv_block_id & id, uint32_t stream_id);
         
    void reserve(size_t max_swap_size, size_t block_bytes);
    
    // Save/Load persistence state
    bool save_state(const std::string & meta_path) const;
    bool load_state(const std::string & meta_path);

    // Swap block back into memory
    bool swap_in_block(
            llama_seq_id seq_id,
            llama_pos pos_start,
            uint32_t cell_start,
            const std::vector<ggml_tensor *> & k_layers,
            const std::vector<ggml_tensor *> & v_layers,
            uint32_t stream_id);

    // Check if block is currently on SSD
    bool is_on_ssd(llama_seq_id seq_id, llama_pos pos_start) const;

    // Remove block records when sequence or range is cleared
    void remove_seq(llama_seq_id seq_id, llama_pos p0 = 0, llama_pos p1 = -1);

    // Shift block positions when sequence is shifted (e.g. infinite text generation)
    void shift_seq(llama_seq_id seq_id, llama_pos p0, llama_pos p1, llama_pos delta);
    void div_seq(llama_seq_id seq_id, llama_pos p0, llama_pos p1, int d);
    void cp_seq(llama_seq_id seq_id_src, llama_seq_id seq_id_dst, uint32_t stream_id_dst, llama_pos p0, llama_pos p1);

    // Metrics tracking
    uint64_t get_evictions_count() const { return n_evictions; }
    uint64_t get_swaps_triggered_count() const { return n_swaps_triggered; }
    uint64_t get_swaps_prevented_count() const { return n_swaps_prevented; }

private:
    uint32_t block_size = LLAMA_KV_SWAP_DEFAULT_BLOCK_SIZE;
    uint32_t kv_size = 0;
    size_t   max_ram_bytes = 10 * 1024 * 1024; // 1GB default
    size_t   used_ram_bytes = 0;
    size_t   block_bytes = 0;
    uint64_t current_ts = 0;
    llama_kv_swap_engine engine;

    const llama_hparams hparams;
    uint32_t n_layers;
    ggml_type type_k;
    ggml_type type_v;
    bool v_trans;

    llama_tq_mode tq_mode_k = llama_tq_mode::TURBO4;
    llama_tq_mode tq_mode_v = llama_tq_mode::TURBO2;
    bool use_turboquant = true;

    // Metrics
    uint64_t n_evictions = 0;
    uint64_t n_swaps_triggered = 0;
    uint64_t n_swaps_prevented = 0;

    std::unique_ptr<llama_kv_swap_store> store;
    llama_kv_block_index index;
    std::unordered_map<llama_seq_id, std::vector<llama_token>> seq_recent_tokens;

    // Aligned pinned I/O buffer
    void * io_buffer = nullptr;
    bool is_pinned = false;

    std::unordered_map<llama_kv_block_id, llama_kv_block_meta, llama_kv_block_id_hash> blocks;
    std::list<llama_kv_block_id> warm_ram_list;
    std::unordered_map<llama_kv_block_id, std::list<llama_kv_block_id>::iterator, llama_kv_block_id_hash> warm_ram_map;
    std::list<llama_kv_block_id> lru_list;
    std::unordered_map<llama_kv_block_id, std::list<llama_kv_block_id>::iterator, llama_kv_block_id_hash> lru_map;
    std::unordered_map<llama_seq_id, llama_kv_block_id> seq_active_block;

    mutable std::recursive_mutex mutex;

    size_t calculate_block_bytes() const;
    void allocate_staging_buffer();
    void free_staging_buffer();
};

