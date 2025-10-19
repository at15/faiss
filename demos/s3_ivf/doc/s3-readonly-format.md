# S3 Read Only Format

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