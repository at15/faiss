// Header only library for Simple Vector Format
// Only support reading SVF file
#pragma once

#include <fstream>
#include <vector>
#include <string>
#include <stdexcept>
#include <cstdint>  // For uint32_t, uint64_t (fixed-width integers)

#include "json.hpp"

namespace svf {

using json = nlohmann::json;

struct SVFData {
    std::vector<std::string> corpus;
    std::vector<float> embeddings;  // Flat array: [row0_dim0, row0_dim1, ..., row1_dim0, ...]
    size_t num_rows;
    size_t dimension;
};

class SVFReader {
public:
    static SVFData read(const std::string& filename) {
        std::ifstream file(filename, std::ios::binary);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open file: " + filename);
        }

        // Read footer from the end
        // Footer structure: <metadata_json><metadata_length(4)><magic(4)>

        // Seek to end - 4 bytes to read magic header
        file.seekg(-4, std::ios::end);
        char magic[5] = {0};
        file.read(magic, 4);
        if (std::string(magic) != "SVF1") {
            throw std::runtime_error("Invalid magic header: " + std::string(magic) + ", expected SVF1");
        }

        // Read metadata length (4 bytes before magic)
        file.seekg(-8, std::ios::end);
        uint32_t metadata_length;
        file.read(reinterpret_cast<char*>(&metadata_length), 4);
        // metadata_length is already in little-endian (x86/ARM are little-endian)

        // Read metadata JSON
        std::streamoff offset = -(static_cast<std::streamoff>(8 + metadata_length));
        file.seekg(offset, std::ios::end);
        std::vector<char> metadata_buffer(metadata_length);
        file.read(metadata_buffer.data(), metadata_length);
        std::string metadata_str(metadata_buffer.begin(), metadata_buffer.end());

        json metadata;
        try {
            metadata = json::parse(metadata_str);
        } catch (const json::exception& e) {
            throw std::runtime_error("Failed to parse metadata JSON: " + std::string(e.what()));
        }

        // Validate metadata
        if (metadata["version"] != "0.1") {
            throw std::runtime_error("Unsupported version: " + metadata["version"].get<std::string>());
        }

        auto columns = metadata["columns"];
        if (columns.size() != 2) {
            throw std::runtime_error("Expected 2 columns, got " + std::to_string(columns.size()));
        }

        // Find vector and string columns
        json vector_col, string_col;
        for (const auto& col : columns) {
            if (col["type"] == "vector") {
                vector_col = col;
            } else if (col["type"] == "string") {
                string_col = col;
            }
        }

        if (vector_col.is_null() || string_col.is_null()) {
            throw std::runtime_error("Missing vector or string column in metadata");
        }

        // Read vectors
        uint64_t vector_offset = vector_col["offset"];
        uint64_t vector_dim = vector_col["vector"]["dimension"];
        std::string vector_type = vector_col["vector"]["type"];

        if (vector_type != "float32") {
            throw std::runtime_error("Unsupported vector type: " + vector_type);
        }

        // Calculate number of rows from string column offset and vector data size
        uint64_t string_offset = string_col["offset"];
        uint64_t vector_bytes = string_offset - vector_offset;
        uint64_t num_rows = vector_bytes / (vector_dim * sizeof(float));

        // Read vector data
        file.seekg(vector_offset, std::ios::beg);
        std::vector<float> embeddings(num_rows * vector_dim);
        file.read(reinterpret_cast<char*>(embeddings.data()), vector_bytes);

        // Read strings
        file.seekg(string_offset, std::ios::beg);
        std::vector<std::string> corpus;
        corpus.reserve(num_rows);

        for (size_t i = 0; i < num_rows; ++i) {
            // Read length prefix (4 bytes, little-endian)
            uint32_t string_length;
            file.read(reinterpret_cast<char*>(&string_length), 4);

            if (!file) {
                throw std::runtime_error("Unexpected end of file while reading string length");
            }

            // Read string data
            std::vector<char> string_buffer(string_length);
            file.read(string_buffer.data(), string_length);

            if (!file) {
                throw std::runtime_error("Unexpected end of file while reading string data");
            }

            corpus.emplace_back(string_buffer.begin(), string_buffer.end());
        }

        SVFData data;
        data.corpus = std::move(corpus);
        data.embeddings = std::move(embeddings);
        data.num_rows = num_rows;
        data.dimension = vector_dim;

        return data;
    }
};

} // namespace svf
