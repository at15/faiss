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