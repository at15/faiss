# Copied from rocksdb_ivf

cmake_minimum_required(VERSION 3.17 FATAL_ERROR)
project (S3_IVF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Debug)
find_package(OpenMP REQUIRED)
find_package(faiss REQUIRED)
# We don't need rocksdb
# find_package(RocksDB REQUIRED)
# TODO: Add AWS C++ SDK

add_executable(demo_s3_ivf demo_s3_ivf.cpp S3InvertedLists.cpp)
target_link_libraries(demo_s3_ivf faiss)
