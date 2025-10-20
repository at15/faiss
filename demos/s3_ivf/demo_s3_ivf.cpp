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

#include "svf.h" // Read from simple vector format to use generated embeddings and text
#include "S3InvertedLists.h"

void build_index(const char* index_file_name) {
    std::cout << "Building index in memory and flushing to s3..." << std::endl;

    const size_t VEC_DIM = 128;     // Vector dimension
    const size_t N_CLUSTERS = 100; // Number of IVF clusters
    const size_t N_VECTORS = 100'000;  // Number of vectors to index

    // Create index, default invert list implementation is memory
    faiss::IndexFlatL2 quantizer(VEC_DIM);
    faiss::IndexIVFFlat index(&quantizer, VEC_DIM, N_CLUSTERS);
    index.verbose = true;

    // Createa a new ArrayInvertedLists so that we can get number of vectors in each cluster
    // and calculate the offset for each cluster.
    faiss::ArrayInvertedLists array_inverted_lists(N_CLUSTERS, index.code_size);
    // TODO: What does the false here mean?
    index.replace_invlists(&array_inverted_lists, false);

    // Generate random vectors
    std::vector<float> xb(VEC_DIM * N_VECTORS);
    faiss::float_rand(xb.data(), VEC_DIM * N_VECTORS, 12345);

    // Train
    // TODO: We can use less vectors for training
    index.train(N_VECTORS, xb.data());

    // Add vectors
    index.add(N_VECTORS, xb.data());

    faiss::write_index(&index, index_file_name);
}

int main(int argc, char* argv[]) {
    std::cout << "Hello, S3 IVF!" << std::endl;

    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <index_file>" << std::endl;
        return 1;
    }

    const char* index_file_name = argv[1];
    build_index(index_file_name);

    return 0;
}