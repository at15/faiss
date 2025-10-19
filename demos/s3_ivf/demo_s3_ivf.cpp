#include <exception>
#include <iostream>
#include <memory>
#include <string>

#include <faiss/IndexFlat.h>
#include <faiss/IndexIVFFlat.h>
#include <faiss/impl/AuxIndexStructures.h>
#include <faiss/impl/FaissException.h>
#include <faiss/index_io.h>
#include <faiss/utils/random.h>

// ============================================================================
// Constants
// ============================================================================
const size_t D = 128;        // Vector dimension
const size_t NLIST = 100;    // Number of IVF clusters
const int NB = 10000;      // Number of vectors to index
const int NQ = 20;         // Number of query vectors
const int K = 5;           // Top-K for k-NN search
const int NPROBE = 2;        // Number of clusters to probe during search

// Build the index in memory and flush it to s3.
// TODO: For simplicity, we write the index to local file and upload to s3 using cli.
void build_index() {
    std::cout << "Building index in memory and flushing to s3..." << std::endl;

    // Create index, default invert list implementation is memory
    faiss::IndexFlatL2 quantizer(D);
    faiss::IndexIVFFlat index(&quantizer, D, NLIST);

}

int main(int argc, char* argv[]) {
    // TODO: actual implementation
    std::cout << "Hello, S3 IVF!" << std::endl;
    return 0;
}