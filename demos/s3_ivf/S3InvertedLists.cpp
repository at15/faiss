#include "S3InvertedLists.h"

#include <cassert>

namespace faiss_s3 {

// Type alias for Faiss index type to avoid polluting namespace
using idx_t = faiss::idx_t;

S3BuildOnlyInvertedLists::S3BuildOnlyInvertedLists(size_t nlist, size_t code_size)
    : faiss::InvertedLists(nlist, code_size) {
    codes.resize(nlist);
    ids.resize(nlist);
}

void S3BuildOnlyInvertedLists::resize(size_t list_no, size_t new_size) {
    ids[list_no].resize(new_size);
    codes[list_no].resize(new_size * code_size);
}

size_t S3BuildOnlyInvertedLists::list_size(size_t list_no) const {
    assert(list_no < nlist);
    return ids[list_no].size();
}

bool S3BuildOnlyInvertedLists::is_empty(size_t list_no, void* inverted_list_context) const {
    FAISS_THROW_IF_NOT(inverted_list_context == nullptr);
    return ids[list_no].size() == 0;
}

void S3BuildOnlyInvertedLists::permute_invlists(const idx_t* map) {
    // Shuffle the clusters according to the map
    // map[new_index] = old_index (where to get data from)
    std::vector<std::vector<uint8_t>> new_codes(nlist);
    std::vector<std::vector<idx_t>> new_ids(nlist);

    for (size_t i = 0; i < nlist; i++) {
        size_t o = map[i];
        // Must be a valid cluster index
        FAISS_THROW_IF_NOT(o < nlist);
        // Use swap instead of copy for O(1) performance (just swaps pointers)
        std::swap(new_codes[i], codes[o]);
        std::swap(new_ids[i], ids[o]);
    }

    // Swap the new permuted data back into the member variables
    std::swap(codes, new_codes);
    std::swap(ids, new_ids);
}

const uint8_t* S3BuildOnlyInvertedLists::get_codes(size_t list_no) const {
    assert(list_no < nlist);
    return codes[list_no].data();
}

const idx_t* S3BuildOnlyInvertedLists::get_ids(size_t list_no) const {
    assert(list_no < nlist);
    return ids[list_no].data();
}

size_t S3BuildOnlyInvertedLists::add_entries(
    size_t list_no,
    size_t n_entry,
    const idx_t* ids_in,
    const uint8_t* code) {
    if (n_entry == 0) {
        // NOTE: return 0 instead of ids[list_no].size() because the return value is
        // not used when n_entry is 0 ...
        // Exit early, though for in memory implementation, it is not providing
        // much performance benefits, would be useful for disk/remote implementation
        // to skip visting a cluster.
        return 0;
    }
    assert(list_no < nlist);

    size_t o = ids[list_no].size();
    // Resize and copy ids
    ids[list_no].resize(o + n_entry);
    memcpy(&ids[list_no][o], ids_in, n_entry * sizeof(idx_t));
    // Resize and copy vectors
    codes[list_no].resize((o + n_entry) * code_size);
    memcpy(&codes[list_no][o * code_size], code, n_entry * code_size);
    return o;
}

void S3BuildOnlyInvertedLists::update_entries(
    size_t list_no,
    size_t offset,
    size_t n_entry,
    const idx_t* ids_in,
    const uint8_t* code) {
    // Cluster exists
    assert(list_no < nlist);
    // Valid offset and n_entry
    assert(n_entry + offset <= ids[list_no].size());
    // Copy ids
    memcpy(&ids[list_no][offset], ids_in, n_entry * sizeof(idx_t));
    // Copy vectors
    memcpy(&codes[list_no][offset * code_size], code, n_entry * code_size);
}

} // namespace faiss_s3