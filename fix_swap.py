import re

with open("src/llama-kv-swap.cpp", "r") as f:
    content = f.read()

content = content.replace("fwrite(&KVSW_MAGIC, sizeof(version), 1, fp);", "uint32_t magic = KVSW_MAGIC; fwrite(&magic, sizeof(magic), 1, fp);")

with open("src/llama-kv-swap.cpp", "w") as f:
    f.write(content)
