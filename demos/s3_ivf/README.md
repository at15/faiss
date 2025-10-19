# Storing Faiss inverted lists in S3

Demo of using S3 for a readonly IVF index. The index is built in memory and flushed to S3.
When using the index, centroids are loaded into memory and the inverted lists are loaded from S3 on demand.

## TODO

- [x] CMake to make sure things are working
- [ ] Figure out the format we want to use, ideally use same format as array inverted lists so user can read it directly when fully downloaded

## Build

```bash
cmake -B build \
  -DOpenMP_C_FLAGS="-Xclang -fopenmp -I/opt/homebrew/opt/libomp/include" \
  -DOpenMP_C_LIB_NAMES="omp" \
  -DOpenMP_omp_LIBRARY=/opt/homebrew/opt/libomp/lib/libomp.dylib \
  -DOpenMP_CXX_FLAGS="-Xclang -fopenmp -I/opt/homebrew/opt/libomp/include" \
  -DOpenMP_CXX_LIB_NAMES="omp" \
  .

cd build
make -j$(nproc)
./demo_s3_ivf
```