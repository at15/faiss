.PHONY: build test format

config:
	./config.sh

build:
	cmake --build build

test: build
	./build/demo_s3_ivf a.ivf

format:
	clang-format -i demo_s3_ivf.cpp test_s3.cpp
