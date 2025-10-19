# Index Reading Hook System and Memory Mapping

This document explains Faiss's advanced hook mechanism for loading inverted lists from `ArrayInvertedLists` format into custom implementations without copying data into RAM. This is the foundation for memory-mapped (mmap) and remote storage (S3, databases) backends.

## Overview

When saving a Faiss index, inverted lists are typically serialized as `ArrayInvertedLists` (fourcc `"ilar"`). On loading, Faiss normally deserializes this into an in-memory `ArrayInvertedLists` object. However, the **hook system** allows you to intercept this process and create a different `InvertedLists` implementation that:

- Memory-maps the data instead of copying to RAM (OnDiskInvertedLists)
- Lazy-loads from S3 or databases on-demand
- Uses shared memory segments
- Implements custom caching strategies

## Key Components

### 1. IO Flags

Defined in [faiss/index_io.h:34-65](../../../faiss/index_io.h#L34-L65):

```cpp
/// skip the storage for graph-based indexes
const int IO_FLAG_SKIP_STORAGE = 1;

// don't load IVF data to RAM, only list sizes
const int IO_FLAG_SKIP_IVF_DATA = 8;

// try to memmap data (useful to load an ArrayInvertedLists as an OnDiskInvertedLists)
const int IO_FLAG_MMAP = IO_FLAG_SKIP_IVF_DATA | 0x646f0000;
```

**Key flag**: `IO_FLAG_SKIP_IVF_DATA` (value `8`)
- Tells the reader to skip deserializing inverted list data
- Only reads metadata: `nlist`, `code_size`, and per-list sizes
- Delegates actual loading to a registered hook

**IO_FLAG_MMAP encoding**:
```
IO_FLAG_MMAP = 0x646f0008
               ^^^^^^^^ = "do" (in hex, for "od" = OnDisk)
                      ^^ = IO_FLAG_SKIP_IVF_DATA (8)
```

The upper 16 bits encode the target fourcc suffix, lower 16 bits contain flags.

### 2. InvertedListsIOHook Interface

Defined in [faiss/invlists/InvertedListsIOHook.h:16-60](../../../faiss/invlists/InvertedListsIOHook.h#L16-L60):

```cpp
struct InvertedListsIOHook {
    const std::string key;       ///< string version of the fourcc
    const std::string classname; ///< typeid.name

    /// write the index to the IOWriter (including the fourcc)
    virtual void write(const InvertedLists* ils, IOWriter* f) const = 0;

    /// called when the fourcc matches this class's fourcc
    virtual InvertedLists* read(IOReader* f, int io_flags) const = 0;

    /** read from a ArrayInvertedLists into this invertedlist type.
     * For this to work, the callback has to be enabled and the io_flag has to
     * be set to IO_FLAG_SKIP_IVF_DATA | (16 upper bits of the fourcc)
     *
     * (default implementation fails)
     */
    virtual InvertedLists* read_ArrayInvertedLists(
            IOReader* f,
            int io_flags,
            size_t nlist,
            size_t code_size,
            const std::vector<size_t>& sizes) const;

    // Hook registration
    static void add_callback(InvertedListsIOHook*);
    static InvertedListsIOHook* lookup(int h);
    static InvertedListsIOHook* lookup_classname(const std::string& classname);
};
```

**Key method**: `read_ArrayInvertedLists`
- Called when `IO_FLAG_SKIP_IVF_DATA` is set and fourcc is `"ilar"`
- Receives metadata (nlist, code_size, sizes) but NO data
- Returns your custom `InvertedLists` implementation
- Default implementation throws an error (must override)

### 3. Reading Flow

From [faiss/impl/index_read.cpp:328-366](../../../faiss/impl/index_read.cpp#L328-L366):

```cpp
InvertedLists* read_InvertedLists(IOReader* f, int io_flags) {
    uint32_t h;
    READ1(h);

    if (h == fourcc("il__")) {
        // No inverted lists stored
        return nullptr;
    } else if (h == fourcc("ilar") && !(io_flags & IO_FLAG_SKIP_IVF_DATA)) {
        // Normal path: load ArrayInvertedLists into RAM
        auto ails = new ArrayInvertedLists(0, 0);
        READ1(ails->nlist);
        READ1(ails->code_size);
        ails->ids.resize(ails->nlist);
        ails->codes.resize(ails->nlist);
        // ... read all data ...
        return ails;

    } else if (h == fourcc("ilar") && (io_flags & IO_FLAG_SKIP_IVF_DATA)) {
        // Hook path: skip data, delegate to custom implementation

        // Construct target fourcc from io_flags upper 16 bits
        int h2 = (io_flags & 0xffff0000) | (fourcc("il__") & 0x0000ffff);

        size_t nlist, code_size;
        READ1(nlist);
        READ1(code_size);
        std::vector<size_t> sizes(nlist);
        read_ArrayInvertedLists_sizes(f, sizes);  // Read only sizes, skip data!

        return InvertedListsIOHook::lookup(h2)->read_ArrayInvertedLists(
                f, io_flags, nlist, code_size, sizes);
    } else {
        // Other types: use normal hook read()
        return InvertedListsIOHook::lookup(h)->read(f, io_flags);
    }
}
```

## Flow Diagrams

### Normal Loading (No Hook)

```
┌─────────────────────────────────────────────────────────────┐
│ read_index("index.faiss", io_flags=0)                       │
└───────────────────────┬─────────────────────────────────────┘
                        │
                        v
┌─────────────────────────────────────────────────────────────┐
│ read_InvertedLists(f, io_flags=0)                           │
│                                                              │
│  1. READ1(h)  →  h = fourcc("ilar") = 0x72616c69           │
│  2. Check: io_flags & IO_FLAG_SKIP_IVF_DATA? → NO           │
│  3. Create: ails = new ArrayInvertedLists()                 │
│  4. READ1(nlist), READ1(code_size)                          │
│  5. For each list i:                                        │
│     - READ1(sizes[i])                                       │
│  6. For each list i:                                        │
│     - READVECTOR(codes[i])  ← COPIES DATA TO RAM            │
│     - READVECTOR(ids[i])    ← COPIES DATA TO RAM            │
│  7. return ails                                             │
└───────────────────────┬─────────────────────────────────────┘
                        │
                        v
            ArrayInvertedLists in RAM
            ┌─────────────────────────┐
            │ codes: vector<vector<>> │
            │ ids:   vector<vector<>> │
            │ [ALL DATA IN MEMORY]    │
            └─────────────────────────┘
```

### Hook Loading with IO_FLAG_MMAP

```
┌──────────────────────────────────────────────────────────────────┐
│ read_index("index.faiss", io_flags=IO_FLAG_MMAP)                 │
│                           io_flags=0x646f0008                    │
│                                     ││││└─── IO_FLAG_SKIP_IVF    │
│                                     └┴┴┴──── "do" (OnDisk)       │
└───────────────────────┬──────────────────────────────────────────┘
                        │
                        v
┌──────────────────────────────────────────────────────────────────┐
│ read_InvertedLists(f, io_flags=0x646f0008)                       │
│                                                                   │
│  1. READ1(h)  →  h = fourcc("ilar") = 0x72616c69                │
│  2. Check: io_flags & IO_FLAG_SKIP_IVF_DATA? → YES!              │
│  3. Construct target fourcc:                                     │
│     h2 = (0x646f0008 & 0xffff0000) | (fourcc("il__") & 0x0000ffff)│
│        = 0x646f0000 | 0x0000_il__                                │
│        = fourcc("ilod")  ← OnDiskInvertedLists fourcc            │
│  4. READ1(nlist), READ1(code_size)                               │
│  5. read_ArrayInvertedLists_sizes(f, sizes)  ← metadata only     │
│  6. Lookup hook: InvertedListsIOHook::lookup(h2)                 │
│     → Returns OnDiskInvertedListsIOHook                          │
│  7. Call hook->read_ArrayInvertedLists(f, ..., nlist,            │
│                                        code_size, sizes)          │
└───────────────────────┬──────────────────────────────────────────┘
                        │
                        v
┌──────────────────────────────────────────────────────────────────┐
│ OnDiskInvertedListsIOHook::read_ArrayInvertedLists()             │
│ [faiss/invlists/OnDiskInvertedLists.cpp:763-808]                 │
│                                                                   │
│  1. ails = new OnDiskInvertedLists()                             │
│  2. ails->nlist = nlist                                          │
│  3. ails->code_size = code_size                                  │
│  4. ails->read_only = true                                       │
│  5. Get file descriptor from FileIOReader                        │
│  6. o0 = ftell(fdesc)  ← current position in file                │
│  7. mmap entire file:                                            │
│     ails->ptr = mmap(NULL, filesize, PROT_READ,                  │
│                      MAP_SHARED, fileno(fdesc), 0)               │
│  8. For each list i:                                             │
│     - lists[i].offset = o  ← offset in mmap'd region             │
│     - lists[i].size = sizes[i]                                   │
│     - o += sizes[i] * (code_size + sizeof(idx_t))                │
│  9. fseek(fdesc, o, SEEK_SET)  ← skip data in file               │
│ 10. return ails                                                  │
└───────────────────────┬──────────────────────────────────────────┘
                        │
                        v
            OnDiskInvertedLists (Zero-Copy!)
            ┌──────────────────────────────────┐
            │ ptr → [mmap'd file region]       │
            │ lists[0].offset = 1000           │
            │ lists[1].offset = 5000           │
            │ ...                              │
            │ [NO DATA IN RAM, LAZY PAGING]    │
            └──────────────────────────────────┘
```

### Accessing Data via mmap

```
User calls: index->search(...)
    │
    v
IndexIVF::search_preassigned()
    │
    v
invlists->get_codes(list_no)
    │
    v
┌────────────────────────────────────────────────────────────┐
│ OnDiskInvertedLists::get_codes(size_t list_no) const      │
│ [faiss/invlists/OnDiskInvertedLists.cpp:385-388]          │
│                                                            │
│  const uint8_t* OnDiskInvertedLists::get_codes(           │
│          size_t list_no) const {                          │
│      return ptr + lists[list_no].offset;  ← NO COPY!      │
│  }                                                         │
└────────────────────────┬───────────────────────────────────┘
                         │
                         v
                Returns pointer into mmap region
                         │
                         v
                OS page fault handler
                         │
                         v
                Kernel loads page from disk
                         │
                         v
                Data available in process memory
```

## Fourcc Encoding Details

The clever encoding scheme allows specifying the target implementation in `io_flags`:

```
io_flags structure for IO_FLAG_SKIP_IVF_DATA:

  31                    16 15              0
  ┌──────────────────────┬─────────────────┐
  │   Target fourcc      │   Flags         │
  │   suffix (2 chars)   │   including (8) │
  └──────────────────────┴─────────────────┘
          │                      │
          │                      └─ IO_FLAG_SKIP_IVF_DATA = 8
          │
          └─ Combined with "il" prefix to form full fourcc

Examples:
  IO_FLAG_MMAP = 0x646f0008
                 ││││└──┴─ 0x08 = IO_FLAG_SKIP_IVF_DATA
                 └┴┴┴───── 0x646f = "do" (reversed)
                           → Combined: "ilod" (OnDisk)

  Custom S3:     0x73330008
                 ││││└──┴─ 0x08 = IO_FLAG_SKIP_IVF_DATA
                 └┴┴┴───── 0x7333 = "s3" (reversed)
                           → Combined: "ils3" (S3)
```

The combining logic in [index_read.cpp:356](../../../faiss/impl/index_read.cpp#L356):

```cpp
int h2 = (io_flags & 0xffff0000) | (fourcc("il__") & 0x0000ffff);
         └─────────┬───────────┘   └──────────┬─────────────┘
           Upper 16 bits from         Lower 16 bits = "il"
           io_flags (suffix)          (prefix for invlists)
```

## OnDiskInvertedLists Implementation

### Hook Registration

From [faiss/invlists/InvertedListsIOHook.cpp:31-50](../../../faiss/invlists/InvertedListsIOHook.cpp#L31-L50):

```cpp
static bool init_callbacks() {
    // Register OnDiskInvertedLists hook with fourcc "ilod"
    InvertedListsIOHook::add_callback(new OnDiskInvertedListsIOHook());
    // ... other hooks ...
    return true;
}

static bool _init_callbacks = init_callbacks();  // Static initialization
```

### Hook Definition

From [faiss/invlists/OnDiskInvertedLists.h:142-152](../../../faiss/invlists/OnDiskInvertedLists.h#L142-L152):

```cpp
struct OnDiskInvertedListsIOHook : InvertedListsIOHook {
    OnDiskInvertedListsIOHook();
    void write(const InvertedLists* ils, IOWriter* f) const override;
    InvertedLists* read(IOReader* f, int io_flags) const override;

    // The magic method!
    InvertedLists* read_ArrayInvertedLists(
            IOReader* f,
            int io_flags,
            size_t nlist,
            size_t code_size,
            const std::vector<size_t>& sizes) const override;
};
```

### The Magic: read_ArrayInvertedLists

From [faiss/invlists/OnDiskInvertedLists.cpp:763-808](../../../faiss/invlists/OnDiskInvertedLists.cpp#L763-L808):

```cpp
InvertedLists* OnDiskInvertedListsIOHook::read_ArrayInvertedLists(
        IOReader* f,
        int /* io_flags */,
        size_t nlist,
        size_t code_size,
        const std::vector<size_t>& sizes) const {
    auto ails = new OnDiskInvertedLists();
    ails->nlist = nlist;
    ails->code_size = code_size;
    ails->read_only = true;
    ails->lists.resize(nlist);

    // Extract file descriptor (mmap only works with files)
    FileIOReader* reader = dynamic_cast<FileIOReader*>(f);
    FAISS_THROW_IF_NOT_MSG(reader, "mmap only supported for File objects");
    FILE* fdesc = reader->f;

    // Current position = start of inverted list data
    size_t o0 = ftell(fdesc);
    size_t o = o0;

    { // Memory-map the entire file
        struct stat buf;
        int ret = fstat(fileno(fdesc), &buf);
        FAISS_THROW_IF_NOT_FMT(ret == 0, "fstat failed: %s", strerror(errno));
        ails->totsize = buf.st_size;
        ails->ptr = (uint8_t*)mmap(
                nullptr,
                ails->totsize,
                PROT_READ,        // Read-only
                MAP_SHARED,       // Shared mapping
                fileno(fdesc),
                0);               // Map from offset 0
        FAISS_THROW_IF_NOT_FMT(
                ails->ptr != MAP_FAILED, "could not mmap: %s", strerror(errno));
    }

    FAISS_THROW_IF_NOT(o <= ails->totsize);

    // Set up list metadata WITHOUT reading the data
    for (size_t i = 0; i < ails->nlist; i++) {
        OnDiskInvertedLists::List& l = ails->lists[i];
        l.size = l.capacity = sizes[i];
        l.offset = o;  // Offset in mmap'd region, not RAM!
        o += l.size * (sizeof(idx_t) + ails->code_size);
    }

    // Skip past the data in the file stream
    // (we don't read it - it's accessible via mmap)
    fseek(fdesc, o, SEEK_SET);

    return ails;
}
```

**Key observations**:
1. **No data copying**: `ptr` points directly to the mmap'd file
2. **Lazy loading**: Data is paged in by the OS on first access
3. **Zero-copy access**: `get_codes()` just returns `ptr + offset`
4. **File seeking**: Skip past the data in the file stream to continue reading other index parts

### Zero-Copy Access

From [faiss/invlists/OnDiskInvertedLists.cpp:385-400](../../../faiss/invlists/OnDiskInvertedLists.cpp#L385-L400):

```cpp
const uint8_t* OnDiskInvertedLists::get_codes(size_t list_no) const {
    return ptr + lists[list_no].offset;  // Direct pointer, no copy!
}

const idx_t* OnDiskInvertedLists::get_ids(size_t list_no) const {
    const uint8_t* codes = get_codes(list_no);
    // IDs stored after codes
    return (const idx_t*)(codes + lists[list_no].size * code_size);
}

size_t OnDiskInvertedLists::list_size(size_t list_no) const {
    return lists[list_no].size;
}
```

## Creating Your Own Hook (S3 Example)

### Step 1: Define Your Hook Class

```cpp
#include <faiss/invlists/InvertedListsIOHook.h>
#include <faiss/invlists/InvertedLists.h>

struct S3InvertedListsIOHook : InvertedListsIOHook {
    S3InvertedListsIOHook()
        : InvertedListsIOHook("ils3",  // fourcc for your type
                             typeid(S3InvertedLists).name()) {}

    void write(const InvertedLists* ils, IOWriter* f) const override {
        // Not needed for read-only S3 implementation
        FAISS_THROW_MSG("S3InvertedLists is read-only");
    }

    InvertedLists* read(IOReader* f, int io_flags) const override {
        // Called when fourcc is "ils3" (direct S3 format)
        FAISS_THROW_MSG("Use read_ArrayInvertedLists instead");
    }

    InvertedLists* read_ArrayInvertedLists(
            IOReader* f,
            int io_flags,
            size_t nlist,
            size_t code_size,
            const std::vector<size_t>& sizes) const override {

        // Create your S3 implementation
        auto s3il = new S3InvertedLists(nlist, code_size);

        // Store metadata
        s3il->list_sizes = sizes;

        // Extract S3 configuration from environment or io_flags
        s3il->bucket = getenv("FAISS_S3_BUCKET");
        s3il->prefix = getenv("FAISS_S3_PREFIX");

        // Calculate total data size
        size_t total_bytes = 0;
        for (size_t i = 0; i < nlist; i++) {
            total_bytes += sizes[i] * (code_size + sizeof(idx_t));
        }

        // Skip the data in the file (we'll fetch from S3 instead)
        FileIOReader* reader = dynamic_cast<FileIOReader*>(f);
        if (reader) {
            fseek(reader->f, total_bytes, SEEK_CUR);
        }

        return s3il;
    }
};
```

### Step 2: Register Your Hook

```cpp
// In your initialization code (before loading any indexes)
InvertedListsIOHook::add_callback(new S3InvertedListsIOHook());
```

### Step 3: Define io_flags Constant

```cpp
// In your header file
// "s3" in hex (little-endian): 0x3373
// But we want big-endian for fourcc, so it's 0x7333
const int IO_FLAG_S3 = IO_FLAG_SKIP_IVF_DATA | 0x73330000;
```

### Step 4: Load Index with Your Hook

```cpp
Index* index = read_index("index.faiss", IO_FLAG_S3);
// This will:
// 1. See IO_FLAG_SKIP_IVF_DATA is set
// 2. Combine 0x7333 with "il" to get "ils3"
// 3. Look up S3InvertedListsIOHook
// 4. Call read_ArrayInvertedLists
// 5. Return S3InvertedLists instead of ArrayInvertedLists
```

### Step 5: Implement Lazy Loading

```cpp
struct S3InvertedLists : InvertedLists {
    std::string bucket;
    std::string prefix;
    std::vector<size_t> list_sizes;

    // Cache for fetched lists
    mutable std::unordered_map<size_t, std::vector<uint8_t>> code_cache;
    mutable std::unordered_map<size_t, std::vector<idx_t>> id_cache;
    mutable std::mutex cache_mutex;

    const uint8_t* get_codes(size_t list_no) const override {
        std::lock_guard<std::mutex> lock(cache_mutex);

        // Check cache
        if (code_cache.find(list_no) != code_cache.end()) {
            return code_cache[list_no].data();
        }

        // Fetch from S3
        std::string key = prefix + "/codes_" + std::to_string(list_no);
        std::vector<uint8_t> data = s3_get_object(bucket, key);

        code_cache[list_no] = std::move(data);
        return code_cache[list_no].data();
    }

    const idx_t* get_ids(size_t list_no) const override {
        std::lock_guard<std::mutex> lock(cache_mutex);

        if (id_cache.find(list_no) != id_cache.end()) {
            return id_cache[list_no].data();
        }

        std::string key = prefix + "/ids_" + std::to_string(list_no);
        std::vector<uint8_t> raw = s3_get_object(bucket, key);
        std::vector<idx_t> ids(raw.size() / sizeof(idx_t));
        memcpy(ids.data(), raw.data(), raw.size());

        id_cache[list_no] = std::move(ids);
        return id_cache[list_no].data();
    }

    size_t list_size(size_t list_no) const override {
        return list_sizes[list_no];
    }
};
```

## File Layout for ArrayInvertedLists

When `ArrayInvertedLists` is saved, the layout is:

```
Offset   Field              Size           Description
─────────────────────────────────────────────────────────────
0        fourcc            4 bytes        "ilar" (0x72616c69)
4        nlist             8 bytes        Number of lists
12       code_size         8 bytes        Bytes per code
20       sizes[0]          8 bytes        Size of list 0
28       sizes[1]          8 bytes        Size of list 1
...      ...               ...            ...
20+8*N   sizes[N-1]        8 bytes        Size of list N-1
         ──────────────────────────────────────────────────
         codes[0]          size[0] * cs   Codes for list 0
         ids[0]            size[0] * 8    IDs for list 0
         codes[1]          size[1] * cs   Codes for list 1
         ids[1]            size[1] * 8    IDs for list 1
         ...               ...            ...
         codes[N-1]        size[N-1] * cs Codes for list N-1
         ids[N-1]          size[N-1] * 8  IDs for list N-1
```

Where:
- `cs` = `code_size`
- `N` = `nlist`

When `IO_FLAG_SKIP_IVF_DATA` is set:
- File position after `read_ArrayInvertedLists_sizes`: Points to start of `codes[0]`
- Your hook receives this file position
- Your hook must `fseek` past all data to continue reading the rest of the index

## Summary

The hook system provides a powerful way to customize inverted list loading:

1. **Normal loading**: Reads everything into `ArrayInvertedLists` (RAM copy)
2. **Hook loading**: Reads metadata, skips data, delegates to your implementation
3. **OnDiskInvertedLists**: Uses mmap for zero-copy access (lazy paging)
4. **Your custom hook**: Can implement S3, databases, shared memory, etc.

**Key benefits**:
- No RAM waste for large indexes
- Lazy loading (on-demand fetching)
- Custom caching strategies
- Support for remote storage backends

**Implementation steps**:
1. Create `InvertedListsIOHook` subclass
2. Implement `read_ArrayInvertedLists` method
3. Register hook with `add_callback`
4. Define custom `io_flags` constant with your fourcc
5. Load index with your flag

The system is designed to be extensible and efficient, enabling advanced use cases like distributed search with S3-backed indexes.
