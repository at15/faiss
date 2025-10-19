#!/usr/bin/env python3
"""
Demo comparing IndexIDMap and IndexIDMap2 implementation differences.

This demonstrates the key difference: IndexIDMap2 maintains a reverse mapping
(unordered_map) that enables O(1) reconstruction by custom ID, while IndexIDMap
would require O(n) linear search.
"""

import numpy as np
import faiss
import time

def demo_basic_difference():
    """Show the core data structure difference"""
    print("=" * 70)
    print("BASIC DATA STRUCTURE DIFFERENCE")
    print("=" * 70)

    d = 64
    nb = 1000

    # Generate data
    vectors = np.random.random((nb, d)).astype('float32')
    custom_ids = np.arange(nb, dtype='int64') + 10000  # IDs: 10000-10999

    # IndexIDMap: Only forward mapping (vector)
    index1 = faiss.IndexIDMap(faiss.IndexFlatL2(d))
    index1.add_with_ids(vectors, custom_ids)

    print("\nIndexIDMap data structures:")
    print(f"  - id_map (vector): {len(index1.id_map)} entries")
    print(f"    Example: internal_id=0 -> custom_id={index1.id_map[0]}")
    print(f"    Example: internal_id=5 -> custom_id={index1.id_map[5]}")
    print("  - No reverse mapping!")
    print("  - reconstruct() is NOT efficiently supported")

    # IndexIDMap2: Forward + reverse mapping
    index2 = faiss.IndexIDMap2(faiss.IndexFlatL2(d))
    index2.add_with_ids(vectors, custom_ids)

    print("\nIndexIDMap2 data structures:")
    print(f"  - id_map (vector): {len(index2.id_map)} entries")
    print(f"    Example: internal_id=0 -> custom_id={index2.id_map[0]}")
    print(f"  - rev_map (unordered_map): {index2.rev_map.size()} entries")
    print(f"    Example: custom_id=10000 -> internal_id={index2.rev_map.at(10000)}")
    print(f"    Example: custom_id=10005 -> internal_id={index2.rev_map.at(10005)}")
    print("  - reconstruct() uses O(1) hash lookup via rev_map")


def demo_reconstruct_functionality():
    """Show reconstruct() behavior difference"""
    print("\n" + "=" * 70)
    print("RECONSTRUCT FUNCTIONALITY")
    print("=" * 70)

    d = 64
    nb = 100

    vectors = np.random.random((nb, d)).astype('float32')
    custom_ids = np.array([5000, 5010, 5020, 5030, 5040] +
                          list(range(6000, 6000 + nb - 5)), dtype='int64')

    # IndexIDMap - reconstruct not efficiently implemented
    index1 = faiss.IndexIDMap(faiss.IndexFlatL2(d))
    index1.add_with_ids(vectors, custom_ids)

    print("\nIndexIDMap:")
    print("  - Does NOT override reconstruct()")
    print("  - Would fall back to base implementation (not ID-aware)")
    print("  - Cannot reconstruct by custom ID efficiently")

    # IndexIDMap2 - efficient reconstruct
    index2 = faiss.IndexIDMap2(faiss.IndexFlatL2(d))
    index2.add_with_ids(vectors, custom_ids)

    print("\nIndexIDMap2:")
    print("  - Overrides reconstruct() with rev_map lookup")

    # Reconstruct by custom ID
    reconstructed = index2.reconstruct(5020)  # Custom ID
    original_idx = 2  # This was the 3rd vector added
    print(f"  - reconstruct(custom_id=5020):")
    print(f"    1. rev_map[5020] -> internal_id={index2.rev_map.at(5020)}")
    print(f"    2. index->reconstruct({index2.rev_map.at(5020)}) -> vector")
    print(f"    3. Match: {np.allclose(reconstructed, vectors[original_idx])}")

    # Try non-existent ID
    print("\n  - reconstruct(non_existent_id=9999):")
    try:
        index2.reconstruct(9999)
        print("    Should have thrown!")
    except RuntimeError as e:
        print(f"    Throws exception: {e}")


def demo_performance_impact():
    """Show performance implications"""
    print("\n" + "=" * 70)
    print("PERFORMANCE & MEMORY IMPACT")
    print("=" * 70)

    d = 64
    nb = 100000

    vectors = np.random.random((nb, d)).astype('float32')
    custom_ids = np.arange(nb, dtype='int64') + 1000000

    # IndexIDMap
    start = time.time()
    index1 = faiss.IndexIDMap(faiss.IndexFlatL2(d))
    index1.add_with_ids(vectors, custom_ids)
    time1 = time.time() - start

    # IndexIDMap2
    start = time.time()
    index2 = faiss.IndexIDMap2(faiss.IndexFlatL2(d))
    index2.add_with_ids(vectors, custom_ids)
    time2 = time.time() - start

    print(f"\nAdding {nb:,} vectors:")
    print(f"  IndexIDMap:  {time1:.4f}s")
    print(f"  IndexIDMap2: {time2:.4f}s (builds rev_map)")
    print(f"  Overhead:    {((time2/time1 - 1) * 100):.1f}%")

    print(f"\nMemory overhead:")
    print(f"  IndexIDMap:")
    print(f"    - id_map: {nb * 8:,} bytes ({nb} × 8 bytes/int64)")
    print(f"  IndexIDMap2:")
    print(f"    - id_map:  {nb * 8:,} bytes")
    print(f"    - rev_map: ~{nb * 16:,} bytes (unordered_map overhead)")
    print(f"    - Total:   ~{nb * 24:,} bytes (~3x the forward-only map)")

    # Reconstruction performance
    if nb <= 100000:  # Only test if reasonable size
        test_ids = custom_ids[::1000][:10]

        start = time.time()
        for custom_id in test_ids:
            reconstructed = index2.reconstruct(custom_id)
        time_reconstruct = time.time() - start

        print(f"\nReconstruction (10 vectors by custom ID):")
        print(f"  IndexIDMap2: {time_reconstruct*1000:.2f}ms")
        print(f"  IndexIDMap:  Not efficiently supported")


def demo_remove_ids_behavior():
    """Show remove_ids implementation difference"""
    print("\n" + "=" * 70)
    print("REMOVE_IDS BEHAVIOR")
    print("=" * 70)

    d = 64
    nb = 100

    vectors = np.random.random((nb, d)).astype('float32')
    custom_ids = np.arange(nb, dtype='int64') + 1000

    # IndexIDMap2
    index2 = faiss.IndexIDMap2(faiss.IndexFlatL2(d))
    index2.add_with_ids(vectors, custom_ids)

    print("\nIndexIDMap2.remove_ids() implementation:")
    print("  1. Calls parent IndexIDMap.remove_ids()")
    print("  2. Rebuilds entire rev_map from scratch (construct_rev_map())")
    print("     - Comment in code: 'This is quite inefficient'")
    print("     - O(n) operation to rebuild hash map")

    selector = faiss.IDSelectorBatch([1005, 1010, 1015])
    index2.remove_ids(selector)

    print(f"\n  After removing 3 IDs:")
    print(f"    - id_map size: {len(index2.id_map)}")
    print(f"    - rev_map size: {index2.rev_map.size()}")
    print(f"    - Consistency check: ", end="")
    try:
        index2.check_consistency()
        print("PASSED")
    except:
        print("FAILED")


def demo_serialization():
    """Show serialization behavior"""
    print("\n" + "=" * 70)
    print("SERIALIZATION")
    print("=" * 70)

    d = 64
    nb = 100

    vectors = np.random.random((nb, d)).astype('float32')
    custom_ids = np.arange(nb, dtype='int64') + 2000

    index2 = faiss.IndexIDMap2(faiss.IndexFlatL2(d))
    index2.add_with_ids(vectors, custom_ids)

    print("\nWhen saving IndexIDMap2:")
    print("  - Writes fourcc code: 'IxM2'")
    print("  - Writes id_map vector")
    print("  - Does NOT write rev_map (reconstructed on load)")

    print("\nWhen loading IndexIDMap2:")
    print("  - Reads id_map vector")
    print("  - Calls construct_rev_map() to rebuild rev_map from id_map")
    print("  - This is why rev_map doesn't need to be serialized!")

    # Save and load
    faiss.write_index(index2, "/tmp/test_idmap2.faiss")
    loaded = faiss.read_index("/tmp/test_idmap2.faiss")

    print(f"\n  Original rev_map size: {index2.rev_map.size()}")
    print(f"  Loaded rev_map size:   {loaded.rev_map.size()}")
    print(f"  Match: {index2.rev_map.size() == loaded.rev_map.size()}")

    # Verify reconstruction still works
    reconstructed = loaded.reconstruct(2050)
    print(f"  Reconstruction works: {reconstructed.shape == (d,)}")


if __name__ == "__main__":
    demo_basic_difference()
    demo_reconstruct_functionality()
    demo_performance_impact()
    demo_remove_ids_behavior()
    demo_serialization()

    print("\n" + "=" * 70)
    print("SUMMARY")
    print("=" * 70)
    print("""
Key Implementation Differences:

1. DATA STRUCTURES:
   IndexIDMap:  id_map (vector only)
   IndexIDMap2: id_map (vector) + rev_map (unordered_map<idx_t, idx_t>)

2. RECONSTRUCT:
   IndexIDMap:  Not efficiently supported
   IndexIDMap2: O(1) lookup via rev_map.at(custom_id) -> internal_id

3. ADD_WITH_IDS:
   IndexIDMap:  Just append to id_map
   IndexIDMap2: Append to id_map + update rev_map hash table

4. REMOVE_IDS:
   IndexIDMap:  Remove from id_map
   IndexIDMap2: Remove from id_map + rebuild entire rev_map (inefficient!)

5. MEMORY:
   IndexIDMap:  O(n) - just vector of IDs
   IndexIDMap2: O(n) - vector + hash map (roughly 3x memory)

6. SERIALIZATION:
   IndexIDMap:  Saves id_map
   IndexIDMap2: Saves id_map, reconstructs rev_map on load

USE CASES:
- Use IndexIDMap when you only need custom IDs in search results
- Use IndexIDMap2 when you need reconstruct() by custom ID
    """)
