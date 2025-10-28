// Version 2, only has a reader and rely on python script to generate the meta for the index
#include <iostream>

#include <faiss/IndexFlat.h>
#include <faiss/IndexIVFFlat.h>
#include <faiss/index_io.h>

#include "S3InvertedLists.h"

void read_local_file() {
  // Read local file to verify the S3ReadNothingInvertedLists is working
  auto index_name = "quora-index-quora-distilbert-multilingual-size-100000.idx";
  faiss::Index* idx = faiss::read_index(index_name, faiss_s3::IO_FLAG_S3);
  auto* index = dynamic_cast<faiss::IndexIVFFlat*>(idx);
  if (!index) {
      throw std::runtime_error("Loaded index is not IndexIVFFlat");
  }

  std::cout << "Index loaded (d=" << index->d
            << ", ntotal=" << index->ntotal << ", nlist=" << index->nlist
            << ")" << std::endl;
}

int main() {
    std::cout << "demo_v2" << std::endl;

    // Register S3 IO so we can skip loading cluster data when reading
    faiss_s3::register_s3_io_hook();

  read_local_file();

    return 0;
}
