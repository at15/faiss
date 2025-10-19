# Faiss I/O Macros Deep Dive

This document explains how Faiss handles serialization through its I/O macro system, including what each macro does, how data flows from code to disk, and important caveats about endianness.

## Table of Contents

1. [The Core Abstraction: IOReader/IOWriter](#the-core-abstraction-ioreaderiowriter)
2. [The Macro System](#the-macro-system)
3. [WRITEANDCHECK Explained](#writeandcheck-explained)
4. [Complete Write Example](#complete-write-example)
5. [Reading Back: The Symmetric Operation](#reading-back-the-symmetric-operation)
6. [Endianness: The Big Gotcha](#endianness-the-big-gotcha)
7. [Why Macros Instead of Functions?](#why-macros-instead-of-functions)
8. [Visual Summary](#visual-summary)

## The Core Abstraction: IOReader/IOWriter

Faiss uses a clever pattern where readers and writers overload `operator()` to act like `fread`/`fwrite`:

### IOWriter Interface

```cpp
struct IOWriter {
    std::string name;  // For error messages

    // Acts like: fwrite(ptr, size, nitems, f)
    virtual size_t operator()(const void* ptr, size_t size, size_t nitems) = 0;

    virtual ~IOWriter() {}
};
```

### FileIOWriter Implementation

```cpp
struct FileIOWriter : IOWriter {
    FILE* f = nullptr;

    // Simple passthrough to fwrite!
    size_t operator()(const void* ptr, size_t size, size_t nitems) override {
        return fwrite(ptr, size, nitems, f);
    }
};
```

### Usage Example

```cpp
FileIOWriter writer("index.faiss");
uint32_t magic = 0x12345678;

// These are equivalent:
writer(&magic, sizeof(magic), 1);  // ← Calls operator()
writer.operator()(&magic, sizeof(magic), 1);  // ← Same thing, explicit
```

### Other IOWriter Implementations

```cpp
// Write to memory buffer
struct VectorIOWriter : IOWriter {
    std::vector<uint8_t> data;

    size_t operator()(const void* ptr, size_t size, size_t nitems) override {
        size_t bytes = size * nitems;
        size_t o = data.size();
        data.resize(o + bytes);
        memcpy(&data[o], ptr, bytes);  // ← Append to vector
        return nitems;
    }
};

// Buffered writer (batches small writes)
struct BufferedIOWriter : IOWriter {
    IOWriter* writer;  // Wraps another writer
    size_t bsz;        // Buffer size
    std::vector<char> buffer;

    size_t operator()(const void* ptr, size_t size, size_t nitems) override {
        // Accumulate in buffer, flush when full
        // ...
    }
};
```

**Key insight:** The abstract interface lets you swap between file, memory, network, S3, etc., without changing serialization code!

## The Macro System

All I/O macros assume there's a variable `f` (IOWriter* or IOReader*) in scope.

### WRITE1(x) - Write a single value

```cpp
#define WRITE1(x) WRITEANDCHECK(&(x), 1)
```

**Usage:**
```cpp
uint32_t h = fourcc("ilar");
WRITE1(h);  // Write 4 bytes
```

**Expands to:**
```cpp
WRITEANDCHECK(&h, 1);
```

### WRITEVECTOR(vec) - Write a std::vector

```cpp
#define WRITEVECTOR(vec) {                     \
    size_t size = (vec).size();                \
    WRITEANDCHECK(&size, 1);           /* Write count first */ \
    WRITEANDCHECK((vec).data(), size); /* Then write data */   \
}
```

**Usage:**
```cpp
std::vector<size_t> sizes = {10, 20, 30};
WRITEVECTOR(sizes);
```

**Expands to:**
```cpp
{
    size_t size = sizes.size();  // = 3
    WRITEANDCHECK(&size, 1);           // Write: 3
    WRITEANDCHECK(sizes.data(), 3);    // Write: 10, 20, 30
}
```

**On disk:**
```
┌──────────────┬──────────────┬──────────────┬──────────────┐
│ count=3      │ data[0]=10   │ data[1]=20   │ data[2]=30   │
│ (8 bytes)    │ (8 bytes)    │ (8 bytes)    │ (8 bytes)    │
└──────────────┴──────────────┴──────────────┴──────────────┘
```

## WRITEANDCHECK Explained

This is the **core macro** that all others build upon:

```cpp
#define WRITEANDCHECK(ptr, n) {                           \
    size_t ret = (*f)(ptr, sizeof(*(ptr)), n);            \
    FAISS_THROW_IF_NOT_FMT(                               \
            ret == (n),                                   \
            "write error in %s: %zd != %zd (%s)",         \
            f->name.c_str(),                              \
            ret,                                          \
            size_t(n),                                    \
            strerror(errno));                             \
}
```

### Breaking It Down

#### Step 1: Call the writer

```cpp
size_t ret = (*f)(ptr, sizeof(*(ptr)), n);
//           ^^^^         ^^^^^^^       ^
//            |              |          |
//         Dereference   Size of     Number of
//         operator()   one element   elements
```

**Examples:**

```cpp
// Example 1: Write a uint32_t
uint32_t value = 0x12345678;
WRITEANDCHECK(&value, 1);
// Expands to:
size_t ret = (*f)(&value, sizeof(value), 1);
//                          ^^^^^^^^^^^^^^
//                          sizeof(uint32_t) = 4 bytes
// Writes 4 bytes total

// Example 2: Write array of 10 floats
float array[10] = {...};
WRITEANDCHECK(array, 10);
// Expands to:
size_t ret = (*f)(array, sizeof(array[0]), 10);
//                       ^^^^^^^^^^^^^^^^
//                       sizeof(float) = 4 bytes
// Writes 40 bytes total (4 × 10)

// Example 3: Write vector data
std::vector<size_t> vec = {1, 2, 3};
WRITEANDCHECK(vec.data(), vec.size());
// Expands to:
size_t ret = (*f)(vec.data(), sizeof(size_t), 3);
//                              ^^^^^^^^^^^^^
//                              sizeof(size_t) = 8 bytes
// Writes 24 bytes total (8 × 3)
```

#### Step 2: Check the return value

```cpp
FAISS_THROW_IF_NOT_FMT(
        ret == (n),  // ← Did we write all items?
        "write error in %s: %zd != %zd (%s)",
        f->name.c_str(),  // File name
        ret,              // Actual items written
        size_t(n),        // Expected items
        strerror(errno)); // System error message
```

**Why check?**
- Disk full
- Permission denied
- Network interruption (for remote writers)
- File handle closed

**Example error:**
```
FaissException: Error in write_InvertedLists at index_write.cpp:288:
write error in index.faiss: 50 != 100 (No space left on device)
```

### The `sizeof(*(ptr))` Trick

This clever trick automatically determines the element size:

```cpp
uint32_t* ptr = &value;
sizeof(*(ptr))  // = sizeof(uint32_t) = 4

float* ptr = array;
sizeof(*(ptr))  // = sizeof(float) = 4

size_t* ptr = vec.data();
sizeof(*(ptr))  // = sizeof(size_t) = 8
```

**Benefit:** Type-safe! The macro knows the element size automatically.

### Complete Expansion Example

```cpp
// Original code:
std::vector<float> data = {1.0f, 2.0f, 3.0f};
WRITEANDCHECK(data.data(), data.size());

// Full expansion:
{
    size_t ret = (*f)(
        data.data(),           // void* ptr
        sizeof(*(data.data())), // = sizeof(float) = 4
        data.size()            // = 3
    );
    // Calls: f->operator()(data.data(), 4, 3)
    // Which calls: fwrite(data.data(), 4, 3, file)
    // Writes: 12 bytes total

    FAISS_THROW_IF_NOT_FMT(
        ret == 3,  // Expects 3 items written
        "write error in %s: %zd != %zd (%s)",
        f->name.c_str(),  // e.g., "index.faiss"
        ret,              // Actual: hopefully 3
        size_t(3),        // Expected: 3
        strerror(errno)   // e.g., "No space left on device"
    );
}
```

## Complete Write Example

Let's trace a complete write operation for `ArrayInvertedLists`:

### Setup
```cpp
// Small example: 3 clusters, sparse format
// Cluster 0: empty
// Cluster 1: 2 vectors (IDs: 100, 101)
// Cluster 2: 1 vector (ID: 200)
// code_size = 16 bytes per vector

ArrayInvertedLists ails(3, 16);
ails.add_entries(1, 2, {100, 101}, codes1);
ails.add_entries(2, 1, {200}, codes2);
```

### Write Code

```cpp
void write_InvertedLists(const InvertedLists* ils, IOWriter* f) {
    const auto& ails = dynamic_cast<const ArrayInvertedLists*>(ils);

    // 1. Write FourCC magic number
    uint32_t h = fourcc("ilar");  // = 0x72616c69
    WRITE1(h);
    // Disk: [69 6c 61 72] (4 bytes, little-endian)

    // 2. Write metadata
    WRITE1(ails->nlist);       // = 3
    WRITE1(ails->code_size);   // = 16
    // Disk: [03 00 00 00 00 00 00 00] [10 00 00 00 00 00 00 00]

    // 3. Determine format (2 non-empty out of 3 = sparse)
    size_t n_non0 = 2;
    int list_type = fourcc("sprs");
    WRITE1(list_type);
    // Disk: [73 70 72 73] (4 bytes)

    // 4. Write sizes array (sparse format)
    std::vector<size_t> sizes;
    // Cluster 1: index=1, size=2
    sizes.push_back(1); sizes.push_back(2);
    // Cluster 2: index=2, size=1
    sizes.push_back(2); sizes.push_back(1);
    // sizes = [1, 2, 2, 1]

    WRITEVECTOR(sizes);
    // First: write count
    // Disk: [04 00 00 00 00 00 00 00] (count=4)
    // Then: write array
    // Disk: [01 00 ... 00] [02 00 ... 00] [02 00 ... 00] [01 00 ... 00]

    // 5. Write actual data for non-empty lists
    for (size_t i = 0; i < ails->nlist; i++) {
        size_t n = ails->ids[i].size();
        if (n > 0) {
            // Write codes
            WRITEANDCHECK(ails->codes[i].data(), n * ails->code_size);
            // Cluster 1: writes 2 × 16 = 32 bytes
            // Cluster 2: writes 1 × 16 = 16 bytes

            // Write IDs
            WRITEANDCHECK(ails->ids[i].data(), n);
            // Cluster 1: writes 2 × 8 = 16 bytes (IDs: 100, 101)
            // Cluster 2: writes 1 × 8 = 8 bytes (ID: 200)
        }
    }
}
```

### Complete Disk Layout

```
Offset │ Size  │ Hex Bytes           │ Content           │ Macro
───────┼───────┼─────────────────────┼───────────────────┼──────────────
0x0000 │ 4     │ 69 6c 61 72         │ "ilar"            │ WRITE1(h)
0x0004 │ 8     │ 03 00 00 00 00...   │ nlist = 3         │ WRITE1(nlist)
0x000C │ 8     │ 10 00 00 00 00...   │ code_size = 16    │ WRITE1(code_size)
0x0014 │ 4     │ 73 70 72 73         │ "sprs"            │ WRITE1(list_type)
0x0018 │ 8     │ 04 00 00 00 00...   │ sizes.size() = 4  │ WRITEVECTOR (count)
0x0020 │ 8     │ 01 00 00 00 00...   │ sizes[0] = 1      │ WRITEVECTOR (data)
0x0028 │ 8     │ 02 00 00 00 00...   │ sizes[1] = 2      │
0x0030 │ 8     │ 02 00 00 00 00...   │ sizes[2] = 2      │
0x0038 │ 8     │ 01 00 00 00 00...   │ sizes[3] = 1      │
0x0040 │ 32    │ [vector data]       │ codes[1] (2×16)   │ WRITEANDCHECK
0x0060 │ 16    │ 64 00... 65 00...   │ ids[1] (100, 101) │ WRITEANDCHECK
0x0070 │ 16    │ [vector data]       │ codes[2] (1×16)   │ WRITEANDCHECK
0x0080 │ 8     │ c8 00 00 00 00...   │ ids[2] (200)      │ WRITEANDCHECK
```

## Reading Back: The Symmetric Operation

### READANDCHECK - Mirror of WRITEANDCHECK

```cpp
#define READANDCHECK(ptr, n) {                            \
    size_t ret = (*f)(ptr, sizeof(*(ptr)), n);            \
    FAISS_THROW_IF_NOT_FMT(                               \
            ret == (n),                                   \
            "read error in %s: %zd != %zd (%s)",          \
            f->name.c_str(),                              \
            ret,                                          \
            size_t(n),                                    \
            strerror(errno));                             \
}
```

**The only difference:** Error message says "read" instead of "write"!

### READ1 and READVECTOR

```cpp
#define READ1(x) READANDCHECK(&(x), 1)

#define READVECTOR(vec) {                              \
    size_t size;                                       \
    READANDCHECK(&size, 1);        /* Read count */   \
    FAISS_THROW_IF_NOT(size >= 0 && size < (1ULL << 40)); /* Sanity check */ \
    (vec).resize(size);            /* Allocate */     \
    READANDCHECK((vec).data(), size); /* Read data */ \
}
```

### Read Example

```cpp
InvertedLists* read_InvertedLists(IOReader* f, int io_flags) {
    // 1. Read FourCC
    uint32_t h;
    READ1(h);  // Reads 4 bytes

    if (h == fourcc("ilar")) {
        auto ails = new ArrayInvertedLists(0, 0);

        // 2. Read metadata
        READ1(ails->nlist);      // Reads 8 bytes → 3
        READ1(ails->code_size);  // Reads 8 bytes → 16

        // 3. Read format type
        uint32_t list_type;
        READ1(list_type);  // → "sprs"

        // 4. Read sizes array
        std::vector<size_t> sizes;
        READVECTOR(sizes);
        // First reads: count = 4
        // Then reads: [1, 2, 2, 1]

        // Convert sparse format to dense
        std::vector<size_t> dense_sizes(ails->nlist, 0);
        for (size_t i = 0; i < sizes.size(); i += 2) {
            size_t list_no = sizes[i];
            size_t list_size = sizes[i + 1];
            dense_sizes[list_no] = list_size;
        }
        // dense_sizes = [0, 2, 1]

        // 5. Allocate space
        ails->ids.resize(ails->nlist);
        ails->codes.resize(ails->nlist);
        for (size_t i = 0; i < ails->nlist; i++) {
            ails->ids[i].resize(dense_sizes[i]);
            ails->codes[i].resize(dense_sizes[i] * ails->code_size);
        }

        // 6. Read actual data
        for (size_t i = 0; i < ails->nlist; i++) {
            size_t n = ails->ids[i].size();
            if (n > 0) {
                READANDCHECK(ails->codes[i].data(), n * ails->code_size);
                READANDCHECK(ails->ids[i].data(), n);
            }
        }

        return ails;
    }
    // ... handle other formats ...
}
```

### Symmetry Visualization

```
WRITE                          READ
─────                          ────
WRITE1(h)           ←────────→ READ1(h)
WRITE1(nlist)       ←────────→ READ1(nlist)
WRITEVECTOR(sizes)  ←────────→ READVECTOR(sizes)
  ├─ write count                 ├─ read count
  └─ write data                  ├─ resize vector
                                 └─ read data
```

## Endianness: The Big Gotcha

**CRITICAL:** Faiss **does NOT handle endianness conversion**!

### What Gets Written

```cpp
size_t FileIOWriter::operator()(const void* ptr, size_t size, size_t nitems) {
    return fwrite(ptr, size, nitems, f);  // ← Raw bytes, NO conversion!
}
```

### Example: Writing uint32_t on Different Architectures

```cpp
uint32_t value = 0x12345678;
WRITE1(value);
```

**On Little-Endian (x86, ARM, most modern CPUs):**
```
Memory: [78] [56] [34] [12]  ← Least significant byte first
Disk:   [78] [56] [34] [12]
```

**On Big-Endian (old PowerPC, SPARC, some embedded):**
```
Memory: [12] [34] [56] [78]  ← Most significant byte first
Disk:   [12] [34] [56] [78]
```

**Result:** Files are **NOT portable** between different endianness!

### size_t Portability Issues

```cpp
size_t value = 1000;
WRITE1(value);
```

**On 64-bit system:**
```
sizeof(size_t) = 8
Disk: [e8 03 00 00 00 00 00 00]  (8 bytes)
```

**On 32-bit system:**
```
sizeof(size_t) = 4
Disk: [e8 03 00 00]  (4 bytes)
```

**Result:** Files written on 64-bit **cannot** be read on 32-bit (and vice versa)!

### Faiss's Assumption

From the codebase practices:
- **Assumes x86-64 or ARM64** (little-endian, 64-bit)
- **Assumes IEEE 754 floats**
- **No platform abstraction layer**

**Practical advice:**
- ✅ Write and read on same architecture
- ✅ Use same OS (Linux, macOS, Windows)
- ❌ Don't mix 32-bit and 64-bit
- ❌ Don't mix endianness

### If You Need Portability

You'd need to write custom IOWriter/IOReader:

```cpp
struct PortableIOWriter : IOWriter {
    IOWriter* wrapped;

    size_t operator()(const void* ptr, size_t size, size_t nitems) override {
        // Convert to network byte order (big-endian)
        for (size_t i = 0; i < nitems; i++) {
            if (size == 4) {
                uint32_t value = htonl(((uint32_t*)ptr)[i]);
                wrapped->operator()(&value, 4, 1);
            } else if (size == 8) {
                // Need to swap 8 bytes...
            }
        }
        return nitems;
    }
};
```

But Faiss doesn't do this - it prioritizes performance over portability.

## Why Macros Instead of Functions?

From [faiss/impl/io_macros.h](../../faiss/impl/io_macros.h):

> We use macros so that we have a line number to report in abort(). This makes debugging a lot easier.

### Example: Error with Macro

```cpp
// File: index_write.cpp, line 42
WRITE1(data);

// Error message:
FaissException: Error in write_InvertedLists at index_write.cpp:42:
write error in index.faiss: 0 != 1 (No space left on device)
                                         ↑
                                    Shows line 42!
```

### Example: Error with Function

```cpp
// File: index_write.cpp, line 42
write_value(f, data);

// Error message:
FaissException: Error in write_value at io.cpp:150:
write error in index.faiss: 0 != 1 (No space left on device)
                                    ↑
                            Shows function internals (io.cpp:150)
                            NOT the call site (index_write.cpp:42)!
```

### Why This Matters

When debugging serialization bugs:
- Need to know **which specific write** failed
- Macro gives **exact line** in high-level code
- Function only gives **location in I/O code**

## Visual Summary

### The Complete Data Flow

```
┌─────────────────────────────────────────────────────────────────┐
│                     Application Code                            │
│  write_index(&index, "index.faiss");                            │
└──────────────────────┬──────────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────────────┐
│              write_InvertedLists(ils, f)                        │
│  WRITE1(fourcc("ilar"));          ← Macro expands              │
│  WRITEVECTOR(sizes);              ← Macro expands              │
│  WRITEANDCHECK(codes.data(), n);  ← Macro expands              │
└──────────────────────┬──────────────────────────────────────────┘
                       │ Macro expansion:
                       ▼
┌─────────────────────────────────────────────────────────────────┐
│  size_t ret = (*f)(ptr, sizeof(*ptr), n);                      │
│               ^^^^                                              │
│         Dereferences f and calls operator()                     │
│                                                                  │
│  FAISS_THROW_IF_NOT_FMT(ret == n, ...);                        │
│                         ^^^^^^^^                                │
│                    Verifies write succeeded                     │
└──────────────────────┬──────────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────────────┐
│      FileIOWriter::operator()(ptr, size, nitems)                │
│  {                                                              │
│      return fwrite(ptr, size, nitems, f);                      │
│             ^^^^^^                                              │
│        Standard C library call                                  │
│  }                                                              │
└──────────────────────┬──────────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────────────┐
│                  Operating System                               │
│         Kernel writes bytes to filesystem                       │
│  ┌───────────────────────────────────────┐                     │
│  │  Disk: [69 6c 61 72] [03 00 00 ...]  │                     │
│  └───────────────────────────────────────┘                     │
└─────────────────────────────────────────────────────────────────┘
```

### Macro Expansion Comparison

```
High-Level Code         Macro Expansion                  What It Does
───────────────         ───────────────                  ────────────
WRITE1(x)          →    WRITEANDCHECK(&x, 1)        →   Write 1 item

WRITEVECTOR(v)     →    size = v.size()             →   Write count, then array
                        WRITEANDCHECK(&size, 1)
                        WRITEANDCHECK(v.data(), size)

WRITEANDCHECK(p,n) →    ret = (*f)(p, sizeof(*p), n) → Write n items, check result
                        FAISS_THROW_IF_NOT(ret == n)
```

## Quick Reference

| Macro | Purpose | Writes | Example |
|-------|---------|--------|---------|
| `WRITE1(x)` | Single value | `sizeof(x)` bytes | `WRITE1(nlist)` → 8 bytes |
| `WRITEVECTOR(v)` | std::vector | count + data | `WRITEVECTOR(sizes)` → 8 + (n×8) bytes |
| `WRITEANDCHECK(p,n)` | Raw array | `sizeof(*p) × n` bytes | `WRITEANDCHECK(codes, 100)` → `sizeof(code) × 100` |
| `READ1(x)` | Single value | `sizeof(x)` bytes | `READ1(nlist)` → 8 bytes |
| `READVECTOR(v)` | std::vector | count + data | `READVECTOR(sizes)` → reads count, allocates, reads data |
| `READANDCHECK(p,n)` | Raw array | `sizeof(*p) × n` bytes | `READANDCHECK(codes, 100)` |

## Key Takeaways

1. **`operator()` pattern** - I/O objects act like `fread`/`fwrite`
2. **WRITEANDCHECK is the core** - All other macros build on it
3. **Macros for debugging** - Shows exact line numbers in error messages
4. **No endianness handling** - Raw bytes written as-is (not portable!)
5. **Size-prefixed vectors** - Always write count first, then data
6. **Error checking** - Every I/O operation verified
7. **Abstraction** - Can swap FileIOWriter for VectorIOWriter, S3Writer, etc.

This system is simple but effective - it's a thin wrapper around standard C file I/O with error checking and debugging support. The macro magic makes errors traceable while keeping the code concise!
