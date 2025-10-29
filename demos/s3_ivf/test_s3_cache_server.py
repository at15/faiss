#!/usr/bin/env python3
"""
Test the real S3 cache server with Quora dataset

This script:
1. Loads the quora embeddings and text from the pickle file
2. Connects to the S3 cache server
3. Loads the quora index from S3
4. Encodes test queries using SentenceTransformer
5. Performs semantic search
6. Maps result IDs back to text
7. Displays human-readable results
"""

import os
import pickle
import numpy as np
from typing import List, Tuple, Dict
import sys

# Add current directory to path for importing test_tcp_client
sys.path.insert(0, os.path.dirname(__file__))
from test_tcp_client import S3CacheClient, S3CacheClientError

# Try to import sentence_transformers
try:
    from sentence_transformers import SentenceTransformer
except ImportError:
    print("ERROR: sentence-transformers not installed")
    print("Please run: pip install sentence-transformers")
    sys.exit(1)

# Configuration
EMBEDDING_FILE = "quora-embeddings-quora-distilbert-multilingual-size-100000.pt"
INDEX_BUCKET = "test-bucket"
INDEX_KEY = "quora-index-quora-distilbert-multilingual-size-100000.idx"
CLUSTER_DATA_OFFSET = 3154059  # From metadata JSON

MODEL_NAME = "quora-distilbert-multilingual"


def load_embeddings() -> Tuple[List[str], np.ndarray]:
    """Load quora embeddings and text from pickle file"""
    print(f"\n[Setup] Loading embeddings from {EMBEDDING_FILE}...")

    if not os.path.exists(EMBEDDING_FILE):
        print(f"ERROR: Embedding file not found: {EMBEDDING_FILE}")
        print("Please run gen_ivf.py first to generate the embeddings")
        sys.exit(1)

    with open(EMBEDDING_FILE, "rb") as f:
        cache_data = pickle.load(f)

    sentences = cache_data["sentences"]
    embeddings = cache_data["embeddings"]

    print(f"  Loaded {len(sentences):,} sentences")
    print(f"  Embedding dimension: {embeddings.shape[1]}")

    return sentences, embeddings


def normalize_embedding(embedding: np.ndarray) -> np.ndarray:
    """Normalize embedding for cosine similarity (Inner Product metric)"""
    norm = np.linalg.norm(embedding)
    if norm == 0:
        return embedding
    return embedding / norm


class QuoraSearchTester:
    def __init__(self):
        self.corpus_sentences = None
        self.corpus_embeddings = None
        self.model = None
        self.client = None

    def setup(self):
        """Load data and models"""
        # Load text mapping
        self.corpus_sentences, self.corpus_embeddings = load_embeddings()

        # Load model for encoding queries
        print(f"\n[Setup] Loading SentenceTransformer model: {MODEL_NAME}...")
        self.model = SentenceTransformer(MODEL_NAME)
        print("  Model loaded successfully")

    def connect_and_load(self) -> Dict[str, int]:
        """Connect to server and load index"""
        print("\n" + "=" * 80)
        print("Connecting to S3 Cache Server")
        print("=" * 80)

        self.client = S3CacheClient()
        self.client.connect()

        print(f"\n[Load] Loading index from S3...")
        print(f"  Bucket: {INDEX_BUCKET}")
        print(f"  Key: {INDEX_KEY}")
        print(f"  Cluster data offset: {CLUSTER_DATA_OFFSET:,}")

        # Send LOAD command
        params = {
            "bucket": INDEX_BUCKET,
            "key": INDEX_KEY,
            "cluster_data_offset": str(CLUSTER_DATA_OFFSET)
        }
        self.client._send_command("LOAD", params)

        # Receive response with metadata
        response_line = self.client._recv_line()
        response = self.client._parse_response(response_line)

        index_info = {
            "index": int(response.get("index", 0)),
            "d": int(response.get("d", 0)),
            "ntotal": int(response.get("ntotal", 0)),
            "nlist": int(response.get("nlist", 0)),
            "metric_type": int(response.get("metric_type", 0))
        }

        print(f"\n[Load] Index loaded successfully:")
        print(f"  Index ID: {index_info['index']}")
        print(f"  Dimension: {index_info['d']}")
        print(f"  Total vectors: {index_info['ntotal']:,}")
        print(f"  Clusters: {index_info['nlist']}")
        print(f"  Metric type: {index_info['metric_type']} (1=INNER_PRODUCT)")

        return index_info

    def search_query(
            self, index_id: int, query: str, k: int = 5
    ) -> Tuple[np.ndarray, np.ndarray]:
        """Search for a single query"""
        # Encode query
        embedding = self.model.encode(query, convert_to_numpy=True)
        embedding = normalize_embedding(embedding)
        embedding = embedding.astype(np.float32)

        # Search
        ids, distances = self.client.search(index_id, embedding, k)

        return ids, distances

    def display_results(
            self, query: str, ids: np.ndarray, distances: np.ndarray
    ):
        """Display search results with text"""
        print(f"\nQuery: \"{query}\"")
        print("-" * 80)

        for rank, (corpus_id, score) in enumerate(zip(ids, distances), 1):
            text = self.corpus_sentences[corpus_id]
            # For INNER_PRODUCT metric, higher is better (closer to 1.0 for normalized vectors)
            print(f"  {rank}. [score={score:.4f}] {text}")

    def run_tests(self):
        """Run comprehensive tests"""
        print("\n" + "=" * 80)
        print("S3 Cache Server - Quora Dataset Testing")
        print("=" * 80)

        # Setup
        self.setup()

        # Connect and load index
        index_info = self.connect_and_load()
        index_id = index_info["index"]

        # Define test queries
        test_queries = [
            "How to find a job",
            "What to eat for lunch",
            "Which sport is similar to tennis",
            "How do I learn programming",
            "What is the best way to lose weight",
            "How can I improve my English speaking skills",
            "What are good books to read",
            "How do I start investing in stocks"
        ]

        print("\n" + "=" * 80)
        print(f"Performing {len(test_queries)} Searches")
        print("=" * 80)

        # Perform searches
        for i, query in enumerate(test_queries, 1):
            print(f"\n--- Search {i}/{len(test_queries)} ---")
            try:
                ids, distances = self.search_query(index_id, query, k=5)
                self.display_results(query, ids, distances)
            except S3CacheClientError as e:
                print(f"  ERROR: {e}")
            except Exception as e:
                print(f"  ERROR: {e}")

        # Display cache statistics
        print("\n" + "=" * 80)
        print("Cache Statistics")
        print("=" * 80)

        try:
            cache_stats = self.client.info_cache()
            print(f"\nGlobal Statistics:")
            print(f"  Index count: {cache_stats['index_count']}")
            print(f"  Cache hits: {cache_stats['cache_hits']:,}")
            print(f"  Cache misses: {cache_stats['cache_misses']:,}")

            if cache_stats['cache_hits'] + cache_stats['cache_misses'] > 0:
                hit_rate = (
                    cache_stats['cache_hits'] /
                    (cache_stats['cache_hits'] + cache_stats['cache_misses']) * 100
                )
                print(f"  Cache hit rate: {hit_rate:.1f}%")

            index_stats = self.client.info_index(index_id)
            print(f"\nIndex {index_id} Statistics:")
            print(f"  Total clusters: {index_stats['cluster_count']}")
            print(f"  Cached clusters: {index_stats['cached_clusters']}")
            print(f"  Cache hits: {index_stats['cache_hits']:,}")
            print(f"  Cache misses: {index_stats['cache_misses']:,}")
            print(f"  Nprobe: {index_stats.get('nprobe', 'N/A')}")

            if index_stats['cache_hits'] + index_stats['cache_misses'] > 0:
                hit_rate = (
                    index_stats['cache_hits'] /
                    (index_stats['cache_hits'] + index_stats['cache_misses']) * 100
                )
                print(f"  Cache hit rate: {hit_rate:.1f}%")

            cache_ratio = (
                index_stats['cached_clusters'] / index_stats['cluster_count'] * 100
            )
            print(f"  Cache coverage: {cache_ratio:.1f}%")

        except Exception as e:
            print(f"ERROR getting cache stats: {e}")

        # Close connection
        self.client.close()

        print("\n" + "=" * 80)
        print("All tests completed!")
        print("=" * 80)


def main():
    """Main entry point"""
    tester = QuoraSearchTester()
    try:
        tester.run_tests()
    except KeyboardInterrupt:
        print("\n\nInterrupted by user")
        if tester.client:
            tester.client.close()
    except Exception as e:
        print(f"\n\nFATAL ERROR: {e}")
        import traceback
        traceback.print_exc()
        if tester.client:
            tester.client.close()
        sys.exit(1)


if __name__ == "__main__":
    main()
