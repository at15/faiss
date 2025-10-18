/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// -*- c++ -*-

#include <exception>
#include <iostream>
#include <memory>
#include <string>

#include "RocksDBInvertedLists.h"

#include <faiss/IndexFlat.h>
#include <faiss/IndexIVFFlat.h>
#include <faiss/impl/AuxIndexStructures.h>
#include <faiss/impl/FaissException.h>
#include <faiss/index_io.h>
#include <faiss/utils/random.h>

using namespace faiss;

// ============================================================================
// Constants
// ============================================================================
const size_t D = 128;        // Vector dimension
const size_t NLIST = 100;    // Number of IVF clusters
const idx_t NB = 10000;      // Number of vectors to index
const idx_t NQ = 20;         // Number of query vectors
const idx_t K = 5;           // Top-K for k-NN search
const float RANGE = 15.0f;   // Radius for range search
const int NPROBE = 2;        // Number of clusters to probe during search

// ============================================================================
// Helper Functions
// ============================================================================

void print_usage(const char* program_name) {
    std::cerr << "Usage:\n";
    std::cerr << "  Build mode:  " << program_name
              << " build <db_directory> <index_file>\n";
    std::cerr << "  Search mode: " << program_name
              << " search <db_directory> <index_file>\n";
    std::cerr << "\n";
    std::cerr << "Modes:\n";
    std::cerr << "  build  - Train index, add data, save to disk\n";
    std::cerr << "  search - Load index from disk and perform searches\n";
    std::cerr << "\n";
    std::cerr << "Arguments:\n";
    std::cerr << "  db_directory - RocksDB directory for inverted lists\n";
    std::cerr << "  index_file   - Faiss index file (centroids + metadata)\n";
    std::cerr << "\n";
    std::cerr << "Example:\n";
    std::cerr << "  " << program_name << " build rocksdb_data index.faiss\n";
    std::cerr << "  " << program_name << " search rocksdb_data index.faiss\n";
}

void print_knn_results(
        idx_t nq,
        idx_t k,
        const std::vector<idx_t>& labels,
        const std::vector<float>& distances) {
    std::cout << "\n=== K-NN Search Results (k=" << k << ") ===" << std::endl;
    for (idx_t iq = 0; iq < nq; iq++) {
        std::cout << "Query " << iq << ": ";
        for (idx_t j = 0; j < k; j++) {
            std::cout << labels[iq * k + j] << " (" << distances[iq * k + j]
                      << ") | ";
        }
        std::cout << std::endl;
    }
}

void print_range_results(idx_t nq, float range, const RangeSearchResult& result) {
    std::cout << "\n=== Range Search Results (radius=" << range
              << ") ===" << std::endl;
    for (idx_t iq = 0; iq < nq; iq++) {
        size_t count = result.lims[iq + 1] - result.lims[iq];
        std::cout << "Query " << iq << " (" << count << " results): ";
        for (auto j = result.lims[iq]; j < result.lims[iq + 1]; j++) {
            std::cout << result.labels[j] << " (" << result.distances[j]
                      << ") | ";
        }
        std::cout << std::endl;
    }
}

void run_searches(
        IndexIVFFlat& index,
        const std::vector<float>& query_vectors) {
    idx_t nq = query_vectors.size() / D;

    // K-NN Search
    std::vector<float> distances(nq * K);
    std::vector<idx_t> labels(nq * K, -1);
    index.search(
            nq, query_vectors.data(), K, distances.data(), labels.data(), nullptr);
    print_knn_results(nq, K, labels, distances);

    // Range Search
    RangeSearchResult result(nq);
    index.range_search(nq, query_vectors.data(), RANGE, &result);
    print_range_results(nq, RANGE, result);
}

// ============================================================================
// Build Mode: Train, Add Data, Save Index
// ============================================================================

void build_mode(const char* db_dir, const char* index_file) {
    std::cout << "\n========================================" << std::endl;
    std::cout << "=== BUILD MODE ===" << std::endl;
    std::cout << "========================================\n" << std::endl;

    std::cout << "[1/5] Creating IVF index..." << std::endl;
    IndexFlatL2 quantizer(D);
    IndexIVFFlat index(&quantizer, D, NLIST);

    std::cout << "[2/5] Connecting to RocksDB at '" << db_dir << "'..."
              << std::endl;
    faiss_rocksdb::RocksDBInvertedLists ril(db_dir, NLIST, index.code_size);
    index.replace_invlists(&ril, false);

    std::cout << "[3/5] Generating and training with " << NB << " vectors..."
              << std::endl;
    std::vector<float> xb(D * NB);
    float_rand(xb.data(), D * NB, 12345);
    std::vector<idx_t> xids(NB);
    std::iota(xids.begin(), xids.end(), 0);

    index.verbose = true;
    index.train(NB, xb.data());
    std::cout << std::endl;

    std::cout << "[4/5] Adding " << NB << " vectors to index (stored in RocksDB)..."
              << std::endl;
    index.add_with_ids(NB, xb.data(), xids.data());

    std::cout << "[5/5] Saving index to '" << index_file << "'..." << std::endl;
    // Temporarily replace invlists to avoid serializing RocksDB structure
    ArrayInvertedLists empty_lists(NLIST, index.code_size);
    index.replace_invlists(&empty_lists, false);
    write_index(&index, index_file);
    // Restore RocksDB invlists
    index.replace_invlists(&ril, false);

    std::cout << "\n=== Build Complete ===" << std::endl;
    std::cout << "Index saved to:        " << index_file << std::endl;
    std::cout << "Inverted lists in:     " << db_dir << std::endl;
    std::cout << "\nIndex Statistics:" << std::endl;
    std::cout << "  Dimension:           " << D << std::endl;
    std::cout << "  Number of clusters:  " << NLIST << std::endl;
    std::cout << "  Total vectors:       " << NB << std::endl;

    // Run sample searches to verify
    std::cout << "\n=== Running Sample Searches to Verify ===" << std::endl;
    std::vector<float> query_vectors(xb.begin(), xb.begin() + D * NQ);
    index.nprobe = NPROBE;
    run_searches(index, query_vectors);

    std::cout << "\nBuild mode completed successfully!" << std::endl;
    std::cout << "You can now run: " << std::endl;
    std::cout << "  ./demo_rocksdb_ivf search " << db_dir << " " << index_file
              << std::endl;
}

// ============================================================================
// Search Mode: Load Index, Connect to RocksDB, Search
// ============================================================================

void search_mode(const char* db_dir, const char* index_file) {
    std::cout << "\n========================================" << std::endl;
    std::cout << "=== SEARCH MODE ===" << std::endl;
    std::cout << "========================================\n" << std::endl;

    std::cout << "[1/3] Loading index from '" << index_file << "'..."
              << std::endl;
    Index* idx = read_index(index_file);
    auto* index = dynamic_cast<IndexIVFFlat*>(idx);
    if (!index) {
        std::cerr << "Error: Loaded index is not an IndexIVFFlat" << std::endl;
        delete idx;
        throw FaissException("Invalid index type");
    }

    std::cout << "[2/3] Connecting to RocksDB at '" << db_dir << "'..."
              << std::endl;
    faiss_rocksdb::RocksDBInvertedLists ril(db_dir, index->nlist, index->code_size);
    index->replace_invlists(&ril, false);

    std::cout << "[3/3] Configuring search parameters..." << std::endl;
    index->nprobe = NPROBE;

    std::cout << "\n=== Load Complete ===" << std::endl;
    std::cout << "Loaded index from:     " << index_file << std::endl;
    std::cout << "Connected to RocksDB:  " << db_dir << std::endl;
    std::cout << "\nIndex Statistics:" << std::endl;
    std::cout << "  Dimension:           " << index->d << std::endl;
    std::cout << "  Number of clusters:  " << index->nlist << std::endl;
    std::cout << "  Total vectors:       " << index->ntotal << std::endl;
    std::cout << "  Search nprobe:       " << index->nprobe << std::endl;

    // Generate query vectors (same seed as build mode for reproducibility)
    std::cout << "\n=== Generating Query Vectors ===" << std::endl;
    std::vector<float> xb(D * NB);
    float_rand(xb.data(), D * NB, 12345);
    std::vector<float> query_vectors(xb.begin(), xb.begin() + D * NQ);

    std::cout << "\n=== Running Searches ===" << std::endl;
    run_searches(*index, query_vectors);

    std::cout << "\nSearch mode completed successfully!" << std::endl;

    delete index;
}

// ============================================================================
// Main Entry Point
// ============================================================================

int main(int argc, char* argv[]) {
    try {
        if (argc != 4) {
            print_usage(argv[0]);
            return 1;
        }

        std::string mode = argv[1];
        const char* db_dir = argv[2];
        const char* index_file = argv[3];

        if (mode == "build") {
            build_mode(db_dir, index_file);
        } else if (mode == "search") {
            search_mode(db_dir, index_file);
        } else {
            std::cerr << "Error: Invalid mode '" << mode << "'" << std::endl;
            std::cerr << "Must be 'build' or 'search'" << std::endl;
            print_usage(argv[0]);
            return 1;
        }

        return 0;

    } catch (FaissException& e) {
        std::cerr << "\nFaiss Error: " << e.what() << std::endl;
        return 1;
    } catch (std::exception& e) {
        std::cerr << "\nError: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "\nUnknown error occurred!" << std::endl;
        return 1;
    }
}
