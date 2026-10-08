# WOLVES: Metadata Write-Once File System Prototype

A C++ user-space prototype of **WOLVES**, a Metadata Write-Once File System designed for Persistent Memory (PM). This project is based on the architectural concepts presented in the OSDI '25 paper: *"Fast and Synchronous Crash Consistency with Metadata Write-Once File System"*.

## 📖 About the Project

With the advent of byte-addressable Persistent Memory (like Intel Optane), file systems are moving towards synchronous crash consistency. However, traditional Journaling and Log-Structured file systems incur massive performance penalties due to redundant, ordered metadata writes. 

WOLVES solves this by aggregating the metadata for any file operation into a single, checksum-protected **Write-Once Package**.

This repository contains a **Version 1 Prototype** built for a college OS project. It proves the validity of the Write-Once algorithms and the Package Translation Layer without the massive overhead of compiling a Linux Kernel Module.

## ✨ Key Features Implemented

* **Simulated Persistent Memory:** Uses an 8-byte aligned memory array backed by a binary file (`simulated_pmem.bin`) to emulate PM behavior and persistence.
* **Write-Once Packages:** A 64-byte metadata structure that captures file operations (`CREATE`, `WRITE`, `DELETE`) with a single ordering point.
* **Package Translation Layer (PTL):** A recovery engine that scans packages sequentially on boot, verifies checksums, and dynamically rebuilds the Virtual File System (VFS) in RAM.
* **FUSE Integration (Linux):** Can be built as a mountable file system in Linux/WSL using FUSE (Filesystem in Userspace).

## 🚀 Getting Started

### Option 1: Standalone Simulator (Windows / Linux)
The standalone simulator runs a hardcoded test that creates files, simulates a power-loss crash, and successfully recovers the data on the next boot.

```bash
mkdir build && cd build
cmake ..
cmake --build .
./wolves_sim
```

### Option 2: Mountable FUSE File System (Linux / WSL Only)
You can compile this project as a real, mountable file system using FUSE.

**Prerequisites:**
```bash
sudo apt update
sudo apt install cmake g++ pkg-config libfuse-dev
```

**Build and Mount:**
```bash
mkdir build_linux && cd build_linux
cmake ..
make

# Create a mount point and mount the file system
mkdir ~/wolves_drive
./wolves_fuse -f ~/wolves_drive
```
*You can now open a second terminal and use standard commands (`cd`, `ls`, `cat`, `echo`) inside `~/wolves_drive`!*

## 🔮 Future Scope
* **OS-Native Memory Mapping:** Transition from manual array flushing to `mmap` / `MapViewOfFile`.
* **Coarse Persistence & GC:** Implement garbage collection to reclaim space from deleted or overwritten packages.
* **Directory Support:** Expand the package enum to support hierarchical directory trees.

## 📄 References
* Pan, Y. et al. *"Fast and Synchronous Crash Consistency with Metadata Write-Once File System."* Proceedings of the 19th USENIX Symposium on Operating Systems Design and Implementation (OSDI '25).
