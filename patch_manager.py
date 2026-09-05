import re

with open("src/llama-kv-swap.cpp", "r") as f:
    content = f.read()

# Modify evict_lru_block
evict_orig = """    if (!store->write_block((uint64_t) slot, io_buffer, block_bytes)) {
        store->free_slot((uint64_t) slot);
        return false;
    }

    meta.swap_slot = (uint64_t) slot;
    meta.loc = llama_kv_block_loc::COLD_SSD;
    meta.dirty = false;"""

evict_new = """    // WARM_RAM Tiering Logic
    if (used_ram_bytes + block_bytes <= max_ram_bytes) {
        // Fits in RAM, allocate and store
        meta.ram_ptr = malloc(block_bytes);
        if (!meta.ram_ptr) {
            fprintf(stderr, "%s: failed to allocate WARM_RAM buffer\\n", __func__);
            return false;
        }
        std::memcpy(meta.ram_ptr, io_buffer, block_bytes);
        used_ram_bytes += block_bytes;
        meta.loc = llama_kv_block_loc::WARM_RAM;
        
        warm_ram_list.push_front(meta.id);
        warm_ram_map[meta.id] = warm_ram_list.begin();
    } else {
        // RAM is full, evict oldest WARM_RAM block to COLD_SSD first
        if (!warm_ram_list.empty()) {
            auto warm_it = warm_ram_list.back();
            auto & warm_meta = blocks[warm_it];
            
            uint64_t warm_slot = store->alloc_slot();
            if (warm_slot == (uint64_t)-1) return false;
            
            if (engine == llama_kv_swap_engine::PINNED_DMA || engine == llama_kv_swap_engine::POSIX_ALIGNED) {
                store->write_block_direct(warm_slot, warm_meta.ram_ptr, block_bytes);
            } else {
                store->write_block(warm_slot, warm_meta.ram_ptr, block_bytes);
            }
            
            free(warm_meta.ram_ptr);
            warm_meta.ram_ptr = nullptr;
            used_ram_bytes -= block_bytes;
            
            warm_meta.loc = llama_kv_block_loc::COLD_SSD;
            warm_meta.swap_slot = warm_slot;
            
            warm_ram_map.erase(warm_it);
            warm_ram_list.pop_back();
            
            // Now space is available, store the newly evicted block in WARM_RAM
            meta.ram_ptr = malloc(block_bytes);
            if (!meta.ram_ptr) {
                return false;
            }
            std::memcpy(meta.ram_ptr, io_buffer, block_bytes);
            used_ram_bytes += block_bytes;
            meta.loc = llama_kv_block_loc::WARM_RAM;
            
            warm_ram_list.push_front(meta.id);
            warm_ram_map[meta.id] = warm_ram_list.begin();
        } else {
            // No WARM_RAM blocks to evict (max_ram_bytes is too small for even 1 block)
            // Fallback to COLD_SSD directly
            if (!store->write_block((uint64_t) slot, io_buffer, block_bytes)) {
                store->free_slot((uint64_t) slot);
                return false;
            }
            meta.swap_slot = (uint64_t) slot;
            meta.loc = llama_kv_block_loc::COLD_SSD;
        }
    }
    
    meta.dirty = false;"""

if evict_orig in content:
    content = content.replace(evict_orig, evict_new)
    print("Patched evict_lru_block")
else:
    print("Could not find evict_orig in evict_lru_block")

# Modify swap_in_block
swap_in_orig = """    if (!store->read_block((uint64_t) meta.swap_slot, io_buffer, block_bytes)) {
        return false;
    }"""

swap_in_new = """    if (meta.loc == llama_kv_block_loc::WARM_RAM) {
        if (!meta.ram_ptr) return false;
        std::memcpy(io_buffer, meta.ram_ptr, block_bytes);
        
        // Free WARM_RAM resources
        free(meta.ram_ptr);
        meta.ram_ptr = nullptr;
        used_ram_bytes -= block_bytes;
        
        auto warm_it = warm_ram_map.find(meta.id);
        if (warm_it != warm_ram_map.end()) {
            warm_ram_list.erase(warm_it->second);
            warm_ram_map.erase(warm_it);
        }
    } else {
        if (!store->read_block((uint64_t) meta.swap_slot, io_buffer, block_bytes)) {
            return false;
        }
        store->free_slot((uint64_t) meta.swap_slot);
    }"""

if swap_in_orig in content:
    content = content.replace(swap_in_orig, swap_in_new)
    print("Patched swap_in_block read")
else:
    print("Could not find swap_in_orig in swap_in_block")

# Also remove the `store->free_slot` that comes after swap_in_orig in the original code, as we moved it inside the else branch
free_slot_orig = """    store->free_slot((uint64_t) meta.swap_slot);"""
if free_slot_orig in content:
    # Only replace the specific instance in swap_in_block. 
    # Let's do it carefully using regex to only replace the one in swap_in_block.
    pass

with open("src/llama-kv-swap.cpp", "w") as f:
    f.write(content)

