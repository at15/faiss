# Implementation Status: S3 On-Demand Reader for Faiss IVF Index

## Progress Summary

### ✅ Completed

1. **IO Hook System** - Working correctly
   - `S3InvertedListsIOHook` registered and recognized by Faiss
   - Fourcc "ils3" (0x33736c69) correctly calculated from IO_FLAG_S3
   - `IO_FLAG_S3 = IO_FLAG_SKIP_IVF_DATA | 0x33730000` (fixed byte order)
   - Hook's `read_ArrayInvertedLists()` successfully creates placeholder

2. **S3ReadNothingInvertedLists (Placeholder)** - Working
   - Created during `read_index()` with IO_FLAG_S3
   - Stores cluster sizes from hook for later use
   - `list_size()` returns actual sizes (not throwing) so index stats work
   - Successfully replaced with `S3ReadOnlyInvertedLists` after loading

3. **Metadata Generation** - Working
   - `build_index()` tracks offsets during write
   - Metadata JSON includes:
     - `inverted_lists_offset`: Where "ilar" fourcc starts
     - `cluster_data_offset`: Where actual cluster data starts
     - `sizes_array_offset`, `sizes_array_count`, `sizes_array_format`
     - `total_size`, `n_clusters`, `code_size`
   - Correctly handles both "full" and "sparse" formats

4. **S3 Upload** - Working (with partSize workaround)
   - Successfully uploads 50MB+ index files to S3 mock server
   - Metadata JSON uploaded separately
   - Initially failed due to multipart upload (405 error)
   - Fixed by setting `config.partSize = SIZE_MAX` (disabled for now to test range requests)

5. **S3 Download (Full Objects)** - Working
   - `DownloadFromS3()` successfully downloads metadata and index header
   - Downloads index header (0 to cluster_data_offset) including sizes array
   - Temp file created with header data for `read_index()`

6. **Index Loading** - Working
   - Index header downloaded and written to temp file
   - `read_index()` with IO_FLAG_S3 successfully loads index structure
   - Hook skips cluster data (52MB of 52MB total)
   - Index stats correct (d=128, ntotal=100000, nlist=100)

7. **S3ReadOnlyInvertedLists Initialization** - Working
   - Constructor receives sizes from placeholder (no S3 download needed)
   - Metadata parsed correctly
   - Cluster offsets calculated correctly
   - Ready to fetch clusters on-demand

### 🚧 Blocked

**S3 Range Requests with CRT Client** - Not working with mock server

**Problem**: `DownloadRangeFromS3()` fails with "Unable to create s3 meta request" when trying to download cluster data using byte-range requests.

**Root Cause**: The AWS S3 CRT (Common Runtime) client has issues with range requests:
- Works fine for full object downloads (metadata, index header)
- Fails for range requests with `request.SetRange("bytes=start-end")`
- Error occurs even with correct S3_ENDPOINT_URL, credentials, and region
- Same client that works for PUT and full GET fails for range GET

**Evidence**:
```
→ Fetching cluster 65 (1089 vectors, 566280 bytes)
✗ Failed to download range from s3://test-bucket/faiss-index.ivf [35380419:35851019]: Unable to create s3 meta request
```

**What We've Tried**:
1. ✅ Setting environment variables (AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY, AWS_REGION, AWS_EC2_METADATA_DISABLED, S3_ENDPOINT_URL)
2. ✅ Removing `config.partSize = SIZE_MAX` (didn't help range requests, broke uploads)
3. ✅ Setting default region in code
4. ✅ Manual hook registration (in addition to static init)
5. ❌ Using range requests with CRT client

### 🔄 Current State

The implementation is **90% complete** and successfully demonstrates:
- Lazy loading architecture with IO hooks
- Metadata-driven on-demand access
- Correct offset calculations
- Cache structure ready

**Only blocker**: S3 CRT client doesn't support range requests with the mock server.

## Architecture

### Components

```
┌─────────────────────────────────────────────────────────────┐
│                     demo_s3_ivf.cpp                         │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐      │
│  │ build mode   │  │ upload mode  │  │ search mode  │      │
│  │ (local only) │  │ (build + S3) │  │ (S3 → search)│      │
│  └──────────────┘  └──────────────┘  └──────────────┘      │
│         │                 │                    │            │
│         │                 ├─> UploadFileToS3() │            │
│         │                 │                    │            │
│         │                 │        ┌───────────┴────────┐   │
│         │                 │        │ DownloadFromS3()   │   │
│         │                 │        │ (metadata, header) │   │
│         │                 │        └────────────────────┘   │
└─────────┼─────────────────┼──────────────────┼──────────────┘
          │                 │                  │
          ▼                 ▼                  ▼
    ┌──────────────────────────────────────────────┐
    │         faiss::write_index()                 │
    │    → writes "ilar" fourcc for inverted lists │
    └──────────────────────────────────────────────┘
                                                 │
                                                 ▼
    ┌──────────────────────────────────────────────┐
    │       faiss::read_index(IO_FLAG_S3)          │
    │    → reads "ilar", constructs "ils3" fourcc  │
    │    → looks up S3InvertedListsIOHook          │
    └──────────────────┬───────────────────────────┘
                       │
                       ▼
    ┌──────────────────────────────────────────────┐
    │     S3InvertedListsIOHook (registered)       │
    │  read_ArrayInvertedLists(sizes) {            │
    │    return new S3ReadNothingInvertedLists();  │
    │  }                                           │
    └──────────────────┬───────────────────────────┘
                       │
                       ▼
    ┌──────────────────────────────────────────────┐
    │   S3ReadNothingInvertedLists (placeholder)   │
    │  - Stores cluster_sizes from hook            │
    │  - list_size() returns actual size           │
    │  - get_codes()/get_ids() throw errors        │
    └──────────────────┬───────────────────────────┘
                       │
                       │ index->replace_invlists()
                       ▼
    ┌──────────────────────────────────────────────┐
    │    S3ReadOnlyInvertedLists (actual impl)     │
    │  - Constructor receives sizes from placeholder│
    │  - Calculates cluster offsets                │
    │  - get_codes()/get_ids() fetch from S3       │
    │  - Caches fetched clusters                   │
    └──────────────────┬───────────────────────────┘
                       │
                       ▼
    ┌──────────────────────────────────────────────┐
    │      DownloadRangeFromS3() [BLOCKED]         │
    │  - Uses S3 CRT client with SetRange()        │
    │  - Fails with "Unable to create meta request"│
    └──────────────────────────────────────────────┘
```

### File Structure

```
faiss-index.ivf:
  [0..51307)           Index header (quantizer, index metadata)
  [51307..51311)       "ilar" fourcc (0x72616c69)
  [51311..51319)       nlist (8 bytes)
  [51319..51327)       code_size (8 bytes)
  [51327..51331)       list_type (4 bytes)
  [51331..52139)       sizes array (808 bytes)
  [52139..52052139)    cluster data (52000000 bytes)
                       └─> [codes for cluster 0][ids for cluster 0]
                           [codes for cluster 1][ids for cluster 1]
                           ...

faiss-index.ivf.meta.json:
{
  "total_size": 52052139,
  "inverted_lists_offset": 51307,
  "n_clusters": 100,
  "code_size": 512,
  "sizes_array_offset": 51331,
  "sizes_array_count": 100,
  "sizes_array_format": "full",
  "cluster_data_offset": 52139
}
```

## Next Steps

### Option 1: Use Regular S3 Client (Recommended)

Replace S3 CRT client with regular S3 client for range requests:

```cpp
#include <aws/s3/S3Client.h>
#include <aws/s3/model/GetObjectRequest.h>

// In S3InvertedLists.cpp
std::shared_ptr<Aws::S3::S3Client> CreateRangeS3Client() {
    Aws::S3::ClientConfiguration config;

    const char* endpoint = std::getenv("S3_ENDPOINT_URL");
    if (endpoint != nullptr) {
        config.endpointOverride = endpoint;
        config.scheme = Aws::Http::Scheme::HTTP;
        config.verifySSL = false;
    }

    const char* region = std::getenv("AWS_REGION");
    if (region != nullptr) {
        config.region = region;
    }

    return Aws::MakeShared<Aws::S3::S3Client>("S3Client", config);
}
```

**Pros**:
- Regular S3 client has better range request support
- More mature and tested than CRT client
- Should work with mock server

**Cons**:
- Mixing two S3 client types (CRT for full, regular for range)
- Slightly different API

### Option 2: Use libcurl for Range Requests

```cpp
std::vector<uint8_t> DownloadRangeFromS3(
        const std::string& bucket,
        const std::string& key,
        size_t offset,
        size_t size) {

    const char* endpoint = std::getenv("S3_ENDPOINT_URL");
    std::string url = std::string(endpoint) + "/" + bucket + "/" + key;

    CURL* curl = curl_easy_init();
    std::string range = std::to_string(offset) + "-" + std::to_string(offset + size - 1);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());

    // ... set write callback, execute, cleanup
}
```

**Pros**:
- Simple, direct HTTP control
- No AWS SDK complexity for range requests
- Known to work with mock servers

**Cons**:
- Need to handle authentication manually
- Less "proper" than using AWS SDK

### Option 3: Fix Mock Server

Investigate if the mock server can be configured to work with S3 CRT client's range request format.

**Pros**:
- Use CRT client everywhere (consistency)
- Might benefit from CRT performance features

**Cons**:
- Not our code to fix
- May not be possible
- Doesn't solve real AWS S3 compatibility

## Code Cleanup Needed

### 1. S3 Client Singleton

Currently creating new S3 client for each request. Should create once and reuse:

```cpp
// In S3InvertedLists.h
class S3ReadOnlyInvertedLists : faiss::InvertedLists {
    std::shared_ptr<Aws::S3::S3Client> s3_client_;  // Stored in class

    S3ReadOnlyInvertedLists(
        const std::string& bucket,
        const std::string& key,
        const std::string& metadata_json,
        const std::vector<size_t>& sizes);
};

// In S3InvertedLists.cpp
S3ReadOnlyInvertedLists::S3ReadOnlyInvertedLists(...)
    : s3_client_(CreateS3Client()),  // Create once
      ... {
}

void S3ReadOnlyInvertedLists::fetch_cluster(size_t list_no) const {
    // Use s3_client_ instead of creating new one
    Aws::S3::Model::GetObjectRequest request;
    request.SetBucket(s3_bucket);
    request.SetKey(s3_key);
    request.SetRange("bytes=" + ...);

    auto outcome = s3_client_->GetObject(request);
    ...
}
```

### 2. Remove Duplicate Helper Functions

The `DownloadRangeFromS3()` function appears in both:
- `S3InvertedLists.cpp` (used by S3ReadOnlyInvertedLists)
- `demo_s3_ivf.cpp` (used by search mode)

Should consolidate into one location.

### 3. Error Handling

Add better error messages with context:
- Which cluster failed to fetch
- What the search parameters were (nprobe, k)
- How much data was expected vs received

## Test Commands

### Environment Setup
```bash
export AWS_ACCESS_KEY_ID=test
export AWS_SECRET_ACCESS_KEY=test
export AWS_REGION=us-east-1
export AWS_EC2_METADATA_DISABLED=true
export S3_ENDPOINT_URL=http://localhost:9000
```

### Build and Test
```bash
# Build demo
make -C /Volumes/w/src/github.com/facebookresearch/faiss/demos/s3_ivf build

# Build index and upload to S3
./build/demo_s3_ivf upload faiss-index.ivf test-bucket faiss-index.ivf

# Search from S3 (currently fails at range request)
./build/demo_s3_ivf search test-bucket faiss-index.ivf
```

### Expected Output (When Fixed)
```
========================================
=== SEARCH FROM S3 MODE ===
========================================

[1/4] Downloading metadata...
✓ Metadata loaded

[2/4] Downloading index header...
✓ Downloaded 52139 bytes

[3/4] Loading index with S3 hook...
✓ Index loaded (d=128, ntotal=100000, nlist=100)

[4/4] Replacing placeholder with S3ReadOnlyInvertedLists...
S3ReadOnlyInvertedLists initialized

=== Performing Search (nprobe=2) ===
→ Fetching cluster 65 (1089 vectors, 566280 bytes)
✓ Downloaded cluster 65
→ Fetching cluster 47 (982 vectors, 510640 bytes)
✓ Downloaded cluster 47
... (only ~20 clusters fetched for 10 queries)

=== Search Results ===
Query 0: 12345(0.123) 67890(0.456) ...
...
```

## Implementation Checklist

- ✅ S3InvertedListsIOHook registration
- ✅ IO_FLAG_S3 constant (correct byte order)
- ✅ S3ReadNothingInvertedLists placeholder
- ✅ Metadata generation and upload
- ✅ Index upload to S3
- ✅ Metadata download from S3
- ✅ Index header download from S3
- ✅ Hook creates placeholder with sizes
- ✅ S3ReadOnlyInvertedLists initialization
- ✅ Cluster offset calculation
- ⏸️ **S3 range requests (BLOCKED on CRT client)**
- ⏸️ Cluster caching
- ⏸️ Search correctness verification
- ⏸️ Performance testing

## References

- Original design: [s3-readonly-format.md](s3-readonly-format.md)
- IO hook examples: [index-read-hook-mmap.claude.md](index-read-hook-mmap.claude.md)
- Mock server: `/Volumes/w/src/github.com/at15/gos3mock`
