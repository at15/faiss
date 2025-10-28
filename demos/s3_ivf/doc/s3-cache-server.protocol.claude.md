# S3 Cache Server Protocol Specification

## Overview

This document specifies the TCP-based protocol for the Faiss S3 Cache Server. The protocol uses a Redis-like text format for commands and responses, with extensions for efficient binary data transfer.

## Connection Model

- **Transport**: TCP, default port 9001
- **Connection Type**: Persistent connections (multiple commands per connection)
- **Threading**: Server spawns one thread per client connection
- **Disconnection**: Graceful cleanup on client disconnect or socket errors

## Message Format

### Text Protocol

Commands and responses use a line-oriented text format:

```
<COMMAND> key1=value1 key2=value2 ...\n
```

**Rules**:
- Commands are case-sensitive (uppercase recommended)
- Parameters are space-separated `key=value` pairs
- Lines terminated with newline (`\n`)
- Values containing spaces must be URL-encoded or base64-encoded
- Maximum line length: 8192 bytes

### Binary Extensions

For array data (query vectors, result IDs, distances), binary sections follow the text line:

```
<TEXT_COMMAND>\n
<BINARY_LENGTH_4_BYTES><BINARY_DATA>
```

**Binary Format**:
- Length prefix: 4-byte little-endian unsigned integer
- Data: Raw binary array (float32 or int64)
- Multiple arrays: Each has its own length prefix

## Commands

### 1. ECHO - Connection Test

**Request**:
```
ECHO msg=<string>
```

**Response**:
```
msg=<string>
```

**Example**:
```
Client: ECHO msg=hello
Server: msg=hello
```

**Errors**:
- `MISSING_PARAM`: If `msg` parameter is missing

---

### 2. LOAD - Load S3-backed Index

**Request**:
```
LOAD bucket=<string> key=<string> cluster_data_offset=<int>
```

**Parameters**:
- `bucket`: S3 bucket name
- `key`: S3 object key (path to index file)
- `cluster_data_offset`: Byte offset where cluster data begins in the S3 object

**Response**:
```
index=<int>
```

**Returns**:
- `index`: Server-assigned unique index ID (monotonically increasing: 1, 2, 3, ...)

**Example**:
```
Client: LOAD bucket=my-bucket key=ivf_index.faiss cluster_data_offset=3154059
Server: index=1
```

**Errors**:
- `MISSING_PARAM`: Missing bucket, key, or cluster_data_offset
- `INVALID_PARAM`: Invalid cluster_data_offset (not a number)
- `LOAD_FAILED`: S3 connection failed or index metadata corrupt

**Notes**:
- Index remains loaded until server restart
- Same S3 location can be loaded multiple times (gets different IDs)
- Index ID is used for subsequent SEARCH commands

---

### 3. SEARCH - Search Index

**Request**:
```
SEARCH index=<int> k=<int> d=<int>
<QUERY_LENGTH><QUERY_VECTOR_FLOAT32>
```

**Parameters**:
- `index`: Index ID (from LOAD response)
- `k`: Number of nearest neighbors to return
- `d`: Dimension of query vector (used for validation)
- **Binary data**: The full query vector as `d` float32 values (4 bytes each)
  - The actual vector data is sent as binary, not just the dimension
  - Total binary bytes: `d * 4` (e.g., 128 dimensions = 512 bytes)

**Response**:
```
k=<int>
<IDS_LENGTH><IDS_INT64><DISTANCES_LENGTH><DISTANCES_FLOAT32>
```

**Returns**:
- `k`: Number of results returned (may be less than requested if index is small)
- Binary data 1: Result IDs as `k` int64 values (8 bytes each)
- Binary data 2: Distances as `k` float32 values (4 bytes each)

**Example**:
```
Client: SEARCH index=1 k=5 d=128\n
        [4 bytes: 512][512 bytes: 128 float32 values = full query vector]

Server: k=5\n
        [4 bytes: 40][40 bytes: 5 int64 values = result IDs]
        [4 bytes: 20][20 bytes: 5 float32 values = distances]
```

**Note on Query Vector Transmission**:
- The client sends the **complete query vector** as binary data, not just the dimension
- For a 128-dimensional vector: 128 floats × 4 bytes/float = 512 bytes of actual vector data
- The `d` parameter tells the server how many float32 values to expect (for validation)
- Example: For query vector `[0.1, 0.2, ..., 0.128]`, all 128 values are transmitted

**Errors**:
- `MISSING_PARAM`: Missing index, k, or d
- `INVALID_PARAM`: Invalid numbers or dimension mismatch
- `INDEX_NOT_FOUND`: Index ID doesn't exist
- `SEARCH_FAILED`: Search operation failed (e.g., S3 read error)
- `INVALID_BINARY`: Binary data length doesn't match expected size

**Notes**:
- Query vector dimension must match index dimension
- Results sorted by distance (nearest first)
- IDs are the original vector IDs from the index

---

### 4. INFO - Query Statistics

#### 4a. Global Cache Statistics

**Request**:
```
INFO about=cache
```

**Response**:
```
index_count=<int> cache_hits=<int> cache_misses=<int>
```

**Returns**:
- `index_count`: Number of loaded indexes
- `cache_hits`: Total cache hits across all indexes
- `cache_misses`: Total cache misses across all indexes

**Example**:
```
Client: INFO about=cache
Server: index_count=3 cache_hits=1542 cache_misses=87
```

---

#### 4b. Per-Index Statistics

**Request**:
```
INFO about=index id=<int>
```

**Response**:
```
cluster_count=<int> cache_hits=<int> cache_misses=<int> cached_clusters=<int>
```

**Returns**:
- `cluster_count`: Total number of clusters in index
- `cache_hits`: Cache hits for this index
- `cache_misses`: Cache misses for this index
- `cached_clusters`: Number of clusters currently cached

**Example**:
```
Client: INFO about=index id=1
Server: cluster_count=4096 cache_hits=542 cache_misses=32 cached_clusters=128
```

**Errors**:
- `MISSING_PARAM`: Missing `about` parameter
- `INVALID_PARAM`: Invalid `about` value or missing `id` for index query
- `INDEX_NOT_FOUND`: Index ID doesn't exist

---

## Error Handling

### Error Response Format

```
ERROR code=<ERROR_CODE> msg=<description>
```

**Example**:
```
Client: SEARCH index=999 k=10 d=128
Server: ERROR code=INDEX_NOT_FOUND msg=Index 999 not loaded
```

### Error Codes

| Code | Description | Common Causes |
|------|-------------|---------------|
| `INVALID_COMMAND` | Unknown command | Typo in command name |
| `MISSING_PARAM` | Required parameter missing | Incomplete command |
| `INVALID_PARAM` | Parameter value invalid | Wrong type, out of range |
| `INDEX_NOT_FOUND` | Index ID doesn't exist | Wrong ID or index not loaded |
| `SEARCH_FAILED` | Search operation failed | S3 error, corrupt data |
| `LOAD_FAILED` | Index load failed | S3 connection, invalid offset |
| `INVALID_BINARY` | Binary data corrupt | Length mismatch, connection error |
| `INTERNAL_ERROR` | Server internal error | Unexpected exception |

### Connection Error Handling

**Client Disconnect**:
- Server detects via `recv()` returning 0 or socket error
- Server logs disconnect and cleans up thread
- Loaded indexes remain available for other clients

**Server Shutdown**:
- Server sends no explicit shutdown message
- Existing connections receive RST on new requests
- Clients should reconnect or handle connection errors

**Malformed Requests**:
- Server returns ERROR response
- Connection remains open for retry
- Server may close connection after repeated errors (optional)

### Binary Data Validation

**Length Validation**:
```
expected_bytes = d * sizeof(float32)  // For query vector
expected_bytes = k * sizeof(int64)    // For result IDs
expected_bytes = k * sizeof(float32)  // For distances

if (binary_length != expected_bytes) {
    return ERROR code=INVALID_BINARY msg=Expected X bytes, got Y
}
```

**Partial Read Handling**:
- Server must read exact `length` bytes after reading length prefix
- If connection closes mid-read, return `INVALID_BINARY`
- Use blocking reads or loop until all bytes received

## Index ID Management

### ID Assignment Strategy

- **Monotonically Increasing**: Start at 1, increment for each LOAD
- **Thread-Safe**: Use atomic counter or mutex for ID generation
- **No Reuse**: IDs never reused, even after server restart
- **Persistent**: IDs valid for server lifetime only (not persisted)

### Example ID Lifecycle

```
Client A: LOAD bucket=b1 key=k1 cluster_data_offset=100
Server:   index=1

Client B: LOAD bucket=b2 key=k2 cluster_data_offset=200
Server:   index=2

Client A: LOAD bucket=b1 key=k1 cluster_data_offset=100  // Same S3 location
Server:   index=3  // New ID assigned

Client A: SEARCH index=1 k=10 d=128
Server:   [results from first load]

Client B: SEARCH index=2 k=10 d=128
Server:   [results from second load]
```

### Index Registry

Server maintains internal map:
```cpp
std::map<int, IndexState> loaded_indexes;

struct IndexState {
    int id;
    std::string bucket;
    std::string key;
    int64_t cluster_data_offset;
    // S3InvertedLists* invlists;  // Actual implementation
    // IndexIVF* index;             // Actual implementation
    int64_t cache_hits;
    int64_t cache_misses;
};
```

## Implementation Notes

### Thread Safety

- **Index Registry**: Protected by mutex (read/write access)
- **Per-Index Stats**: Atomic counters or per-index mutex
- **Global Stats**: Atomic counters
- **Socket I/O**: No locking needed (one thread per connection)

### Memory Management

- **Query Vectors**: Allocate buffer, free after search
- **Result Arrays**: Allocate for k results, free after send
- **Index Data**: Keep loaded indexes in memory until shutdown
- **Cache**: Implement LRU eviction (future enhancement)

### Performance Considerations

- **Binary Data**: Use zero-copy techniques where possible
- **Parsing**: Avoid string copies, use string_view
- **Locking**: Minimize critical sections (use read/write locks)
- **Connection Pooling**: Clients should reuse connections

### Future Extensions

Potential protocol extensions (not implemented yet):

1. **UNLOAD**: Remove index from server
   ```
   UNLOAD index=<int>
   ```

2. **BATCH_SEARCH**: Search multiple queries in one request
   ```
   BATCH_SEARCH index=<int> k=<int> d=<int> num_queries=<int>
   <binary: queries[num_queries][d]>
   ```

3. **PRELOAD_CLUSTERS**: Pre-fetch specific clusters
   ```
   PRELOAD_CLUSTERS index=<int> clusters=<comma-separated-list>
   ```

4. **CACHE_CONTROL**: Configure cache behavior
   ```
   CACHE_CONTROL action=clear
   CACHE_CONTROL action=set_size max_bytes=<int>
   ```

## Testing Recommendations

### Unit Tests

1. **Protocol Parsing**: Test command parser with valid/invalid inputs
2. **Binary Serialization**: Test float32/int64 array encoding/decoding
3. **Error Generation**: Test all error codes
4. **ID Assignment**: Test concurrent LOAD operations

### Integration Tests

1. **Connection Test**: ECHO command roundtrip
2. **Load Test**: LOAD with S3Mock, verify ID assignment
3. **Search Test**: SEARCH with dummy index, validate results
4. **Multi-Client**: Concurrent connections with different indexes
5. **Error Handling**: Invalid commands, missing parameters, bad IDs
6. **Disconnect**: Client disconnect mid-request
7. **Large Data**: Search with high-dimensional vectors

### Performance Tests

1. **Throughput**: Requests per second (single/multiple clients)
2. **Latency**: Search latency distribution
3. **Memory**: Cache size under load
4. **Concurrency**: Performance with N concurrent clients

## Example Session

```
# Client connects to localhost:9001

Client: ECHO msg=test_connection
Server: msg=test_connection

Client: LOAD bucket=my-bucket key=sift1m.ivf cluster_data_offset=3154059
Server: index=1

Client: SEARCH index=1 k=10 d=128
        [4 bytes: 512][512 bytes: float32[128]]
Server: k=10
        [4 bytes: 80][80 bytes: int64[10]]
        [4 bytes: 40][40 bytes: float32[10]]

Client: INFO about=index id=1
Server: cluster_count=4096 cache_hits=1 cache_misses=10 cached_clusters=10

Client: INFO about=cache
Server: index_count=1 cache_hits=1 cache_misses=10

Client: SEARCH index=999 k=10 d=128
Server: ERROR code=INDEX_NOT_FOUND msg=Index 999 not loaded

# Client disconnects
```
