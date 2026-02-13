# VSFS Metadata Journaling (Crash Consistency)

A C implementation of **Write-Ahead Logging (Journaling)** for a Very Simple File System (VSFS). This project demonstrates how modern operating systems prevent disk corruption during power failures by treating metadata updates as atomic transactions.

**Course:** CSE321 - Operating Systems  
**Language:** C

## 💥 The Problem
In a standard file system, creating a file involves three separate write operations to the disk:
1.  **Inode Bitmap:** Mark an inode as "used".
2.  **Inode Table:** Initialize the file's metadata (size, permissions, etc.).
3.  **Directory Data:** Link the filename to the inode.

If the system crashes (power loss) after step 1 but before step 3, the filesystem becomes **inconsistent** (corrupted). An inode is marked "used" but no file points to it.

## 🛡️ The Solution: Journaling
This project implements **Journaling** to ensure crash consistency. Instead of writing directly to the final disk locations, we:
1.  **Log:** Write all pending changes to a designated "Journal" area on the disk.
2.  **Commit:** Write a special "Transaction End" record.
3.  **Checkpoint (Install):** Copy the data from the journal to the actual disk structures.

If a crash occurs *before* the commit, the transaction is discarded (no corruption). If it occurs *after* the commit, the system can replay the journal to restore data.

## 💾 Disk Layout
The system operates on a disk image with the following block structure:

| Block Index | Region | Purpose |
|:---:|:---|:---|
| **0** | Superblock | Filesystem metadata (Magic number, block counts) |
| **1 - 16** | **Journal Area** | **Circular log for recording transactions** |
| **17** | Inode Bitmap | Tracks allocated inodes |
| **18** | Data Bitmap | Tracks allocated data blocks |
| **19 - 20** | Inode Table | Stores file metadata (128 bytes per inode) |
| **21 - 84** | Data Blocks | Stores actual file/directory content |

## 🚀 Usage

### 1. Compile
```bash
gcc -o journal journal.c
```

### 2. Create a File (Journal Write)
```bash
./journal create my_file.txt
```
Output: Journaled creation of file 'my_file.txt'. Run './journal install' to apply changes.

### 3. Install Changes (Checkpoint)
```bash
./journal install
```
Output: Installed transaction with 3 block writes.

## ⚠️ Notes
1. This tool requires a valid VSFS disk image (default: vsfs.img) to operate.
2. The journal is fixed at 16 blocks (64KB), allowing for small transactions.
