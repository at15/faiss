.PHONY: build test format

config:
	./config.sh

build:
	cmake --build build

test: build
#./build/demo_s3_ivf a.ivf
	./build/demo_v2

format:
	clang-format -i demo_v2.cpp demo_s3_ivf.cpp test_s3.cpp S3InvertedLists.cpp S3InvertedLists.h
