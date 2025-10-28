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