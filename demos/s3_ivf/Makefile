.PHONY: build test format

build:
	cmake --build build

test: build
	./build/demo_s3_ivf a.ivf

format:
	clang-format -i demo_s3_ivf.cpp