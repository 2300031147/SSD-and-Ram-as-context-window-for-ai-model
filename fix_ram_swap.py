import re

with open("src/llama-kv-swap.cpp", "r") as f:
    content = f.read()

# Replace COLD_SSD with WARM_RAM in evict_lru_block
content = content.replace("out_evicted.loc = llama_kv_block_loc::COLD_SSD;", "out_evicted.loc = llama_kv_block_loc::WARM_RAM;")
# Wait, let's see how evict_lru_block sets the location

# Actually, the user wants: "vram to ram, ram to ssd, ssd to vram, and model will first check the ram for old context if not found then it will comes to ssd and vram over flow to ram, ram overflow to ssd"
# Currently the swap manager only swaps from VRAM (HOT_VRAM) to SSD (COLD_SSD). We need to implement WARM_RAM caching as a middle layer.

