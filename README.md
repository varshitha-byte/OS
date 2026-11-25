# WOLVES: Advanced Metadata Write-Once File System Prototype

A high-performance C++ prototype of **WOLVES**, a Metadata Write-Once File System designed for Persistent Memory (PMEM). This project is based on the architectural concepts presented in the OSDI '25 paper: *"Fast and Synchronous Crash Consistency with Metadata Write-Once File System"* (Pan et al., USENIX OSDI '25), extended with **Tracks A, C, D, and F** advanced systems upgrades.

---

## 📖 About the Project

With the advent of byte-addressable Persistent Memory (like Intel Optane and CXL-attached PMEM), modern storage systems demand synchronous crash consistency without sacrificing throughput. Traditional file systems rely on redundant write mechanisms:
- **Journaling File Systems (e.g., ext4, NOVA):** Incur double writes and ordered flushes (`J_data` $\rightarrow$ `PCOMMIT` $\rightarrow$ `J_metadata` $\rightarrow$ `PCOMMIT`).
- **Log-Structured File Systems:** Require recursive pointer cascades, continuous checkpointing, and heavy garbage-collection write amplification.

**WOLVES** eliminates redundant writes by aggregating the metadata of any file system operation into a single, checksum-protected **Write-Once Package** written directly to persistent memory.

---

## 🚀 Key Architectural Innovations & Tracks

### 1. Core Write-Once Architecture
* **80-Byte Packed Write-Once Packages:** Packed, 8-byte aligned metadata structure capturing `CREATE_FILE`, `WRITE_DATA`, `DELETE_FILE`, `CREATE_DIR`, `RENAME`, and `SNAPSHOT` with a single ordering point.
* **Compound Operations (Atomic RENAME):** Links multi-file operations via 64-bit forward pointers (`fwd_ptr`), guaranteeing atomic cross-directory movement without journals.
* **Storage Compaction & Garbage Collection:** In-memory VFS and PTL re-packer that purges dead historical transactions and compacts live data, reclaiming 100% of fragmented space.
* **POSIX FUSE Driver (Linux / WSL):** Full mountable POSIX file system exposing `mkdir`, `rmdir`, `mv`, `rm`, `ls`, and standard file I/O.

---

### 2. Implemented Advanced Tracks

#### 📸 Track A: Instant Zero-Copy Snapshots & Time-Travel Rollback
* **Write-Once Snapshot Packages:** Emits an 80-byte `SNAPSHOT (OpCode=6)` package recording the current global sequence threshold ($S_{thresh}$).
* **Zero-Copy Instant Capture ($O(1)$):** No file cloning, tree duplication, or block copying required.
* **Time-Travel Rollback (`fs.restore_snapshot(name)`):** Directs the Package Translation Layer (PTL) to reconstruct the in-memory VFS filtering out any packages where $seq > S_{thresh}$.
* **Instant Resurrection:** Reverts accidental file deletions, purges uncommitted modifications, and restores historical file contents instantaneously.

#### ⚡ Track C: Coarse Persistence Bitmaps & 4 KiB Package Groups (§4.4 & §5.1)
* **Superblock & Coarse Bitmap (Block 0):** Reserved 4 KiB header at offset 0 (`SUPERBLOCK_MAGIC = 0x574F5342 'WOSB'`) maintaining an active-group bitmap (1 bit per 4 KiB group).
* **Package Group Boundary Protection:** Packages strictly never cross 4 KiB boundaries; boundary alignments are handled automatically.
* **Sub-Millisecond Cold Boot:** PTL scans only active 4 KiB groups indicated in the bitmap, completely skipping unallocated storage space (e.g., scanning 5 groups instead of 25,600 groups for a 100 MB drive).

#### 🧪 Track D: Chaos Engineering & Crash-Consistency Fuzzer (`fuzz_crash.py`)
* **Automated Fault Injection Suite:** Tests the PTL recovery engine against real-world storage and power failure modes:
  1. **Torn Package Writes:** Simulates sudden power loss mid-write (truncating an 80-byte package to 24 bytes). PTL safely detects invalid length/checksum and skips cleanly.
  2. **Silent Bit-Rot & Single Bit-Flips:** Injects bit errors into filename/metadata fields. PTL detects checksum mismatch and discards corrupted packages without crashing.
  3. **Uncommitted Data Payloads:** Simulates power failure between raw data allocation and metadata package emission. PTL safely ignores orphaned payload bytes.
  4. **Broken Compound Forward-Pointers:** Simulates torn multi-package transactions. PTL detects broken forward links and automatically rolls back compound state.
* **100% Crash-Consistency Guarantee:** Automated assertions ensure no memory leaks, no unhandled exceptions, and no corrupted inodes survive recovery.

#### 🧵 Track F: Fine-Grained Multi-Threaded Concurrency
* **ReaderWriter VFS Synchronization:** Protects the in-memory VFS with `std::shared_mutex`. Readers acquire shared locks (`std::shared_lock`), while writers acquire unique locks (`std::unique_lock`).
* **Deadlock-Free Architecture:** Unlocked internal helpers (`resolve_path_unlocked`, `read_file_unlocked`, `get_gc_stats_unlocked`) ensure recursive calls inside locks never deadlock.
* **Concurrent PMEM Allocation:** Low-level persistent memory allocation and dirty-range disk persistence are serialized with `std::mutex pmem_mutex_`.
* **High-Stress Concurrency Test:** Multi-threaded verification with 2 concurrent readers and 2 concurrent writers operating across 40 files simultaneously.

---

## 🛠️ Build & Execution Guide

### Option 1: End-to-End Automated Pipeline (PowerShell / Windows)
Runs the simulator, inspects raw binary persistent memory, and executes the Track D crash fuzzer:
```powershell
.\run_demo.ps1
```

### Option 2: Linux / WSL Build & Execution
```bash
# 1. Install prerequisites
sudo apt update && sudo apt install -y cmake g++ pkg-config libfuse-dev python3

# 2. Build the project
mkdir -p build_linux && cd build_linux
cmake ..
make -j$(nproc)

# 3. Run the simulator
./wolves_sim

# 4. Run recovery verification mode
./wolves_sim --recover
```

### Option 3: Run the Crash-Consistency Chaos Monkey & Fuzzer (Track D)
```bash
python fuzz_crash.py
```
Output:
```text
===========================================================================
 WOLVES Crash-Consistency Chaos Engineering & Fuzzer (Track D)
===========================================================================

[Test 1] Injecting Torn Package Write (Power Loss Mid-Write)...
  [PASSED] Torn package at offset 0x4000 safely detected and ignored during recovery!

[Test 2] Injecting Silent Bit-Rot / Single Bit-Flip Corruption...
  [PASSED] Checksum mismatch caught corrupted package at offset 0x4000 and discarded it cleanly!

[Test 3] Injecting Crash Between Raw Data Write and Metadata Package...
  [PASSED] Uncommitted data payload at offset 0x4000 safely ignored by PTL!

[Test 4] Injecting Broken Compound Forward-Pointer (Torn Transaction)...
  [PASSED] PTL detected invalid forward link at offset 0x4000 and rolled back compound transaction!

===========================================================================
 [ALL 4 FAULT-INJECTION CHAOS TESTS PASSED WITH 100% CRASH CONSISTENCY!] 
===========================================================================
```

### Option 4: Inspect Raw Persistent Memory Binary
Inspect the binary memory layout of your simulated PMEM storage:
```bash
python inspect_pmem.py
```

### Option 5: Mountable FUSE File System (Linux / WSL Only)
```bash
cd build_linux
mkdir -p ~/wolves_drive
./wolves_fuse -f ~/wolves_drive
```
Open a separate terminal to interact via standard POSIX shell commands:
```bash
ls -la ~/wolves_drive
mkdir ~/wolves_drive/photos
echo "Testing WOLVES on FUSE" > ~/wolves_drive/photos/test.txt
cat ~/wolves_drive/photos/test.txt
```

---

## 📁 Repository Structure

```text
WOLVES_Prototype/
├── CMakeLists.txt              # Build configuration with POSIX pthread and FUSE
├── run_demo.ps1                # Automated Windows PowerShell demo runner
├── inspect_pmem.py             # Binary PMEM storage decoder (Superblock & Packages)
├── fuzz_crash.py               # Track D Chaos Engineering & Fault-Injection Fuzzer
├── README.md                   # Project documentation
├── IMPLEMENTATION_PLAN.md      # Detailed engineering roadmap and track specs
└── src/
    ├── package.h               # 80-Byte Write-Once Package struct & OpCodes
    ├── pmem.h / pmem.cpp       # Simulated PMEM, 4KB Package Groups, Coarse Bitmap
    ├── wofs.h / wofs.cpp       # In-Memory VFS, PTL Recovery, Snapshots, Locks
    ├── main.cpp                # Concurrency stress tests & crash simulator
    └── fuse_main.cpp           # POSIX FUSE file system driver
```

---

## 📄 References
* Pan, Y. et al. *"Fast and Synchronous Crash Consistency with Metadata Write-Once File System."* Proceedings of the 19th USENIX Symposium on Operating Systems Design and Implementation (**USENIX OSDI '25**).
