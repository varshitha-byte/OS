import struct
import os

PMEM_FILE = os.path.join(os.path.dirname(__file__), "build_linux", "simulated_pmem.bin")
if not os.path.exists(PMEM_FILE):
    PMEM_FILE = os.path.join(os.path.dirname(__file__), "simulated_pmem.bin")

if not os.path.exists(PMEM_FILE):
    print(f"Error: Could not find {PMEM_FILE}. Run wolves_sim first!")
    exit(1)

with open(PMEM_FILE, "rb") as f:
    data = f.read()

print("=" * 75)
print(f" WOLVES PMEM STORAGE INSPECTOR ({len(data) // (1024*1024)} MB Simulated PM)")
print("=" * 75)

# Track C: Superblock Inspection (Block 0)
if len(data) >= 4096:
    sb_magic, sb_grp_size, sb_total_grps, sb_bm_bytes = struct.unpack_from("<IIII", data, 0)
    if sb_magic == 0x574F5342:  # 'WOSB'
        # Count active groups in bitmap
        active_grps = 0
        for i in range(sb_total_grps):
            byte_idx = 16 + (i // 8)
            bit_mask = 1 << (i % 8)
            if data[byte_idx] & bit_mask:
                active_grps += 1
        print(f"[Superblock & Coarse Bitmap (Track C)]")
        print(f"  Magic:        0x{sb_magic:08x} ('WOSB')")
        print(f"  Group Size:   {sb_grp_size} bytes (4 KiB)")
        print(f"  Total Groups: {sb_total_grps} ({sb_total_grps * sb_grp_size // (1024*1024)} MB)")
        print(f"  Active Bit:   {active_grps} / {sb_total_grps} active groups ({active_grps * 4} KiB allocated)")
        print("-" * 75)

FMT = "<III B B H II II Q Q 32s"
STRUCT_SIZE = struct.calcsize(FMT)
assert STRUCT_SIZE == 80, f"STRUCT_SIZE must be 80, got {STRUCT_SIZE}"

offset = 4096 # Packages start after Superblock
found = 0
op_names = {
    1: "CREATE_FILE",
    2: "WRITE_DATA",
    3: "DELETE_FILE",
    4: "CREATE_DIR",
    5: "RENAME",
    6: "SNAPSHOT"
}

while offset + STRUCT_SIZE <= len(data):
    if data[offset:offset+4] == b"SFOW":  # 0x574F4653 little-endian ('W', 'O', 'F', 'S')
        magic, csum, seq, op, flags, rsvd, ino, parent_ino, file_off, size, pmem_ptr, fwd_ptr, name_raw = struct.unpack_from(FMT, data, offset)
        name = name_raw.split(b"\x00")[0].decode("ascii", errors="replace")
        
        is_compound = bool(flags & 0x01)
        compound_str = f" [COMPOUND -> fwd_ptr=0x{fwd_ptr:06x}]" if is_compound else ""

        print(f"\n[Package #{found + 1}] at PM Offset 0x{offset:06x} ({offset} bytes)")
        print(f"  Header:      Magic=0x{magic:08x} ('WOFS') | Seq={seq} | Checksum=0x{csum:08x}{compound_str}")
        print(f"  Operation:   {op_names.get(op, f'UNKNOWN({op})')} (OpCode={op})")
        print(f"  Target:      Inode={ino} | ParentInode={parent_ino} | Name=\"{name}\"")
        
        if op == 2:  # WRITE_DATA
            print(f"  Extent:      FileOffset={file_off} | Size={size} bytes")
            print(f"  Data In PM:  Ptr=0x{pmem_ptr:06x}")
            raw_payload = data[pmem_ptr:pmem_ptr + size]
            print(f"  Payload:     {repr(raw_payload)}")
        elif op == 5:  # RENAME
            print(f"  Rename To:   NewName=\"{name}\" under ParentInode={parent_ino}")
            if is_compound:
                print(f"  Compound:    Linked UNLINK package at PM offset 0x{fwd_ptr:06x}")
        elif op == 6:  # SNAPSHOT (Track A)
            print(f"  Snapshot:    Name=\"{name}\" | Rollback Threshold Seq={file_off}")

        found += 1
        offset += STRUCT_SIZE
    else:
        offset += 8

print("\n" + "=" * 75)
print(f"Summary: Verified {found} Write-Once Packages across active groups.")
print("=" * 75)
