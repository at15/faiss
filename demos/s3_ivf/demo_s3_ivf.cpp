#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include <faiss/IndexFlat.h>
#include <faiss/IndexIVFFlat.h>
#include <faiss/impl/AuxIndexStructures.h>
#include <faiss/impl/FaissException.h>
#include <faiss/impl/io.h>
#include <faiss/index_io.h>
#include <faiss/utils/random.h>

#include "S3InvertedLists.h"
#include "json.hpp"
#include "svf.h" // Read from simple vector format to use generated embeddings and text

using json = nlohmann::json;

// Custom IOWriter that wraps FileIOWriter and tracks total bytes written
class TrackingIOWriter : public faiss::IOWriter {
   public:
    explicit TrackingIOWriter(const char* filename) : bytes_written(0) {
        file = fopen(filename, "wb");
        if (!file) {
            FAISS_THROW_FMT("Could not open %s for writing", filename);
        }
        name = filename;
    }

    ~TrackingIOWriter() override {
        if (file) {
            fclose(file);
        }
    }

    size_t operator()(const void* ptr, size_t size, size_t nitems) override {
        size_t ret = fwrite(ptr, size, nitems, file);
        bytes_written += size * ret;
        return ret;
    }

    size_t get_bytes_written() const {
        return bytes_written;
    }

   private:
    FILE* file = nullptr;
    size_t bytes_written;
};

void build_index(const char* index_file_name) {
    std::cout
            << "Building index in memory and writing to disk with offset tracking..."
            << std::endl;

    const size_t VEC_DIM = 128;       // Vector dimension
    const size_t N_CLUSTERS = 100;    // Number of IVF clusters
    const size_t N_VECTORS = 100'000; // Number of vectors to index

    // Create index, default invert list implementation is memory
    faiss::IndexFlatL2 quantizer(VEC_DIM);
    faiss::IndexIVFFlat index(&quantizer, VEC_DIM, N_CLUSTERS);
    index.verbose = true;

    // Create a new ArrayInvertedLists so that we can get number of vectors in
    // each cluster and calculate the offset for each cluster.
    faiss::ArrayInvertedLists array_inverted_lists(N_CLUSTERS, index.code_size);
    // false means the index doesn't own the inverted lists (we manage it
    // ourselves)
    index.replace_invlists(&array_inverted_lists, false);

    // Generate random vectors
    std::vector<float> xb(VEC_DIM * N_VECTORS);
    faiss::float_rand(xb.data(), VEC_DIM * N_VECTORS, 12345);

    // Train
    // TODO: We can use less vectors for training
    index.train(N_VECTORS, xb.data());

    // Add vectors
    index.add(N_VECTORS, xb.data());

    // Calculate inverted list section sizes before writing
    // This mirrors the write_InvertedLists implementation from index_write.cpp

    // Count non-empty clusters to determine format (full vs sparse)
    size_t n_non0 = 0;
    for (size_t i = 0; i < array_inverted_lists.nlist; i++) {
        if (array_inverted_lists.list_size(i) > 0) {
            n_non0++;
        }
    }

    bool is_full_format = (n_non0 > array_inverted_lists.nlist / 2);

    // Calculate sizes of different sections
    // Header: fourcc(4) + nlist(8) + code_size(8) + list_type(4) = 24 bytes
    size_t invlist_header_size = 4 + 8 + 8 + 4;

    // Sizes array: WRITEVECTOR writes count(8) + data
    size_t sizes_array_count;
    size_t sizes_array_data_size;
    if (is_full_format) {
        // Full format: one size_t per cluster
        sizes_array_count = array_inverted_lists.nlist;
        sizes_array_data_size = array_inverted_lists.nlist * sizeof(size_t);
    } else {
        // Sparse format: pairs of (cluster_id, size) for non-empty clusters
        sizes_array_count = n_non0 * 2;
        sizes_array_data_size = n_non0 * 2 * sizeof(size_t);
    }
    size_t sizes_array_size = 8 + sizes_array_data_size; // count + data

    // Calculate total size of cluster data (codes + ids)
    size_t inverted_list_data_size = 0;
    for (size_t i = 0; i < array_inverted_lists.nlist; i++) {
        size_t n = array_inverted_lists.list_size(i);
        if (n > 0) {
            inverted_list_data_size +=
                    n * array_inverted_lists.code_size;          // codes
            inverted_list_data_size += n * sizeof(faiss::idx_t); // ids
        }
    }

    size_t inverted_lists_total_size =
            invlist_header_size + sizes_array_size + inverted_list_data_size;

    std::cout << "Format: " << (is_full_format ? "full" : "sparse")
              << std::endl;
    std::cout << "Non-empty clusters: " << n_non0 << " / "
              << array_inverted_lists.nlist << std::endl;
    std::cout << "Inverted lists header size: " << invlist_header_size
              << " bytes" << std::endl;
    std::cout << "Sizes array size: " << sizes_array_size << " bytes"
              << std::endl;
    std::cout << "Cluster data size: " << inverted_list_data_size << " bytes"
              << std::endl;

    // Write index using custom tracking writer
    TrackingIOWriter writer(index_file_name);
    faiss::write_index(&index, &writer);

    size_t total_size = writer.get_bytes_written();

    // The inverted lists section starts at: total_size -
    // inverted_lists_total_size
    size_t inverted_lists_offset = total_size - inverted_lists_total_size;

    // The sizes array starts after the inverted lists header
    size_t sizes_array_offset = inverted_lists_offset + invlist_header_size;

    // The cluster data starts after the sizes array
    size_t cluster_data_offset = sizes_array_offset + sizes_array_size;

    std::cout << "Total file size: " << total_size << " bytes" << std::endl;
    std::cout << "Inverted lists offset: " << inverted_lists_offset << " bytes"
              << std::endl;
    std::cout << "Sizes array offset: " << sizes_array_offset << " bytes"
              << std::endl;
    std::cout << "Cluster data offset: " << cluster_data_offset << " bytes"
              << std::endl;

    // Generate metadata JSON
    json metadata;
    metadata["total_size"] = total_size;
    metadata["inverted_lists_offset"] = inverted_lists_offset;
    metadata["n_clusters"] = array_inverted_lists.nlist;
    metadata["code_size"] = array_inverted_lists.code_size;

    // Information about the sizes array in the file
    metadata["sizes_array_offset"] = sizes_array_offset;
    metadata["sizes_array_count"] = sizes_array_count;
    metadata["sizes_array_format"] = is_full_format ? "full" : "sparse";

    // Cluster data section
    metadata["cluster_data_offset"] = cluster_data_offset;

    // Write metadata to .meta.json file
    std::string metadata_file = std::string(index_file_name) + ".meta.json";
    std::ofstream meta_out(metadata_file);
    if (!meta_out) {
        FAISS_THROW_FMT("Could not open %s for writing", metadata_file.c_str());
    }
    meta_out << metadata.dump(2) << std::endl;
    meta_out.close();

    std::cout << "Metadata written to: " << metadata_file << std::endl;
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