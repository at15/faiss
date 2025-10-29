# S3 Cache Server - Real Implementation Design

## Overview

This document describes the implementation of the real S3 cache server that integrates:
- **TCP Protocol**: From `test_tcp_server.cpp` (text + binary protocol)
- **Faiss Search**: Vector similarity search using IndexIVFFlat
- **S3 On-Demand Loading**: From `S3OnDemandInvertedLists` (lazy cluster fetching)
- **Quora Dataset**: 100K question embeddings for testing

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    TCP Client (Python)                       │
│  ┌────────────────┐    ┌──────────────┐    ┌──────────────┐│
│  │ Load Embeddings│    │Encode Queries│    │ Map ID→Text  ││
│  │  (quora.pt)    │    │(SentenceXfmr)│    │              ││
│  └────────────────┘    └──────────────┘    └──────────────┘│
└─────────────────────────────────────────────────────────────┘
                              │
                    TCP (LOAD, SEARCH, INFO)
                              ↓
┌─────────────────────────────────────────────────────────────┐
│                   S3 Cache Server (C++)                      │
│  ┌──────────────────────────────────────────────────────┐  │
│  │  ServerState                                          │  │
│  │  ┌───────────────────────────────────────────┐      │  │
│  │  │ IndexState (per loaded index)             │      │  │
│  │  │  - Faiss IndexIVFFlat                     │      │  │
│  │  │  - S3CrtClient                            │      │  │
│  │  │  - S3OnDemandInvertedLists                │      │  │
│  │  │    ├─ Cluster cache (LRU-like)            │      │  │
│  │  │    ├─ Cache hits/misses                   │      │  │
│  │  │    └─ S3 range request logic              │      │  │
│  │  └───────────────────────────────────────────┘      │  │
│  └──────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
                              ↓
                    S3 Range Requests
                              ↓
┌─────────────────────────────────────────────────────────────┐
│                      Amazon S3                               │
│  quora-index-quora-distilbert-multilingual.idx              │
│  ┌─────────────────┬──────────────────────────────────────┐│
│  │ Index Metadata  │      Cluster Data                    ││
│  │  (0 - 3154058)  │   (3154059 - 311154059)              ││
│  │  - Quantizer    │   - Cluster 0: codes + ids           ││
│  │  - nlist=1024   │   - Cluster 1: codes + ids           ││
│  │  - d=768        │   - ...                              ││
│  │  - code_size    │   - Cluster 1023: codes + ids        ││
│  └─────────────────┴──────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────┘
```

## Data Structures

### IndexState (Enhanced)

```cpp
struct IndexState {
    // Identification
    int id;                          // Server-assigned ID (1, 2, 3, ...)
    std::string bucket;              // S3 bucket name
    std::string key;                 // S3 object key
    int64_t cluster_data_offset;     // Byte offset where clusters start

    // Faiss components (NEW)
    std::shared_ptr<faiss::Index> index;              // The Faiss index
    std::shared_ptr<Aws::S3Crt::S3CrtClient> s3_client;  // S3 client
    faiss_s3::S3OnDemandInvertedLists* s3_invlists;   // Lazy-loading inverted lists

    // Statistics (read from s3_invlists)
    int64_t get_cache_hits() const {
        return s3_invlists ? s3_invlists->cache_hits() : 0;
    }

    int64_t get_cache_misses() const {
        return s3_invlists ? s3_invlists->cache_misses() : 0;
    }

    size_t get_cached_clusters() const {
        return s3_invlists ? s3_invlists->cache_size() : 0;
    }
};
```

### ServerState (Enhanced)

```cpp
class ServerState {
public:
    // AWS SDK lifecycle
    Aws::SDKOptions sdk_options;

    ServerState() {
        // Initialize AWS SDK
        Aws::InitAPI(sdk_options);

        // Register S3 IO hook for Faiss
        faiss_s3::register_s3_io_hook();
    }

    ~ServerState() {
        // Cleanup indexes
        {
            std::lock_guard<std::mutex> lock(indexes_mutex);
            loaded_indexes.clear();
        }

        // Shutdown AWS SDK
        Aws::ShutdownAPI(sdk_options);
    }

    // Index management
    std::atomic<int> next_index_id{1};
    std::map<int, IndexState> loaded_indexes;
    std::mutex indexes_mutex;

    // Global statistics
    std::atomic<int64_t> global_cache_hits{0};
    std::atomic<int64_t> global_cache_misses{0};

    int add_index(
        const std::string& bucket,
        const std::string& key,
        int64_t cluster_data_offset,
        std::shared_ptr<faiss::Index> index,
        std::shared_ptr<Aws::S3Crt::S3CrtClient> s3_client,
        faiss_s3::S3OnDemandInvertedLists* s3_invlists
    );

    bool get_index(int id, IndexState** out_state);
    int get_index_count();
};
```

## LOAD Command Implementation

### Flow

1. **Parse Parameters**: Extract bucket, key, cluster_data_offset
2. **Create S3 Client**: Configure with credentials and endpoint
3. **Download Metadata**: S3 range request for bytes `0` to `cluster_data_offset - 1`
4. **Parse Index**: Use `faiss::read_index()` with `IO_FLAG_S3` to parse metadata
5. **Extract Placeholder**: Get `S3ReadNothingInvertedLists` from parsed index
6. **Create S3 Inverted Lists**: Initialize `S3OnDemandInvertedLists` with S3 details
7. **Replace Inverted Lists**: Attach to index
8. **Store in Registry**: Add to server's index map
9. **Return Metadata**: Send index ID + dimension info to client

### Code Implementation

```cpp
void ClientHandler::handle_load(const std::map<std::string, std::string>& params) {
    // 1. Parse parameters
    auto bucket_it = params.find("bucket");
    auto key_it = params.find("key");
    auto offset_it = params.find("cluster_data_offset");

    if (bucket_it == params.end() || key_it == params.end() ||
        offset_it == params.end()) {
        send_error("MISSING_PARAM",
                   "Missing required parameters (bucket, key, cluster_data_offset)");
        return;
    }

    std::string bucket = bucket_it->second;
    std::string key = key_it->second;
    int64_t cluster_data_offset;

    try {
        cluster_data_offset = std::stoll(offset_it->second);
    } catch (...) {
        send_error("INVALID_PARAM", "Invalid cluster_data_offset");
        return;
    }

    std::cout << "[Client " << client_socket << "] Loading index: "
              << "s3://" << bucket << "/" << key
              << " (offset=" << cluster_data_offset << ")" << std::endl;

    try {
        // 2. Create S3 client
        auto s3_client = create_s3_client();

        // 3. Download index metadata (everything before cluster data)
        std::cout << "[Client " << client_socket << "] Downloading metadata (0-"
                  << cluster_data_offset - 1 << ")" << std::endl;

        auto metadata_bytes = DownloadRangeFromS3(
            s3_client, bucket, key, 0, cluster_data_offset);

        // 4. Parse index with S3 flag
        faiss::VectorIOReader reader;
        reader.data = std::move(metadata_bytes);

        faiss::Index* raw_index = faiss::read_index(
            &reader, faiss_s3::IO_FLAG_S3);

        // Cast to IVF index
        auto* ivf_index = dynamic_cast<faiss::IndexIVFFlat*>(raw_index);
        if (!ivf_index) {
            delete raw_index;
            send_error("LOAD_FAILED", "Index is not IndexIVFFlat type");
            return;
        }

        std::cout << "[Client " << client_socket << "] Parsed index: "
                  << "d=" << ivf_index->d
                  << ", ntotal=" << ivf_index->ntotal
                  << ", nlist=" << ivf_index->nlist << std::endl;

        // 5. Extract placeholder inverted lists
        auto* placeholder = dynamic_cast<faiss_s3::S3ReadNothingInvertedLists*>(
            ivf_index->invlists);

        if (!placeholder) {
            delete raw_index;
            send_error("LOAD_FAILED", "Invalid inverted lists type");
            return;
        }

        // 6. Create S3OnDemandInvertedLists
        auto* s3_invlists = new faiss_s3::S3OnDemandInvertedLists(
            s3_client,
            bucket,
            key,
            cluster_data_offset,
            ivf_index->nlist,
            ivf_index->code_size,
            placeholder->cluster_sizes
        );

        // 7. Replace inverted lists
        ivf_index->replace_invlists(s3_invlists, true);  // owns=true

        // 8. Store in server state
        std::shared_ptr<faiss::Index> index_ptr(raw_index);
        int index_id = server_state->add_index(
            bucket, key, cluster_data_offset,
            index_ptr, s3_client, s3_invlists);

        std::cout << "[Client " << client_socket << "] Index loaded with ID="
                  << index_id << std::endl;

        // 9. Send response with metadata
        std::map<std::string, std::string> response;
        response["index"] = std::to_string(index_id);
        response["d"] = std::to_string(ivf_index->d);
        response["ntotal"] = std::to_string(ivf_index->ntotal);
        response["nlist"] = std::to_string(ivf_index->nlist);
        response["metric_type"] = std::to_string(ivf_index->metric_type);

        send_response(ProtocolParser::format_response(response));

    } catch (const std::exception& e) {
        send_error("LOAD_FAILED", std::string("Failed to load index: ") + e.what());
    }
}
```

### Helper: S3 Client Creation

```cpp
static std::shared_ptr<Aws::S3Crt::S3CrtClient> create_s3_client() {
    Aws::S3Crt::ClientConfiguration config;

    // Check for custom endpoint (for S3Mock or MinIO)
    const char* endpoint = std::getenv("S3_ENDPOINT_URL");
    if (endpoint) {
        config.endpointOverride = endpoint;
    }

    // Check for region
    const char* region = std::getenv("AWS_REGION");
    if (region) {
        config.region = region;
    } else {
        config.region = "us-east-1";  // Default
    }

    return std::make_shared<Aws::S3Crt::S3CrtClient>(config);
}
```

### Helper: S3 Range Download

```cpp
static std::vector<uint8_t> DownloadRangeFromS3(
        std::shared_ptr<Aws::S3Crt::S3CrtClient> client,
        const std::string& bucket,
        const std::string& key,
        size_t offset,
        size_t size) {

    Aws::S3Crt::Model::GetObjectRequest request;
    request.SetBucket(bucket);
    request.SetKey(key);

    // S3 range format: "bytes=start-end" (end is inclusive)
    std::ostringstream range_stream;
    range_stream << "bytes=" << offset << "-" << (offset + size - 1);
    request.SetRange(range_stream.str());

    auto outcome = client->GetObject(request);

    if (!outcome.IsSuccess()) {
        throw std::runtime_error(
            "S3 GetObject failed: " +
            outcome.GetError().GetMessage());
    }

    auto& stream = outcome.GetResultWithOwnership().GetBody();
    std::vector<uint8_t> data(size);
    stream.read(reinterpret_cast<char*>(data.data()), size);

    if (stream.gcount() != static_cast<std::streamsize>(size)) {
        throw std::runtime_error(
            "S3 read size mismatch: expected " + std::to_string(size) +
            ", got " + std::to_string(stream.gcount()));
    }

    return data;
}
```

## SEARCH Command Implementation

### Flow

1. **Parse Parameters**: Extract index ID, k, dimension
2. **Read Query Vector**: Binary data (d × 4 bytes of float32)
3. **Validate**: Check dimension matches index
4. **Get Index**: Retrieve from server state
5. **Perform Search**: Call `index->search()` (triggers S3 fetching as needed)
6. **Return Results**: Send (IDs, distances) as binary arrays

### Code Implementation

```cpp
void ClientHandler::handle_search(const std::map<std::string, std::string>& params) {
    // 1. Parse parameters
    auto index_it = params.find("index");
    auto k_it = params.find("k");
    auto d_it = params.find("d");

    if (index_it == params.end() || k_it == params.end() || d_it == params.end()) {
        send_error("MISSING_PARAM", "Missing required parameters (index, k, d)");
        return;
    }

    int index_id, k, d;
    try {
        index_id = std::stoi(index_it->second);
        k = std::stoi(k_it->second);
        d = std::stoi(d_it->second);
    } catch (...) {
        send_error("INVALID_PARAM", "Invalid parameter values");
        return;
    }

    // 2. Read query vector (binary) - MUST read before validation
    //    to consume socket data
    std::vector<uint8_t> query_data;
    if (!BinaryIO::read_binary_array(client_socket, query_data)) {
        send_error("INVALID_BINARY", "Failed to read query vector");
        return;
    }

    // 3. Validate query vector size
    size_t expected_size = d * sizeof(float);
    if (query_data.size() != expected_size) {
        send_error("INVALID_BINARY",
                   "Expected " + std::to_string(expected_size) +
                   " bytes, got " + std::to_string(query_data.size()));
        return;
    }

    // 4. Get index from server state
    IndexState* index_state;
    if (!server_state->get_index(index_id, &index_state)) {
        send_error("INDEX_NOT_FOUND",
                   "Index " + std::to_string(index_id) + " not loaded");
        return;
    }

    // Cast to IVF index
    auto* ivf_index = dynamic_cast<faiss::IndexIVFFlat*>(
        index_state->index.get());

    if (!ivf_index) {
        send_error("INTERNAL_ERROR", "Index is not IVF type");
        return;
    }

    // 5. Validate dimension
    if (ivf_index->d != static_cast<size_t>(d)) {
        send_error("INVALID_PARAM",
                   "Dimension mismatch: index has d=" +
                   std::to_string(ivf_index->d) +
                   ", query has d=" + std::to_string(d));
        return;
    }

    std::cout << "[Client " << client_socket << "] Search: "
              << "index=" << index_id << ", k=" << k << ", d=" << d << std::endl;

    try {
        // 6. Perform Faiss search
        const float* query = reinterpret_cast<const float*>(query_data.data());

        std::vector<float> distances(k);
        std::vector<faiss::idx_t> labels(k);

        // This will trigger on-demand S3 fetching via S3OnDemandInvertedLists
        ivf_index->search(1, query, k, distances.data(), labels.data());

        std::cout << "[Client " << client_socket << "] Search completed: "
                  << "cache_hits=" << index_state->s3_invlists->cache_hits()
                  << ", cache_misses=" << index_state->s3_invlists->cache_misses()
                  << std::endl;

        // 7. Send text response
        std::map<std::string, std::string> response;
        response["k"] = std::to_string(k);
        if (!send_response(ProtocolParser::format_response(response))) {
            return;
        }

        // 8. Send binary data: IDs (int64)
        if (!BinaryIO::write_binary_array(client_socket, labels.data(),
                                          k * sizeof(faiss::idx_t))) {
            std::cout << "[Client " << client_socket
                      << "] Failed to send result IDs" << std::endl;
            return;
        }

        // 9. Send binary data: distances (float32)
        if (!BinaryIO::write_binary_array(client_socket, distances.data(),
                                          k * sizeof(float))) {
            std::cout << "[Client " << client_socket
                      << "] Failed to send distances" << std::endl;
            return;
        }

        std::cout << "[Client " << client_socket << "] Results sent successfully"
                  << std::endl;

    } catch (const std::exception& e) {
        send_error("SEARCH_FAILED", std::string("Search failed: ") + e.what());
    }
}
```

### Search Mechanics

When `index->search()` is called:

1. **Quantizer Phase**: Faiss finds the `nprobe` nearest cluster centroids to the query
2. **Cluster Fetching**: For each of the `nprobe` clusters:
   - `S3OnDemandInvertedLists::get_codes(cluster_id)` is called
   - Checks cache - if hit, return cached data
   - If miss, fetch from S3 using range request
   - Cache the cluster data
3. **Distance Computation**: Compute distances within fetched clusters
4. **Top-K Selection**: Select k nearest neighbors across all clusters
5. **Return Results**: (ids, distances) arrays

### S3 Range Request Example

For cluster 5 in quora index:
- **Cluster size**: 97 vectors
- **Code size**: 3072 bytes per vector
- **Offset calculation**:
  ```
  offset = cluster_data_offset + sum(sizes[0:5] * (code_size + 8))
         = 3154059 + (59+102+98+88+115) * (3072 + 8)
         = 3154059 + 462 * 3080
         = 3154059 + 1422960
         = 4577019
  ```
- **Size**: `97 * 3072 + 97 * 8 = 297984 + 776 = 298760 bytes`
- **S3 Request**: `GET /key Range: bytes=4577019-4875778`

## INFO Command Implementation

### Global Cache Statistics

```cpp
if (about == "cache") {
    int index_count = server_state->get_index_count();

    // Aggregate statistics from all indexes
    int64_t total_hits = 0;
    int64_t total_misses = 0;

    {
        std::lock_guard<std::mutex> lock(server_state->indexes_mutex);
        for (const auto& [id, state] : server_state->loaded_indexes) {
            if (state.s3_invlists) {
                total_hits += state.s3_invlists->cache_hits();
                total_misses += state.s3_invlists->cache_misses();
            }
        }
    }

    response["index_count"] = std::to_string(index_count);
    response["cache_hits"] = std::to_string(total_hits);
    response["cache_misses"] = std::to_string(total_misses);
    send_response(ProtocolParser::format_response(response));
}
```

### Per-Index Statistics

```cpp
if (about == "index") {
    int index_id = std::stoi(params.at("id"));

    IndexState* state;
    if (!server_state->get_index(index_id, &state)) {
        send_error("INDEX_NOT_FOUND", "Index not found");
        return;
    }

    auto* ivf = dynamic_cast<faiss::IndexIVFFlat*>(state->index.get());

    response["cluster_count"] = std::to_string(ivf->nlist);
    response["cache_hits"] = std::to_string(state->s3_invlists->cache_hits());
    response["cache_misses"] = std::to_string(state->s3_invlists->cache_misses());
    response["cached_clusters"] = std::to_string(state->s3_invlists->cache_size());
    response["nprobe"] = std::to_string(ivf->nprobe);

    send_response(ProtocolParser::format_response(response));
}
```

## Test Client Implementation (Python)

### File: `test_s3_cache_server.py`

```python
#!/usr/bin/env python3
"""
Test the real S3 cache server with Quora dataset
"""

import pickle
import numpy as np
from sentence_transformers import SentenceTransformer
from test_tcp_client import S3CacheClient

# Configuration
EMBEDDING_FILE = "quora-embeddings-quora-distilbert-multilingual-size-100000.pt"
INDEX_BUCKET = "test-bucket"  # Or your actual bucket
INDEX_KEY = "quora/quora-index-quora-distilbert-multilingual-size-100000.idx"
CLUSTER_DATA_OFFSET = 3154059  # From metadata JSON

def load_embeddings():
    """Load quora embeddings and text"""
    print(f"Loading embeddings from {EMBEDDING_FILE}...")
    with open(EMBEDDING_FILE, "rb") as f:
        cache_data = pickle.load(f)

    sentences = cache_data["sentences"]
    embeddings = cache_data["embeddings"]

    print(f"Loaded {len(sentences)} sentences with {embeddings.shape[1]}-dim embeddings")
    return sentences, embeddings


def normalize_embedding(embedding):
    """Normalize embedding for cosine similarity (Inner Product metric)"""
    return embedding / np.linalg.norm(embedding)


def test_search():
    """Test semantic search with real queries"""

    # Load text mapping
    corpus_sentences, _ = load_embeddings()

    # Load model for encoding queries
    print("Loading SentenceTransformer model...")
    model = SentenceTransformer("quora-distilbert-multilingual")

    # Test queries
    queries = [
        "How to find a job",
        "What to eat for lunch",
        "Which sport is similar to tennis",
        "How do I learn programming",
        "What is the best way to lose weight"
    ]

    print("\n" + "=" * 80)
    print("Testing S3 Cache Server with Quora Dataset")
    print("=" * 80)

    with S3CacheClient() as client:
        # Test 1: Load index
        print("\n[Test 1] Loading index from S3...")
        index_info = client.load(INDEX_BUCKET, INDEX_KEY, CLUSTER_DATA_OFFSET)
        index_id = index_info  # Will enhance protocol to return metadata

        print(f"  Index loaded: ID={index_id}")

        # Test 2: Perform searches
        print(f"\n[Test 2] Performing {len(queries)} searches...")

        for i, query in enumerate(queries, 1):
            print(f"\n--- Query {i}: \"{query}\" ---")

            # Encode query
            embedding = model.encode(query, convert_to_numpy=True)
            embedding = normalize_embedding(embedding)
            embedding = embedding.astype(np.float32)

            # Search
            k = 5
            ids, distances = client.search(index_id, embedding, k)

            # Display results
            print(f"  Top {k} results:")
            for rank, (corpus_id, score) in enumerate(zip(ids, distances), 1):
                text = corpus_sentences[corpus_id]
                print(f"    {rank}. [score={score:.4f}] {text}")

        # Test 3: Check cache statistics
        print("\n[Test 3] Cache statistics...")

        cache_stats = client.info_cache()
        print(f"  Global: {cache_stats}")

        index_stats = client.info_index(index_id)
        print(f"  Index {index_id}: {index_stats}")

        hit_rate = (index_stats['cache_hits'] /
                   (index_stats['cache_hits'] + index_stats['cache_misses']) * 100
                   if (index_stats['cache_hits'] + index_stats['cache_misses']) > 0
                   else 0)

        print(f"  Cache hit rate: {hit_rate:.1f}%")
        print(f"  Cached clusters: {index_stats['cached_clusters']} / {index_stats['cluster_count']}")

    print("\n" + "=" * 80)
    print("All tests completed!")
    print("=" * 80)


if __name__ == "__main__":
    test_search()
```

### Enhanced Client Protocol

To return index metadata from LOAD, update the client:

```python
def load(self, bucket: str, key: str, cluster_data_offset: int) -> Dict[str, int]:
    """Load an index from S3"""
    params = {
        "bucket": bucket,
        "key": key,
        "cluster_data_offset": str(cluster_data_offset)
    }
    self._send_command("LOAD", params)
    response_line = self._recv_line()
    response = self._parse_response(response_line)

    return {
        "index": int(response["index"]),
        "d": int(response.get("d", 0)),
        "ntotal": int(response.get("ntotal", 0)),
        "nlist": int(response.get("nlist", 0)),
    }
```

## Error Handling

### S3 Errors

```cpp
try {
    auto data = DownloadRangeFromS3(...);
} catch (const std::exception& e) {
    send_error("LOAD_FAILED",
               "S3 download failed: " + std::string(e.what()));
    return;
}
```

Common S3 errors:
- **NoSuchBucket**: Bucket doesn't exist
- **NoSuchKey**: Object doesn't exist
- **AccessDenied**: Invalid credentials
- **InvalidRange**: Byte range out of bounds

### Faiss Errors

```cpp
try {
    auto* index = faiss::read_index(&reader, flags);
} catch (const faiss::FaissException& e) {
    send_error("LOAD_FAILED",
               "Failed to parse index: " + std::string(e.what()));
    return;
}
```

Common Faiss errors:
- **IO error**: Corrupt index data
- **Bad index type**: Not an IVF index
- **Version mismatch**: Index written with incompatible Faiss version

### Dimension Mismatch

```cpp
if (ivf_index->d != query_dimension) {
    send_error("INVALID_PARAM",
               "Query dimension " + std::to_string(query_dimension) +
               " doesn't match index dimension " + std::to_string(ivf_index->d));
    return;
}
```

## Build Configuration

### CMakeLists.txt

```cmake
# Add real S3 cache server
add_executable(s3_cache_server s3_cache_server.cpp S3InvertedLists.cpp)
target_link_libraries(s3_cache_server
    faiss
    ${AWSSDK_LINK_LIBRARIES}
    pthread
)
```

### .gitignore

```
# TCP servers
test_tcp_server
s3_cache_server
server.log
s3_cache_server.log
```

## Testing Strategy

### Unit Tests

1. **LOAD Command**:
   - Valid S3 path → Success
   - Invalid bucket → LOAD_FAILED
   - Invalid offset → LOAD_FAILED
   - Corrupt index data → LOAD_FAILED

2. **SEARCH Command**:
   - Valid query → Returns k results
   - Wrong dimension → INVALID_PARAM
   - Invalid index ID → INDEX_NOT_FOUND
   - Empty index → Returns fewer than k results

3. **Cache Behavior**:
   - First search → Cache misses
   - Repeat search → Cache hits
   - Different query, same clusters → Cache hits

### Integration Tests

1. **Load quora index** from S3
2. **Search with sample queries**:
   - "How to find a job" → Should return job-related questions
   - "What to eat for lunch" → Should return food-related questions
3. **Verify cache statistics**:
   - Cache misses on first query
   - Cache hits on subsequent queries in same clusters
4. **Concurrent clients**:
   - Multiple clients searching simultaneously
   - Verify thread safety

### Performance Benchmarks

1. **Cold start** (empty cache):
   - Measure S3 download time
   - Measure search latency

2. **Warm cache**:
   - Measure search latency with cached clusters
   - Should be ~100x faster than cold start

3. **Cache hit rate**:
   - Search 100 queries
   - Measure % of clusters cached
   - With nprobe=2, expect ~200/1024 = 19.5% clusters accessed

## Deployment Considerations

### Environment Variables

```bash
# AWS credentials
export AWS_ACCESS_KEY_ID="..."
export AWS_SECRET_ACCESS_KEY="..."
export AWS_REGION="us-east-1"

# Optional: Custom S3 endpoint (for S3Mock/MinIO)
export S3_ENDPOINT_URL="http://localhost:9000"

# Server configuration
export SERVER_PORT=9001
export SERVER_LOG_LEVEL=INFO
```

### Running the Server

```bash
# Build
cmake -B build .
make -C build s3_cache_server

# Run in background
./build/s3_cache_server > s3_cache_server.log 2>&1 &

# Or with custom port
./build/s3_cache_server 9002 > s3_cache_server.log 2>&1 &
```

### Running Tests

```bash
# Install Python dependencies
pip install sentence-transformers numpy

# Run test client
python test_s3_cache_server.py
```

## Performance Characteristics

### Latency Breakdown

For a typical search with `nprobe=2` on quora index:

| Phase | Cold Cache | Warm Cache |
|-------|-----------|------------|
| Network (query → server) | 0.1 ms | 0.1 ms |
| Query parsing | 0.01 ms | 0.01 ms |
| Quantizer search | 5 ms | 5 ms |
| S3 cluster fetch (2 clusters) | 200 ms | 0 ms |
| Distance computation | 10 ms | 10 ms |
| Top-k selection | 1 ms | 1 ms |
| Network (results → client) | 0.1 ms | 0.1 ms |
| **Total** | **~216 ms** | **~16 ms** |

### Cache Efficiency

- **Cache size**: ~6 MB per cluster (for quora: 97 vectors × 3080 bytes)
- **Memory usage**: With 100 cached clusters = 600 MB
- **Hit rate**: Depends on query distribution
  - Random queries: Low hit rate (~2%)
  - Related queries: High hit rate (50-90%)

### Optimization Opportunities

1. **Prefetching**: Predict likely clusters based on query history
2. **LRU Eviction**: Currently cache-forever, could add size limit
3. **Batch Searches**: Process multiple queries in one request
4. **Compression**: Compress cached cluster data
5. **Local Caching**: Client-side result caching

## Future Enhancements

### Protocol Extensions

1. **BATCH_SEARCH**: Search multiple queries at once
2. **SET_NPROBE**: Configure search quality vs speed
3. **PRELOAD**: Warm cache with specific clusters
4. **CACHE_CLEAR**: Reset cache for testing

### Advanced Features

1. **Query Routing**: Route to different index shards
2. **Load Balancing**: Multiple server instances
3. **Monitoring**: Prometheus metrics
4. **Streaming Results**: Send results as they're computed

### Alternative Backends

1. **Local Files**: Use mmap instead of S3
2. **Databases**: Store clusters in PostgreSQL/MongoDB
3. **Redis**: Use as distributed cache layer

## Conclusion

The real S3 cache server integrates:
- ✅ **Proven components**: S3OnDemandInvertedLists, Faiss IndexIVFFlat
- ✅ **Efficient protocol**: Text + binary for optimal performance
- ✅ **Lazy loading**: Only fetches needed clusters from S3
- ✅ **Thread-safe**: Multiple concurrent clients
- ✅ **Observable**: Cache statistics and logging

The implementation follows the patterns from `demo_v2.cpp` but adds:
- Multi-client TCP server
- Persistent index registry
- Proper error handling
- Client-side text mapping

This enables Python (and other languages) to perform efficient semantic search on S3-backed Faiss indexes without linking C++ libraries.
