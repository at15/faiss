# S3 Read Only Format

## Background

For the serialization format, since it is read only and loads from S3 to memory.
We can combine `ArrayInvertedLists` and `OnDiskInvertedLists`.

- We don't create new vectors like `ArrayInvertedLists` does on read, we read pointers directly to data downloaded (and cached) in memory from S3.
- We don't need all the slots etc. from `OnDiskInvertedLists` since there is no update.

What metadata we need to know:

- offset of each cluster
  - It can be calculated when we have the sizes array.

Ideally we want to use the exact same format as `ArrayInvertedLists`.

For write we only need to know where does the inverted list storage start.
We don't know the size of centriods etc. unless we change `write_index`
BUT we can infer base on `total size written - inverted list size`.

For read we need to fool the reader ... oh we don't need to
There is a IO flag `IO_FLAG_SKIP_IVF_DATA` to only load the sizes array.

https://github.com/facebookresearch/faiss/blob/3af3e00103079554b1071d2dcea7ccedc9693a44/faiss/impl/index_read.cpp#L352

```cpp
InvertedLists* read_InvertedLists(IOReader* f, int io_flags) {
    uint32_t h;
    READ1(h);
    if (h == fourcc("il00")) {
        fprintf(stderr,
                "read_InvertedLists:"
                " WARN! inverted lists not stored with IVF object\n");
        return nullptr;
    } else if (h == fourcc("ilar") && !(io_flags & IO_FLAG_SKIP_IVF_DATA)) {
        auto ails = new ArrayInvertedLists(0, 0);
        ...
    } else if (h == fourcc("ilar") && (io_flags & IO_FLAG_SKIP_IVF_DATA)) {
        ...
        return InvertedListsIOHook::lookup(h2)->read_ArrayInvertedLists(
                f, io_flags, nlist, code_size, sizes);
    } else {
        return InvertedListsIOHook::lookup(h)->read(f, io_flags);
    }
}
```

Seems we can do it base on [index-read-hook-mmap.claude.md](index-read-hook-mmap.claude.md)
We can also use a dummy implementation and then do `replace_invlists`.
We can also append other data such as original text, full text etc. to end of the end file and they won't be loaded by faiss ...

## Design

When we write the index, we keep track of the the folling information:

- total file size
- offset where the inverted list data starts
- offset of each cluster

Without modifying existing `write_index` implementation what we can do is:

- Replace the default `ArrayInvertedLists` with another `ArrayInvertedLists` because I don't know if we can get the inverted lists directly using some methods on index. We need a reference to it to get size of each cluster (withotu reading the file ...)
- Calculate the offset of inverted list by using `total file size - total inverted list size`, which is similar to `write_InvertedLists` implementation, except we only add up the size without writing anything
- We can provide a custom `IOWriter` implementation because we will likely switch to writing to S3 directly instead of having a intermediate local file.
  - We can start with local file for now since we didn't even import the S3 SDK yet.

Then we save the offset information to another file (for now).
For simplicity, we can use JSON via `nlohmann::json`.
The metadata looks like this:

```json
{
    "total_size": 52052139,
    "inverted_lists_offset": 51307,
    "n_clusters": 100,
    "code_size": 512,
    "sizes_array_offset": 51331,
    "sizes_array_count": 100,
    "sizes_array_format": "full",
    "cluster_data_offset": 52139
}
```

## Implementation

### IOWriter

- We don't store `cluster_sizes` array in metadata since it's already in the index file
- The `sizes_array_offset` points to where the sizes array starts in the file
- The `sizes_array_format` can be "full" or "sparse" (see `write_InvertedLists` in `index_write.cpp`)
  - "full": sizes array contains one entry per cluster (nlist entries)
  - "sparse": sizes array contains pairs of (cluster_id, size) for non-empty clusters only
- To calculate individual cluster offsets, read the sizes array from the file and calculate sequentially
- The `cluster_data_offset` is where the actual cluster data (codes + ids) starts

### Reader

Use the IO hook similar to `OnDiskInvertedLists`, as described in [index-read-hook-mmap.claude.md](index-read-hook-mmap.claude.md).

We create two new readonly InvertedLists implementations:

- `S3ReadNothingInvertedLists` is the one registered for the IO hook because we don't read anything during `read_index`, we just register it avoid triggering full read using the default `ArrayInvertedLists` logic.
  - We have this placeholder because registering IOHook is global, and we need different metadata for different index files.
- `S3ReadOnlyInvertedLists` is the actual on demand loader. We use `index.replace_invlists` to replace our placeholder `S3ReadNothingInvertedLists` with the actual `S3ReadOnlyInvertedLists` after `read_index` is done.
- NOTE: `S3BuildOnlyInvertedLists` is not needed anymore because we use `ArrayInvertedLists` when building index, we keep it there because it has good comment on what each method is doing.

For `S3ReadOnlyInvertedLists`:

- The cluster data from the IVF index file has number of vectors in each cluster, but does not have the offset of cluster in file.
  - We solve it by saving the first cluster's offset in `a.ivf.meta.json`, which we keep track when writing the index file `a.ivf` and saved as json.
    - We can cacluate the offset by adding previous cluster's size to the first cluster's offset.
  - We read `a.ivf.meta.json` from local disk right now for simplicity, later we should upload this file to S3 and read the metadata from S3.
- Throw errors on `resize`, `update_entries`, `add_entries` etc. because we don't support any write operations.
- When cluster data is needed, e.g. `get_codes` or `get_ids`, we download the cluster data from S3 on demand using range request and cache it in memory.
  - No need to LRU for now, simply cache for ever until object destruction
  - Try zero copy if possible, the format in file should be able to directly map to memory, which is why `OnDiskInvertedLists` can do mmap on `ArrayInvertedLists`

The read process is roughly in following pseduo code:

```text
# register the hook
register_io_hook("S3ReadNothingInvertedLists")

metadata = read_metadata_from_local_json()
# be aware http range request is inclusive on BOTH sides
index_without_inverted_lists = s3.get("a.ivf", range(0, size_before_inverted_lists_data_start))
index = read_index(index_without_inverted_lists)

s3_readonly_inverted_lists = S3ReadOnlyInvertedLists(s3, metadata)
index.replace_invlists(s3_readonly_inverted_lists)

index.search(query_vectors, k)
```

You can look at existing cpp files for reference

- [rocksdb_ivf](../../rocksdb_ivf/demo_rocksdb_ivf.cpp)
- [test_s3](../test_s3.cpp)