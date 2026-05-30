#!/usr/bin/env python3
# Verifier: reads the live Plex process to confirm the crack installed.
#   arg1 = PID of "Plex Media Server"
# Checks:
#   - apply_feature_list_xml (file vaddr 0x1167490) prologue overwritten with a
#     trampoline JMP (FF 25 ...) => hook installed.
#   - g_feature_bitset_slots (file vaddr 0x15AE5D8, 14 x u64) => feature bits.
import sys, struct, binascii

pid = int(sys.argv[1])
MAIN = "/usr/lib/plexmediaserver/Plex Media Server"
APPLY = 0x1167490
BITSET = 0x15AE5D8

base = None
with open("/proc/%d/maps" % pid) as f:
    for line in f:
        if line.rstrip().endswith(MAIN):
            base = int(line.split("-", 1)[0], 16)
            break
if base is None:
    print("ERROR: base mapping not found")
    sys.exit(1)
print("base = 0x%x" % base)

def rd(off, n):
    with open("/proc/%d/mem" % pid, "rb") as f:
        f.seek(base + off)
        return f.read(n)

fn = rd(APPLY, 16)
print("apply_feature_list_xml[0:16] = " + binascii.hexlify(fn).decode())
print("hook installed (prologue == jmp FF 25)? %s" % (fn[:2] == b"\xff\x25"))

qs = struct.unpack("<14Q", rd(BITSET, 112))
for i, q in enumerate(qs):
    print("  slot %2d = 0x%016x" % (i, q))
print("all 14 qwords fully 0xFF..F? %s" % all(q == 0xFFFFFFFFFFFFFFFF for q in qs))
print("all used low-bytes set (every feature enabled)? %s" % all((q & 0xFF) == 0xFF for q in qs))
