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

#include "S3InvertedLists.h"

// ============================================================================
// Constants
// ============================================================================
// TODO: Use more meaningful constant names such as VEC_DIM, N_CLUSTERS, N_VECTORS
const size_t D = 128;        // Vector dimension
const size_t NLIST = 100;    // Number of IVF clusters
const size_t NB = 10000;      // Number of vectors to index

const size_t NQ = 20;         // Number of query vectors
const size_t K = 5;           // Top-K for k-NN search
const size_t NPROBE = 2;        // Number of clusters to probe during search

// Build the index in memory and flush it to s3.
void build_index(const char* index_file_name) {
    std::cout << "Building index in memory and flushing to s3..." << std::endl;

    // Create index, default invert list implementation is memory
    faiss::IndexFlatL2 quantizer(D);
    faiss::IndexIVFFlat index(&quantizer, D, NLIST);

    // Create S3 inverted lists TODO: not really s3 right now ...
    // TODO: We can also replace it with ArrayInvertedLists
    // We just need to get size of each cluster and we can load the vectors properly
    faiss_s3::S3BuildOnlyInvertedLists s3_inverted_lists(NLIST, index.code_size);
    // TODO: What does the false here mean?
    index.replace_invlists(&s3_inverted_lists, false);

    // Generate random vectors
    std::vector<float> xb(D * NB);
    faiss::float_rand(xb.data(), D * NB, 12345);

    // Train
    // TODO: We can use less vectors for training
    index.verbose = true;
    index.train(NB, xb.data());

    // Add vectors
    index.add(NB, xb.data());

    // TODO: Do we want the local file format to be same as the default ivf file format?
    // all we need for reading from S3 on demand is the offset for each cluster.

    // TODO: We need to replace the list with empty ArrayInvertedLists
    // otherwise write_index try to lookup our classname and fail
    // NOTE: We cannot use write_index because we didn't register it
    // libc++abi: terminating due to uncaught exception of type faiss::FaissException: Error in static InvertedListsIOHook *faiss::InvertedListsIOHook::lookup_classname(const std::string &) at /Volumes/w/src/github.com/facebookresearch/faiss/faiss/invlists/InvertedListsIOHook.cpp:75: read_InvertedLists: could not find classname N8faiss_s324S3BuildOnlyInvertedListsE
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