import re

with open("src/llama-kv-swap.cpp", "r") as f:
    content = f.read()

# Fix remove_seq
remove_orig = """        if (it->second.loc == llama_kv_block_loc::COLD_SSD) {
            store->free_slot(it->second.swap_slot);
        }

        auto evict_it = lru_map.find(it->first);
        if (evict_it != lru_map.end()) {
            lru_list.erase(evict_it->second);
            lru_map.erase(evict_it);
        }

        index.remove_block(it->first);

        it = blocks.erase(it);"""

remove_new = """        if (it->second.loc == llama_kv_block_loc::COLD_SSD) {
            store->free_slot(it->second.swap_slot);
        } else if (it->second.loc == llama_kv_block_loc::WARM_RAM) {
            if (it->second.ram_ptr) {
                free(it->second.ram_ptr);
                it->second.ram_ptr = nullptr;
                used_ram_bytes -= block_bytes;
            }
            auto warm_it = warm_ram_map.find(it->first);
            if (warm_it != warm_ram_map.end()) {
                warm_ram_list.erase(warm_it->second);
                warm_ram_map.erase(warm_it);
            }
        }

        auto evict_it = lru_map.find(it->first);
        if (evict_it != lru_map.end()) {
            lru_list.erase(evict_it->second);
            lru_map.erase(evict_it);
        }

        index.remove_block(it->first);

        it = blocks.erase(it);"""

if remove_orig in content:
    content = content.replace(remove_orig, remove_new)
    print("Patched remove_seq")
else:
    print("Could not find remove_orig in remove_seq")

with open("src/llama-kv-swap.cpp", "w") as f:
    f.write(content)
