/*
 * ============================================================================
 * JOURNAL.C - Metadata Journaling for VSFS (Very Simple File System)
 * ============================================================================
 * 
 * WHAT IS JOURNALING?
 * -------------------
 * Journaling is a technique to prevent filesystem corruption during crashes.
 * 
 * THE PROBLEM:
 * When creating a file, we need to update multiple blocks on disk:
 *   1. Inode Bitmap (mark that an inode is now used)
 *   2. Inode Table (store the new file's metadata)
 *   3. Directory Data (add the filename to the directory)
 * 
 * If the computer crashes BETWEEN these writes, the filesystem becomes
 * corrupted (inconsistent). For example:
 *   - Bitmap says inode 5 is used
 *   - But inode 5 has no data
 *   - Directory has no entry pointing to inode 5
 *   = CORRUPTION!
 * 
 * THE SOLUTION (JOURNALING):
 * Instead of writing directly to disk, we:
 *   1. Write ALL changes to a special "journal" area first
 *   2. Write a "COMMIT" record to mark the transaction as complete
 *   3. Later, "install" applies these changes to actual disk locations
 * 
 * If we crash:
 *   - Before COMMIT: We discard the incomplete transaction (no harm done)
 *   - After COMMIT: We can replay from the journal (data is safe)
 * 
 * ============================================================================
 */

#include <errno.h>    /* For error codes like ENOENT */
#include <fcntl.h>    /* For open() flags like O_RDWR */
#include <stdint.h>   /* For fixed-size types like uint32_t */
#include <stdio.h>    /* For printf(), fprintf() */
#include <stdlib.h>   /* For exit(), malloc() */
#include <string.h>   /* For memcpy(), strcmp(), strlen() */
#include <time.h>     /* For time() to get current timestamp */
#include <unistd.h>   /* For read(), write(), close() */

/* ============================================================================
 * CONSTANTS - These define the layout of our filesystem
 * ============================================================================
 * 
 * DISK LAYOUT (Block numbers):
 * 
 *   Block 0       = Superblock (filesystem metadata)
 *   Blocks 1-16   = Journal area (16 blocks = 65,536 bytes)
 *   Block 17      = Inode Bitmap (which inodes are used)
 *   Block 18      = Data Bitmap (which data blocks are used)
 *   Blocks 19-20  = Inode Table (stores 64 inodes, 32 per block)
 *   Blocks 21-84  = Data Blocks (actual file/directory contents)
 * 
 */

#define FS_MAGIC          0x56534653U  /* "VSFS" in hex - identifies valid filesystem */
#define JOURNAL_MAGIC     0x4A524E4CU  /* "JRNL" in hex - identifies valid journal */

#define BLOCK_SIZE        4096U   /* Each block is 4KB (4096 bytes) */
#define INODE_SIZE         128U   /* Each inode is 128 bytes */
#define JOURNAL_BLOCK_IDX    1U   /* Journal starts at block 1 */
#define JOURNAL_BLOCKS      16U   /* Journal uses 16 blocks */
#define INODE_BLOCKS         2U   /* Inode table uses 2 blocks */
#define DATA_BLOCKS         64U   /* We have 64 data blocks */

/* Calculate block indices based on layout */
#define INODE_BMAP_IDX     (JOURNAL_BLOCK_IDX + JOURNAL_BLOCKS)  /* = 17 */
#define DATA_BMAP_IDX      (INODE_BMAP_IDX + 1U)                 /* = 18 */
#define INODE_START_IDX    (DATA_BMAP_IDX + 1U)                  /* = 19 */
#define DATA_START_IDX     (INODE_START_IDX + INODE_BLOCKS)      /* = 21 */
#define TOTAL_BLOCKS       (DATA_START_IDX + DATA_BLOCKS)        /* = 85 */
#define DIRECT_POINTERS     8U    /* Each inode has 8 direct block pointers */

/* Journal capacity = 16 blocks × 4096 bytes = 65,536 bytes total */
#define JOURNAL_CAPACITY   (JOURNAL_BLOCKS * BLOCK_SIZE)
#define DEFAULT_IMAGE      "vsfs.img"  /* Default disk image filename */

/* Record types used in the journal */
#define RECORD_TYPE_DATA   1U   /* DATA record: contains a block to write */
#define RECORD_TYPE_COMMIT 2U   /* COMMIT record: marks transaction as complete */

/* ============================================================================
 * DATA STRUCTURES
 * ============================================================================
 */

/*
 * SUPERBLOCK - The "master table of contents" (128 bytes)
 * 
 * This is stored at Block 0 and tells us where everything is on the disk.
 * It's the first thing we read when mounting a filesystem.
 */
struct superblock {
    uint32_t magic;         /* Must be 0x56534653 to be valid */
    uint32_t block_size;    /* Size of each block (4096) */
    uint32_t total_blocks;  /* Total number of blocks (85) */
    uint32_t inode_count;   /* Total number of inodes (64) */

    uint32_t journal_block; /* Where journal starts (block 1) */
    uint32_t inode_bitmap;  /* Where inode bitmap is (block 17) */
    uint32_t data_bitmap;   /* Where data bitmap is (block 18) */
    uint32_t inode_start;   /* Where inode table starts (block 19) */
    uint32_t data_start;    /* Where data blocks start (block 21) */

    uint8_t  _pad[128 - 9 * 4];  /* Padding to make struct exactly 128 bytes */
};

/*
 * INODE - Stores metadata about a file or directory (128 bytes)
 * 
 * Every file and directory has exactly one inode.
 * The inode does NOT store the filename - that's in the directory entry.
 * 
 * Types:
 *   0 = unused/free inode
 *   1 = regular file
 *   2 = directory
 */
struct inode {
    uint16_t type;      /* 0=free, 1=file, 2=directory */
    uint16_t links;     /* Number of directory entries pointing to this inode */
    uint32_t size;      /* Size of file contents in bytes */

    uint32_t direct[DIRECT_POINTERS];  /* Block numbers containing file data */
                                        /* direct[0] = first data block, etc. */

    uint32_t ctime;     /* Creation time (Unix timestamp) */
    uint32_t mtime;     /* Last modification time */

    uint8_t _pad[128 - (2 + 2 + 4 + DIRECT_POINTERS * 4 + 4 + 4)];  /* Padding */
};

/*
 * DIRECTORY ENTRY (DIRENT) - Maps a filename to an inode (32 bytes)
 * 
 * Directories are just files that contain a list of these entries.
 * Each entry says: "the file named X is stored in inode Y"
 */
struct dirent {
    uint32_t inode;     /* Inode number this name refers to */
    char name[28];      /* The filename (null-terminated, max 27 chars) */
};

/*
 * JOURNAL HEADER - Stored at the very beginning of the journal area
 * 
 * This tells us how much of the journal is currently in use.
 */
struct journal_header {
    uint32_t magic;        /* Must be 0x4A524E4C ("JRNL") to be valid */
    uint32_t nbytes_used;  /* Number of bytes used in journal (including header) */
};

/*
 * DATA RECORD - Stores one block of data to be written
 * 
 * When we want to write a block, we first store it here in the journal.
 * Later, during "install", we copy this data to its actual location.
 * 
 * Size: 4 + 4 + 4096 = 4104 bytes
 */
struct data_record {
    uint32_t type;         /* Must be 1 (RECORD_TYPE_DATA) */
    uint32_t block_no;     /* Which block on disk this data belongs to */
    uint8_t  data[BLOCK_SIZE];  /* The actual 4096 bytes of block data */
};

/*
 * COMMIT RECORD - Marks the end of a transaction
 * 
 * When we see this, we know all the DATA records before it are complete
 * and safe to apply to the disk.
 * 
 * Size: 4 bytes
 */
struct commit_record {
    uint32_t type;         /* Must be 2 (RECORD_TYPE_COMMIT) */
};

/* Compile-time checks to ensure our structs are the right size */
_Static_assert(sizeof(struct superblock) == 128, "superblock must be 128 bytes");
_Static_assert(sizeof(struct inode) == 128, "inode must be 128 bytes");
_Static_assert(sizeof(struct dirent) == 32, "dirent must be 32 bytes");

/* ============================================================================
 * HELPER FUNCTIONS
 * ============================================================================
 */

/*
 * die() - Print error message and exit program
 * 
 * Used when we encounter a fatal error that we can't recover from.
 */
static void die(const char *msg) {
    perror(msg);      /* Print the error message with system error description */
    exit(EXIT_FAILURE);  /* Exit with error code */
}

/*
 * pread_block() - Read one block from the disk
 * 
 * Parameters:
 *   fd          - File descriptor of the disk image
 *   block_index - Which block to read (0, 1, 2, ...)
 *   buf         - Buffer to store the data (must be BLOCK_SIZE bytes)
 * 
 * pread() is like read(), but lets us specify an offset (position)
 * without changing the file's current position.
 */
static void pread_block(int fd, uint32_t block_index, void *buf) {
    /* Calculate byte offset: block 0 starts at byte 0, block 1 at byte 4096, etc. */
    off_t offset = (off_t)block_index * BLOCK_SIZE;
    
    /* Read BLOCK_SIZE bytes from the file at the specified offset */
    ssize_t n = pread(fd, buf, BLOCK_SIZE, offset);
    
    /* Check if we read the expected number of bytes */
    if (n != (ssize_t)BLOCK_SIZE) {
        die("pread_block");
    }
}

/*
 * pwrite_block() - Write one block to the disk
 * 
 * Parameters:
 *   fd          - File descriptor of the disk image
 *   block_index - Which block to write (0, 1, 2, ...)
 *   buf         - Buffer containing the data (must be BLOCK_SIZE bytes)
 */
static void pwrite_block(int fd, uint32_t block_index, const void *buf) {
    off_t offset = (off_t)block_index * BLOCK_SIZE;
    ssize_t n = pwrite(fd, buf, BLOCK_SIZE, offset);
    if (n != (ssize_t)BLOCK_SIZE) {
        die("pwrite_block");
    }
}

/*
 * pread_journal() - Read bytes from the journal area
 * 
 * The journal is treated as one continuous byte array spanning blocks 1-16.
 * This function lets us read any portion of it.
 * 
 * Parameters:
 *   fd     - File descriptor of the disk image
 *   offset - Byte offset WITHIN the journal (0 = start of block 1)
 *   buf    - Buffer to store the data
 *   len    - Number of bytes to read
 */
static void pread_journal(int fd, uint32_t offset, void *buf, size_t len) {
    /* Convert journal offset to disk offset */
    /* Journal starts at block 1, so add JOURNAL_BLOCK_IDX * BLOCK_SIZE */
    off_t disk_offset = (off_t)JOURNAL_BLOCK_IDX * BLOCK_SIZE + offset;
    ssize_t n = pread(fd, buf, len, disk_offset);
    if (n != (ssize_t)len) {
        die("pread_journal");
    }
}

/*
 * pwrite_journal() - Write bytes to the journal area
 * 
 * Parameters:
 *   fd     - File descriptor of the disk image
 *   offset - Byte offset WITHIN the journal
 *   buf    - Buffer containing the data
 *   len    - Number of bytes to write
 */
static void pwrite_journal(int fd, uint32_t offset, const void *buf, size_t len) {
    off_t disk_offset = (off_t)JOURNAL_BLOCK_IDX * BLOCK_SIZE + offset;
    ssize_t n = pwrite(fd, buf, len, disk_offset);
    if (n != (ssize_t)len) {
        die("pwrite_journal");
    }
}

/*
 * bitmap_test() - Check if a bit is set in a bitmap
 * 
 * Bitmaps use 1 bit per item. If bit is 1, the item is used; if 0, it's free.
 * 
 * How it works:
 *   - index / 8 = which byte the bit is in
 *   - index % 8 = which bit within that byte (0-7)
 *   - We shift right and mask with 0x1 to get just that bit
 * 
 * Example: To check bit 10
 *   - Byte 1 (10/8 = 1), Bit 2 (10%8 = 2)
 *   - Shift byte 1 right by 2, then AND with 1
 * 
 * Returns: 1 if bit is set, 0 if not
 */
static int bitmap_test(const uint8_t *bitmap, uint32_t index) {
    return (bitmap[index / 8] >> (index % 8)) & 0x1;
}

/*
 * bitmap_set() - Set a bit in a bitmap to 1 (mark as used)
 * 
 * We OR the byte with a 1 shifted to the right position.
 * 
 * Example: To set bit 10
 *   - Byte 1, Bit 2
 *   - OR byte 1 with (1 << 2) = 0b00000100
 */
static void bitmap_set(uint8_t *bitmap, uint32_t index) {
    bitmap[index / 8] |= (uint8_t)(1U << (index % 8));
}

/* ============================================================================
 * CREATE COMMAND
 * ============================================================================
 * 
 * ./journal create <filename>
 * 
 * This creates a new empty file in the root directory, but using journaling.
 * The changes are written to the JOURNAL only, not to the actual disk blocks.
 * You must run "./journal install" afterward to apply the changes.
 * 
 * STEPS:
 * 1. Read the superblock to verify valid filesystem
 * 2. Read the inode bitmap to find a free inode
 * 3. Read the root inode and its directory data
 * 4. Find a free slot in the directory
 * 5. Prepare UPDATED versions of all affected blocks (in memory)
 * 6. Write DATA records to journal for each updated block
 * 7. Write COMMIT record to finalize the transaction
 * 
 */
static int do_create(const char *image_path, const char *filename) {
    
    /* ========== VALIDATION ========== */
    
    /* Check filename length (must be 1-27 characters to fit in dirent.name) */
    if (strlen(filename) == 0 || strlen(filename) >= 28) {
        fprintf(stderr, "Error: filename must be 1-27 characters\n");
        return 1;
    }

    /* Open the disk image file for reading and writing */
    int fd = open(image_path, O_RDWR);
    if (fd < 0) {
        die("open");
    }

    /* ========== STEP 1: READ AND VERIFY SUPERBLOCK ========== */
    
    struct superblock sb;
    pread_block(fd, 0, &sb);  /* Superblock is at block 0 */
    
    /* Verify this is a valid VSFS filesystem */
    if (sb.magic != FS_MAGIC) {
        fprintf(stderr, "Error: invalid filesystem magic\n");
        close(fd);
        return 1;
    }

    /* ========== STEP 2: FIND A FREE INODE ========== */
    
    /* Read the inode bitmap (block 17) */
    uint8_t inode_bitmap[BLOCK_SIZE];
    pread_block(fd, INODE_BMAP_IDX, inode_bitmap);

    /* Scan the bitmap to find a free inode */
    /* Skip inode 0 because it's reserved for the root directory */
    uint32_t inode_count = sb.inode_count;  /* = 64 */
    uint32_t new_inode_idx = 0;
    int found_inode = 0;
    
    for (uint32_t i = 1; i < inode_count; i++) {
        if (!bitmap_test(inode_bitmap, i)) {
            /* Found a free inode! */
            new_inode_idx = i;
            found_inode = 1;
            break;
        }
    }
    
    if (!found_inode) {
        fprintf(stderr, "Error: no free inodes available\n");
        close(fd);
        return 1;
    }

    /* ========== STEP 3: READ ROOT DIRECTORY ========== */
    
    /* Read the inode table block that contains the root inode */
    /* Root inode is inode 0, which is in block INODE_START_IDX (block 19) */
    uint8_t inode_block[BLOCK_SIZE];
    uint32_t inode_block_idx = INODE_START_IDX + (new_inode_idx / (BLOCK_SIZE / INODE_SIZE));
    pread_block(fd, inode_block_idx, inode_block);

    /* Read root inode specifically (it's the first inode in block 19) */
    uint8_t root_inode_block[BLOCK_SIZE];
    pread_block(fd, INODE_START_IDX, root_inode_block);
    struct inode *root_inode = (struct inode *)root_inode_block;

    /* Read the root directory's data block */
    /* root_inode->direct[0] tells us which block has the directory entries */
    uint32_t root_data_block_idx = root_inode->direct[0];
    uint8_t dir_data[BLOCK_SIZE];
    pread_block(fd, root_data_block_idx, dir_data);
    struct dirent *entries = (struct dirent *)dir_data;

    /* ========== STEP 4: FIND FREE DIRECTORY SLOT ========== */
    
    /* Each block can hold BLOCK_SIZE / sizeof(dirent) = 4096 / 32 = 128 entries */
    uint32_t max_entries = BLOCK_SIZE / sizeof(struct dirent);
    int free_entry_idx = -1;
    
    /* Scan existing entries */
    for (uint32_t i = 0; i < max_entries; i++) {
        /* A free entry has inode=0 and empty name */
        if (entries[i].inode == 0 && entries[i].name[0] == '\0') {
            if (free_entry_idx == -1) {
                free_entry_idx = (int)i;  /* Remember first free slot */
            }
        } else if (strcmp(entries[i].name, filename) == 0) {
            /* File already exists! */
            fprintf(stderr, "Error: file '%s' already exists\n", filename);
            close(fd);
            return 1;
        }
    }

    /* If no free slot found, maybe we can extend the directory */
    uint32_t current_entries = root_inode->size / sizeof(struct dirent);
    if (free_entry_idx == -1 && current_entries < max_entries) {
        free_entry_idx = (int)current_entries;  /* Add at end */
    }

    if (free_entry_idx == -1) {
        fprintf(stderr, "Error: root directory is full\n");
        close(fd);
        return 1;
    }

    /* ========== STEP 5: PREPARE UPDATED BLOCKS IN MEMORY ========== */
    
    /*
     * IMPORTANT: We DO NOT write to disk yet!
     * We prepare the new versions of all blocks in memory first.
     */

    /* ----- UPDATE 1: Mark new inode as used in bitmap ----- */
    uint8_t new_inode_bitmap[BLOCK_SIZE];
    memcpy(new_inode_bitmap, inode_bitmap, BLOCK_SIZE);  /* Copy original */
    bitmap_set(new_inode_bitmap, new_inode_idx);          /* Set the bit */

    /* ----- UPDATE 2: Create the new inode AND update root inode ----- */
    /*
     * IMPORTANT: The new inode might be in the same block as root inode!
     * Since BLOCK_SIZE=4096 and INODE_SIZE=128, each block holds 32 inodes.
     * Inodes 0-31 are in block 19, inodes 32-63 are in block 20.
     * So if new_inode_idx < 32, it's in the same block as root (inode 0).
     * We need to update BOTH in the same block to avoid overwriting.
     */
    
    time_t now = time(NULL);  /* Get current time for timestamps */
    
    /* Start with the root inode block (which contains inode 0) */
    uint8_t combined_inode_block[BLOCK_SIZE];
    memcpy(combined_inode_block, root_inode_block, BLOCK_SIZE);
    
    /* Update root inode (inode 0) - increase directory size if needed */
    struct inode *updated_root = (struct inode *)combined_inode_block;
    uint32_t needed_size = ((uint32_t)free_entry_idx + 1) * sizeof(struct dirent);
    if (needed_size > updated_root->size) {
        updated_root->size = needed_size;  /* Directory grew */
    }
    updated_root->mtime = (uint32_t)now;  /* Update modification time */
    
    /* Calculate where in the block the new inode should go */
    /* Each inode is 128 bytes, so inode N is at offset N*128 within its block */
    uint32_t inode_offset_in_block = (new_inode_idx % (BLOCK_SIZE / INODE_SIZE)) * INODE_SIZE;
    struct inode *new_inode = (struct inode *)(combined_inode_block + inode_offset_in_block);
    
    /* Initialize the new inode */
    memset(new_inode, 0, sizeof(struct inode));  /* Clear all fields */
    new_inode->type = 1;           /* 1 = regular file */
    new_inode->links = 1;          /* One directory entry points to it */
    new_inode->size = 0;           /* Empty file (no content) */
    new_inode->ctime = (uint32_t)now;
    new_inode->mtime = (uint32_t)now;

    /* ----- UPDATE 3: Add directory entry for the new file ----- */
    uint8_t new_dir_data[BLOCK_SIZE];
    memcpy(new_dir_data, dir_data, BLOCK_SIZE);  /* Copy original */
    struct dirent *new_entries = (struct dirent *)new_dir_data;
    
    /* Fill in the new entry */
    new_entries[free_entry_idx].inode = new_inode_idx;
    memset(new_entries[free_entry_idx].name, 0, 28);      /* Clear name buffer */
    strncpy(new_entries[free_entry_idx].name, filename, 27);  /* Copy filename */

    /* ========== STEP 6: WRITE TO JOURNAL ========== */
    
    /*
     * Now we write all our changes to the JOURNAL, not to the actual blocks.
     * The journal is a simple append-only log.
     * 
     * Journal layout:
     *   [HEADER] [DATA] [DATA] [DATA] [COMMIT] [DATA] [DATA] [COMMIT] ...
     */

    /* Read current journal header to find where to append */
    struct journal_header jh;
    pread_journal(fd, 0, &jh, sizeof(jh));
    
    /* Initialize journal if this is the first use */
    if (jh.magic != JOURNAL_MAGIC) {
        jh.magic = JOURNAL_MAGIC;
        jh.nbytes_used = sizeof(struct journal_header);  /* Header takes 8 bytes */
    }

    uint32_t write_offset = jh.nbytes_used;  /* Append after existing data */

    /* Calculate how much space we need */
    /* We're writing 3 DATA records + 1 COMMIT record */
    size_t data_record_size = sizeof(struct data_record);    /* ~4104 bytes */
    size_t commit_record_size = sizeof(struct commit_record); /* 4 bytes */
    size_t total_needed = 3 * data_record_size + commit_record_size;

    /* Check if journal has enough space */
    if (write_offset + total_needed > JOURNAL_CAPACITY) {
        fprintf(stderr, "Error: journal is full, run 'install' first\n");
        close(fd);
        return 1;
    }

    /* ----- Write DATA record 1: Updated inode bitmap ----- */
    struct data_record dr1;
    dr1.type = RECORD_TYPE_DATA;
    dr1.block_no = INODE_BMAP_IDX;  /* This data belongs at block 17 */
    memcpy(dr1.data, new_inode_bitmap, BLOCK_SIZE);
    pwrite_journal(fd, write_offset, &dr1, data_record_size);
    write_offset += data_record_size;

    /* ----- Write DATA record 2: Updated inode table block ----- */
    /* This contains BOTH the updated root inode AND the new file's inode */
    struct data_record dr2;
    dr2.type = RECORD_TYPE_DATA;
    dr2.block_no = INODE_START_IDX;  /* Block 19 */
    memcpy(dr2.data, combined_inode_block, BLOCK_SIZE);
    pwrite_journal(fd, write_offset, &dr2, data_record_size);
    write_offset += data_record_size;

    /* ----- Write DATA record 3: Updated directory data block ----- */
    struct data_record dr3;
    dr3.type = RECORD_TYPE_DATA;
    dr3.block_no = root_data_block_idx;  /* Usually block 21 */
    memcpy(dr3.data, new_dir_data, BLOCK_SIZE);
    pwrite_journal(fd, write_offset, &dr3, data_record_size);
    write_offset += data_record_size;

    /* ----- Write COMMIT record ----- */
    /* This marks the transaction as complete and safe to apply */
    struct commit_record cr;
    cr.type = RECORD_TYPE_COMMIT;
    pwrite_journal(fd, write_offset, &cr, commit_record_size);
    write_offset += commit_record_size;

    /* ========== STEP 7: UPDATE JOURNAL HEADER ========== */
    
    /* Update the header to reflect the new bytes used */
    jh.nbytes_used = write_offset;
    pwrite_journal(fd, 0, &jh, sizeof(jh));

    /* Sync to ensure everything is written to disk */
    if (fsync(fd) < 0) {
        die("fsync");
    }

    close(fd);
    
    printf("Journaled creation of file '%s' (inode %u)\n", filename, new_inode_idx);
    printf("Run './journal install' to apply changes to disk.\n");
    return 0;
}

/* ============================================================================
 * INSTALL COMMAND
 * ============================================================================
 * 
 * ./journal install
 * 
 * This reads the journal and applies (replays) all committed transactions
 * to their actual disk locations.
 * 
 * ALGORITHM:
 * 1. Read journal header to find how much data is in the journal
 * 2. Parse through records:
 *    - DATA record: Save in memory (don't apply yet)
 *    - COMMIT record: Apply all pending DATA records to disk
 * 3. If we reach the end with pending DATA but no COMMIT:
 *    - Discard them (incomplete transaction = crash recovery)
 * 4. Clear the journal when done
 * 
 */
static int do_install(const char *image_path) {
    
    /* Open the disk image */
    int fd = open(image_path, O_RDWR);
    if (fd < 0) {
        die("open");
    }

    /* Read journal header */
    struct journal_header jh;
    pread_journal(fd, 0, &jh, sizeof(jh));

    /* Check if journal is valid/initialized */
    if (jh.magic != JOURNAL_MAGIC) {
        printf("Journal is empty or uninitialized. Nothing to install.\n");
        close(fd);
        return 0;
    }

    /* Check if journal has any transactions */
    if (jh.nbytes_used <= sizeof(struct journal_header)) {
        printf("Journal has no transactions to install.\n");
        close(fd);
        return 0;
    }

    /* ========== REPLAY JOURNAL ========== */
    
    uint32_t read_offset = sizeof(struct journal_header);  /* Start after header */
    uint32_t end_offset = jh.nbytes_used;
    
    /*
     * We need to collect DATA records until we see a COMMIT.
     * Only then do we apply them to disk.
     * This ensures atomicity: either ALL changes apply or NONE.
     */
    #define MAX_DATA_RECORDS 64
    struct data_record pending_records[MAX_DATA_RECORDS];
    uint32_t pending_count = 0;
    uint32_t transactions_installed = 0;

    /* Parse through the journal */
    while (read_offset < end_offset) {
        
        /* Peek at the record type (first 4 bytes of any record) */
        uint32_t record_type;
        pread_journal(fd, read_offset, &record_type, sizeof(record_type));

        if (record_type == RECORD_TYPE_DATA) {
            /*
             * Found a DATA record.
             * Store it in our pending list - don't apply yet!
             */
            if (pending_count >= MAX_DATA_RECORDS) {
                fprintf(stderr, "Error: too many data records in transaction\n");
                close(fd);
                return 1;
            }
            
            /* Read the full DATA record */
            pread_journal(fd, read_offset, &pending_records[pending_count], 
                         sizeof(struct data_record));
            pending_count++;
            read_offset += sizeof(struct data_record);

        } else if (record_type == RECORD_TYPE_COMMIT) {
            /*
             * Found a COMMIT record!
             * Now we apply all pending DATA records to their actual disk locations.
             * This is the "replay" or "redo" phase.
             */
            for (uint32_t i = 0; i < pending_count; i++) {
                /* Write the data to its actual block on disk */
                pwrite_block(fd, pending_records[i].block_no, pending_records[i].data);
            }
            
            printf("Installed transaction with %u block writes.\n", pending_count);
            transactions_installed++;
            pending_count = 0;  /* Clear pending list for next transaction */
            read_offset += sizeof(struct commit_record);

        } else {
            /*
             * Unknown record type - journal might be corrupted.
             * Stop processing to be safe.
             */
            fprintf(stderr, "Warning: unknown record type %u at offset %u, stopping\n", 
                    record_type, read_offset);
            break;
        }
    }

    /* ========== HANDLE INCOMPLETE TRANSACTIONS ========== */
    
    /*
     * If we have pending DATA records but no COMMIT, it means the system
     * crashed in the middle of writing a transaction.
     * 
     * SAFETY: We discard these incomplete records.
     * This is the key to crash recovery - incomplete = never happened!
     */
    if (pending_count > 0) {
        printf("Discarding %u uncommitted data records.\n", pending_count);
    }

    /* ========== CLEAR THE JOURNAL ========== */
    
    /* Reset journal to empty state */
    jh.nbytes_used = sizeof(struct journal_header);
    pwrite_journal(fd, 0, &jh, sizeof(jh));

    /* Sync to disk */
    if (fsync(fd) < 0) {
        die("fsync");
    }

    close(fd);
    printf("Journal install complete. %u transaction(s) applied.\n", transactions_installed);
    return 0;
}

/* ============================================================================
 * MAIN FUNCTION
 * ============================================================================
 */

/*
 * Print usage help
 */
static void print_usage(const char *prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s create <filename> [image]  - Journal file creation\n", prog);
    fprintf(stderr, "  %s install [image]            - Apply journal to disk\n", prog);
}

/*
 * Main entry point
 * 
 * Usage:
 *   ./journal create myfile.txt     - Create file (journaled)
 *   ./journal install               - Apply journal changes
 *   ./journal create myfile.txt vsfs2.img   - Use different image
 */
int main(int argc, char *argv[]) {
    
    /* Need at least one argument (the command) */
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *command = argv[1];

    if (strcmp(command, "create") == 0) {
        /* CREATE command */
        if (argc < 3) {
            fprintf(stderr, "Error: 'create' requires a filename\n");
            print_usage(argv[0]);
            return 1;
        }
        const char *filename = argv[2];
        const char *image = (argc > 3) ? argv[3] : DEFAULT_IMAGE;
        return do_create(image, filename);

    } else if (strcmp(command, "install") == 0) {
        /* INSTALL command */
        const char *image = (argc > 2) ? argv[2] : DEFAULT_IMAGE;
        return do_install(image);

    } else {
        /* Unknown command */
        fprintf(stderr, "Error: unknown command '%s'\n", command);
        print_usage(argv[0]);
        return 1;
    }
}
