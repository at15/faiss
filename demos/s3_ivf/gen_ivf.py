# Python script to generate a IVF index from text using SentenceTransformers

import os
import time
import json

import pandas as pd
import numpy as np
import pickle
import faiss

# 768 dimensions https://huggingface.co/sentence-transformers/quora-distilbert-multilingual
model_name = "quora-distilbert-multilingual"
embedding_size = 768  # Size of embeddings
top_k_hits = 10  # Output k hits

dataset_path = "quora_duplicate_questions.tsv"
# 100k was too much on my mac for the kNN search by util.semantic_search
max_corpus_size = 100_000

# NOTE: model loading is 1.64s, the import package is actually taking even longer ...
def load_model():
    from sentence_transformers import SentenceTransformer
    print(f"Loading model: {model_name}...")
    start_time = time.monotonic()
    model = SentenceTransformer(model_name)
    end_time = time.monotonic()
    print(f"Model loaded in {end_time - start_time:.2f} seconds")
    return model

def get_embedding_cache_path():
    return "quora-embeddings-{}-size-{}.pt".format(model_name.replace("/", "_"), max_corpus_size)

def load_embeddings():
    embedding_cache_path = get_embedding_cache_path()
    if os.path.exists(embedding_cache_path):
        with open(embedding_cache_path, "rb") as f:
            cache_data = pickle.load(f)
        return cache_data["sentences"], cache_data["embeddings"]
    raise FileNotFoundError(f"Embeddings cache not found at {embedding_cache_path}")

# Create embedding returns numpy array
def create_embeddings(model):
    # Check if the dataset exists
    if not os.path.exists(dataset_path):
        raise FileNotFoundError(f"Dataset file {dataset_path} not found")

    # Get all unique sentences from the file using pandas
    df = pd.read_csv(dataset_path, sep="\t")
    print(df.head())

    # Combine question1 and question2 columns and get unique values
    corpus_sentences = set()
    for _, row in df.iterrows():
        corpus_sentences.add(row["question1"])
        # Do the length check twice to avoid having 1001...
        if len(corpus_sentences) >= max_corpus_size:
            break
        corpus_sentences.add(row["question2"])
        if len(corpus_sentences) >= max_corpus_size:
            break

    corpus_sentences = list(corpus_sentences)
    print(f"Loaded {len(corpus_sentences)} unique sentences from the corpus")

    print("Encoding the corpus. This might take a while...")
    start_time = time.monotonic()
    corpus_embeddings = model.encode(corpus_sentences, show_progress_bar=True, convert_to_numpy=True)
    end_time = time.monotonic()
    total_time = end_time - start_time
    avg_time = total_time / len(corpus_sentences)
    print(f"Encoding completed in {total_time:.2f} seconds")
    print(f"Average time per embedding: {avg_time * 1000:.2f} ms")

    embedding_cache_path = get_embedding_cache_path()
    print(f"Storing embeddings to {embedding_cache_path}")
    with open(embedding_cache_path, "wb") as f:
        pickle.dump({"sentences": corpus_sentences, "embeddings": corpus_embeddings}, f)
    print("Embeddings created and saved successfully!")
    return corpus_sentences, corpus_embeddings

def get_index_cache_path():
    return "quora-index-{}-size-{}.idx".format(model_name.replace("/", "_"), max_corpus_size)

def create_index(corpus_embeddings):
    n_clusters = 1024
    quantizer = faiss.IndexFlatIP(embedding_size)
    index = faiss.IndexIVFFlat(quantizer, embedding_size, n_clusters, faiss.METRIC_INNER_PRODUCT)
    # Number of clusters to explorer at search time. We will search for nearest neighbors in 3 clusters.
    index.nprobe = 3
    # Normalize so that dot product is cosine similarity
    corpus_embeddings = corpus_embeddings / np.linalg.norm(corpus_embeddings, axis=1)[:, None]
    # Train on all data (for now)
    index.train(corpus_embeddings)
    index.add(corpus_embeddings)

    # Write the index
    index_cache_path = get_index_cache_path()
    faiss.write_index(index, index_cache_path)
    print(f"Index created and saved to {index_cache_path}")
    return index

def load_index():
    index_cache_path = get_index_cache_path()
    if os.path.exists(index_cache_path):
        return faiss.read_index(index_cache_path)
    raise FileNotFoundError(f"Index not found at {index_cache_path}")

def calculate_inverted_lists_offsets(ivf_index, index_file_path):
    """
    Calculate the file offsets for inverted lists data, mirroring the C++ implementation.
    This helps in understanding where each inverted list is stored in the index file.
    """
    # Get the inverted lists (ArrayInvertedLists)
    invlists = ivf_index.invlists
    nlist = ivf_index.nlist
    code_size = invlists.code_size

    # Count non-empty clusters
    n_non0 = 0
    list_sizes = []
    for i in range(nlist):
        size = ivf_index.get_list_size(i)
        list_sizes.append(size)
        if size > 0:
            n_non0 += 1

    # Determine format (full vs sparse)
    is_full_format = (n_non0 > nlist / 2)

    # Calculate sizes of different sections
    # Header: fourcc(4) + nlist(8) + code_size(8) + list_type(4) = 24 bytes
    invlist_header_size = 4 + 8 + 8 + 4

    # Sizes array: count(8) + data
    if is_full_format:
        # Full format: one size_t per cluster
        sizes_array_count = nlist
        sizes_array_data_size = nlist * 8  # sizeof(size_t) = 8
    else:
        # Sparse format: pairs of (cluster_id, size) for non-empty clusters
        sizes_array_count = n_non0 * 2
        sizes_array_data_size = n_non0 * 2 * 8

    sizes_array_size = 8 + sizes_array_data_size  # count + data

    # Calculate total size of cluster data (codes + ids)
    inverted_list_data_size = 0
    cluster_offsets = []
    current_offset = 0

    for i in range(nlist):
        n = list_sizes[i]
        cluster_info = {
            "cluster_id": i,
            "size": n,
            "codes_offset": current_offset,
            "ids_offset": current_offset + n * code_size
        }
        cluster_offsets.append(cluster_info)

        if n > 0:
            inverted_list_data_size += n * code_size  # codes
            inverted_list_data_size += n * 8  # ids (sizeof(idx_t) = 8)
            current_offset += n * code_size + n * 8

    inverted_lists_total_size = invlist_header_size + sizes_array_size + inverted_list_data_size

    # Get the actual file size
    file_size = os.path.getsize(index_file_path)

    # Calculate offsets in the file
    inverted_lists_offset = file_size - inverted_lists_total_size
    sizes_array_offset = inverted_lists_offset + invlist_header_size
    cluster_data_offset = sizes_array_offset + sizes_array_size

    # Adjust cluster offsets to be relative to the file start
    for cluster_info in cluster_offsets:
        cluster_info["codes_offset"] += cluster_data_offset
        cluster_info["ids_offset"] += cluster_data_offset

    # Generate metadata
    metadata = {
        "index_file": index_file_path,
        "total_size": file_size,
        "inverted_lists_offset": inverted_lists_offset,
        "n_clusters": nlist,
        "code_size": code_size,
        "sizes_array_offset": sizes_array_offset,
        "sizes_array_count": sizes_array_count,
        "sizes_array_format": "full" if is_full_format else "sparse",
        "cluster_data_offset": cluster_data_offset,
        "cluster_data_size": inverted_list_data_size,
        "non_empty_clusters": n_non0,
        "inverted_lists_total_size": inverted_lists_total_size,
        "cluster_offsets": cluster_offsets
    }

    print(f"\nOffset Calculation Results:")
    print(f"  Format: {metadata['sizes_array_format']}")
    print(f"  Non-empty clusters: {n_non0} / {nlist}")
    print(f"  Inverted lists header size: {invlist_header_size} bytes")
    print(f"  Sizes array size: {sizes_array_size} bytes")
    print(f"  Cluster data size: {inverted_list_data_size} bytes")
    print(f"  Total file size: {file_size} bytes")
    print(f"  Inverted lists offset: {inverted_lists_offset} bytes")
    print(f"  Sizes array offset: {sizes_array_offset} bytes")
    print(f"  Cluster data offset: {cluster_data_offset} bytes")

    return metadata

def write_metadata_json(metadata, index_file_path):
    """Write metadata to a JSON file with the same name as the index file + .json suffix"""
    metadata_file = index_file_path + ".json"
    with open(metadata_file, 'w') as f:
        json.dump(metadata, f, indent=2)
    print(f"\nMetadata written to: {metadata_file}")
    return metadata_file

def main():
    print("Starting IVF index generation...")
    
    # Create embeddings
    # model = load_model()
    # corpus_sentences, corpus_embeddings = create_embeddings(model)

    # Create index
    # corpus_sentences, corpus_embeddings = load_embeddings()
    # print(f"Corpus embeddings shape: {corpus_embeddings.shape}")
    # print(f"Corpus sentences shape: {len(corpus_sentences)}")
    # index = create_index(corpus_embeddings)

    # Read index
    index = load_index()
    index_file_path = get_index_cache_path()
    print("Loaded index", type(index))

    # Cast to IndexIVFFlat and print list sizes
    if isinstance(index, faiss.IndexIVFFlat):
        ivf_index = index
        print(f"\nIndex Information:")
        print(f"  Number of lists (clusters): {ivf_index.nlist}")
        print(f"  Total vectors: {ivf_index.ntotal}")
        print(f"  nprobe: {ivf_index.nprobe}")

        # Print size of each inverted list
        print("\nInverted list sizes:")
        list_sizes = []
        for list_no in range(ivf_index.nlist):
            list_size = ivf_index.get_list_size(list_no)
            list_sizes.append(list_size)
            if list_size > 0:  # Only print non-empty lists (limit output)
                if list_no < 10 or list_no >= ivf_index.nlist - 5:
                    print(f"  List {list_no}: {list_size} vectors")
                elif list_no == 10:
                    print(f"  ... (showing first 10 and last 5 non-empty lists)")

        # Print summary statistics
        non_empty_lists = [s for s in list_sizes if s > 0]
        print(f"\nSummary:")
        print(f"  Non-empty lists: {len(non_empty_lists)} / {ivf_index.nlist}")
        print(f"  Min list size: {min(list_sizes)}")
        print(f"  Max list size: {max(list_sizes)}")
        print(f"  Avg list size: {sum(list_sizes) / len(list_sizes):.2f}")
        if non_empty_lists:
            print(f"  Avg non-empty list size: {sum(non_empty_lists) / len(non_empty_lists):.2f}")

        # Calculate and write inverted lists offsets
        metadata = calculate_inverted_lists_offsets(ivf_index, index_file_path)
        write_metadata_json(metadata, index_file_path)
    else:
        print(f"Index is not IndexIVFFlat, it's {type(index)}")

if __name__ == "__main__":
    main()