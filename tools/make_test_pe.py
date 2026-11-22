#!/usr/bin/env python3
"""Build a tiny hand-assembled PE32 console program for testing the loader, shim and CPU.

The program sums 5+4+3+2+1 in a loop, calls a local function that adds 100, prints
"Hello from x86!" via kernel32!WriteFile, then calls ExitProcess(sum) -> exit code 115.
"""
import struct, sys

IMAGE_BASE = 0x400000
TEXT_RVA, RDATA_RVA = 0x1000, 0x2000


def build(path):
    imports = ["GetStdHandle", "WriteFile", "ExitProcess"]

    # ---- .rdata: import directory, ILT, IAT, names, strings, scratch ----
    ilt_off = 40
    iat_off = ilt_off + 16
    names_off = iat_off + 16
    names, name_rvas = b"", []
    for n in imports:
        name_rvas.append(RDATA_RVA + names_off + len(names))
        s = struct.pack("<H", 0) + n.encode() + b"\0"
        names += s + (b"\0" if len(s) % 2 else b"")
    dll_off = names_off + len(names)
    dll = b"KERNEL32.dll\0"
    msg_off = dll_off + len(dll)
    msg = b"Hello from x86!\n"
    written_off = (msg_off + len(msg) + 3) & ~3

    rdata = bytearray(0x200)
    rdata[0:20] = struct.pack("<IIIII", RDATA_RVA + ilt_off, 0, 0, RDATA_RVA + dll_off, RDATA_RVA + iat_off)
    for i, rva in enumerate(name_rvas):
        struct.pack_into("<I", rdata, ilt_off + 4 * i, rva)
        struct.pack_into("<I", rdata, iat_off + 4 * i, rva)
    rdata[names_off:names_off + len(names)] = names
    rdata[dll_off:dll_off + len(dll)] = dll
    rdata[msg_off:msg_off + len(msg)] = msg

    iat = {n: IMAGE_BASE + RDATA_RVA + iat_off + 4 * i for i, n in enumerate(imports)}
    msg_va = IMAGE_BASE + RDATA_RVA + msg_off
    written_va = IMAGE_BASE + RDATA_RVA + written_off

    # ---- .text ----
    c = bytearray()
    c += b"\xB9" + struct.pack("<I", 5)          # 0   mov ecx,5
    c += b"\x31\xC0"                             # 5   xor eax,eax
    loop = len(c)
    c += b"\x01\xC8"                             # 7   add eax,ecx
    c += b"\x49"                                 # 9   dec ecx
    c += b"\x75" + struct.pack("<b", loop - (len(c) + 2))   # jnz loop
    c += b"\x89\xC7"                             #     mov edi,eax
    call_at = len(c)
    c += b"\xE8" + b"\0\0\0\0"                   #     call func (patched below)
    c += b"\x6A\xF5"                             #     push -11
    c += b"\xFF\x15" + struct.pack("<I", iat["GetStdHandle"])
    c += b"\x89\xC6"                             #     mov esi,eax
    c += b"\x6A\x00"                             #     push 0            lpOverlapped
    c += b"\x68" + struct.pack("<I", written_va) #     push &written
    c += b"\x6A" + bytes([len(msg)])             #     push len
    c += b"\x68" + struct.pack("<I", msg_va)     #     push msg
    c += b"\x56"                                 #     push esi          hFile
    c += b"\xFF\x15" + struct.pack("<I", iat["WriteFile"])
    c += b"\x57"                                 #     push edi
    c += b"\xFF\x15" + struct.pack("<I", iat["ExitProcess"])
    func = len(c)
    c += b"\x83\xC7\x64"                         # func: add edi,100
    c += b"\xC3"                                 #       ret
    struct.pack_into("<i", c, call_at + 1, func - (call_at + 5))
    text = bytes(c).ljust(0x200, b"\xCC")

    # ---- headers ----
    dos = bytearray(0x40); dos[0:2] = b"MZ"; struct.pack_into("<I", dos, 0x3C, 0x40)
    coff = struct.pack("<HHIIIHH", 0x14C, 2, 0, 0, 0, 224, 0x0103)
    opt = struct.pack(
        "<HBBIIIIIIIIIHHHHHHIIIIHHIIIIII",
        0x10B, 0, 0, 0x200, 0x200, 0, TEXT_RVA, TEXT_RVA, RDATA_RVA, IMAGE_BASE, 0x1000, 0x200,
        4, 0, 0, 0, 4, 0, 0, 0x3000, 0x200, 0, 3, 0, 0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
    dd = bytearray(128); struct.pack_into("<II", dd, 8, RDATA_RVA, 40)
    opt += bytes(dd)
    assert len(opt) == 224, len(opt)

    def sh(name, vsize, rva, rawsz, rawoff, flags):
        return struct.pack("<8sIIIIIIHHI", name, vsize, rva, rawsz, rawoff, 0, 0, 0, 0, flags)

    headers = bytes(dos) + b"PE\0\0" + coff + opt
    headers += sh(b".text", len(c), TEXT_RVA, 0x200, 0x200, 0x60000020)
    headers += sh(b".rdata", 0x200, RDATA_RVA, 0x200, 0x400, 0xC0000040)
    headers = headers.ljust(0x200, b"\0")

    with open(path, "wb") as f:
        f.write(headers + text + bytes(rdata))


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "hello.exe"
    build(out)
    print("wrote", out)
