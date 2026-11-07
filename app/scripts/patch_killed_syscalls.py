#!/usr/bin/env python3
"""Make inline syscalls that the HarmonyOS app sandbox kills return -ENOSYS.

The app seccomp policy kills the process on some newer syscalls instead of
failing them (the syscall-probe test shows which). Libraries such as
rustix issue them with an inline `svc #0` and fall back when the kernel
answers ENOSYS. In executable segments, each `svc #0` whose syscall number
(`mov w8|x8, #nr` before it) is in the killed set becomes `mov x0, #-38`,
which is the raw ENOSYS return. Patching happens before the HAP is signed.

Inline ioctls whose request the sandbox refuses with an error the caller
treats as fatal get the error it falls back on instead. TIOCGPTPEER fails
with EACCES in the app sandbox; rustix-openpty (Alacritty) opens the pty by
name after ENOSYS or EPERM, so the call becomes `mov x0, #-1`, raw -EPERM.

usage: patch_killed_syscalls.py <elf>...
"""
import struct
import sys

KILLED = {
    293, 435, 436, 425, 426, 427, 444, 445, 446, 447, 441, 437, 449, 236, 272, 97, 289, 264,
}
SVC0 = 0xD4000001
MOVN_X0_ENOSYS = 0x928004A0  # mov x0, #-38
MOVN_X0_EPERM = 0x92800000  # mov x0, #-1
IOCTL = 29
# ioctl request -> the raw return the call site gets instead.
DENIED_IOCTLS = {0x5441: MOVN_X0_EPERM}  # TIOCGPTPEER
LOOKBACK = 8


def register_immediate(words, index, register):
    """Returns the immediate last moved into w<register>/x<register> before
    words[index], or None."""
    for back in range(1, LOOKBACK + 1):
        if index - back < 0:
            return None
        insn = words[index - back]
        # MOVZ w/x<register>, #imm16 (shift 0)
        if insn & 0x7FE0001F == 0x52800000 | register:
            return (insn >> 5) & 0xFFFF
        # Any other write to the register ends the search.
        if insn & 0x1F == register:
            return None
    return None


def syscall_number(words, index):
    return register_immediate(words, index, 8)


def executable_ranges(data):
    if data[:4] != b"\x7fELF" or data[4] != 2:
        return []
    phoff = struct.unpack_from("<Q", data, 0x20)[0]
    phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
    ranges = []
    for i in range(phnum):
        p_type, p_flags, p_offset, _, _, p_filesz = struct.unpack_from(
            "<IIQQQQ", data, phoff + i * phentsize)
        if p_type == 1 and p_flags & 1:
            ranges.append((p_offset, p_offset + p_filesz))
    return ranges


def patch(path):
    with open(path, "rb") as file:
        data = bytearray(file.read())
    patched = []
    for start, end in executable_ranges(data):
        start = (start + 3) & ~3
        count = (end - start) // 4
        words = list(struct.unpack_from(f"<{count}I", data, start))
        for index, insn in enumerate(words):
            if insn != SVC0:
                continue
            number = syscall_number(words, index)
            if number in KILLED:
                struct.pack_into("<I", data, start + index * 4, MOVN_X0_ENOSYS)
                patched.append(number)
            elif number == IOCTL and register_immediate(words, index, 1) in DENIED_IOCTLS:
                struct.pack_into("<I", data, start + index * 4, DENIED_IOCTLS[register_immediate(words, index, 1)])
                patched.append(number)
    if patched:
        with open(path, "wb") as file:
            file.write(data)
        print(f"{path}: {len(patched)} sandbox-refused syscalls now fail softly {sorted(set(patched))}")


if __name__ == "__main__":
    for elf in sys.argv[1:]:
        patch(elf)
