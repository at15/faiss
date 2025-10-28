# Faiss Custom InvertedLists: Implementation Options

## Overview

This document explains how to implement custom `InvertedLists` implementations for Faiss indexes, particularly for custom storage backends like S3, Redis, or other remote storage systems.

**Key Finding:** Unlike `IOReader`/`IOWriter`, you **cannot** implement `InvertedLists` directly in pure Python. You need C++ for custom implementations, but you don't need to modify the Faiss codebase.

## The InvertedLists Interface

### C++ Interface

**Source:** [faiss/invlists/InvertedLists.h:38-150](../../faiss/invlists/InvertedLists.h#L38-L150)

```cpp
/** Table of inverted lists
 * multithreading rules:
 * - concurrent read accesses are allowed
 * - concurrent update accesses are allowed
 * - for resize and add_entries, only concurrent access to different lists
 *   are allowed
 */
struct InvertedLists {
    size_t nlist;     ///< number of possible key values
    size_t code_size; ///< code size per vector in bytes

    /// request to use iterator rather than get_codes / get_ids
    bool use_iterator = false;

    InvertedLists(size_t nlist, size_t code_size);
    virtual ~InvertedLists();

    /// used for BlockInvertedLists, where the codes are packed into groups
    static const size_t INVALID_CODE_SIZE = static_cast<size_t>(-1);

    /*************************
     *  Read only functions */

    /// get the size of a list
    virtual size_t list_size(size_t list_no) const = 0;

    /** get the codes for an inverted list
     * must be released by release_codes
     * @return codes    size list_size * code_size
     */
    virtual const uint8_t* get_codes(size_t list_no) const = 0;

    /** get the ids for an inverted list
     * must be released by release_ids
     * @return ids      size list_size
     */
    virtual const idx_t* get_ids(size_t list_no) const = 0;

    /// release codes returned by get_codes (default implementation is nop)
    virtual void release_codes(size_t list_no, const uint8_t* codes) const;

    /// release ids returned by get_ids
    virtual void release_ids(size_t list_no, const idx_t* ids) const;

    /// @return a single id in an inverted list
    virtual idx_t get_single_id(size_t list_no, size_t offset) const;

    /// @return a single code in an inverted list
    /// (should be deallocated with release_codes)
    virtual const uint8_t* get_single_code(size_t list_no, size_t offset) const;

    /// prepare the following lists (default does nothing)
    virtual void prefetch_lists(const idx_t* list_nos, int nlist) const;

    /*****************************************
     * Iterator interface (with context)     */

    /// check if the list is empty
    virtual bool is_empty(size_t list_no, void* inverted_list_context = nullptr) const;

    /// get iterable for lists that use_iterator
    virtual InvertedListsIterator* get_iterator(
            size_t list_no,
            void* inverted_list_context = nullptr) const;

    /*************************
     * writing functions     */

    /// add one entry to an inverted list
    virtual size_t add_entry(
            size_t list_no,
            idx_t theid,
            const uint8_t* code,
            void* inverted_list_context = nullptr);

    virtual size_t add_entries(
            size_t list_no,
            size_t n_entry,
            const idx_t* ids,
            const uint8_t* code) = 0;

    virtual void update_entry(
            size_t list_no,
            size_t offset,
            idx_t id,
            const uint8_t* code);

    virtual void update_entries(
            size_t list_no,
            size_t offset,
            size_t n_entry,
            const idx_t* ids,
            const uint8_t* code) = 0;

    virtual void resize(size_t list_no, size_t new_size) = 0;

    virtual void reset();

    /*************************
     * high level functions  */

    /// move all entries from oivf (empty on output)
    void merge_from(InvertedLists* oivf, size_t add_id);
};
```

### Key Methods to Implement

For a **read-only** custom backend (like S3), you need:

1. **`list_size(size_t list_no)`** - Return number of vectors in a list
2. **`get_codes(size_t list_no)`** - Return encoded vectors (must allocate memory)
3. **`get_ids(size_t list_no)`** - Return vector IDs (must allocate memory)
4. **`release_codes()`** - Free memory allocated by `get_codes()`
5. **`release_ids()`** - Free memory allocated by `get_ids()`

For a **read-write** backend, additionally implement:

6. **`add_entries()`** - Add vectors to a list
7. **`update_entries()`** - Update existing vectors
8. **`resize()`** - Resize a list

## Using `replace_invlists()` in Python

### Method Signature

**Source:** [faiss/IndexIVF.h:451](../../faiss/IndexIVF.h#L451)

```cpp
/// replace the inverted lists, old one is deallocated if own_invlists
void replace_invlists(InvertedLists* il, bool own = false);
```

**Python Usage:**

```python
# Default: don't transfer ownership (own=False)
index.replace_invlists(new_invlists)

# Transfer ownership to the index
index.replace_invlists(new_invlists, own=True)
```

### Example: Using OnDiskInvertedLists

**Source:** [tests/test_index_composite.py:78-81](../../tests/test_index_composite.py#L78-L81)

```python
invlists = faiss.OnDiskInvertedLists(
    index1.nlist, index1.code_size,
    filename)
index1.replace_invlists(invlists)
```

### Example: Managing Ownership

**Source:** [tests/test_index_composite.py:641-667](../../tests/test_index_composite.py#L641-L667)

```python
il = index.invlists
maxsz = max(il.list_size(i) for i in range(il.nlist))

il2 = faiss.StopWordsInvertedLists(il, maxsz + 1)
index.own_invlists = False

# Replace but don't transfer ownership
index.replace_invlists(il2, False)
D1, I1 = index.search(xq, 10)

# cleanup to avoid segfault on exit
index.replace_invlists(il, False)

# ... more operations ...

# avoid mem leak - transfer ownership
index.replace_invlists(il, True)
```

## Implementation Options

### Option 1: C++ Implementation Outside Faiss (✅ RECOMMENDED)

You **do NOT need to modify the Faiss codebase**. Create a separate C++ library.

#### Example: S3InvertedLists

**Your implementation:** [S3InvertedLists.h](../S3InvertedLists.h), [S3InvertedLists.cpp](../S3InvertedLists.cpp)

```cpp
// S3InvertedLists.h
#pragma once

#include <faiss/invlists/InvertedLists.h>
#include <string>
#include <memory>

class S3InvertedLists : public faiss::InvertedLists {
public:
    S3InvertedLists(
        size_t nlist,
        size_t code_size,
        const std::string& bucket,
        const std::string& prefix
    );

    virtual ~S3InvertedLists();

    // Read-only methods
    size_t list_size(size_t list_no) const override;
    const uint8_t* get_codes(size_t list_no) const override;
    const idx_t* get_ids(size_t list_no) const override;
    void release_codes(size_t list_no, const uint8_t* codes) const override;
    void release_ids(size_t list_no, const idx_t* ids) const override;

    // Write methods (if needed)
    size_t add_entries(
        size_t list_no,
        size_t n_entry,
        const faiss::idx_t* ids,
        const uint8_t* code
    ) override;

    void update_entries(
        size_t list_no,
        size_t offset,
        size_t n_entry,
        const faiss::idx_t* ids,
        const uint8_t* code
    ) override;

    void resize(size_t list_no, size_t new_size) override;

private:
    std::string bucket_;
    std::string prefix_;

    // S3 client, caching, metadata
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
```

```cpp
// S3InvertedLists.cpp
#include "S3InvertedLists.h"
#include <aws/s3/S3Client.h>
#include <aws/s3/model/GetObjectRequest.h>

S3InvertedLists::S3InvertedLists(
    size_t nlist,
    size_t code_size,
    const std::string& bucket,
    const std::string& prefix
) : InvertedLists(nlist, code_size),
    bucket_(bucket),
    prefix_(prefix)
{
    // Initialize S3 client
}

size_t S3InvertedLists::list_size(size_t list_no) const {
    // Read metadata from S3 (e.g., from a JSON file)
    // Return the size of this inverted list
}

const uint8_t* S3InvertedLists::get_codes(size_t list_no) const {
    // Download codes from S3
    // Allocate buffer and return pointer
    // Must be freed in release_codes()

    size_t size = list_size(list_no);
    uint8_t* codes = new uint8_t[size * code_size];

    // S3 download logic here
    std::string key = prefix_ + "/codes_" + std::to_string(list_no);
    // ... download to codes buffer ...

    return codes;
}

const faiss::idx_t* S3InvertedLists::get_ids(size_t list_no) const {
    // Similar to get_codes, but for IDs
    size_t size = list_size(list_no);
    faiss::idx_t* ids = new faiss::idx_t[size];

    // S3 download logic here
    std::string key = prefix_ + "/ids_" + std::to_string(list_no);
    // ... download to ids buffer ...

    return ids;
}

void S3InvertedLists::release_codes(size_t list_no, const uint8_t* codes) const {
    delete[] codes;
}

void S3InvertedLists::release_ids(size_t list_no, const faiss::idx_t* ids) const {
    delete[] ids;
}

size_t S3InvertedLists::add_entries(
    size_t list_no,
    size_t n_entry,
    const faiss::idx_t* ids,
    const uint8_t* code
) {
    // Upload to S3
    // Return new list size
}

void S3InvertedLists::update_entries(
    size_t list_no,
    size_t offset,
    size_t n_entry,
    const faiss::idx_t* ids,
    const uint8_t* code
) {
    // Update on S3
}

void S3InvertedLists::resize(size_t list_no, size_t new_size) {
    // Resize on S3
}
```

#### Build as Shared Library

```bash
# CMakeLists.txt or Makefile
g++ -shared -fPIC -o libs3_invlists.so \
    S3InvertedLists.cpp \
    -I/path/to/faiss/include \
    -lfaiss \
    -laws-cpp-sdk-s3
```

#### Use from C++

```cpp
#include <faiss/IndexIVF.h>
#include "S3InvertedLists.h"

int main() {
    // Create index
    faiss::IndexFlatL2 quantizer(d);
    faiss::IndexIVFFlat index(&quantizer, d, nlist);
    index.train(xb);

    // Replace with S3 backend
    auto s3_il = new S3InvertedLists(nlist, index.code_size, "my-bucket", "indexes/my-index");
    index.replace_invlists(s3_il, true);  // transfer ownership

    // Now searches use S3!
    index.search(nq, xq, k, D, I);
}
```

### Option 2: Create Python Bindings with SWIG

Create a SWIG interface for your custom module:

```swig
// s3_invlists.swig
%module s3_invlists

%{
#include <faiss/invlists/InvertedLists.h>
#include "S3InvertedLists.h"
%}

// Include Faiss base types
%include <stdint.i>
%include <typemaps.i>

// Tell SWIG about Faiss types
namespace faiss {
    typedef long idx_t;
}

// Include the headers
%include <faiss/invlists/InvertedLists.h>
%include "S3InvertedLists.h"
```

**Build the Python module:**

```bash
# Generate wrapper
swig -c++ -python -I/path/to/faiss s3_invlists.swig

# Compile
g++ -fPIC -shared s3_invlists_wrap.cxx S3InvertedLists.cpp \
    -o _s3_invlists.so \
    -I/path/to/python/include \
    -I/path/to/faiss/include \
    -lfaiss \
    -laws-cpp-sdk-s3
```

**Use from Python:**

```python
import faiss
import s3_invlists

# Create index
quantizer = faiss.IndexFlatL2(d)
index = faiss.IndexIVFFlat(quantizer, d, nlist)
index.train(xb)

# Replace with S3 backend
s3_il = s3_invlists.S3InvertedLists(nlist, index.code_size, "my-bucket", "indexes/my-index")
index.replace_invlists(s3_il, False)  # keep Python reference

# Keep reference to prevent garbage collection
index.s3_backend = s3_il

# Now searches use S3!
D, I = index.search(xq, k)
```

### Option 3: Pure Python (❌ NOT SUPPORTED)

**Important:** SWIG directors are **NOT** enabled for `InvertedLists` in Faiss Python bindings.

This means you **cannot** do:

```python
# THIS DOES NOT WORK!
class MyPythonInvertedLists(faiss.InvertedLists):
    def list_size(self, list_no):
        return my_custom_logic()
    # ... etc
```

The example in [tests/test_io.py:232-256](../../tests/test_io.py#L232-L256) is **not** a working InvertedLists implementation:

```python
class PyOndiskInvertedLists:
    """ wraps an OnDisk object for use from C++ """

    def __init__(self, oil):
        self.oil = oil

    def list_size(self, list_no):
        return self.oil.list_size(list_no)

    def get_codes(self, list_no):
        oil = self.oil
        assert 0 <= list_no < oil.lists.size()
        l = oil.lists.at(list_no)
        with open(oil.filename, 'rb') as f:
            f.seek(l.offset)
            return f.read(l.size * oil.code_size)

    def get_ids(self, list_no):
        oil = self.oil
        assert 0 <= list_no < oil.lists.size()
        l = oil.lists.at(list_no)
        with open(oil.filename, 'rb') as f:
            f.seek(l.offset + l.capacity * oil.code_size)
            return f.read(l.size * 8)
```

This is **just a Python helper class** for convenience, **not** something you can pass to `index.replace_invlists()`.

## Built-in InvertedLists Implementations

Faiss provides several built-in implementations you can use:

### ArrayInvertedLists

Standard in-memory implementation.

```python
import faiss

# Create explicitly
invlists = faiss.ArrayInvertedLists(nlist, code_size)

# Or use default (indexes create this automatically)
index = faiss.IndexIVFFlat(quantizer, d, nlist)
# index.invlists is ArrayInvertedLists
```

### OnDiskInvertedLists

Memory-mapped disk storage.

**Source:** [tests/test_index_composite.py:78-81](../../tests/test_index_composite.py#L78-L81)

```python
import faiss
import tempfile

filename = tempfile.mkstemp()[1]
invlists = faiss.OnDiskInvertedLists(
    index.nlist,
    index.code_size,
    filename
)
index.replace_invlists(invlists)
```

### VStackInvertedLists

Vertical stacking of multiple inverted lists (concatenate lists).

**Source:** [tests/test_index_composite.py:617-620](../../tests/test_index_composite.py#L617-L620)

```python
# Stack multiple InvertedLists vertically
il2 = faiss.VStackInvertedLists(ilv.size(), ilv.data())

index2 = faiss.IndexIVFFlat(quantizer, d, 30)
index2.replace_invlists(il2)
index2.ntotal = index.ntotal
```

### HStackInvertedLists

Horizontal stacking (also called `ConcatenatedInvertedLists`).

```python
# Combine inverted lists from multiple indexes
il1 = index1.invlists
il2 = index2.invlists
il_combined = faiss.HStackInvertedLists([il1, il2])
```

### StopWordsInvertedLists

Filters out lists that are too large (useful for handling stopwords).

**Source:** [tests/test_index_composite.py:644-648](../../tests/test_index_composite.py#L644-L648)

```python
il = index.invlists
maxsz = max(il.list_size(i) for i in range(il.nlist))

# Skip lists larger than maxsz + 1
il2 = faiss.StopWordsInvertedLists(il, maxsz + 1)
index.replace_invlists(il2, False)
```

### MaskedInvertedLists

Masks certain inverted lists.

```python
# Create a mask (e.g., skip certain clusters)
mask = [True, False, True, True, ...]  # length = nlist
il_masked = faiss.MaskedInvertedLists(original_il, mask)
index.replace_invlists(il_masked, False)
```

## Memory Management and Ownership

### The `own` Parameter

**Source:** [faiss/IndexIVF.cpp:1247-1257](../../faiss/IndexIVF.cpp#L1247-L1257)

```cpp
void IndexIVF::replace_invlists(InvertedLists* il, bool own) {
    if (own_invlists) {
        delete invlists;
        invlists = nullptr;
    }
    if (il) {
        FAISS_THROW_IF_NOT(il->nlist == nlist);
        FAISS_THROW_IF_NOT(
                il->code_size == code_size ||
                il->code_size == InvertedLists::INVALID_CODE_SIZE);
    }
    invlists = il;
    own_invlists = own;
}
```

### Ownership Rules

1. **`own=False` (default)**:
   - Index does **not** take ownership
   - You must keep the InvertedLists object alive
   - Index will **not** delete it on destruction
   - In Python: keep a reference to prevent garbage collection

2. **`own=True`**:
   - Index **takes ownership**
   - Index **will delete** the InvertedLists on destruction
   - Don't use the InvertedLists after the index is destroyed
   - In Python: the SWIG wrapper may call `.disown()` to transfer ownership

### Python Reference Management

**Source:** [faiss/python/__init__.py:210-211](../../faiss/python/__init__.py#L210-L211)

```python
# Faiss automatically adds references for certain constructors
add_ref_in_constructor(BufferedIOReader, 0)
```

For custom inverted lists, you should manually keep references:

```python
# Good: Keep reference
index.my_invlists = my_custom_invlists
index.replace_invlists(my_custom_invlists, False)

# Bad: Invlists may be garbage collected!
index.replace_invlists(MyInvertedLists(...), False)
```

### Example: Careful Ownership Management

**Source:** [tests/test_index_composite.py:641-667](../../tests/test_index_composite.py#L641-L667)

```python
# Save original
il = index.invlists

# Create wrapper
il2 = faiss.StopWordsInvertedLists(il, maxsz + 1)

# Must set own_invlists=False before replacing
index.own_invlists = False

# Replace without transferring ownership
index.replace_invlists(il2, False)

# Do some work...
D1, I1 = index.search(xq, 10)

# Restore original
index.replace_invlists(il, False)

# More work...

# Finally transfer ownership at the end
index.replace_invlists(il, True)
```

## Performance Considerations

### Caching

For remote backends like S3, implement caching:

```cpp
class S3InvertedLists : public faiss::InvertedLists {
private:
    // LRU cache for codes/ids
    mutable std::unordered_map<size_t, std::vector<uint8_t>> codes_cache_;
    mutable std::unordered_map<size_t, std::vector<idx_t>> ids_cache_;

public:
    const uint8_t* get_codes(size_t list_no) const override {
        // Check cache first
        auto it = codes_cache_.find(list_no);
        if (it != codes_cache_.end()) {
            return it->second.data();
        }

        // Cache miss - download from S3
        // ...
    }
};
```

### Prefetching

Implement `prefetch_lists()` for better performance:

```cpp
void S3InvertedLists::prefetch_lists(const idx_t* list_nos, int nlist) const override {
    // Start async downloads for these lists
    std::vector<std::future<void>> futures;
    for (int i = 0; i < nlist; i++) {
        futures.push_back(std::async(std::launch::async, [this, list_nos, i]() {
            this->download_list(list_nos[i]);
        }));
    }
    // Wait for all downloads
    for (auto& f : futures) {
        f.wait();
    }
}
```

### Iterator Interface

For streaming large lists, use the iterator interface:

```cpp
class S3InvertedListsIterator : public faiss::InvertedListsIterator {
public:
    bool is_available() const override { return pos_ < size_; }
    void next() override { pos_++; }
    std::pair<idx_t, const uint8_t*> get_id_and_codes() override {
        // Stream from S3 chunk by chunk
    }
};
```

Enable it in your InvertedLists:

```cpp
S3InvertedLists::S3InvertedLists(...) {
    use_iterator = true;  // Enable iterator interface
}

InvertedListsIterator* S3InvertedLists::get_iterator(
    size_t list_no,
    void* inverted_list_context
) const override {
    return new S3InvertedListsIterator(this, list_no);
}
```

## Summary

| Approach | Can Use in Python? | Performance | Complexity | Use Case |
|----------|-------------------|-------------|------------|----------|
| **C++ outside Faiss** | ✅ Yes (with bindings) | Excellent | Medium | Production, custom backends |
| **Pure Python** | ❌ No (not supported) | N/A | N/A | Not possible |
| **Built-in classes** | ✅ Yes | Good | Low | Standard use cases |

### Recommendations

1. **For S3/Cloud Storage**: Use C++ implementation (Option 1)
   - Best performance
   - Full control over caching, prefetching
   - Can add Python bindings with SWIG

2. **For Simple Use Cases**: Use built-in classes
   - `OnDiskInvertedLists` for memory-mapped files
   - `VStackInvertedLists` for combining indexes
   - `StopWordsInvertedLists` for filtering

3. **For Quick Prototyping**: Mock with built-in classes
   - Use `OnDiskInvertedLists` with a local file as a prototype
   - Later replace with S3/remote implementation

## Your S3 Implementation

Your current implementation in [S3InvertedLists.h](../S3InvertedLists.h) and [S3InvertedLists.cpp](../S3InvertedLists.cpp) follows the **recommended approach** (Option 1):

```cpp
class S3InvertedLists : public faiss::InvertedLists {
    // Your implementation
};
```

**Next Steps:**

1. ✅ Complete the C++ implementation (what you're doing)
2. ✅ Test from C++ code ([demo_s3_ivf.cpp](../demo_s3_ivf.cpp))
3. Optional: Add Python bindings via SWIG
4. Optional: Add caching layer for better performance
5. Optional: Implement prefetch for search optimization

## References

- **InvertedLists Interface**: [faiss/invlists/InvertedLists.h](../../faiss/invlists/InvertedLists.h)
- **IndexIVF**: [faiss/IndexIVF.h](../../faiss/IndexIVF.h), [faiss/IndexIVF.cpp](../../faiss/IndexIVF.cpp)
- **replace_invlists() implementation**: [faiss/IndexIVF.cpp:1247-1257](../../faiss/IndexIVF.cpp#L1247-L1257)
- **Python bindings**: [faiss/python/__init__.py](../../faiss/python/__init__.py)
- **Test examples**: [tests/test_index_composite.py](../../tests/test_index_composite.py)
- **Your S3 implementation**: [S3InvertedLists.h](../S3InvertedLists.h), [S3InvertedLists.cpp](../S3InvertedLists.cpp)
- **Related: Python IO**: [python-io.claude.md](./python-io.claude.md)
