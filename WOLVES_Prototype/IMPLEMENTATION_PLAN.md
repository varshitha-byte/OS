# WOLVES Prototype — Detailed Implementation Plan

> **Building "Fast and Synchronous Crash Consistency with Metadata Write-Once File System" (OSDI '25, Pan et al.) as a user-space C++ prototype for a college OS project.**
>
> This is deliberately *not* a full WOLVES kernel port. It is a user-space simulator that faithfully demonstrates the paper's core ideas: write-once packages, the Package Translation Layer (PTL), a non-log layout with reuse-based reclamation, and coarse persistence for fast recovery.

---

## 0. Current State of the Prototype & Completed Upgrades

### Completed Advanced Tracks (Tracks A, C, D, F)
| Track | Description | Status | Implementation Details |
|---|---|---|---|
| **Track A** | Instant Time-Travel & Zero-Copy Snapshots | ✅ **Complete & Verified** | `SNAPSHOT (OpCode=6)` packages, $O(1)$ capture, threshold sequence filtering `rollback_to_seq()`, instant resurrect/rollback |
| **Track C** | Coarse Persistence Bitmaps (4 KiB Package Groups) | ✅ **Complete & Verified** | Block 0 Superblock (`0x574F5342 'WOSB'`), 1-bit coarse bitmap per 4KB group, sub-millisecond PTL boot acceleration skipping unallocated space, 4KB boundary packing |
| **Track D** | Chaos Engineering & Crash-Consistency Fuzzer | ✅ **Complete & Verified** | Automated python fuzzer (`fuzz_crash.py`) with 4 fault injection suites: torn writes, bit-rot/checksum mismatch, orphan payloads, broken compound links (100% pass) |
| **Track F** | Multi-Threaded Concurrency | ✅ **Complete & Verified** | Fine-grained reader/writer locking with `std::shared_mutex`, shared lock for reads, unique lock for writes, recursive deadlock-free unlocked helpers, multi-threaded stress suite |

---

### Core Paper Concepts Status Matrix

| Paper concept | Prototype status | Where |
|---|---|---|
| Package (magic + CRC + op + payload) | ✅ Full | `src/package.h` — 80-byte packed struct with CRC checksum, flags, sequence numbers |
| Write-once with single ordering point | ✅ | `wofs.cpp:append_package()` — allocate + write + persist |
| PTL (package translation layer) | ✅ Full | `wofs.cpp:package_translation_layer()` — accelerated coarse bitmap scan, snapshot filtering, compound rollback |
| Data-then-package ordering (D → JM\|JC) | ✅ | `write_file()` persists data first, then the package |
| Crash recovery & Cold Boot Verification | ✅ Verified | `main.cpp` and `fuzz_crash.py` verify full cold boot reconstruction from PMEM binary |
| PM simulation | ✅ | `pmem.cpp` — RAM array backed by `simulated_pmem.bin` with dirty-range flushing |
| Atomic compound packages (Rename) | ✅ | Forward pointers (`fwd_ptr`) + compound flags with rollback on broken links |
| Non-log layout + storage compaction | ✅ | `compact()` purges superseded versions and dead space, compacting live packages |
| Coarse persistence (4 KiB pkg-groups + bitmap) | ✅ Full (Track C) | Block 0 Superblock + 1-bit coarse bitmap per 4KB group |
| Directory hierarchy | ✅ Full | Multi-level hierarchical directory paths (`/documents/projects/file.txt`), parent inodes |
| Multi-threaded concurrency / locking | ✅ Full (Track F) | ReaderWriter VFS mutex (`std::shared_mutex`) + PMEM allocation mutex |
| POSIX FUSE file system | ✅ Full | Mountable on Linux/WSL (`wolves_fuse`) |

**Existing known gaps/bugs to fix as part of the work (below):**
1. `package_translation_layer()` walks PM in `sizeof(Package)` steps, so a package written at a non-`sizeof(Package)`-aligned offset is never found — the allocator hands out contiguous slabs, so today every package lands at a multiple of `sizeof(Package)`, but **the moment we add heterogeneous package sizes (0/64-byte or 256-byte packages, §4.2 of the paper), linear scanning breaks.** The plan therefore introduces the pkg-group model, which fixes scanning and gives end-of-PM bounds.
2. Fixed-size struct: `Package` is ~80 bytes; WRITE packages padded to 80 waste space; the paper's Write/Attr/Unlink packages are 64 bytes and Create is 256.
3. `filename[32]` truncates names; Create pkg should use a 128-byte region holding parent-ino + linked-ino + name.
4. Corrupted packages are found but **never reclaimed**; nothing marks their space free.
5. No `wofs.cpp` handling for a write pkg that references a deleted inode (a crash between D and package, or between create and write) — recovery silently drops the extent (acceptable per the paper, but should be surfaced/demonstrated).

---

## 1. Architecture After the Plan (target design)

```
+-------------------------------------------------------------+
|  main.cpp (demo driver)      fuse_main.cpp (Linux/WSL)      |
+-------------------------------------------------------------+
|  high-level ops: create / write / read / unlink / rename /   |
|                  stat / readdir / chmod                      |
+-------------------------------------------------------------+
|  PTL (DRAM):                                                 |
|   inode_table  = unordered_map<ino, InodePTR>                |
|   Inode        = C-node (static) + A-node (dynamic attrs)    |
|   file index   = dynamic array of W-nodes (extents)          |
|   dent list    = name -> C-node (per directory)              |
|   order map    = addr(create pkg) -> addr(unlink pkg)        |
+-------------------------------------------------------------+
|  tl-allocator: free-list of 64-byte slots inside pkg-groups, |
|   free-list of 4 KiB blocks, per-CPU later (optional)        |
+-------------------------------------------------------------+
|  PM simulator: byte-addressable RAM array backed by a file,  |
|   persist() = the "PCOMMIT" ordering point                   |
+-------------------------------------------------------------+
```

### Package layout (mirrors paper §4.2)

All packages share the 16-byte header (paper: magic, type, timestamp, CRC32; we add `seq` for deterministic 72-line demo sweeps and `created/free` bookkeeping):

```c
struct PkgHeader {              // 16 bytes
  uint32_t magic;               // 'WOF1'
  uint8_t  type;                // CREATE / WRITE / ATTR / UNLINK
  uint8_t  flags;               // bit0 = compound (linked via fwd_ptr)
  uint32_t crc;                 // CRC-32 over [type..end]
  uint64_t seq;                 // logical op counter; timestamp stand-in
  uint64_t fwd_ptr;             // offset of a linked sub-package (0 = none)
};
static_assert(sizeof(PkgHeader) == 24);  // see note below
```

Note: 16 vs 24 bytes — the paper packs the header into what remains after reordering so packages are exactly 64/256 bytes; for the prototype, fill 24 and pad. Exact on-package size is a *nice-to-have*, not required; what matters is (a) CRC over the payload, (b) the write-once rule, and (c) 8-byte alignment.

```c
struct CreatePkg {  // 64 bytes total  (paper: 256 B; we shrink because we
                    //  keep flat names and skip xattrs; explain deviation in report)
  PkgHeader hdr;            // 24
  uint32_t ino;             // inode number of the new file
  uint32_t parent_ino;      // directory ino (0 = root)
  uint32_t linked_ino;      // link target (hard link)
  uint32_t parent_mcv;      // proof of parent length/size epoch
  uint64_t name_off;        // not needed in flat layout; reserved
  char     name[32];        // file name (uint8 NUL-terminated)
};                          // pad to 64
static_assert(sizeof(CreatePkg) == 64);

struct WritePkg {   // 64 bytes total
  PkgHeader hdr;            // 24
  uint32_t ino;
  uint32_t off;             // file offset (see clampToU32 note below)
  uint32_t size;
  uint32_t reserved1;       // 8-byte append slot (paper §5.2): appended
                            //  through it w/o a new CRC pass
  uint64_t data_off;        // PM offset of first data block
  uint32_t nblocks;
  uint32_t reserved2;
};                          // 64

struct AttrPkg {    // 64 bytes total
  PkgHeader hdr;
  uint32_t ino;
  uint32_t mode;            // chmod
  uint32_t uid;
  uint32_t gid;
  uint32_t reserved;
};                          // 64

struct UnlinkPkg {  // 64 bytes total
  PkgHeader hdr;
  uint32_t ino;             // inode being unlinked
  uint32_t parent_ino;      // directory affected
  uint32_t parent_mcv;      // evidence of the parent's epoch for parse
};                          // 64
```

**Why 24-byte header?** We keep `seq` (a global logical counter) so the demo recovery can show deterministic causal-ordering; a real deployment would use a 64-bit wall timestamp + per-CPU counter. `flags` supports compound ops. Padding is explained in the report — the prototype concentrates on *semantics*, not bit-squeezing.

---

## 2. Milestones

Each milestone is independently demo-able and testable; the numbering `M0…M7` corresponds to the Git commit series you can mirror.

### M0 — Testing scaffolding + algorithmic fixes (1–2 days)
1. **CRC-32** (`src/crc32.cpp/h`): standard reflected CRC-32 (poly `0xEDB88320`), table-driven, unit tests for known vectors `123456789` → `0xCBF43926`.
2. **PM simulator discipline**: keep `allocate()` obeying 8-byte alignment, verbose `persist()` (single `clwb+sfence` stand-in); **add `pmem_layout_dump()` so tests can prove D → P order in the binary image.**  Add panic-early on out-of-space rather than silent guard resets.
3. **Fix the linear-scan assumption** in `package_translation_layer()`: iterate `offset += 64` from the first bytes of the PM (after the head blocks) matching on the magic in a 64-byte-aligned knob; on a magic hit, read the full package size (`CreatePkg` 64 vs `WritePkg` 64); on corruption, `offset += 64` and continue.
4. **Unit test harness** (`tests/`): a tiny `tests/test_main.cpp` with a micro assertion framework (no external deps) and a per-test fresh PM image (use a suffixed backing file per test, e.g. `simulated_pmem_test1.bin`). Tests:
   - `t_create_read_roundtrip`
   - `t_write_read_roundtrip` (incl. non-contiguous extents)
   - **t_crash_create_only** — create a file, "crash" (drop DRAM state, remount): file survives.
   - **t_crash_before_pkg** — persist data but never write the package (simulate by writing D, then *not* issuing the package, then remount): file size does NOT include the un-referenced data. Proves package-correctness of `D → P`.
   - **t_crash_mid_package** — store a package whose CRC field does NOT match a recomputation (flip one bit in a payload byte). PTL must skip it and continue scanning. This is the paper's core "incomplete package detected via checksum" claim.
   - **t_delete_gone** — create, write, delete, remount → file is gone from the inode table.

### M1 — The four atomic packages (2–3 days)
Replace the flat `Package` struct with the 4 package types (header + type-specific payload), each written by one `append_package()` call. PTL parses by type:

- **Create pkg** → build C-node (static attrs: ino, name, parent-ino), insert into dent list of the parent.
- **Write pkg** → append a W-node (`{off, size, data_off}`) to the file's data list.
- **Attr pkg** → update the file's dynamic attributes (a per-ino `A-node` the PTL replaces on each attr pkg).
- **Unlink pkg** → remove C-node from parent's dent list, remove the whole file entry, and *(importantly, do NOT immediately free data blocks—see M3)*.
   - **Order map**: `addr(create) → addr(unlink)`. On unlink, record it. When a create pkg slot is reallocated, look up the order map and free the paired unlink pkg's slot as well (paper §4.4).
- Add operations exposed to the driver/FUSE: `chmod`/`chown` → Attr pkg; `rm` → Unlink pkg.
- Tests: create→chmod→remount keeps mode; unlink→remount drops the file; chmod after unlink (impossible) is dropped because inode is absent — and the stale attr pkg is discarded at scan.

### M2 — Compound packages: rename and symlink (2 days)
- **Rename = create|unlink** (paper §4.2): pre-allocated Create and Unlink pkgs each with `flags.compound = 1`. The create pkg's `fwd_ptr` points at the unlink pkg's PM offset. They are written **without an ordering point between them** (each gets its own PCOMMIT, no sfence-before-the-other requirement in the simulator).
- The paper writes P1|P2|…|PN without an ordering point between sub-packages and detects incompleteness two ways: (a) a forward-pointer from P1 → P2 exists but P2's CRC fails → the whole rename is discarded (the create half is never exposed to VFS); (b) an unlink pkg of compound type exists but no create pkg points to it → it is ignored and reclaimed. In the simulator, note that "concurrent" just means no persist ordering is forced between the two PCOMMITs — but the determines recovery order, which is fine for the demo sweep.
- Recovery adds a two-phase check: scan create pkgs with compound flag → follow `fwd_ptr` → verify target CRC + type, and record matched pairs in a temp hash map; then on unlink pass, any compound-type unlink pkg not in the map is reclaimed and ignored.
- **Symlink = create + write** in the same pattern, with type `CREATE|SYMLINK` and the target path stored as the "data".
- Tests: rename survives crash; rename dies atomically if either half corrupts (flip a byte in each half in two tests, file keeps its *original* name in both cases); symlink → read through to the target.

### M3 — Non-log layout + reuse-based reclamation (2–3 days)
This is the paper's §4.4 and the biggest semantic delta versus today's `next_free_offset_` bump allocator.
- **tl-allocator** (`src/allocator.cpp/h`):
  - **Blocks (4 KiB)**: free-list of block indexes; `alloc_block()` pops, `free_block()` pushes. Data blocks are plain 4 KiB blocks; packages are carved from pkg-groups (M4) using a per-group 64-bit slot bitmap.
  - **Packages (64-byte slots)**: pkg-groups (M4) — each 4 KiB block holding up to 64 slots of 64 bytes; a tiny 64-bit slot bitmap per group.
  - **Reclamation rules** (paper §4.4):
    - create pkg: freed when its file is unlinked (order-map pairing).
    - write pkg: freed when a newer write pkg overwrites the same [off, off+size) region *fully* contained — the old W-node is reclaimed; if the new write overlaps partially, the old W-node is shrunk to the non-overlapping range and a *new* write pkg is emitted for the rest (paper's three-case overlap handling).
    - attr pkg: freed when a newer attr pkg for the same ino is written.
    - unlink pkg: freed per the order-map rule.
    - compound pkg: freed only when ALL sub-packages are reclaimed (track a per-compound refcount node).
  - Data blocks freed when no W-node references them after a truncate/overwrite/unlink.
- **COW overwrite**: on overwrite of [c,d) ⊆ [a,b), new data blocks are allocated and persisted first (D), then a new write pkg is written (P). The old write pkg and its blocks are then reclaimed only after the new package is durable — preserves `D → P`.
- **Tests**: fill PM with files and delete half; assert free-slot count recovers; overwrite within a file and remount — data must read the newest value and old blocks are reusable (fill a probe pattern into the reclaimed block and confirm next write of a different file does not resurrect stale data).

### M4 — Coarse persistence (coarse bitmap) + fast recovery (2–3 days)
Paper §4.5. Today recovery scans every byte of PM; after this milestone it scans only the bitmap + numbered pkg-groups.
- **Reserved head region in the PM image**:
  - block 0: superblock (magic, version, PM size, bitmap layout constants)
  - blocks 1..B: **coarse bitmap** — 1 bit per 4 KiB block; bit set = "this block contains at least one live/possibly-live package" (a pkg-group that was allocated). Persisted *whenever a new pkg-group is allocated* (the paper allows `P | mcp` — no ordering between pkg and bitmap needed: if the bitmap bit is set but the pkg group has no valid pkg, just skip; if a pkg group was allocated but the bit wasn't persisted before a crash, the group's packages are lost — which is safe for metadata-only crash semantics... but careful: data blocks + their packages may be lost too. Since WOLVES has packages reference data blocks, losing a package pair loses both anyway. Document this.)
  - blocks after bitmap: pkg-groups and data blocks.
- **Allocation flow**: allocate a 4 KiB block as pkg-group → set & persist the bitmap bit (one persist) → hand out 64-byte slots within it.
- **Recovery**: read the superblock, scan bits, and for each set bit read its 4 KiB group (a scan of `#allocated_groups × 4 KiB`, not PM size); validate every 64-byte slot's magic/CRC; rebuild PTL in the order: create-packages (inodes + dent lists) → write pkgs (data lists + overlap resolution by seq) → attr pkgs (final attrs) → unlink-compound pairing (M2 logic).
- **Tests**: mount, ops, unmount+crash, remount — recovered tree identical (compare hash of full PTL dump); measure recovery scan bytes vs. old full-scan to quantify the CP benefit for the report.
- Demo measurement hook: `wofs_bench_recover` prints "scanned X of Y bytes (Z%)" — nice number for the project report.

### M5 — FUSE mount (Linux/WSL) polish (1–2 days)
`fuse_main.cpp` exists but is wired to the old API. Update it:
- Implement `getattr`, `readdir`, `mknod + create`, `open`, `read`, `write`, `unlink`, `rename`, `chmod` (each maps to packages + PTL ops);
- path resolution: walk from root by splitting `/a/b/c` across dent lists (this is where `parent_ino` in Create/Unlink pkgs gets used);
- flush on `release()` (the simulator's "persist" is a no-op marker; this is a semantics note in the report).
Build/run per the existing README's Option 2 and extend the demo script for a graded demo: mount in WSL, `cd`, `echo > file`, `cat`, `ctrl^C the mount, then remount and show the file is there (synchronous crash consistency without fsync!).

### M6 — Evaluation + report (2–3 days)
- **Microbenchmarks** (`wofs_bench`):
  - create/delete throughput; write/append throughput (4 KiB, 16 KiB, 256 KiB) before/after M3 reclamation;
  - recovery time / scanned-bytes metric from M4;
  - package-io = number of ordering points per op (should be exactly 1 for metadata ops; 2 for writes);
  - `persist()` counter: number of persist calls per op (a stand-in for PCOMMITs) — reported as "ordering points per operation".
- **Crash-consistency sweep**: extend the M0 harness into a **randomized crash sweater**
  - run N random ops, at each step randomly simulate a crash at every op boundary + "mid-package" corruption (flip N random bits in the NEXT package region before remount);
  - after each crash, remount and compare against a shadow filesystem that applied the full op sequence — recovered state must equal the state after the last *successfully completed* op.
- **Write the report** (update `project_report.tex`): motivation → WOFS model → what the prototype does and doesn't cover (per-CPU PTL, bit-squeezed headers, AVX/RA/HA — explicitly out of scope) → benchmark numbers → crash-consistency sweep results → limitations & future work.

### M7 — Stretch goals (optional, in a nice-to-have order)
1. **Append optimization** (paper §5.2): append writes update the file size via the 8-byte reserved field of the existing write pkg without recomputing the CRC; recovery checks CRC after zeroing the reserved field first, then re-checks with the size. This shows the paper's "append without a new package" trick.
2. **Per-CPU PTL** + std::mutex per directory/inode — demonstrates the paper's per-core design in principle; even a 2-thread mutation benchmark showing no correctness regression is a strong report section.
3. **Huge allocation (HA)** for appends: allocate 2 MiB-style slabs for append-heavy workloads to reduce pkg-group churn; measure the throughput delta in the report.
4. **Read-ahead (RA)**: 256-byte stride prefetch "simulation" — in the RAM-backed array this does little, so implement it as a measured counter; report why it doesn't help on a RAM simulator (honest note).

---

## 3. Deliverables checklist (what "done" looks like)

- [ ] `src/` with `package.h` (4 pkg types + compound flags), `crc32.{h,cpp}`, `allocator.{h,cpp}`, `wofs.{h,cpp}` (PTL incl. order map, compound checks, reclaim rules), `pmem.{h,cpp}` (alignment, persist counter), `fuse_main.cpp` (full op set), `main.cpp` (demo driver)
- [ ] `tests/` + crash tests from M0–M4
- [ ] `scripts/` demo scripts: `demo_sim.sh`, `demo_fuse.sh`, `bench_recover`
- [ ] `project_report.tex` updated with results and explicit out-of-scope list (per-CPU PTL, HA/RA, bit-squeezing headers, multi-core locking)
- [ ] README updated: build+run for Windows sim, WSL FUSE, test/bench commands
- [ ] Crash-consistency sweep results table in the report

---

## 4. Time budget (suggested, ~2–3 weeks part-time)

| Week | Focus |
|---|---|
| 1 | M0 + M1 (testing harness, CRC, 4 packages, order map) |
| 2 | M2 + M3 + M4 (compound pkgs, reclamation, coarse persistence) |
| 3 | M5 + M6 (FUSE ops, benches, sweep, report). M7 only if time remains. |

If you must cut: M4 (coarse persistence) and M2 (rename/symlink) are the highest-value-vs-effort items to keep; the sacrifice order is M7 → M5-polish → M6 benches → but **never cut M0's crash tests** — they are what makes this a real OS project demonstration of the paper's thesis.

---

## 5. Key file-by-file change list

| File | Change |
|---|---|
| `src/package.h` | Replace flat struct with header Create/Write/Attr/Unlink pkg structs, compound flags + forward pointer, 64-byte sizes |
| `src/crc32.{h,cpp}` | NEW — table-driven CRC-32 + unit-test vectors |
| `src/pmem.{h,cpp}` | Count persist() calls, enforce 8-byte alignment, add layout dump utility |
| `src/allocator.{h,cpp}` | NEW — pkg-group slot allocator + 4 KiB block free-list |
| `src/wofs.cpp` | PTL rewrite: type-dispatched parsing, order map, compound 2-phase validation, reclaim per §4.4, coarse bitmap head region |
| `src/wofs.h` | New API surface: rename/symlink/chmod/readdir/stat; Inode = C-node + A-node + data list of W-nodes |
| `src/fuse_main.cpp` | Update to new API, add rename/chmod/readdir/getattr |
| `src/main.cpp` | Demo driver: create → write → crash → remount → verify; rename demo; recovery-stats demo |
| `tests/` | NEW — M0 crash tests, M1–M4 per-milestone tests, randomized crash sweep |
| `scripts/` | NEW — demo & bench scripts |

## 6. Risks & mitigations

- **Implicit struct-layout/endianness assumptions** — the backing file is architecture-specific. Fine for a college project on one machine; note it in the report.
- **`uint32_t off` overflow** — file offsets are capped at 4 GiB (documented). The benchmark uses ≤ 256 KiB ops; DRAM structures keep `size_t`.
- **Compound-package detection subtlety**: if the create half of a rename is valid but its forward-ptr target is invalid, you must *discard the rename entirely* — do not apply the create half as a normal create. Guarded by the `flags.compound` bit; a compound create never populates the dent list until its partner validates.
- **Reclaim-on-realloc ordering (order map)** — the unlink pkg slot must outlive its create pkg; the order map lookup happens *at reallocation time*, so keep the map keyed by PM offset, not by inode (offsets are unique; inodes get reused).
- **Careful with `simulated_pmem.bin` kept in git** — once the format changes, old images are unreadable; delete/regenerate in tests. Add `simulated_pmem*.bin` to `.gitignore` additions except keep one seed image only if the demo script creates it.
- **Bitmap persistence pacing**: persisting the bitmap bit per pkg-group allocation is the paper-permitted `P | mcp` non-ordering; do NOT add an ordering point between package and bitmap persist (that would reintroduce a JFS-style order and defeat the design).