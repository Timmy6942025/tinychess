#!/usr/bin/env python3
"""Convert a RukChess 768xNH->1 float .nnue into Dog's embedded blob.

Emits app/src/weights-ruk.cpp (constexpr data + arch constants). The C++ loader
validates magic / source-hash / dims / blob length before using the net.

RukChess format (NNUE2.cpp): u32 magic 'BRKR' (0x524b5242), u64 arch hash,
then float32 arrays: feature weights [768*H], feature bias [H],
output weights [2*H], output bias [1]. Quant: in x64, out x512 (int16),
output bias int32 x(64*512).
"""
import struct, sys, os

def to_cpp(data: bytearray, name: str) -> str:
    out = [f"alignas(64) constexpr const uint8_t {name}[] {{"]
    for i in range(0, len(data), 26):
        chunk = data[i:i+26]
        row = ", ".join(f"0x{b:02x}" for b in chunk)
        out.append("  " + row + ",")
    out[-1] = out[-1].rstrip(",")
    out.append("};")
    return "\n".join(out)

def convert(src: str, dst: str) -> None:
    d = open(src, "rb").read()
    if len(d) < 12:
        raise SystemExit(f"{src}: too short")
    magic, hh = struct.unpack("<IQ", d[:12])
    if magic != 0x524B5242:
        raise SystemExit(f"{src}: bad magic {magic:08x}")
    hidden = int(sys.argv[3]) if len(sys.argv) > 3 else 512
    input_dim = 768
    q_in, q_out = 64, 512
    expect = 12 + (input_dim*hidden + hidden + 2*hidden + 1) * 4
    if len(d) != expect:
        raise SystemExit(f"{src}: size {len(d)} != expected {expect}")
    off = 12
    def rf(n: int):
        nonlocal off
        v = struct.unpack(f"<{n}f", d[off:off+4*n])
        off += 4*n
        return v
    fw = rf(input_dim*hidden)
    fb = rf(hidden)
    ow = rf(2*hidden)
    ob = rf(1)[0]
    assert off == len(d)
    def q16(v: float) -> int:
        # match RukChess's I16 cast: signed 16-bit wrap on overflow
        w = int(round(v))
        return ((w + 32768) % 65536) - 32768
    fw16 = [q16(v*q_in) for v in fw]
    fb16 = [q16(v*q_in) for v in fb]
    ow16 = [q16(v*q_out) for v in ow]
    ob32 = int(round(ob * q_in * q_out))
    blob = bytearray()
    blob += b"MDRK"                                    # blob magic (loader checks)
    blob += struct.pack("<I", magic)                 # source magic
    blob += struct.pack("<Q", hh)                    # source arch hash
    blob += struct.pack("<IIIIII", hidden, input_dim, 1, q_in, q_out, 0)
    for v in fw16: blob += struct.pack("<h", v)
    for v in fb16: blob += struct.pack("<h", v)
    for v in ow16: blob += struct.pack("<h", v)
    blob += struct.pack("<i", ob32)
    src_name = os.path.basename(src)
    hdr = f"""// Auto-converted from {src} ({src_name})
// RukChess 768->H->1 float net; quant in x{q_in} out x{q_out}; output bias i32.
// source magic 0x524b5242, source arch hash 0x{hh:016x}, blob length {len(blob)}.
constexpr uint64_t ruk_src_hash = 0x{hh:016x}ull;
constexpr int32_t ruk_hidden = {hidden};
constexpr int32_t ruk_input  = {input_dim};
constexpr int32_t ruk_qin    = {q_in};
constexpr int32_t ruk_qout   = {q_out};
constexpr int ruk_weights_size = {len(blob)};
{to_cpp(blob, "ruk_weights_data")}
"""
    open(dst, "w").write(hdr)
    print(f"wrote {dst} ({len(blob)} bytes) hash 0x{hh:016x} ob32={ob32}")

if __name__ == "__main__":
    convert(sys.argv[1], sys.argv[2])
