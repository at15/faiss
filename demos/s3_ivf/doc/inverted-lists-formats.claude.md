# Faiss Inverted Lists Serialization Formats

This document explains the serialization format differences between the three main inverted list implementations in Faiss and how they are stored on disk.

## Overview

Faiss uses different serialization formats for different inverted list types, each optimized for specific use cases:

1. **ArrayInvertedLists** (`"ilar"`) - Standard in-memory format (built-in)
2. **OnDiskInvertedLists** (`"ilod"`) - Memory-mapped format for huge datasets
3. **BlockInvertedLists** (`"ilbl"`) - Compressed block-based format

## How Registration Works

### Built-in vs Hook-based Serialization

**ArrayInvertedLists is special** - it's hardcoded directly in the serialization code:

```cpp
void write_InvertedLists(const InvertedLists* ils, IOWriter* f) {
    if (ils == nullptr) {
        // Handle NULL case
    } else if (dynamic_cast<const ArrayInvertedLists*>(ils)) {
        // ✅ BUILT-IN: Handle ArrayInvertedLists directly
        uint32_t h = fourcc("ilar");
        WRITE1(h);
        // ... write data directly ...
    } else {
        // ❌ CUSTOM: Look up registered hook or fail
        InvertedListsIOHook* callback =
                InvertedListsIOHook::lookup_classname(typeid(*ils).name());
        callback->write(ils, f);
    }
}
```

**Custom types** (OnDisk, Block) register hooks at static initialization:

```cpp
// In InvertedListsIOHook.cpp
struct IOHookTable : std::vector<InvertedListsIOHook*> {
    IOHookTable() {
        push_back(new OnDiskInvertedListsIOHook());  // Auto-registered
        push_back(new BlockInvertedListsIOHook());   // Auto-registered
    }
};

static IOHookTable InvertedListsIOHook_table;  // Static init before main()
```

## 1. ArrayInvertedLists (`"ilar"`)

**Purpose**: Standard in-memory format, optimized for full serialization

**Key Features**:
- **Adaptive**: Uses "full" or "sparse" format based on density (>50% non-empty = full)
- **Contiguous layout**: All data in one continuous buffer (excellent for mmap)
- **No metadata overhead**: Just sizes + raw data
- **Built-in**: No hook registration needed

### Disk Layout (Full format - >50% lists non-empty)

```
┌────────────────────────────────────────────────────────────┐
│ FourCC: "ilar" (4 bytes)                                   │
├────────────────────────────────────────────────────────────┤
│ nlist (8 bytes)                                            │
├────────────────────────────────────────────────────────────┤
│ code_size (8 bytes)                                        │
├────────────────────────────────────────────────────────────┤
│ list_type: "full" (4 bytes)                                │
├────────────────────────────────────────────────────────────┤
│ sizes array (nlist × 8 bytes)                              │
│ ┌──────────────────────────────────────┐                   │
│ │ size[0], size[1], ..., size[nlist-1] │                   │
│ └──────────────────────────────────────┘                   │
├────────────────────────────────────────────────────────────┤
│ ╔════════════════════════════════════╗                     │
│ ║ List 0 Data                        ║                     │
│ ╟────────────────────────────────────╢                     │
│ ║ codes[0] (size[0] × code_size)     ║ Contiguous         │
│ ║ ids[0]   (size[0] × 8 bytes)       ║ data buffer        │
│ ╟────────────────────────────────────╢                     │
│ ║ List 1 Data                        ║                     │
│ ║ codes[1] (size[1] × code_size)     ║ ← All lists        │
│ ║ ids[1]   (size[1] × 8 bytes)       ║   in sequence      │
│ ╟────────────────────────────────────╢                     │
│ ║ ...                                ║                     │
│ ╟────────────────────────────────────╢                     │
│ ║ List nlist-1 Data                  ║                     │
│ ║ codes[nlist-1]                     ║                     │
│ ║ ids[nlist-1]                       ║                     │
│ ╚════════════════════════════════════╝                     │
└────────────────────────────────────────────────────────────┘
```

### Disk Layout (Sparse format - ≤50% lists non-empty)

```
┌────────────────────────────────────────────────────────────┐
│ FourCC: "ilar" (4 bytes)                                   │
├────────────────────────────────────────────────────────────┤
│ nlist, code_size                                           │
├────────────────────────────────────────────────────────────┤
│ list_type: "sprs" (4 bytes)                                │
├────────────────────────────────────────────────────────────┤
│ sizes array (only non-empty lists)                         │
│ ┌──────────────────────────────────────┐                   │
│ │ [list_idx_0, size_0,                 │ Pairs of         │
│ │  list_idx_1, size_1,                 │ (index, size)    │
│ │  ...                                 │ for non-empty    │
│ │  list_idx_k, size_k]                 │ lists only       │
│ └──────────────────────────────────────┘                   │
├────────────────────────────────────────────────────────────┤
│ Data for non-empty lists only                              │
│ (same format as full, but skips empty lists)               │
└────────────────────────────────────────────────────────────┘
```

**Why adaptive?** Saves significant space when many inverted lists are empty (common after training but before adding all data).

## 2. OnDiskInvertedLists (`"ilod"`)

**Purpose**: Memory-mapped file format for datasets too large to fit in RAM

**Key Features**:
- **Metadata only in index**: Actual data stored in separate file
- **Memory-mapped access**: Can work with datasets larger than RAM
- **Pointer-based**: Uses offsets to locate data in external file
- **Free space tracking**: Slots system for efficient incremental adds

### Disk Layout (Index file)

```
┌────────────────────────────────────────────────────────────┐
│ FourCC: "ilod" (4 bytes)                                   │
├────────────────────────────────────────────────────────────┤
│ nlist (8 bytes)                                            │
├────────────────────────────────────────────────────────────┤
│ code_size (8 bytes)                                        │
├────────────────────────────────────────────────────────────┤
│ lists array (nlist × OnDiskOneList structs)                │
│ ┌──────────────────────────────────────┐                   │
│ │ struct OnDiskOneList {               │                   │
│ │   size_t size;      // # vectors     │ For each         │
│ │   size_t capacity;  // allocated     │ list             │
│ │   size_t offset;    // byte offset   │                  │
│ │ }                                    │                   │
│ │ list[0], list[1], ..., list[nlist-1] │                   │
│ └──────────────────────────────────────┘                   │
├────────────────────────────────────────────────────────────┤
│ slots array (free space tracking)                          │
│ ┌──────────────────────────────────────┐                   │
│ │ vector of (offset, capacity) pairs   │                   │
│ └──────────────────────────────────────┘                   │
├────────────────────────────────────────────────────────────┤
│ filename (path to external data file)                      │
│ ┌──────────────────────────────────────┐                   │
│ │ "path/to/invlists.dat" (string)      │                   │
│ └──────────────────────────────────────┘                   │
├────────────────────────────────────────────────────────────┤
│ totsize (total bytes in external file)                     │
└────────────────────────────────────────────────────────────┘
```

### External Data File Layout

```
External data file (e.g., "invlists.dat"):
┌────────────────────────────────────────────────────────────┐
│ Memory-mapped file (totsize bytes)                         │
│ ┌──────────────────────────────────────┐                   │
│ │ @ offset[0]: codes[0] + ids[0]       │                   │
│ │ @ offset[1]: codes[1] + ids[1]       │ Random           │
│ │ ...                                  │ access via        │
│ │ @ offset[k]: codes[k] + ids[k]       │ offsets          │
│ │ [free space slots]                   │                   │
│ └──────────────────────────────────────┘                   │
└────────────────────────────────────────────────────────────┘
```

**Why separate file?**
- Enables memory mapping for huge datasets
- Index file stays small and fast to load
- Can update data file without rewriting entire index

## 3. BlockInvertedLists (`"ilbl"`)

**Purpose**: Compressed storage using fixed-size blocks

**Key Features**:
- **Block compression**: Groups vectors into fixed-size blocks
- **Trade-off**: Reduced memory at cost of decompression overhead
- **Self-contained**: All data in index file (no external files)
- **Configurable**: `n_per_block` and `block_size` parameters

### Disk Layout

```
┌────────────────────────────────────────────────────────────┐
│ FourCC: "ilbl" (4 bytes)                                   │
├────────────────────────────────────────────────────────────┤
│ nlist (8 bytes)                                            │
├────────────────────────────────────────────────────────────┤
│ code_size (8 bytes)                                        │
├────────────────────────────────────────────────────────────┤
│ n_per_block (8 bytes)   ← vectors per block               │
├────────────────────────────────────────────────────────────┤
│ block_size (8 bytes)    ← bytes per compressed block      │
├────────────────────────────────────────────────────────────┤
│ For each list i in 0..nlist-1:                             │
│ ┌──────────────────────────────────────────────────────────┤
│ │ ids[i] vector                                            │
│ │ ┌────────────────────────────────────┐                   │
│ │ │ count (8 bytes)                    │                   │
│ │ │ id[0], id[1], ..., id[count-1]     │                   │
│ │ └────────────────────────────────────┘                   │
│ ├───��──────────────────────────────────────────────────────┤
│ │ codes[i] vector (COMPRESSED BLOCKS)                      │
│ │ ┌────────────────────────────────────┐                   │
│ │ │ count (8 bytes)                    │                   │
│ │ │ ╔══════════════════════════════╗   │                   │
│ │ │ ║ Block 0 (block_size bytes)   ║   │ Fixed-size       │
│ │ │ ║ n_per_block vectors packed   ║   │ blocks           │
│ │ │ ╟──────────────────────────────╢   │                   │
│ │ │ ║ Block 1 (block_size bytes)   ║   │                   │
│ │ │ ╟──────────────────────────────╢   │                   │
│ │ │ ║ ...                          ║   │                   │
│ │ │ ╚══════════════════════════════╝   │                   │
│ │ └────────────────────────────────────┘                   │
│ └──────────────────────────────────────────────────────────┤
│ (Repeat for all lists)                                     │
└────────────────────────────────────────────────────────────┘
```

**Why blocks?**
- Compression works better on groups of similar vectors
- Predictable memory layout
- Balance between compression ratio and access speed

## Comparison Table

| Feature | ArrayInvertedLists | OnDiskInvertedLists | BlockInvertedLists |
|---------|-------------------|--------------------|--------------------|
| **FourCC** | `"ilar"` | `"ilod"` | `"ilbl"` |
| **Storage** | All in index file | Metadata + external file | All in index file |
| **Memory footprint** | Full (all in RAM) | Small (mmap) | Medium (compressed) |
| **Access speed** | Fastest | Fast (mmap) | Medium (decompress) |
| **Compression** | None | None | Block-based |
| **Adaptive format** | ✅ Yes (full/sparse) | ❌ No | ❌ No |
| **External file** | ❌ No | ✅ Yes | ❌ No |
| **Registration** | Built-in | Hook-based | Hook-based |
| **Best for** | Standard use | Huge datasets | Memory-constrained |

## Example File Sizes

For 10,000,000 vectors × 128D float32 (~5GB raw data):

```
┌─────────────────────────────┬──────────────┬─────────────┐
│ Format                      │ Index File   │ Extra Files │
├─────────────────────────────┼──────────────┼─────────────┤
│ ArrayInvertedLists (full)   │ ~5.5 GB      │ 0 GB        │
│ ArrayInvertedLists (sparse) │ ~3 GB        │ 0 GB        │
│ OnDiskInvertedLists         │ ~10 MB       │ ~5 GB       │
│ BlockInvertedLists          │ ~3-4 GB      │ 0 GB        │
└─────────────────────────────┴──────────────┴─────────────┘
```

## FourCC Codes Reference

**FourCC** (Four Character Code) is a 4-byte identifier stored at the beginning of each serialized inverted list:

| FourCC | Type | Meaning |
|--------|------|---------|
| `"il00"` | NULL | No inverted lists |
| `"ilar"` | ArrayInvertedLists | InvertedLists ARray |
| `"ilod"` | OnDiskInvertedLists | InvertedLists On Disk |
| `"ilbl"` | BlockInvertedLists | InvertedLists BLock |
| `"full"` | - | Full format (ArrayInvertedLists) |
| `"sprs"` | - | Sparse format (ArrayInvertedLists) |

## Custom Inverted Lists (like S3BuildOnlyInvertedLists)

If you create a custom inverted list type that **doesn't** register an IOHook, you must convert it to `ArrayInvertedLists` before saving:

```cpp
// Your custom inverted lists
S3BuildOnlyInvertedLists s3_invlists(nlist, code_size);
index.replace_invlists(&s3_invlists, false);

// Add data...
index.add(n, vectors);

// Convert to ArrayInvertedLists before saving
ArrayInvertedLists array_invlists(nlist, code_size);
for (size_t i = 0; i < nlist; i++) {
    size_t list_size = s3_invlists.list_size(i);
    if (list_size > 0) {
        const uint8_t* codes = s3_invlists.get_codes(i);
        const idx_t* ids = s3_invlists.get_ids(i);
        array_invlists.add_entries(i, list_size, ids, codes);
    }
}

index.replace_invlists(&array_invlists, false);
write_index(&index, "index.faiss");  // ✅ Works!
```

This is the approach used by the RocksDB demo, since you can't serialize a database connection!

## Why These Differences?

Each format is optimized for specific use cases:

### ArrayInvertedLists
- **Why adaptive (full/sparse)?** Saves space when many lists are empty
- **Why contiguous?** Enables efficient memory-mapping and cache locality
- **Why built-in?** Most common use case, needs to be fast

### OnDiskInvertedLists
- **Why external file?** Dataset too large for RAM
- **Why store offsets?** Memory-map file and access data without loading
- **Why slots tracking?** Manage free space for incremental adds

### BlockInvertedLists
- **Why blocks?** Compress similar vectors together
- **Why fixed size?** Predictable memory layout, easier management
- **Why not external?** Trade compression for keeping in memory

## Key Takeaway

**Different use cases demand different trade-offs** between memory, speed, and storage. Choose the right format for your needs:

- **In-memory, standard use**: `ArrayInvertedLists`
- **Huge datasets (>RAM)**: `OnDiskInvertedLists`
- **Memory-constrained**: `BlockInvertedLists`
- **Custom storage (S3, DB, etc.)**: Build with custom type, save as `ArrayInvertedLists`
