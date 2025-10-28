# Copied from rocksdb_ivf

cmake_minimum_required(VERSION 3.17 FATAL_ERROR)
project (S3_IVF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Debug)

# Deps
find_package(OpenMP REQUIRED)
find_package(faiss REQUIRED)
find_package(AWSSDK REQUIRED COMPONENTS s3-crt)

add_executable(demo_v2 demo_v2.cpp S3InvertedLists.cpp)
add_executable(demo_s3_ivf demo_s3_ivf.cpp S3InvertedLists.cpp)
add_executable(test_s3 test_s3.cpp)

target_link_libraries(demo_v2 faiss ${AWSSDK_LINK_LIBRARIES})
target_link_libraries(demo_s3_ivf faiss ${AWSSDK_LINK_LIBRARIES})
target_link_libraries(test_s3 ${AWSSDK_LINK_LIBRARIES})
