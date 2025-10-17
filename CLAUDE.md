# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

Faiss is a library for efficient similarity search and clustering of dense vectors developed by Meta's Fundamental AI Research group. It's written in C++ with Python bindings and optional GPU support (CUDA/ROCm/cuVS).

## Build System

Faiss uses **CMake** as its build system.

### Basic Build Commands

```bash
# Configure build (creates build/ directory)
cmake -B build .

# Build C++ library
make -C build -j faiss

# Build Python bindings
make -C build -j swigfaiss
cd build/faiss/python && python setup.py install

# Install C++ library and headers system-wide
make -C build install
```

### Important CMake Options

- `-DFAISS_ENABLE_GPU=OFF` - Disable GPU support (default: ON)
- `-DFAISS_ENABLE_PYTHON=OFF` - Disable Python bindings (default: ON)
- `-DFAISS_ENABLE_CUVS=ON` - Enable NVIDIA cuVS implementations (default: OFF)
- `-DFAISS_ENABLE_ROCM=ON` - Enable AMD ROCm support (default: OFF)
- `-DFAISS_ENABLE_C_API=ON` - Build C API (default: OFF)
- `-DBUILD_TESTING=OFF` - Disable building tests (default: ON)
- `-DBUILD_SHARED_LIBS=ON` - Build shared library instead of static
- `-DCMAKE_BUILD_TYPE=Release` - Enable optimization flags
- `-DFAISS_OPT_LEVEL=avx2|avx512|avx512_spr|sve` - SIMD optimization level
- `-DFAISS_USE_LTO=ON` - Enable Link-Time Optimization (default: OFF)
- `-DBLA_VENDOR=Intel10_64_dyn` - Use Intel MKL for BLAS (recommended for performance)

### Architecture-Specific Builds

For optimized SIMD builds, build the correct target first:

```bash
# AVX2
make -C build -j faiss_avx2

# AVX512
make -C build -j faiss_avx512

# AVX512 SPR (Sapphire Rapids)
make -C build -j faiss_avx512_spr
```

## Testing

### C++ Tests

```bash
# Run all C++ tests (requires -DBUILD_TESTING=ON)
make -C build test

# Build and run a specific demo
make -C build demo_ivfpq_indexing
./build/demos/demo_ivfpq_indexing
```

### Python Tests

```bash
# Build Python package
cd build/faiss/python && python setup.py build

# Run Python tests
PYTHONPATH="$(ls -d ./build/faiss/python/build/lib*/)" pytest tests/test_*.py
```

### GPU Tests

GPU tests are located in `faiss/gpu/test/` and are built automatically when `FAISS_ENABLE_GPU=ON`.

## Code Architecture

### Core Components

- **`faiss/Index.h`** - Base class for all indexes. Key methods:
  - `train()` - Train the index on representative vectors
  - `add()` - Add vectors to the index
  - `search()` - Query k-nearest neighbors
  - `range_search()` - Find all vectors within a radius
  - `reconstruct()` - Reconstruct stored vectors

- **Index Types** - Faiss provides multiple index implementations:
  - `IndexFlat*` - Exact search (brute force)
  - `IndexIVF*` - Inverted file indexes with clustering
  - `IndexHNSW` - Hierarchical Navigable Small World graphs
  - `IndexPQ*` - Product quantization
  - `IndexLSH` - Locality-sensitive hashing
  - `IndexNSG` / `IndexNNDescent` - Graph-based methods
  - `*FastScan` variants - Optimized SIMD implementations

### Directory Structure

- **`faiss/`** - Main C++ library source
  - `impl/` - Core implementations (quantizers, distance computers, etc.)
  - `gpu/` - GPU implementations (CUDA/ROCm)
  - `utils/` - Utility functions and data structures
  - `invlists/` - Inverted list implementations
  - `cppcontrib/` - Contributed optimizations (SIMD kernels, etc.)
  - `python/` - Python bindings (SWIG)

- **`tests/`** - C++ and Python test suites
- **`demos/`** - Example programs and usage demonstrations
- **`benchs/`** - Benchmarking scripts
- **`tutorial/`** - Tutorial code

### GPU Architecture

GPU indexes inherit from `GpuIndex` (defined in `faiss/gpu/GpuIndex.h`):
- Handles CPU/GPU memory transfers automatically
- Supports paging for large datasets
- Can use NVIDIA cuVS backend when enabled
- GPU indexes typically mirror CPU implementations (e.g., `IndexIVFFlat` → `GpuIndexIVFFlat`)

### Index Factory

The `index_factory()` function in `index_factory.cpp` creates indexes from string descriptors:
```cpp
// Example: "IVF4096,PQ64" creates IVF index with 4096 clusters and PQ compression
Index* index = index_factory(d, "IVF4096,PQ64", METRIC_L2);
```

### Metric Types

Defined in `MetricType.h`:
- `METRIC_L2` - Euclidean distance
- `METRIC_INNER_PRODUCT` - Dot product (for cosine with normalized vectors)
- Binary metrics for `IndexBinary*` classes

## Coding Style

- **Indentation**: 4 spaces (no tabs)
- **Line length**: 80 characters
- **Language level**: C++17
- **OpenMP**: Version 2+ required for parallelization

## Key Dependencies

- **BLAS implementation** (required): Intel MKL recommended for best performance
- **nvcc + CUDA Toolkit** (optional): For GPU support
- **AMD ROCm** (optional): For AMD GPU support
- **libcuvs** (optional): For NVIDIA cuVS backend
- **Python 3 + NumPy + SWIG** (optional): For Python bindings

## Common Workflows

### Adding a New Index Type

1. Create header in `faiss/` (e.g., `IndexNewType.h`)
2. Implement in corresponding `.cpp` file
3. Inherit from `Index` and implement required virtual methods
4. Add factory support in `index_factory.cpp` if needed
5. Add tests in `tests/`
6. Consider GPU implementation in `faiss/gpu/` if applicable

### Working with SIMD Optimizations

SIMD-optimized kernels are in `faiss/cppcontrib/` and architecture-specific implementations use `-inl.h` suffixes (e.g., `PQ-avx2-inl.h`, `PQ-neon-inl.h`).

### Running Benchmarks

The `benchs/` directory contains various benchmarking scripts. Most require the SIFT1M dataset from http://corpus-texmex.irisa.fr/ to be extracted to `sift1M/` directory.

```bash
# Example auto-tuning benchmark
mkdir tmp
python demos/demo_auto_tune.py
```

## Important Notes

- The codebase uses `idx_t` type (typically `int64_t`) for vector indices
- Vectors are always stored in row-major format: `x[i * d + j]` for component j of vector i
- Many operations are optimized for batch processing
- GPU implementations handle memory transfers automatically but performance is better when data stays GPU-resident
- The library is thread-safe for search operations but not for concurrent modifications
