#!/bin/sh

mkdir aws-sdk-cpp-home
AWS_SDK_INSTALL_DIR="$(realpath aws-sdk-cpp-home)"

git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/aws/aws-sdk-cpp
cd aws-sdk-cpp
mkdir build
cd build

# Build only the S3 CRT client to save time
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DBUILD_ONLY="s3-crt" \
         -DCMAKE_INSTALL_PREFIX="$AWS_SDK_INSTALL_DIR" \
         -DENABLE_TESTING=OFF

make -j$(nproc)
make install