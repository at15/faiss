# Storing Faiss inverted lists in S3

Demo of using S3 for a readonly IVF index. The index is built in memory and flushed to S3.
When using the index, centroids are loaded into memory and the inverted lists are loaded from S3 on demand.

Currently the implementation is a cache server with a custom TCP protocol.
The in memory cache has no LRU and big lock is used everywhere.

Future development is using https://github.com/at15/faiss-s3 because we don't plan to create a new python binding and can just provides a binary server that statically links to official faiss library.

## TODO

- [x] CMake to make sure things are working
- [x] Figure out the format we want to use, ideally use same format as array inverted lists so user can read it directly when fully downloaded
- [ ] Clean up the code, verify it works manually and using actual data, that's what the `svf.h` is fow. There is likly thread, lock granularity, memory management issues as well.
  - [ ] the claude generated code is having a big lock on cache right now, which makes both read and write slow and serial ..., GPT and claude suggest using `shared_future`. TIL: C++ has future... (well that's a pun)
- [ ] Update the python binding
- [ ] Figure out how to build the C++ library and python wheel (in container, so we I can run becnhmark in other places)

## Build

```bash
./build-s3-sdk.sh
./config.sh
make build
```

For S3

```bash
export AWS_ACCESS_KEY_ID=test
export AWS_SECRET_ACCESS_KEY=test
export AWS_REGION=us-east-1
export AWS_EC2_METADATA_DISABLED=true
export S3_ENDPOINT_URL=http://localhost:9000
export AWS_ENDPOINT_URL_S3=http://localhost:9000

# Write to local s3mock
./build/test_s3 test-bucket test.txt put "Hello, S3 test"
# Read from local s3mock
./build/test_s3 test-bucket test.txt get
```