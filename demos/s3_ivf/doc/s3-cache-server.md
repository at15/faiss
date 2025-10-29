# S3 Cache Server

## Background

Now `demo_v2.cpp` shows we can read index from S3 on demand for IVF.
We need to allow using it from python. There are a few ways:

- Generate a python binding
- Provide a server that run outside of python process and can be used by other langauges as well

I prefer the client server approach because

- Eaiser to build and release
- Makes monitoring usage easier, different process

We are providing a search API, not directly memory access, so we don't really need to work as a python extension and share memory.

For the protocol, I decided to use TCP directly because:

- Redis use TCP
- HTTP(s) requires a lot more libraries
- gRPC is way even more dependencies

The protocl expose the following APIs:

- Load and index with given S3 bucket, key and metadata (cluster data offset is the only thing we need right now)
- Search a loaded index with query vector and top_k
- Info about the entire cache or specific index

The human readable format looks like this (kind of like redis):

```text
# Echo
ECHO msg=hello
# Respone
msg=hello

# Load index
LOAD bucket=test-bucket key=quora/index.idx cluster_data_offset=3154059
# Returns and id for the loaded index to use for later queries
index=1

# Search index
SEARCH index=1 k=5 query=[0.1, 0.2, 0.3]
# Returns the results as two lists of ids and distances
ids=[1, 2, 3, 4, 5]
distances=[0.1, 0.2, 0.3, 0.4, 0.5]

# Info about the entire cache
INFO about=cache
# Returns the total number of cached index files
index_count=10
cache_hits=100 # bytes

# Info about a specific index
INFO about=index id=1
# Returns the total number of cached clusters for this index
cluster_count=100
cache_hits=100 # bytes
cache_misses=10
```

Basically the syntax is using `k=v` and space for a list of arguments.
A list of arguments is essentially a dictionary.
The supported types are

- string
- int
- list of floats
  - assume float32 for now, we should support quantized later

```text
Request
<Command> <Arguments>
Response
<Arguments>
```

The schema for each command is fixed so when we do `k=v` we do NOT need to specify the type of v, because there is only one accepted type for each key.

## Design

We can start with

- A simple server in C++ withou dummy logic and a simple client in Python
- Impelemt the actual server base on the dummy server and actual S3 faiss logic demoed in `demo_v2.cpp`

## Instructions for Claude on Simple Server

- Write the simple server in C++ in `test_tcp_server.cpp`
 - server should start a new thread for each client and handle the client discconnection as well
- Write the simple client in Python in `test_tcp_client.py`
- You can define the serialization format as you see fit e.g. how to split different messages

## Instructions for Claude on Actual Server

We have following examples

- @gen_ivf.py#265-288 the `query_index` function is a good example of how to query the faiss index from disk
- `demo_v2.cpp` shows how to read the index from S3 on demand
- `test_tcp_server.cpp` and `test_tcp_client.py` show the simple server and client

Now we need to glue them together to implement the real server and client in `s3_cache_server.cpp` and `test_s3_cache_server.py`

- In cache server, it should load the index's metadata from S3 when user send `LOAD` command
- When user do `SEARCH` it should find the loaded index and run faiss search (similar to `demo_v2.cpp` is doing in `read_s3_file` )
- `test_s3_cache_server.py` should use same existing qurora dataset and print the text of hitted result so human can inspect to see if it is working as expected

## Implementation Status

✅ **Completed**

### Files Created

1. **Protocol Design**:
   - `s3-cache-server.protocol.claude.md` - Complete wire protocol specification
   - `s3-cache-server.impl.claude.md` - Implementation design document

2. **Server Implementation**:
   - `test_tcp_server.cpp` - Dummy server with hardcoded responses (for protocol testing)
   - `s3_cache_server.cpp` - **Real server with Faiss + S3 integration**

3. **Client Implementation**:
   - `test_tcp_client.py` - Generic TCP client for the protocol
   - `test_s3_cache_server.py` - Test client with Quora dataset integration

### Key Features Implemented

**Server (`s3_cache_server.cpp`)**:
- ✅ Multi-threaded TCP server (one thread per client)
- ✅ AWS S3 SDK integration with S3Mock support
- ✅ Faiss IndexIVFFlat integration
- ✅ S3OnDemandInvertedLists for lazy cluster fetching
- ✅ **Index deduplication**: Same S3 location returns same index ID
- ✅ Real cache statistics from S3OnDemandInvertedLists
- ✅ Error handling for S3 failures, dimension mismatches, etc.
- ✅ Graceful shutdown on SIGINT/SIGTERM

**Client (`test_s3_cache_server.py`)**:
- ✅ Loads Quora embeddings (100K questions, 768-dim vectors)
- ✅ Uses SentenceTransformer for query encoding
- ✅ Maps vector IDs → question text
- ✅ Displays human-readable semantic search results
- ✅ Shows cache statistics

### Test Results

**Semantic Search Quality** - Excellent! Example queries:
```
Query: "How to find a job"
  1. [score=0.9928] How can I find a job?
  2. [score=0.9617] What's the best and quickest way to find a job?

Query: "What is the best way to lose weight"
  1. [score=0.9985] What are the best ways to lose weight?
  2. [score=0.9981] Which are the best ways to lose weight?

Query: "How do I learn programming"
  1. [score=0.9933] What are some of the best ways to learn programming?
  2. [score=0.9933] What is the the best way to learn programming?
```

**Performance**:
- Index: 100,000 vectors, 768 dimensions, 1024 clusters
- Cache efficiency: 50% hit rate after 8 queries
- Only fetches needed clusters on-demand from S3

**Index Deduplication**:
- Multiple clients loading same S3 location get same index ID
- Prevents duplicate downloads and memory waste
- Tested: `LOAD` twice returns ID=1 both times, index_count=1

### Running the Server

```bash
# Build
./config.sh
make -C build s3_cache_server

# Set S3 environment (for S3Mock or real S3)
export AWS_ACCESS_KEY_ID=test
export AWS_SECRET_ACCESS_KEY=test
export AWS_REGION=us-east-1
export S3_ENDPOINT_URL=http://localhost:9000  # For S3Mock

# Run server (default port 9001)
./build/s3_cache_server > s3_cache_server.log 2>&1 &

# Run test client
python3 test_s3_cache_server.py
```

### Protocol Wire Format

See `s3-cache-server.protocol.claude.md` for complete specification.

**Key points**:
- Text commands: `COMMAND key1=value1 key2=value2\n`
- Binary data: 4-byte length prefix + raw bytes
- Query vectors sent as binary float32 arrays
- Results returned as binary int64 (IDs) + float32 (distances) arrays

### Architecture

```
Python Client → TCP → S3 Cache Server (C++)
                         ↓
                    Faiss IndexIVFFlat
                         ↓
                  S3OnDemandInvertedLists
                         ↓
                      S3 (lazy cluster fetch)
```

### Known Limitations & Future Work

1. **No LRU eviction**: Currently caches clusters forever (until server restart)
2. **No authentication**: TCP connection is unencrypted and unauthenticated
3. **No batch search**: One query per SEARCH command
4. **No nprobe configuration**: Uses default Faiss nprobe value
5. **Single server**: No load balancing or replication