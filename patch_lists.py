import re

with open("src/llama-kv-swap.cpp", "r") as f:
    content = f.read()

old_add = """    for (const auto & meta : to_add) {
        blocks[meta.id] = meta;
        if (meta.loc == llama_kv_block_loc::HOT_VRAM) {
            lru_list.push_front(meta.id);
            lru_map[meta.id] = lru_list.begin();
        }
    }"""

new_add = """    for (const auto & meta : to_add) {
        blocks[meta.id] = meta;
        if (meta.loc == llama_kv_block_loc::HOT_VRAM) {
            lru_list.push_front(meta.id);
            lru_map[meta.id] = lru_list.begin();
        } else if (meta.loc == llama_kv_block_loc::WARM_RAM) {
            warm_ram_list.push_front(meta.id);
            warm_ram_map[meta.id] = warm_ram_list.begin();
        }
    }"""

if old_add in content:
    content = content.replace(old_add, new_add)
    with open("src/llama-kv-swap.cpp", "w") as f:
        f.write(content)
    print("Patched cp_seq to add to warm_ram_list")
else:
    print("Could not find old_add")
