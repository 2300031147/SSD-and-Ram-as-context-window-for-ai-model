from struct import unpack
with open("models/qwen2.5-7b-instruct-q5_k_m.gguf", "rb") as f:
    f.read(4)
    version, tensors, kvs = unpack("<IQQ", f.read(20))
    for i in range(kvs):
        l = unpack("<Q", f.read(8))[0]
        key = f.read(l).decode('utf-8')
        t = unpack("<I", f.read(4))[0]
        if t == 4: # uint32
            val = unpack("<I", f.read(4))[0]
            if "context_length" in key or "n_ctx" in key:
                print(key, val)
        else:
            # skip
            pass
