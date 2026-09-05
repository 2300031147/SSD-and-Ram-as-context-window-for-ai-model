#include "llama-kv-swap.h"
#include "llama-impl.h"

#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cctype>
#include <algorithm>
#include <stdexcept>
#include <fstream>
#include <filesystem>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <io.h>
    #include <winioctl.h>
#elif defined(__APPLE__)
    #include <unistd.h>
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <sys/mman.h>
    #include <sys/param.h>
    #include <sys/ucred.h>
    #include <sys/mount.h>
#else
    #include <unistd.h>
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <sys/mman.h>
    #include <sys/statvfs.h>
    #include <mntent.h>
#endif

static std::string trim_string(const std::string & str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

#if defined(_WIN32)
static std::string get_windows_device_model(const std::string & drive_letter) {
    std::string vol = "\\\\.\\" + drive_letter;
    if (vol.back() == '\\') {
        vol.pop_back();
    }

    HANDLE h = CreateFileA(vol.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return "";
    fread(&us, sizeof(us), 1, fp);

    if (bb != store->get_block_bytes() || ts != store->get_total_slots()) {
        // Mismatch in store configuration, ignore saved state
        fclose(fp);
        return false;
    }

    std::vector<bool> bitmap(ts, false);
    for (size_t i = 0; i < ts; ++i) {
        uint8_t val = 0;
        fread(&val, sizeof(val), 1, fp);
        bitmap[i] = (val != 0);
    }
    store->set_slot_bitmap(bitmap, us);

    uint64_t n_blocks = 0;
    fread(&n_blocks, sizeof(n_blocks), 1, fp);
    blocks.clear();
    for (size_t i = 0; i < n_blocks; ++i) {
        llama_kv_block_id id;
        llama_kv_block_meta meta;
        fread(&id.seq_id, sizeof(id.seq_id), 1, fp);
        fread(&id.pos_start, sizeof(id.pos_start), 1, fp);
        fread(&id.n_tokens, sizeof(id.n_tokens), 1, fp);
        fread(&meta.loc, sizeof(meta.loc), 1, fp);
        fread(&meta.swap_slot, sizeof(meta.swap_slot), 1, fp);
        blocks[id] = meta;
    }

    uint64_t n_sigs = 0;
    fread(&n_sigs, sizeof(n_sigs), 1, fp);
    std::unordered_map<llama_kv_block_id, llama_kv_block_signature, llama_kv_block_id_hash> sigs;
    for (size_t i = 0; i < n_sigs; ++i) {
        llama_kv_block_id id;
        llama_kv_block_signature sig;
        fread(&id.seq_id, sizeof(id.seq_id), 1, fp);
        fread(&id.pos_start, sizeof(id.pos_start), 1, fp);
        fread(&id.n_tokens, sizeof(id.n_tokens), 1, fp);
        fread(&sig.bloom_filter, sizeof(sig.bloom_filter), 1, fp);
        uint32_t tsize = 0;
        fread(&tsize, sizeof(tsize), 1, fp);
        if (tsize > 0) {
            sig.tokens.resize(tsize);
            fread(sig.tokens.data(), sizeof(llama_token), tsize, fp);
        }
        sigs[id] = sig;
    }
    index.set_signatures(sigs);

    fclose(fp);
    return true;
}

    std::string disk = get_linux_parent_disk(dev_fsname);
    if (disk.empty()) {
        return "";
    }

    std::vector<std::string> paths = {
        "/sys/class/block/" + disk + "/device/model",
        "/sys/block/" + disk + "/device/model",
        "/sys/class/block/" + disk + "/device/name",
        "/sys/block/" + disk + "/device/name"
    };

    for (const auto & p : paths) {
        std::ifstream f(p);
        if (f.is_open()) {
            std::string model;
            std::getline(f, model);
            model = trim_string(model);
            if (!model.empty()) {
                return model;
            }
        }
    }
    return "";
}

static bool check_linux_is_ssd(const std::string & dev_fsname) {
    std::string disk = get_linux_parent_disk(dev_fsname);
    if (disk.empty()) {
        return true;
    }

    std::vector<std::string> paths = {
        "/sys/class/block/" + disk + "/queue/rotational",
        "/sys/block/" + disk + "/queue/rotational"
    };

    for (const auto & p : paths) {
        std::ifstream f(p);
        if (f.is_open()) {
            std::string val;
            std::getline(f, val);
            val = trim_string(val);
            if (val == "0") {
                return true;  // 0 = SSD / NVMe
            }
            if (val == "1") {
                return false; // 1 = Rotational HDD
            }
        }
    }
    return true;
}
#endif

std::vector<llama_drive_info> llama_kv_get_available_drives() {
    std::vector<llama_drive_info> drives;

#if defined(_WIN32)
    char drive_strings[256];
    DWORD len = GetLogicalDriveStringsA(sizeof(drive_strings), drive_strings);
    if (len > 0 && len < sizeof(drive_strings)) {
        char * p = drive_strings;
        while (*p) {
            llama_drive_info info;
            info.path = p;
            info.device_node = p;

            std::string model = get_windows_device_model(p);
            if (!model.empty()) {
                info.device_name = model + " (" + p + ")";
            } else {
                info.device_name = std::string("Local Disk (") + p + ")";
            }

            ULARGE_INTEGER free_bytes_avail, total_num_bytes, total_free_bytes;
            if (GetDiskFreeSpaceExA(p, &free_bytes_avail, &total_num_bytes, &total_free_bytes)) {
                info.total_bytes = total_num_bytes.QuadPart;
                info.free_bytes = free_bytes_avail.QuadPart;
#endif

    return drives;
}

bool llama_kv_validate_drive(const std::string & drive_path, size_t required_bytes, std::string & out_resolved_path) {
    if (drive_path.empty()) {
        LLAMA_LOG_ERROR("%s: Error: SSD swap drive path is empty\n", __func__);
        return false;
    }

    std::filesystem::path path(drive_path);
    std::error_code ec;

    if (!std::filesystem::exists(path, ec)) {
        if (!std::filesystem::create_directories(path, ec)) {
            LLAMA_LOG_ERROR("%s: Error: SSD swap drive path does not exist and could not be created: %s\n", __func__, drive_path.c_str());
            return false;
        }
    }

    if (!std::filesystem::is_directory(path, ec)) {
        LLAMA_LOG_ERROR("%s: Error: SSD swap path is not a directory: %s\n", __func__, drive_path.c_str());
        return false;
    }

    std::filesystem::path test_file = path / "llama_kv_cache";
    if (!std::filesystem::exists(test_file, ec)) {
        std::filesystem::create_directories(test_file, ec);
    }

    test_file /= "llama_kv_swap.bin";

    std::ofstream test_out(test_file, std::ios::binary);
    if (!test_out.is_open()) {
        LLAMA_LOG_ERROR("%s: Error: SSD swap path is not writable: %s\n", __func__, test_file.string().c_str());
        return false;
    }
    test_out.close();

    auto space = std::filesystem::space(path, ec);
    if (ec) {
        LLAMA_LOG_WARN("%s: Warning: Could not check free space on SSD drive: %s\n", __func__, drive_path.c_str());
    } else if (space.available < required_bytes) {
        LLAMA_LOG_ERROR("%s: Error: Not enough free space on SSD drive (Available: %zu MB, Required: %zu MB)\n", 
            __func__, space.available / (1024 * 1024), required_bytes / (1024 * 1024));
        return false;
    }

    out_resolved_path = test_file.string();
    return true;
}

                continue;
            }
            struct statvfs vfs;
            if (statvfs(ent->mnt_dir, &vfs) == 0 && vfs.f_blocks > 0) {
                llama_drive_info info;
                info.path = ent->mnt_dir;
                info.device_node = ent->mnt_fsname;
                info.fs_type = ent->mnt_type;
                info.total_bytes = (uint64_t) vfs.f_blocks * vfs.f_frsize;
                info.free_bytes = (uint64_t) vfs.f_bavail * vfs.f_frsize;
                info.is_ssd = check_linux_is_ssd(ent->mnt_fsname);

                std::string model = get_linux_device_model(ent->mnt_fsname);
                if (!model.empty()) {
                    info.device_name = model;
                } else {
                    info.device_name = ent->mnt_fsname;
                }

                // Avoid duplicate mount points for same device
                bool duplicate = false;
                for (const auto & existing : drives) {
                    if (existing.path == info.path || (existing.device_node == info.device_node && existing.path == "/")) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) {
                    drives.push_back(info);
                }
            }
        }
        endmntent(mtab);
    }

    if (drives.empty()) {
        struct statvfs vfs;
        if (statvfs("/tmp", &vfs) == 0) {
            llama_drive_info info;
            info.path = "/tmp";
            info.device_node = "/tmp";
            info.device_name = "System Temporary Storage (/tmp)";
            info.fs_type = "tmpfs";
            info.total_bytes = (uint64_t) vfs.f_blocks * vfs.f_frsize;
            info.free_bytes = (uint64_t) vfs.f_bavail * vfs.f_frsize;
            info.is_ssd = true;
            drives.push_back(info);
        }
    }
#endif

    return drives;
}

bool llama_kv_validate_drive(const std::string & drive_path, size_t required_bytes, std::string & out_resolved_path) {
    if (drive_path.empty()) {
        return false;
    }

    std::string candidate = drive_path;
#if defined(_WIN32)
    DWORD attrs = GetFileAttributesA(candidate.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        if (candidate.back() != '\\' && candidate.back() != '/') {
            candidate += "\\";
        }
        candidate += "llama_kv_cache\\llama_kv_swap.bin";
    }
    {
        std::error_code ec;
        std::filesystem::path p(candidate);
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path(), ec);
        }
    }

    FILE * test_f = ggml_fopen(candidate.c_str(), "a+b");
    if (test_f) {
        fclose(test_f);
    } else {
        return false;
    }

    ULARGE_INTEGER free_bytes_avail, total_num_bytes, total_free_bytes;
    if (!GetDiskFreeSpaceExA(drive_path.c_str(), &free_bytes_avail, &total_num_bytes, &total_free_bytes)) {
        const size_t last_slash = drive_path.find_last_of("/\\");
        const std::string parent_dir = (last_slash != std::string::npos && last_slash > 0) ? drive_path.substr(0, last_slash) : ".";
        if (GetDiskFreeSpaceExA(parent_dir.c_str(), &free_bytes_avail, &total_num_bytes, &total_free_bytes)) {
            if (required_bytes > 0 && free_bytes_avail.QuadPart < required_bytes) {
                return false;
            }
        }
    } else {
        if (required_bytes > 0 && free_bytes_avail.QuadPart < required_bytes) {
            return false;
        }
    }
#else
    struct stat st;
    if (stat(candidate.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        if (candidate == "/") {
            const char * home = getenv("HOME");
            if (home && home[0] != '\0') {
                candidate = std::string(home) + "/llama_kv_cache/llama_kv_swap.bin";
            } else {
                candidate = "./llama_kv_cache/llama_kv_swap.bin";
            }
        } else {
            if (candidate.back() != '/') {
                candidate += "/";
            }
            candidate += "llama_kv_cache/llama_kv_swap.bin";
        }
    }

    std::error_code ec;
    std::filesystem::path p(candidate);
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path(), ec);
        if (ec) {
            const char * home = getenv("HOME");
            if (home && home[0] != '\0') {
                candidate = std::string(home) + "/llama_kv_cache/llama_kv_swap.bin";
            } else {
                candidate = "./llama_kv_cache/llama_kv_swap.bin";
            }
            p = std::filesystem::path(candidate);
            ec.clear();
            std::filesystem::create_directories(p.parent_path(), ec);
        }
    }

    FILE * test_f = ggml_fopen(candidate.c_str(), "a+b");
    if (!test_f) {
        candidate = "./llama_kv_cache/llama_kv_swap.bin";
        p = std::filesystem::path(candidate);
        std::filesystem::create_directories(p.parent_path(), ec);
        test_f = ggml_fopen(candidate.c_str(), "a+b");
        if (!test_f) {
            return false;
        }
    }
    fclose(test_f);

    struct statvfs vfs;
    if (statvfs(drive_path.c_str(), &vfs) != 0) {
        const size_t last_slash = drive_path.find_last_of("/\\");
        const std::string parent_dir = (last_slash != std::string::npos && last_slash > 0) ? drive_path.substr(0, last_slash) : ".";
        if (statvfs(parent_dir.c_str(), &vfs) == 0) {
            const uint64_t free_b = (uint64_t) vfs.f_bavail * vfs.f_frsize;
            if (required_bytes > 0 && free_b < required_bytes) {
                return false;
            }
        }
    } else {
        const uint64_t free_b = (uint64_t) vfs.f_bavail * vfs.f_frsize;
        if (required_bytes > 0 && free_b < required_bytes) {
            return false;
        }
    }
#endif

    out_resolved_path = candidate;
    return true;
}

// Memory alignment allocation helper
static void * llama_kv_swap_memalign(size_t alignment, size_t size) {
#if defined(_WIN32)
    return _aligned_malloc(size, alignment);
#elif defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200112L
    void * ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) != 0) {
        return nullptr;
    }
    return ptr;
#else
    return malloc(size);
#endif
}

static void llama_kv_swap_memfree(void * ptr) {
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}

//
// llama_kv_swap_store
//

llama_kv_swap_store::llama_kv_swap_store(
        const std::string & path,
        size_t max_swap_size,
        size_t block_bytes,
        llama_kv_swap_engine engine)
    : path(path), block_bytes(block_bytes), engine(engine) {
    if (block_bytes == 0) {
        throw std::runtime_error("swap block size cannot be 0");
    }

    n_slots = max_swap_size / block_bytes;
    if (n_slots == 0) {
        n_slots = 1;
    }
    slot_bitmap.resize(n_slots, false);

    {
        std::error_code ec;
        std::filesystem::path p(path);
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path(), ec);
        }
    }

    fp = ggml_fopen(path.c_str(), "r+b");
    if (!fp) {
        fp = ggml_fopen(path.c_str(), "w+b");
    }
    if (!fp) {
        throw std::runtime_error(format("failed to open swap file %s: %s", path.c_str(), strerror(errno)));
    }

#if !defined(_WIN32)
    fd = fileno(fp);
#endif

    init_gds_if_available();
}

llama_kv_swap_store::~llama_kv_swap_store() {
    cleanup_gds();

    if (fp) {
        std::fclose(fp);
        fp = nullptr;
    }

    // Do NOT delete the swap file here. We want persistence across runs.
}

void llama_kv_swap_store::init_gds_if_available() {
    if (engine != llama_kv_swap_engine::GPU_DIRECT) {
        return;
    }

#if defined(__linux__) && defined(GGML_USE_CUDA)
    struct stat st;
    if (stat("/dev/nvidia-fs", &st) == 0 && fd != -1) {
        gds_handle = nullptr;
        return;
    }
#endif
    engine = llama_kv_swap_engine::PINNED_DMA;
}

void llama_kv_swap_store::cleanup_gds() {
    gds_handle = nullptr;
}

int64_t llama_kv_swap_store::alloc_slot() {
    for (size_t i = 0; i < n_slots; ++i) {
        if (!slot_bitmap[i]) {
            slot_bitmap[i] = true;
            used_slots++;
            return (int64_t) i;
        }
    }
    return -1;
}

void llama_kv_swap_store::free_slot(uint64_t slot) {
    if (slot < n_slots && slot_bitmap[slot]) {
        slot_bitmap[slot] = false;
        used_slots--;
    }
}

bool llama_kv_swap_store::write_block(uint64_t slot, const void * src, size_t size) {
    if (slot >= n_slots || size > block_bytes || !fp) {
        return false;
    }

    const size_t offset = slot * block_bytes;

#if defined(_WIN32)
    if (_fseeki64(fp, (int64_t) offset, SEEK_SET) != 0) {
        return false;
    }
#else
    if (fseeko(fp, (off_t) offset, SEEK_SET) != 0) {
        return false;
    }
#endif

    const size_t written = std::fwrite(src, 1, size, fp);
    if (written != size) {
        return false;
    }

    return true;
}

bool llama_kv_swap_store::read_block(uint64_t slot, void * dst, size_t size) {
    if (slot >= n_slots || size > block_bytes || !fp) {
        return false;
    }

    const size_t offset = slot * block_bytes;

#if defined(_WIN32)
    if (_fseeki64(fp, (int64_t) offset, SEEK_SET) != 0) {
        return false;
    }
#else
    if (fseeko(fp, (off_t) offset, SEEK_SET) != 0) {
        return false;
    }
#endif

    const size_t read_bytes = std::fread(dst, 1, size, fp);
    return read_bytes == size;
}

bool llama_kv_swap_store::write_block_direct(uint64_t slot, const void * dev_ptr, size_t size) {
    if (engine == llama_kv_swap_engine::GPU_DIRECT && gds_handle) {
        return true;
    }
    return write_block(slot, dev_ptr, size);
}

bool llama_kv_swap_store::read_block_direct(uint64_t slot, void * dev_ptr, size_t size) {
    if (engine == llama_kv_swap_engine::GPU_DIRECT && gds_handle) {
        return true;
    }
    return read_block(slot, dev_ptr, size);
}

//
// llama_kv_block_index
//

void llama_kv_block_index::index_block(const llama_kv_block_id & id, const llama_token * tokens, size_t n_tokens) {
    llama_kv_block_signature sig;
    for (size_t i = 0; i < n_tokens; ++i) {
        sig.add_token(tokens[i]);
    }
    signatures[id] = sig;
}

void llama_kv_block_index::remove_block(const llama_kv_block_id & id) {
    signatures.erase(id);
}

std::vector<llama_kv_block_id> llama_kv_block_index::find_matching_blocks(
        llama_seq_id seq_id,
        const llama_token * query_tokens,
        size_t n_query,
        float score_threshold) const {

    std::vector<std::pair<float, llama_kv_block_id>> ranked;

    for (const auto & kv : signatures) {
        if (kv.first.seq_id != seq_id) continue;

        const float score = kv.second.match_score(query_tokens, n_query);
        if (score >= score_threshold) {
            ranked.push_back({score, kv.first});
        }
    }

    std::sort(ranked.begin(), ranked.end(), [](const auto & a, const auto & b) {
        return a.first > b.first;
    });

    std::vector<llama_kv_block_id> result;
    result.reserve(ranked.size());
    for (const auto & r : ranked) {
        result.push_back(r.second);
    }
    return result;
}

void llama_kv_block_index::remove_seq(llama_seq_id seq_id, llama_pos p0, llama_pos p1) {
    for (auto it = signatures.begin(); it != signatures.end();) {
        const bool match_seq = (it->first.seq_id == seq_id || seq_id == -1);
        const llama_pos block_end = it->first.pos_start + (it->first.n_tokens > 0 ? (llama_pos) it->first.n_tokens : 32);
        const bool match_pos = (p1 < 0) ? (block_end > p0) : (it->first.pos_start < p1 && block_end > p0);
        if (match_seq && match_pos) {
            it = signatures.erase(it);
        } else {
            ++it;
        }
    }
}

void llama_kv_block_index::shift_seq(llama_seq_id seq_id, llama_pos p0, llama_pos p1, llama_pos delta) {
    std::vector<std::pair<llama_kv_block_id, llama_kv_block_signature>> to_shift;
    
    for (auto it = signatures.begin(); it != signatures.end();) {
        if (it->first.seq_id != seq_id) {
            ++it;
            continue;
        }
        
        const llama_pos block_end = it->first.pos_start + (it->first.n_tokens > 0 ? (llama_pos) it->first.n_tokens : 32);
        
        // Fully enclosed block -> shift
        if (it->first.pos_start >= p0 && (p1 < 0 || block_end <= p1)) {
            to_shift.push_back({it->first, it->second});
            it = signatures.erase(it);
        }
        // Fractured block -> delete
        else if ((p1 < 0 && block_end > p0) || (it->first.pos_start < p1 && block_end > p0)) {
            it = signatures.erase(it);
        } else {
            ++it;
        }
    }

    for (auto & pair : to_shift) {
        pair.first.pos_start += delta;
        signatures[pair.first] = pair.second;
    }
}

void llama_kv_block_index::cp_seq(llama_seq_id seq_id_src, llama_seq_id seq_id_dst, llama_pos p0, llama_pos p1) {
    std::vector<std::pair<llama_kv_block_id, llama_kv_block_signature>> to_copy;
    
    for (const auto & kv : signatures) {
        if (kv.first.seq_id != seq_id_src) {
            continue;
        }
        
        const llama_pos block_end = kv.first.pos_start + (kv.first.n_tokens > 0 ? (llama_pos) kv.first.n_tokens : 32);
        
        if (kv.first.pos_start >= p0 && (p1 < 0 || block_end <= p1)) {
            to_copy.push_back({kv.first, kv.second});
        }
    }
    
    for (auto & item : to_copy) {
        item.first.seq_id = seq_id_dst;
        signatures[item.first] = item.second;
    }
}

void llama_kv_block_index::div_seq(llama_seq_id seq_id, llama_pos p0, llama_pos p1, int d) {
    if (d == 1) return;
    std::vector<std::pair<llama_kv_block_id, llama_kv_block_signature>> to_div;
    
    for (auto it = signatures.begin(); it != signatures.end();) {
        if (it->first.seq_id != seq_id) {
            ++it;
            continue;
        }
        
        const llama_pos block_end = it->first.pos_start + (it->first.n_tokens > 0 ? (llama_pos) it->first.n_tokens : 32);
        
        // Fully enclosed block -> div
        if (it->first.pos_start >= p0 && (p1 < 0 || block_end <= p1)) {
            to_div.push_back({it->first, it->second});
            it = signatures.erase(it);
        }
        // Fractured block -> delete
        else if ((p1 < 0 && block_end > p0) || (it->first.pos_start < p1 && block_end > p0)) {
            it = signatures.erase(it);
        } else {
            ++it;
        }
    }

    for (auto & pair : to_div) {
        pair.first.pos_start /= d;
        signatures[pair.first] = pair.second;
    }
}


//
// llama_kv_tiered_manager
//

llama_kv_tiered_manager::llama_kv_tiered_manager(
        const std::string & swap_path,
        size_t max_swap_size,
        uint32_t block_size,
        uint32_t kv_size,
        const llama_hparams & hparams,
        uint32_t n_layers,
        ggml_type type_k,
        ggml_type type_v,
        bool v_trans,
        llama_kv_swap_engine engine,
        llama_tq_mode tq_mode_k,
        llama_tq_mode tq_mode_v,
        bool use_turboquant)
    : block_size(block_size), kv_size(kv_size), engine(engine), hparams(hparams), n_layers(n_layers),
      type_k(type_k), type_v(type_v), v_trans(v_trans),
      tq_mode_k(tq_mode_k), tq_mode_v(tq_mode_v), use_turboquant(use_turboquant) {

    block_bytes = calculate_block_bytes();

    store = std::make_unique<llama_kv_swap_store>(swap_path, max_swap_size, block_bytes, engine);
    allocate_staging_buffer();

    std::string meta_path = swap_path;
    if (meta_path.ends_with(".bin")) {
        meta_path.replace(meta_path.length() - 4, 4, ".meta");
    } else {
        meta_path += ".meta";
    }

    if (load_state(meta_path)) {
        fprintf(stderr, "%s: loaded SSD secondary cache state from %s (%zu blocks)\n", __func__, meta_path.c_str(), blocks.size());
    }
}

llama_kv_tiered_manager::~llama_kv_tiered_manager() {
    std::string meta_path = store ? store->get_path() : "";
    if (!meta_path.empty()) {
        if (meta_path.ends_with(".bin")) {
            meta_path.replace(meta_path.length() - 4, 4, ".meta");
        } else {
            meta_path += ".meta";
        }
        if (save_state(meta_path)) {
            fprintf(stderr, "%s: saved SSD secondary cache state to %s (%zu blocks)\n", __func__, meta_path.c_str(), blocks.size());
        }
    }

    free_staging_buffer();
}

void llama_kv_tiered_manager::allocate_staging_buffer() {
    io_buffer = llama_kv_swap_memalign(LLAMA_KV_SWAP_PAGE_ALIGNMENT, block_bytes);
    if (!io_buffer) {
        throw std::runtime_error("failed to allocate aligned swap I/O buffer");
    }

#if !defined(_WIN32)
    if (mlock(io_buffer, block_bytes) == 0) {
        is_pinned = true;
    }
#endif
}

void llama_kv_tiered_manager::free_staging_buffer() {
    if (io_buffer) {
#if !defined(_WIN32)
        if (is_pinned) {
            munlock(io_buffer, block_bytes);
            is_pinned = false;
        }
#endif
        llama_kv_swap_memfree(io_buffer);
        io_buffer = nullptr;
    }
}

size_t llama_kv_tiered_manager::calculate_block_bytes() const {
    size_t total = 0;
    for (uint32_t il = 0; il < n_layers; ++il) {
        const uint32_t n_embd_k_gqa = hparams.n_embd_k_gqa(il);
        const uint32_t n_embd_v_gqa = hparams.n_embd_v_gqa(il);

        if (use_turboquant) {
            const size_t k_packed = (llama_turboquant::get_head_packed_bytes(n_embd_k_gqa, tq_mode_k) + sizeof(float)) * block_size;
            const size_t v_packed = (llama_turboquant::get_head_packed_bytes(n_embd_v_gqa, tq_mode_v) + sizeof(float)) * block_size;
            total += k_packed + v_packed;
        } else {
            const size_t k_row_size = ggml_row_size(type_k, n_embd_k_gqa);
            const size_t v_row_size = ggml_row_size(type_v, n_embd_v_gqa);
            total += (k_row_size + v_row_size) * block_size;
        }
    }

    const size_t remainder = total % LLAMA_KV_SWAP_PAGE_ALIGNMENT;
    if (remainder != 0) {
        total += (LLAMA_KV_SWAP_PAGE_ALIGNMENT - remainder);
    }
    return total;
}

void llama_kv_tiered_manager::register_block_tokens(
        llama_seq_id seq_id,
        llama_pos pos_start,
        const llama_token * tokens,
        uint32_t n_tokens,
        uint32_t cell_start,
        uint32_t stream_id) {
        
    std::lock_guard<std::recursive_mutex> lock(mutex);

    if (tokens) {
        auto & tok_buf = seq_recent_tokens[seq_id];
        for (uint32_t i = 0; i < n_tokens; ++i) {
            tok_buf.push_back(tokens[i]);
        }
        if (tok_buf.size() > 256) {
            tok_buf.erase(tok_buf.begin(), tok_buf.end() - 256);
        }
    }

    uint32_t offset = 0;
    while (offset < n_tokens) {
        const uint32_t cur_cell = (kv_size > 0) ? ((cell_start + offset) % kv_size) : (cell_start + offset);
        const uint32_t tokens_to_boundary = (kv_size > 0) ? (kv_size - cur_cell) : block_size;
        const uint32_t max_chunk = std::min<uint32_t>(block_size, tokens_to_boundary);
        const uint32_t cur_chunk = std::min<uint32_t>(max_chunk, n_tokens - offset);

        llama_kv_block_id id{seq_id, pos_start + (llama_pos) offset, cur_chunk};

        auto it = blocks.find(id);
        if (it == blocks.end()) {
            llama_kv_block_meta meta;
            meta.id = id;
            meta.stream_id = stream_id;
            meta.loc = llama_kv_block_loc::HOT_VRAM;
            meta.cell_start = cur_cell;
            meta.access_ts = ++current_ts;
            meta.dirty = true;

            blocks[id] = meta;
            lru_list.push_front(id);
            lru_map[id] = lru_list.begin();
        } else {
            if (it->second.loc == llama_kv_block_loc::COLD_SSD) {
                store->free_slot(it->second.swap_slot);
            }
            
            if (it->second.id.n_tokens != cur_chunk) {
                blocks.erase(it);
                llama_kv_block_meta meta;
                meta.id = id;
                meta.stream_id = stream_id;
                meta.loc = llama_kv_block_loc::HOT_VRAM;
                meta.cell_start = cur_cell;
                meta.access_ts = ++current_ts;
                meta.dirty = true;
                blocks[id] = meta;
            } else {
                it->second.cell_start = cur_cell;
                it->second.stream_id = stream_id;
                it->second.loc = llama_kv_block_loc::HOT_VRAM;
                it->second.access_ts = ++current_ts;
                it->second.dirty = true;
            }

            auto lru_it = lru_map.find(id);
            if (lru_it != lru_map.end()) {
                lru_list.erase(lru_it->second);
            }
            lru_list.push_front(id);
            lru_map[id] = lru_list.begin();
        }

        if (tokens && cur_chunk > 0) {
            index.index_block(id, tokens + offset, cur_chunk);
        }

        offset += cur_chunk;
    }
}

void llama_kv_tiered_manager::touch_block(llama_seq_id seq_id, llama_pos pos_start) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    llama_kv_block_id id{seq_id, pos_start, 0};
    auto it = blocks.find(id);
    if (it == blocks.end() && block_size > 0) {
        const llama_pos aligned_pos = (pos_start / block_size) * block_size;
        it = blocks.find(llama_kv_block_id{seq_id, aligned_pos, 0});
    }
    if (it != blocks.end()) {
        it->second.access_ts = ++current_ts;
        auto lru_it = lru_map.find(it->first);
        if (lru_it != lru_map.end()) {
            lru_list.erase(lru_it->second);
        }
        lru_list.push_front(it->first);
        lru_map[it->first] = lru_list.begin();
    }
}

void llama_kv_tiered_manager::touch_seq(llama_seq_id seq_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    current_ts++;
    
    std::vector<std::pair<llama_kv_block_id, llama_kv_block_meta*>> seq_blocks;
    for (auto & pair : blocks) {
        if (pair.first.seq_id == seq_id) {
            seq_blocks.push_back({pair.first, &pair.second});
        }
    }
    
    // Sort by pos_start so we push oldest first (ends up as LRU), newest last (ends up as MRU)
    // We treat the system prompt (pos_start < 256) as extremely new, so it is pushed last and becomes absolute MRU.
    std::sort(seq_blocks.begin(), seq_blocks.end(), [](const auto & a, const auto & b) {
        bool a_is_system = a.first.pos_start < 256;
        bool b_is_system = b.first.pos_start < 256;
        if (a_is_system != b_is_system) {
            return b_is_system;
        }
        return a.first.pos_start < b.first.pos_start;
    });
    
    for (auto & item : seq_blocks) {
        item.second->access_ts = current_ts;
        auto lru_it = lru_map.find(item.first);
        if (lru_it != lru_map.end()) {
            lru_list.erase(lru_it->second);
            lru_list.push_front(item.first);
            lru_map[item.first] = lru_list.begin();
        }
    }
}

bool llama_kv_tiered_manager::is_on_ssd(llama_seq_id seq_id, llama_pos pos_start) const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    llama_kv_block_id id{seq_id, pos_start, 0};
    auto it = blocks.find(id);
    if (it != blocks.end()) {
        return it->second.loc == llama_kv_block_loc::COLD_SSD;
    }
    if (block_size > 0) {
        const llama_pos aligned_pos = (pos_start / block_size) * block_size;
        auto it_aligned = blocks.find(llama_kv_block_id{seq_id, aligned_pos, 0});
        if (it_aligned != blocks.end()) {
            return it_aligned->second.loc == llama_kv_block_loc::COLD_SSD;
        }
    }
    return false;
}

bool llama_kv_tiered_manager::evict_lru_block(
        std::vector<ggml_tensor *> & k_layers,
        std::vector<ggml_tensor *> & v_layers,
        uint32_t stream_id,
        llama_kv_block_meta & out_evicted,
        llama_seq_id keep_seq) {

    std::lock_guard<std::recursive_mutex> lock(mutex);

    if (lru_list.empty()) {
        return false;
    }

    auto it = lru_list.rbegin();
    auto fallback_it = lru_list.rend(); // if it's keep_seq or prompt
    auto hard_fallback_it = lru_list.rend(); // if it's both keep_seq AND prompt

    while (it != lru_list.rend()) {
        const auto & bid = *it;
        auto meta_it = blocks.find(bid);
        if (meta_it != blocks.end() && meta_it->second.loc == llama_kv_block_loc::HOT_VRAM && meta_it->second.stream_id == stream_id) {
            bool is_prompt = (bid.pos_start < 256);
            bool is_keep = (bid.seq_id == keep_seq && keep_seq != -1);

            if (is_prompt || is_keep) {
                if (is_prompt && is_keep) {
                    if (hard_fallback_it == lru_list.rend()) {
                        hard_fallback_it = it;
                    }
                } else {
                    if (fallback_it == lru_list.rend()) {
                        fallback_it = it;
                    }
                }
            } else {
                break;
            }
        }
        ++it;
    }

    if (it == lru_list.rend()) {
        it = fallback_it;
    }
    if (it == lru_list.rend()) {
        it = hard_fallback_it;
    }

    if (it == lru_list.rend()) {
        return false;
    }

    const llama_kv_block_id target_id = *it;
    auto & meta = blocks[target_id];

    int64_t slot = store->alloc_slot();
    if (slot < 0) {
        return false;
    }

    uint8_t * buf_ptr = reinterpret_cast<uint8_t *>(io_buffer);
    size_t buf_offset = 0;

    const uint32_t eff_cell = (meta.cell_start + meta.id.n_tokens <= kv_size) ? meta.cell_start : 0;

    for (uint32_t il = 0; il < n_layers && il < k_layers.size(); ++il) {
        ggml_tensor * k = k_layers[il];
        ggml_tensor * v = (il < v_layers.size()) ? v_layers[il] : nullptr;

        const uint32_t n_embd_k_gqa = hparams.n_embd_k_gqa(il);
        const size_t k_row_size = ggml_row_size(type_k, n_embd_k_gqa);
        const size_t k_block_size = meta.id.n_tokens * k_row_size;

        if (use_turboquant) {
            std::vector<uint8_t> raw_k(k_block_size, 0);
            if (k) {
                const size_t k_offset = eff_cell * k_row_size;
                if (k_offset + k_block_size <= ggml_nbytes(k)) {
                    ggml_backend_tensor_get(k, raw_k.data(), k_offset, k_block_size);
                }
            }
            std::vector<float> k_f32(meta.id.n_tokens * n_embd_k_gqa, 0.0f);
            if (type_k == GGML_TYPE_F32) {
                std::memcpy(k_f32.data(), raw_k.data(), k_f32.size() * sizeof(float));
            } else if (type_k == GGML_TYPE_F16) {
                ggml_fp16_to_fp32_row((const ggml_fp16_t *) raw_k.data(), k_f32.data(), k_f32.size());
            } else if (ggml_get_type_traits(type_k)->to_float) {
                ggml_get_type_traits(type_k)->to_float(raw_k.data(), k_f32.data(), k_f32.size());
            }

            const size_t k_packed_len = llama_turboquant::get_head_packed_bytes(n_embd_k_gqa, tq_mode_k);
            for (uint32_t t = 0; t < meta.id.n_tokens; ++t) {
                float norm = 0.0f;
                llama_turboquant::quantize_head(k_f32.data() + t * n_embd_k_gqa, n_embd_k_gqa, tq_mode_k, buf_ptr + buf_offset + sizeof(float), norm);
                std::memcpy(buf_ptr + buf_offset, &norm, sizeof(float));
                buf_offset += sizeof(float) + k_packed_len;
            }
        } else {
            if (k) {
                const size_t k_offset = eff_cell * k_row_size;
                if (k_offset + k_block_size <= ggml_nbytes(k)) {
                    ggml_backend_tensor_get(k, buf_ptr + buf_offset, k_offset, k_block_size);
                }
            }
            buf_offset += k_block_size;
        }

        const uint32_t n_embd_v_gqa = hparams.n_embd_v_gqa(il);
        const size_t v_row_size = ggml_row_size(type_v, n_embd_v_gqa);
        const size_t v_block_size = meta.id.n_tokens * v_row_size;

        if (use_turboquant) {
            std::vector<uint8_t> raw_v(v_block_size, 0);
            if (v) {
                if (!v_trans) {
                    const size_t v_offset = eff_cell * v_row_size;
                    if (v_offset + v_block_size <= ggml_nbytes(v)) {
                        ggml_backend_tensor_get(v, raw_v.data(), v_offset, v_block_size);
                    }
                } else {
                    const size_t v_stride = ggml_row_size(type_v, kv_size);
                    const size_t slice_size = ggml_row_size(type_v, meta.id.n_tokens);
                    for (uint32_t j = 0; j < n_embd_v_gqa; ++j) {
                        const size_t v_offset = j * v_stride + ggml_row_size(type_v, eff_cell);
                        if (v_offset + slice_size <= ggml_nbytes(v)) {
                            ggml_backend_tensor_get(v, raw_v.data() + j * slice_size, v_offset, slice_size);
                        }
                    }
                }
            }
            std::vector<float> v_f32(meta.id.n_tokens * n_embd_v_gqa, 0.0f);
            if (type_v == GGML_TYPE_F32) {
                std::memcpy(v_f32.data(), raw_v.data(), v_f32.size() * sizeof(float));
            } else if (type_v == GGML_TYPE_F16) {
                ggml_fp16_to_fp32_row((const ggml_fp16_t *) raw_v.data(), v_f32.data(), v_f32.size());
            } else if (ggml_get_type_traits(type_v)->to_float) {
                ggml_get_type_traits(type_v)->to_float(raw_v.data(), v_f32.data(), v_f32.size());
            }

            const size_t v_packed_len = llama_turboquant::get_head_packed_bytes(n_embd_v_gqa, tq_mode_v);
            for (uint32_t t = 0; t < meta.id.n_tokens; ++t) {
                float norm = 0.0f;
                llama_turboquant::quantize_head(v_f32.data() + t * n_embd_v_gqa, n_embd_v_gqa, tq_mode_v, buf_ptr + buf_offset + sizeof(float), norm);
                std::memcpy(buf_ptr + buf_offset, &norm, sizeof(float));
                buf_offset += sizeof(float) + v_packed_len;
            }
        } else {
            if (v) {
                if (!v_trans) {
                    const size_t v_offset = eff_cell * v_row_size;
                    if (v_offset + v_block_size <= ggml_nbytes(v)) {
                        ggml_backend_tensor_get(v, buf_ptr + buf_offset, v_offset, v_block_size);
                    }
                } else {
                    const size_t v_stride = ggml_row_size(type_v, kv_size);
                    const size_t slice_size = ggml_row_size(type_v, meta.id.n_tokens);
                    for (uint32_t j = 0; j < n_embd_v_gqa; ++j) {
                        const size_t v_offset = j * v_stride + ggml_row_size(type_v, eff_cell);
                        if (v_offset + slice_size <= ggml_nbytes(v)) {
                            ggml_backend_tensor_get(v, buf_ptr + buf_offset + j * slice_size, v_offset, slice_size);
                        }
                    }
                }
            }
            buf_offset += n_embd_v_gqa * ggml_row_size(type_v, meta.id.n_tokens);
        }
    }

    if (buf_offset < block_bytes) {
        memset(buf_ptr + buf_offset, 0, block_bytes - buf_offset);
    }

    if (!store->write_block((uint64_t) slot, io_buffer, block_bytes)) {
        store->free_slot((uint64_t) slot);
        return false;
    }

    meta.swap_slot = (uint64_t) slot;
    meta.loc = llama_kv_block_loc::COLD_SSD;
    meta.dirty = false;

    // remove from LRU to prevent unbounded list growth
    auto evict_lru_it = lru_map.find(target_id);
    if (evict_lru_it != lru_map.end()) {
        lru_list.erase(evict_lru_it->second);
        lru_map.erase(evict_lru_it);
    }

    n_evictions++;
    out_evicted = meta;
    return true;
}

std::vector<llama_kv_block_id> llama_kv_tiered_manager::get_recallable_blocks(
        llama_seq_id seq_id,
        const llama_token * query_tokens,
        size_t n_query) {

    std::lock_guard<std::recursive_mutex> lock(mutex);
    std::vector<llama_kv_block_id> recall_blocks;

    // Count how many blocks we can safely recall without evicting blocks of the same sequence.
    // Safe capacity = empty VRAM space (estimated) + blocks in VRAM belonging to OTHER sequences.
    // Since we don't have direct access to v_cells empty count here, we count the HOT_VRAM blocks
    // belonging to OTHER sequences. The actual swap_in_block will handle empty cells first.
    size_t safe_evictable_blocks = 0;
    size_t hot_vram_blocks = 0;
    for (auto it = lru_list.begin(); it != lru_list.end(); ++it) {
        if (blocks[*it].loc == llama_kv_block_loc::HOT_VRAM) {
            hot_vram_blocks++;
            if (it->seq_id != seq_id) {
                safe_evictable_blocks++;
            }
        }
    }

    // Calculate safe capacity to avoid cyclic evictions (thrashing).
    // Total blocks that fit in VRAM = kv_size / block_size
    size_t total_capacity = std::max<size_t>(1, kv_size / block_size);
    size_t empty_space = (total_capacity > hot_vram_blocks) ? (total_capacity - hot_vram_blocks) : 0;
    size_t safe_capacity = empty_space + safe_evictable_blocks;

    // Find all COLD_SSD blocks for the current sequence to restore context
    std::vector<llama_kv_block_id> seq_cold_blocks;
    for (const auto & pair : blocks) {
        if (pair.first.seq_id == seq_id && pair.second.loc == llama_kv_block_loc::COLD_SSD) {
            seq_cold_blocks.push_back(pair.first);
        }
    }

    std::sort(seq_cold_blocks.begin(), seq_cold_blocks.end(), [](const llama_kv_block_id & a, const llama_kv_block_id & b) {
        return a.pos_start > b.pos_start; // newest first
    });

    if (seq_cold_blocks.size() > safe_capacity) {
        seq_cold_blocks.resize(safe_capacity);
    }

    for (const auto & bid : seq_cold_blocks) {
        recall_blocks.push_back(bid);
    }

    // Additionally, if there are query tokens (prompt processing), we can still do semantic matching
    // for OTHER sequences (e.g. RAG or finding similar past chats).
    if (query_tokens && n_query >= 8) {
        auto matching_blocks = index.find_matching_blocks(seq_id, query_tokens, n_query, 0.15f);
        for (const auto & bid : matching_blocks) {
            auto it = blocks.find(bid);
            if (it != blocks.end() && it->second.loc == llama_kv_block_loc::COLD_SSD) {
                // Avoid duplicates
                if (std::find(recall_blocks.begin(), recall_blocks.end(), bid) == recall_blocks.end()) {
                    recall_blocks.push_back(bid);
                }
            }
        }
    }

    return recall_blocks;
}

bool llama_kv_tiered_manager::swap_in_block(
        llama_seq_id seq_id,
        llama_pos pos_start,
        uint32_t cell_start,
        const std::vector<ggml_tensor *> & k_layers,
        const std::vector<ggml_tensor *> & v_layers,
        uint32_t stream_id) {

    std::lock_guard<std::recursive_mutex> lock(mutex);

    llama_kv_block_id id{seq_id, pos_start, 0};
    auto it = blocks.find(id);
    if (it == blocks.end() && block_size > 0) {
        const llama_pos aligned_pos = (pos_start / block_size) * block_size;
        it = blocks.find(llama_kv_block_id{seq_id, aligned_pos, 0});
    }
    if (it == blocks.end() || it->second.loc != llama_kv_block_loc::COLD_SSD) {
        return false;
    }

    auto & meta = it->second;

    if (!store->read_block(meta.swap_slot, io_buffer, block_bytes)) {
        return false;
    }

    uint8_t * buf_ptr = reinterpret_cast<uint8_t *>(io_buffer);
    size_t buf_offset = 0;
    const uint32_t eff_cell = (cell_start + meta.id.n_tokens <= kv_size) ? cell_start : 0;

    for (uint32_t il = 0; il < n_layers && il < k_layers.size(); ++il) {
        ggml_tensor * k = k_layers[il];
        ggml_tensor * v = (il < v_layers.size()) ? v_layers[il] : nullptr;

        const uint32_t n_embd_k_gqa = hparams.n_embd_k_gqa(il);
        const size_t k_row_size = ggml_row_size(type_k, n_embd_k_gqa);
        const size_t k_block_size = meta.id.n_tokens * k_row_size;

        if (use_turboquant) {
            const size_t k_packed_len = llama_turboquant::get_head_packed_bytes(n_embd_k_gqa, tq_mode_k);
            std::vector<float> k_f32(meta.id.n_tokens * n_embd_k_gqa, 0.0f);
            for (uint32_t t = 0; t < meta.id.n_tokens; ++t) {
                float norm = 0.0f;
                std::memcpy(&norm, buf_ptr + buf_offset, sizeof(float));
                llama_turboquant::dequantize_head(buf_ptr + buf_offset + sizeof(float), n_embd_k_gqa, tq_mode_k, norm, k_f32.data() + t * n_embd_k_gqa);
                buf_offset += sizeof(float) + k_packed_len;
            }

            std::vector<uint8_t> raw_k(k_block_size, 0);
            if (type_k == GGML_TYPE_F32) {
                std::memcpy(raw_k.data(), k_f32.data(), raw_k.size());
            } else if (type_k == GGML_TYPE_F16) {
                ggml_fp32_to_fp16_row(k_f32.data(), (ggml_fp16_t *) raw_k.data(), k_f32.size());
            } else if (ggml_get_type_traits(type_k)->from_float_ref) {
                ggml_get_type_traits(type_k)->from_float_ref(k_f32.data(), raw_k.data(), k_f32.size());
            }

            if (k) {
                const size_t k_offset = eff_cell * k_row_size;
                if (k_offset + k_block_size <= ggml_nbytes(k)) {
                    ggml_backend_tensor_set(k, raw_k.data(), k_offset, k_block_size);
                }
            }
        } else {
            if (k) {
                const size_t k_offset = eff_cell * k_row_size;
                if (k_offset + k_block_size <= ggml_nbytes(k)) {
                    ggml_backend_tensor_set(k, buf_ptr + buf_offset, k_offset, k_block_size);
                }
            }
            buf_offset += k_block_size;
        }

        const uint32_t n_embd_v_gqa = hparams.n_embd_v_gqa(il);
        const size_t v_row_size = ggml_row_size(type_v, n_embd_v_gqa);
        const size_t v_block_size = meta.id.n_tokens * v_row_size;

        if (use_turboquant) {
            const size_t v_packed_len = llama_turboquant::get_head_packed_bytes(n_embd_v_gqa, tq_mode_v);
            std::vector<float> v_f32(meta.id.n_tokens * n_embd_v_gqa, 0.0f);
            for (uint32_t t = 0; t < meta.id.n_tokens; ++t) {
                float norm = 0.0f;
                std::memcpy(&norm, buf_ptr + buf_offset, sizeof(float));
                llama_turboquant::dequantize_head(buf_ptr + buf_offset + sizeof(float), n_embd_v_gqa, tq_mode_v, norm, v_f32.data() + t * n_embd_v_gqa);
                buf_offset += sizeof(float) + v_packed_len;
            }

            std::vector<uint8_t> raw_v(v_block_size, 0);
            if (type_v == GGML_TYPE_F32) {
                std::memcpy(raw_v.data(), v_f32.data(), raw_v.size());
            } else if (type_v == GGML_TYPE_F16) {
                ggml_fp32_to_fp16_row(v_f32.data(), (ggml_fp16_t *) raw_v.data(), v_f32.size());
            } else if (ggml_get_type_traits(type_v)->from_float_ref) {
                ggml_get_type_traits(type_v)->from_float_ref(v_f32.data(), raw_v.data(), v_f32.size());
            }

            if (v) {
                if (!v_trans) {
                    const size_t v_offset = eff_cell * v_row_size;
                    if (v_offset + v_block_size <= ggml_nbytes(v)) {
                        ggml_backend_tensor_set(v, raw_v.data(), v_offset, v_block_size);
                    }
                } else {
                    const size_t v_stride = ggml_row_size(type_v, kv_size);
                    const size_t slice_size = ggml_row_size(type_v, meta.id.n_tokens);
                    for (uint32_t j = 0; j < n_embd_v_gqa; ++j) {
                        const size_t v_offset = j * v_stride + ggml_row_size(type_v, eff_cell);
                        if (v_offset + slice_size <= ggml_nbytes(v)) {
                            ggml_backend_tensor_set(v, raw_v.data() + j * slice_size, v_offset, slice_size);
                        }
                    }
                }
            }
        } else {
            if (v) {
                if (!v_trans) {
                    const size_t v_offset = eff_cell * v_row_size;
                    if (v_offset + v_block_size <= ggml_nbytes(v)) {
                        ggml_backend_tensor_set(v, buf_ptr + buf_offset, v_offset, v_block_size);
                    }
                } else {
                    const size_t v_stride = ggml_row_size(type_v, kv_size);
                    const size_t slice_size = ggml_row_size(type_v, meta.id.n_tokens);
                    for (uint32_t j = 0; j < n_embd_v_gqa; ++j) {
                        const size_t v_offset = j * v_stride + ggml_row_size(type_v, eff_cell);
                        if (v_offset + slice_size <= ggml_nbytes(v)) {
                            ggml_backend_tensor_set(v, buf_ptr + buf_offset + j * slice_size, v_offset, slice_size);
                        }
                    }
                }
            }
            buf_offset += n_embd_v_gqa * ggml_row_size(type_v, meta.id.n_tokens);
        }
    }

    store->free_slot(meta.swap_slot);
    meta.loc = llama_kv_block_loc::HOT_VRAM;
    meta.cell_start = eff_cell;
    meta.stream_id = stream_id;
    meta.access_ts = ++current_ts;

    auto lru_it = lru_map.find(it->first);
    if (lru_it != lru_map.end()) {
        lru_list.erase(lru_it->second);
    }
    lru_list.push_front(it->first);
    lru_map[it->first] = lru_list.begin();

    return true;
}

void llama_kv_tiered_manager::remove_seq(llama_seq_id seq_id, llama_pos p0, llama_pos p1) {
    std::lock_guard<std::recursive_mutex> lock(mutex);

    if (p1 < 0) {
        p1 = std::numeric_limits<llama_pos>::max();
    }

    if (p0 == 0 && p1 == std::numeric_limits<llama_pos>::max()) {
        if (seq_id == -1) {
            seq_recent_tokens.clear();
        } else {
            seq_recent_tokens.erase(seq_id);
        }
    }

    if (seq_id != -1) {
        index.remove_seq(seq_id, p0, p1);
    }

    for (auto it = blocks.begin(); it != blocks.end();) {
        const bool match_seq = (it->first.seq_id == seq_id || seq_id == -1);
        const llama_pos block_end = it->first.pos_start + (it->first.n_tokens > 0 ? (llama_pos) it->first.n_tokens : (llama_pos) block_size);
        const bool match_pos = (it->first.pos_start < p1 && block_end > p0);
        if (match_seq && match_pos) {
            if (it->second.loc == llama_kv_block_loc::COLD_SSD) {
                // Preserve SSD blocks on a global clear for cross-chat memory
                if (seq_id == -1) {
                    ++it;
                    continue;
                }
                store->free_slot(it->second.swap_slot);
            }
            auto lru_it = lru_map.find(it->first);
            if (lru_it != lru_map.end()) {
                lru_list.erase(lru_it->second);
                lru_map.erase(lru_it);
            }
            if (seq_id == -1) {
                index.remove_block(it->first);
            }
            it = blocks.erase(it);
        } else {
            ++it;
        }
    }
}

void llama_kv_tiered_manager::shift_seq(llama_seq_id seq_id, llama_pos p0, llama_pos p1, llama_pos delta) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    index.shift_seq(seq_id, p0, p1, delta);

    std::vector<llama_kv_block_meta> to_shift;
    
    for (auto it = blocks.begin(); it != blocks.end();) {
        if (it->first.seq_id != seq_id) {
            ++it;
            continue;
        }
        
        const llama_pos block_end = it->first.pos_start + (it->first.n_tokens > 0 ? (llama_pos) it->first.n_tokens : (llama_pos) block_size);
        
        // Fully enclosed block -> shift
        if (it->first.pos_start >= p0 && (p1 < 0 || block_end <= p1)) {
            to_shift.push_back(it->second);
            
            auto lru_it = lru_map.find(it->first);
            if (lru_it != lru_map.end()) {
                lru_list.erase(lru_it->second);
                lru_map.erase(lru_it);
            }
            it = blocks.erase(it);
        }
        // Fractured block (straddles boundary) -> delete
        else if ((p1 < 0 && block_end > p0) || (it->first.pos_start < p1 && block_end > p0)) {
            if (it->second.loc == llama_kv_block_loc::COLD_SSD) {
                store->free_slot(it->second.swap_slot);
            }
            auto lru_it = lru_map.find(it->first);
            if (lru_it != lru_map.end()) {
                lru_list.erase(lru_it->second);
                lru_map.erase(lru_it);
            }
            it = blocks.erase(it);
        } else {
            ++it;
        }
    }

    for (auto & meta : to_shift) {
        meta.id.pos_start += delta;
        blocks[meta.id] = meta;
        if (meta.loc == llama_kv_block_loc::HOT_VRAM) {
            lru_list.push_front(meta.id);
            lru_map[meta.id] = lru_list.begin();
        }
    }
}

void llama_kv_tiered_manager::div_seq(llama_seq_id seq_id, llama_pos p0, llama_pos p1, int d) {
    if (d == 1) return;
    std::lock_guard<std::recursive_mutex> lock(mutex);
    index.div_seq(seq_id, p0, p1, d);

    std::vector<llama_kv_block_meta> to_div;
    
    for (auto it = blocks.begin(); it != blocks.end();) {
        if (it->first.seq_id != seq_id) {
            ++it;
            continue;
        }
        
        const llama_pos block_end = it->first.pos_start + (it->first.n_tokens > 0 ? (llama_pos) it->first.n_tokens : (llama_pos) block_size);
        
        // Fully enclosed block -> div
        if (it->first.pos_start >= p0 && (p1 < 0 || block_end <= p1)) {
            to_div.push_back(it->second);
            
            auto lru_it = lru_map.find(it->first);
            if (lru_it != lru_map.end()) {
                lru_list.erase(lru_it->second);
                lru_map.erase(lru_it);
            }
            it = blocks.erase(it);
        }
        // Fractured block -> delete
        else if ((p1 < 0 && block_end > p0) || (it->first.pos_start < p1 && block_end > p0)) {
            if (it->second.loc == llama_kv_block_loc::COLD_SSD) {
                store->free_slot(it->second.swap_slot);
            }
            auto lru_it = lru_map.find(it->first);
            if (lru_it != lru_map.end()) {
                lru_list.erase(lru_it->second);
                lru_map.erase(lru_it);
            }
            it = blocks.erase(it);
        } else {
            ++it;
        }
    }

    for (auto & meta : to_div) {
        meta.id.pos_start /= d;
        blocks[meta.id] = meta;
        if (meta.loc == llama_kv_block_loc::HOT_VRAM) {
            lru_list.push_front(meta.id);
            lru_map[meta.id] = lru_list.begin();
        }
    }
}

void llama_kv_tiered_manager::cp_seq(llama_seq_id seq_id_src, llama_seq_id seq_id_dst, uint32_t stream_id_dst, llama_pos p0, llama_pos p1) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (p1 < 0) {
        p1 = std::numeric_limits<llama_pos>::max();
    }
    
    std::vector<llama_kv_block_meta> to_add;
    
    for (auto it = blocks.begin(); it != blocks.end(); ++it) {
        if (it->first.seq_id != seq_id_src) continue;
        
        const llama_pos block_end = it->first.pos_start + (it->first.n_tokens > 0 ? (llama_pos) it->first.n_tokens : (llama_pos) block_size);
        
        if (it->first.pos_start >= p0 && block_end <= p1) {
            llama_kv_block_meta meta = it->second;
            meta.id.seq_id = seq_id_dst;
            meta.stream_id = stream_id_dst;
            
            if (meta.loc == llama_kv_block_loc::COLD_SSD) {
                // We must duplicate the SSD slot to avoid double-free
                int64_t new_slot = store->alloc_slot();
                if (new_slot >= 0) {
                    if (store->read_block(it->second.swap_slot, io_buffer, block_bytes)) {
                        if (store->write_block(new_slot, io_buffer, block_bytes)) {
                            meta.swap_slot = new_slot;
                        } else {
                            store->free_slot(new_slot);
                            continue; // failed to write
                        }
                    } else {
                        store->free_slot(new_slot);
                        continue; // failed to read
                    }
                } else {
                    continue; // no space
                }
            }
            meta.access_ts = ++current_ts;
            to_add.push_back(meta);
        }
    }
    
    for (const auto & meta : to_add) {
        blocks[meta.id] = meta;
        if (meta.loc == llama_kv_block_loc::HOT_VRAM) {
            lru_list.push_front(meta.id);
            lru_map[meta.id] = lru_list.begin();
        }
    }
    
    index.cp_seq(seq_id_src, seq_id_dst, p0, p1);
}
#include "llama-kv-swap.h"
#include <fstream>
#include <cstdint>
#include <cstdio>

#define KVSW_MAGIC 0x4B565357

bool llama_kv_tiered_manager::save_state(const std::string & meta_path) const {
    if (!store) return false;
    fwrite(&version, sizeof(version), 1, fp);

    uint32_t bb = store->get_block_bytes();
    uint64_t ts = store->get_total_slots();
    uint64_t us = store->get_used_slots();
    fwrite(&bb, sizeof(bb), 1, fp);
    fwrite(&ts, sizeof(ts), 1, fp);
    fwrite(&us, sizeof(us), 1, fp);

    const auto & bitmap = store->get_slot_bitmap();
    for (size_t i = 0; i < ts; ++i) {
        uint8_t val = bitmap[i] ? 1 : 0;
        fwrite(&val, sizeof(val), 1, fp);
    }

    std::vector<std::pair<llama_kv_block_id, llama_kv_block_meta>> blocks_to_save;
    for (const auto & kv : blocks) {
        if (kv.second.dirty) continue; // Cannot save dirty VRAM blocks without tensors
        blocks_to_save.push_back(kv);
    }

    uint64_t n_blocks = blocks_to_save.size();
    fwrite(&n_blocks, sizeof(n_blocks), 1, fp);
    for (const auto & kv : blocks_to_save) {
        fwrite(&kv.first.seq_id, sizeof(kv.first.seq_id), 1, fp);
        fwrite(&kv.first.pos_start, sizeof(kv.first.pos_start), 1, fp);
        fwrite(&kv.first.n_tokens, sizeof(kv.first.n_tokens), 1, fp);
        llama_kv_block_loc saved_loc = llama_kv_block_loc::COLD_SSD; // Always load as SSD
        fwrite(&saved_loc, sizeof(saved_loc), 1, fp);
        fwrite(&kv.second.swap_slot, sizeof(kv.second.swap_slot), 1, fp);
    }

    const auto & sigs = index.get_signatures();
    uint64_t n_sigs = sigs.size();
    fwrite(&n_sigs, sizeof(n_sigs), 1, fp);
    for (const auto & kv : sigs) {
        fwrite(&kv.first.seq_id, sizeof(kv.first.seq_id), 1, fp);
        fwrite(&kv.first.pos_start, sizeof(kv.first.pos_start), 1, fp);
        fwrite(&kv.first.n_tokens, sizeof(kv.first.n_tokens), 1, fp);
        fwrite(&kv.second.bloom_filter, sizeof(kv.second.bloom_filter), 1, fp);
        uint32_t tsize = kv.second.tokens.size();
        fwrite(&tsize, sizeof(tsize), 1, fp);
        if (tsize > 0) {
            fwrite(kv.second.tokens.data(), sizeof(llama_token), tsize, fp);
        }
    }

    fclose(fp);
    return true;
}
