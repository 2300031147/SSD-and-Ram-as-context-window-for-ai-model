import json

def reconstruct(path, out_name):
    lines_by_num = {}
    with open("/home/ravuririthesh/.gemini/antigravity-ide/brain/a4039a34-6135-4ad5-94dd-77a127ee9ac1/.system_generated/logs/transcript_full.jsonl", "r") as f:
        for line in f:
            try:
                data = json.loads(line)
                if data.get("type") == "VIEW_FILE" and data.get("status") == "DONE":
                    content = data.get("content", "")
                    if path in content:
                        lines = content.split('\n')
                        start_reading = False
                        for l in lines:
                            if "The following code has been modified to include a line number" in l:
                                start_reading = True
                                continue
                            if "The above content does NOT show the entire file contents" in l or "The above content shows the entire" in l:
                                start_reading = False
                                continue
                            if start_reading:
                                idx = l.find(":")
                                if idx > 0 and l[:idx].isdigit():
                                    num = int(l[:idx])
                                    lines_by_num[num] = l[idx+2:]
            except:
                pass
    
    if lines_by_num:
        max_num = max(lines_by_num.keys())
        with open(out_name, "w") as out:
            for i in range(1, max_num + 1):
                out.write(lines_by_num.get(i, f"// MISSING LINE {i}") + "\n")
        print(f"Reconstructed {out_name} with {max_num} lines.")

reconstruct("src/llama-kv-swap.h", "recovered_llama-kv-swap.h")
reconstruct("src/llama-kv-swap.cpp", "recovered_llama-kv-swap.cpp")
reconstruct("src/llama-kv-cache.h", "recovered_llama-kv-cache.h")
reconstruct("src/llama-kv-cache.cpp", "recovered_llama-kv-cache.cpp")
