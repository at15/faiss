#pragma once

#include <faiss/invlists/InvertedLists.h>

namespace faiss_s3 {

// Type alias for Faiss index type to avoid polluting namespace
using idx_t = faiss::idx_t;

// Build index in memory and flush to S3.
// Base on the default ArrayInvertedLists
struct S3BuildOnlyInvertedLists : faiss::InvertedLists {
    // Stores vectors for all clusters.
    // Top level is the cluster, codes.size() == nlist.
    // std::vector<uint8_t> is a gaint vector to store all the vectors
    // in that cluster. No need for another vector because size
    // of each vector is same code_size. All we want is a large chunk
    // of memory. uint8_t is char, i.e. 1 byte.
    std::vector<std::vector<uint8_t>> codes;

    // Maps vectors within a cluster to their global id.
    // Top level is the cluster, ids.size() == nlist.
    // For std::vector<idx_t>
    // - the array offset is the index of vector within the cluster
    // - the value is the global id of the vector
    // NOTE: We need this mapping because the id is across clusters
    // and assigned base on insertion order (by default).
    // https://github.com/facebookresearch/faiss/wiki/Pre--and-post-processing#faiss-id-mapping
    std::vector<std::vector<idx_t>> ids;

    // nlist is number of clusters
    // code_size is size of single (encoded) vector e.g. 128*sizeof(float)
    // e.g. for IVFFlat this is the original vector size
    // for IVFPQ this is the PQ code size
    S3BuildOnlyInvertedLists(size_t nlist, size_t code_size);

    // Resize a cluster
    void resize(size_t list_no, size_t new_size) override;
    // Get number of vectors in a cluster
    size_t list_size(size_t list_no) const override;
    // inverted_list_context is for sharing centroid, only used by DispatchingInvertedLists
    bool is_empty(size_t list_no, void* inverted_list_context = nullptr) const override;
    // Reorder the clusters, for example shuffle the centroids base on cluster size.
    // Both the centroids and the vectors should be shuffled at same time.
    void permute_invlists(const idx_t* map);

    // Get all the vectors for a cluster as a raw pointer
    const uint8_t* get_codes(size_t list_no) const override;
    // Get all ids for a cluster as a raw pointer
    const idx_t* get_ids(size_t list_no) const override;

    // Add vectors to a cluster
    // - list_no is the cluster index
    // - n_entry is the number of vectors to add
    // - ids_in is the raw pointer and visit base on n_entry and sizeof(idx_t)
    // - code is the raw pointer and visit base on n_entry and code_size
    // Returns the offset of the first new vector in the cluster
    size_t add_entries(
        size_t list_no,
        size_t n_entry,
        const idx_t* ids_in,
        const uint8_t* code) override;

    // Update vectors in a cluster in place.
    // Similar to add_entries but gives offset to start updating from.
    void update_entries(
        size_t list_no,
        size_t offset,
        size_t n_entry,
        const idx_t* ids_in,
        const uint8_t* code) override;


    // TODO: What does the get_iterator do?
    // Seems it is used for getting vectors one by one
    // TODO: Is this efficient? Why not get vectors in batches to compute distance again the query vector?
    // There is a default CodeArrayIterator implementation in InvertedLists.cpp
};

// Lazy load data from S3 on demand
// struct S3ReadOnlyInvertedLists : faiss::InvertedLists {
// };

// TODO: Check IndexIVFlatPanorama https://github.com/facebookresearch/faiss/pull/4606
// TODO: I remember claude code mentioned there is prefetch .... might need to check the rocksdb implementation

} // namespace faiss_s3