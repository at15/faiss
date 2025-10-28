# Faiss Python IO: Reading Indexes with Custom Readers

## Overview

Faiss Python bindings support both filename-based and IO reader object-based reading, just like the C++ API. This allows you to read indexes from custom storage backends (S3, HTTP, compressed files, pipes, etc.) by implementing or using the `IOReader` interface.

## Available Signatures

The `read_index()` function has two overloads in Python:

```python
# From filename (string)
index = faiss.read_index(fname, io_flags=0)

# From IO reader object (implements IOReader interface)
index = faiss.read_index(reader, io_flags=0)
```

**Source:** [faiss/index_io.h:67-69](../../faiss/index_io.h#L67-L69)

```cpp
Index* read_index(const char* fname, int io_flags = 0);
Index* read_index(FILE* f, int io_flags = 0);
Index* read_index(IOReader* reader, int io_flags = 0);
```

## The IOReader Interface

The base `IOReader` interface is simple and designed for sequential reading:

**Source:** [faiss/impl/io.h:27-38](../../faiss/impl/io.h#L27-L38)

```cpp
struct IOReader {
    // name that can be used in error messages
    std::string name;

    // fread. Returns number of items read or 0 in case of EOF.
    virtual size_t operator()(void* ptr, size_t size, size_t nitems) = 0;

    // return a file number that can be memory-mapped
    virtual int filedescriptor();

    virtual ~IOReader() {}
};
```

**Key Design Features:**
- Sequential I/O only (no seek required)
- Works with pipes and streams
- Optional file descriptor for memory mapping
- Name field for better error messages

## Built-in IOReader Implementations

### 1. FileIOReader

Reads from a file on disk.

**Source:** [faiss/impl/io.h:64-77](../../faiss/impl/io.h#L64-L77)

```python
# From filename
reader = faiss.FileIOReader(filename)
index = faiss.read_index(reader)
```

**Example:** [tests/test_io.py:336-338](../../tests/test_io.py#L336-L338)

```python
reader = faiss.FileIOReader(fname)
vt = faiss.read_VectorTransform(reader)
del reader
```

### 2. VectorIOReader

Reads from an in-memory byte vector.

**Source:** [faiss/impl/io.h:53-57](../../faiss/impl/io.h#L53-L57)

```python
reader = faiss.VectorIOReader()
# Populate reader.data with bytes
faiss.copy_array_to_vector(byte_array, reader.data)
index = faiss.read_index(reader)
```

**Example:** [tests/test_index_composite.py:516-519](../../tests/test_index_composite.py#L516-L519)

```python
# Direct transfer of vector
reader = faiss.VectorIOReader()
reader.data.swap(writer.data)
index2 = faiss.read_index(reader)
```

**Also used for deserialization:** [faiss/python/__init__.py:325-328](../../faiss/python/__init__.py#L325-L328)

```python
def deserialize_index(data, io_flags=0):
    reader = faiss.VectorIOReader()
    copy_array_to_vector(data, reader.data)
    return read_index(reader, io_flags)
```

### 3. PyCallbackIOReader

Uses a Python callback function for reading. This is the most flexible option for custom backends.

**Source:** [faiss/python/python_callbacks.h:36-47](../../faiss/python/python_callbacks.h#L36-L47)

```cpp
struct PyCallbackIOReader : faiss::IOReader {
    PyObject* callback;
    size_t bs; // maximum buffer size

    /** Callback: Python function that takes a size and returns a
     * bytes object with the resulting read */
    explicit PyCallbackIOReader(PyObject* callback, size_t bs = 1024 * 1024);

    size_t operator()(void* ptrv, size_t size, size_t nitems) override;
    ~PyCallbackIOReader() override;
};
```

**Python Usage:**

```python
# The callback should take a size parameter and return bytes
def read_callback(size):
    return data_source.read(size)

reader = faiss.PyCallbackIOReader(read_callback, buffer_size=1024*1024)
index = faiss.read_index(reader)
```

**Example 1: Reading from Python file object** - [tests/test_io.py:131-137](../../tests/test_io.py#L131-L137)

```python
with open(fname, 'rb') as f:
    reader = faiss.PyCallbackIOReader(f.read, 1234)

    if bsz > 0:
        reader = faiss.BufferedIOReader(reader, bsz)

    index2 = faiss.read_index(reader)
```

**Example 2: Reading from Unix pipe** - [tests/test_io.py:209-211](../../tests/test_io.py#L209-L211)

```python
def index_from_pipe():
    reader = faiss.PyCallbackIOReader(lambda size: os.read(rf, size))
    return faiss.read_index(reader)

with ThreadPool(1) as pool:
    fut = pool.apply_async(index_from_pipe, ())
    # ... write to pipe ...
    index2 = fut.get()
```

**Example 3: Reading raw bytes** - [tests/test_io.py:98-115](../../tests/test_io.py#L98-L115)

```python
def test_buf_read(self):
    x = np.random.uniform(size=20)

    fd, fname = tempfile.mkstemp()
    os.close(fd)
    try:
        x.tofile(fname)

        with open(fname, 'rb') as f:
            reader = faiss.PyCallbackIOReader(f.read, 1234)

            bsz = 123
            reader = faiss.BufferedIOReader(reader, bsz)

            y = np.zeros_like(x)
            reader(faiss.swig_ptr(y), y.nbytes, 1)

        np.testing.assert_array_equal(x, y)
    finally:
        if os.path.exists(fname):
            os.unlink(fname)
```

### 4. BufferedIOReader

Wraps another IOReader to perform buffered reads, reducing the number of system calls.

**Source:** [faiss/impl/io.h:103-118](../../faiss/impl/io.h#L103-L118)

```cpp
struct BufferedIOReader : IOReader {
    IOReader* reader;
    size_t bsz;
    size_t ofs;    ///< offset in input stream
    size_t ofs2;   ///< number of bytes returned to caller
    size_t b0, b1; ///< range of available bytes in the buffer
    std::vector<char> buffer;

    /**
     * @param bsz    buffer size (bytes). Reads will be done by batched of
     *               this size
     */
    explicit BufferedIOReader(IOReader* reader, size_t bsz = 1024 * 1024);

    size_t operator()(void* ptr, size_t size, size_t nitems) override;
};
```

**Python Usage:**

```python
reader = faiss.BufferedIOReader(
    faiss.FileIOReader(fname),
    buffer_size=1234
)
index = faiss.read_index(reader)
```

**Example:** [tests/test_io.py:180-183](../../tests/test_io.py#L180-L183)

```python
reader = faiss.BufferedIOReader(
    faiss.FileIOReader(fname), 1234)

index2 = faiss.read_index(reader)
```

**Note:** BufferedIOReader keeps a reference to the underlying reader - [faiss/python/__init__.py:210-211](../../faiss/python/__init__.py#L210-L211)

```python
add_ref_in_constructor(BufferedIOReader, 0)
```

### 5. ZeroCopyIOReader

Zero-copy reading from an existing memory buffer.

**Example:** [tests/test_io.py:530-536](../../tests/test_io.py#L530-L536)

```python
def test_zerocopy(self):
    # ... create index ...

    serialized_index = faiss.serialize_index(index)
    reader = faiss.ZeroCopyIOReader(
        faiss.swig_ptr(serialized_index),
        serialized_index.size
    )
    index2 = faiss.read_index(reader)
```

## IOWriter Interface

Similarly, writing indexes supports both filenames and IOWriter objects:

**Source:** [faiss/impl/io.h:40-51](../../faiss/impl/io.h#L40-L51)

```cpp
struct IOWriter {
    std::string name;

    // fwrite. Return number of items written
    virtual size_t operator()(const void* ptr, size_t size, size_t nitems) = 0;

    // return a file number that can be memory-mapped
    virtual int filedescriptor();

    virtual ~IOWriter() noexcept(false) {}
};
```

### PyCallbackIOWriter

**Source:** [faiss/python/python_callbacks.h:22-34](../../faiss/python/python_callbacks.h#L22-L34)

```cpp
struct PyCallbackIOWriter : faiss::IOWriter {
    PyObject* callback;
    size_t bs; // maximum write size

    /** Callback: Python function that takes a bytes object and
     *  returns the number of bytes successfully written.
     */
    explicit PyCallbackIOWriter(PyObject* callback, size_t bs = 1024 * 1024);

    size_t operator()(const void* ptrv, size_t size, size_t nitems) override;

    ~PyCallbackIOWriter() override;
};
```

**Example:** [tests/test_io.py:68-76](../../tests/test_io.py#L68-L76)

```python
f = io.BytesIO()
# test with small block size
writer = faiss.PyCallbackIOWriter(f.write, 1234)

if bsz > 0:
    writer = faiss.BufferedIOWriter(writer, bsz)

faiss.write_index(index, writer)
del writer   # make sure all writes committed
```

**Pipe example:** [tests/test_io.py:217-218](../../tests/test_io.py#L217-L218)

```python
# write to pipe
writer = faiss.PyCallbackIOWriter(lambda b: os.write(wf, b))
faiss.write_index(index, writer)
```

## Advanced Python Wrapper Features

### IOReader.read_bytes() Method

The Python bindings add a convenience method to IOReader:

**Source:** [faiss/python/class_wrappers.py:1150-1158](../../faiss/python/class_wrappers.py#L1150-L1158)

```python
def handle_IOReader(the_class):
    """ add a read_bytes method """

    def read_bytes(self, totsz):
        buf = bytearray(totsz)
        was_read = self(swig_ptr(buf), 1, len(buf))
        return bytes(buf[:was_read])

    the_class.read_bytes = read_bytes
```

This is applied to all IOReader classes - [faiss/python/__init__.py:40](../../faiss/python/__init__.py#L40)

```python
class_wrappers.handle_IOReader(IOReader)
```

## IO Flags

Several IO flags control index reading behavior:

**Source:** [faiss/index_io.h:33-65](../../faiss/index_io.h#L33-L65)

```cpp
/// skip the storage for graph-based indexes
const int IO_FLAG_SKIP_STORAGE = 1;

// The read_index flags are implemented only for a subset of index types.
const int IO_FLAG_READ_ONLY = 2;

// strip directory component from ondisk filename, and assume it's in
// the same directory as the index file
const int IO_FLAG_ONDISK_SAME_DIR = 4;

// don't load IVF data to RAM, only list sizes
const int IO_FLAG_SKIP_IVF_DATA = 8;

// don't initialize precomputed table after loading
const int IO_FLAG_SKIP_PRECOMPUTE_TABLE = 16;

// don't compute the sdc table for PQ-based indices
const int IO_FLAG_PQ_SKIP_SDC_TABLE = 32;

// try to memmap data (useful to load an ArrayInvertedLists as an
// OnDiskInvertedLists)
const int IO_FLAG_MMAP = IO_FLAG_SKIP_IVF_DATA | 0x646f0000;

// mmap that handles codes for IndexFlatCodes-derived indices and HNSW.
const int IO_FLAG_MMAP_IFC = 1 << 9;
```

**Example:** [tests/test_io.py:471-473](../../tests/test_io.py#L471-L473)

```python
index_a = faiss.read_index(fname)
index_b = faiss.read_index(
    fname, faiss.IO_FLAG_SKIP_PRECOMPUTE_TABLE)
```

**Memory mapping example:** [tests/test_io.py:507](../../tests/test_io.py#L507)

```python
index2 = faiss.read_index(fname, faiss.IO_FLAG_MMAP_IFC)
```

## Use Cases for Custom IOReaders

### 1. Cloud Storage (S3, GCS, Azure Blob)

```python
import boto3

def s3_reader(bucket, key):
    s3 = boto3.client('s3')
    obj = s3.get_object(Bucket=bucket, Key=key)
    stream = obj['Body']

    def read_callback(size):
        return stream.read(size)

    return faiss.PyCallbackIOReader(read_callback)

# Usage
reader = s3_reader('my-bucket', 'indexes/my_index.faiss')
index = faiss.read_index(reader)
```

### 2. HTTP/HTTPS Streaming

```python
import requests

def http_reader(url):
    response = requests.get(url, stream=True)
    response.raise_for_status()

    iterator = response.iter_content(chunk_size=1024*1024)
    buffer = bytearray()

    def read_callback(size):
        nonlocal buffer
        while len(buffer) < size:
            try:
                chunk = next(iterator)
                buffer.extend(chunk)
            except StopIteration:
                break

        result = bytes(buffer[:size])
        buffer = buffer[size:]
        return result

    return faiss.PyCallbackIOReader(read_callback)

# Usage
reader = http_reader('https://example.com/my_index.faiss')
index = faiss.read_index(reader)
```

### 3. Compressed Files

```python
import gzip

def gzip_reader(filename):
    f = gzip.open(filename, 'rb')
    return faiss.PyCallbackIOReader(f.read)

# Usage
reader = gzip_reader('index.faiss.gz')
index = faiss.read_index(reader)
```

### 4. Range Requests (Partial Downloads)

```python
import requests

class RangeReader:
    def __init__(self, url):
        self.url = url
        self.offset = 0

    def read(self, size):
        headers = {
            'Range': f'bytes={self.offset}-{self.offset + size - 1}'
        }
        response = requests.get(self.url, headers=headers)
        response.raise_for_status()
        self.offset += len(response.content)
        return response.content

# Usage
range_reader = RangeReader('https://example.com/large_index.faiss')
reader = faiss.PyCallbackIOReader(range_reader.read)
index = faiss.read_index(reader)
```

## Other Related Functions

All index I/O functions support both filenames and reader/writer objects:

```python
# VectorTransform
faiss.write_VectorTransform(vt, writer)
vt = faiss.read_VectorTransform(reader)

# ProductQuantizer
faiss.write_ProductQuantizer(pq, writer)
pq = faiss.read_ProductQuantizer(reader)

# InvertedLists
faiss.write_InvertedLists(ils, writer)
ils = faiss.read_InvertedLists(reader, io_flags)

# Binary indexes
faiss.write_index_binary(index, writer)
index = faiss.read_index_binary(reader, io_flags)
```

## Summary

The Faiss Python bindings provide full access to the flexible C++ I/O system:

1. **Multiple built-in readers**: FileIOReader, VectorIOReader, PyCallbackIOReader, BufferedIOReader, ZeroCopyIOReader
2. **Custom backends**: Implement custom storage via PyCallbackIOReader
3. **Buffering support**: Wrap any reader with BufferedIOReader for performance
4. **Streaming support**: Sequential reading works with pipes, network streams, etc.
5. **Zero-copy**: Read from existing memory buffers without copying
6. **IO flags**: Control loading behavior (skip precompute, mmap, etc.)

This flexibility allows you to load Faiss indexes from any source: local files, cloud storage (S3/GCS), HTTP endpoints, compressed files, or custom storage systems.

## References

- **C++ IO Interface**: [faiss/impl/io.h](../../faiss/impl/io.h)
- **Index I/O Functions**: [faiss/index_io.h](../../faiss/index_io.h)
- **Python Callbacks**: [faiss/python/python_callbacks.h](../../faiss/python/python_callbacks.h), [faiss/python/python_callbacks.cpp](../../faiss/python/python_callbacks.cpp)
- **Python Wrappers**: [faiss/python/class_wrappers.py](../../faiss/python/class_wrappers.py)
- **Python Init**: [faiss/python/__init__.py](../../faiss/python/__init__.py)
- **Test Examples**: [tests/test_io.py](../../tests/test_io.py)
