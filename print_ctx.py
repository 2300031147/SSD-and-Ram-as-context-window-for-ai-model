import struct
import json
import subprocess

out = subprocess.check_output(["./build/bin/llama-gguf-split", "--merge", "models/qwen2.5-7b-instruct-q5_k_m.gguf", "dummy.gguf", "--dry-run"], stderr=subprocess.STDOUT, text=True)
for line in out.splitlines():
    if "context_length" in line or "n_ctx" in line:
        print(line)
