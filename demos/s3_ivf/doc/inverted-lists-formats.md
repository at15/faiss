# InvertedLists Formats

A short version base on [inverted-lists-formats.claude.md](inverted-lists-formats.claude.md) and [io-macros.calude.md](io-macros.calude.md)

- If you write a custom format, need to register for `write_index` to work.
- Use macros to read/write data to get execption with line number

## ArrayInvertedLists

The default format. The disk format from `write_InvertedLists` is:

- header, `ilar`, number of clusters, code size (size of single encoded vector)
- the full/sparse is NOT for vector, only for list (cluster).
  - I think it is only useful for large number of clusters where more than 50% are empty, e.g. if you have 10k clusters and only 1 is not empty, then you wasted well ... 80kb not really much
- write vector and id in ~~row format~~ columnar format
  - all the vectors are written together
  - then write all the ids

https://github.com/facebookresearch/faiss/blob/3af3e00103079554b1071d2dcea7ccedc9693a44/faiss/impl/index_write.cpp#L284-L290

```cpp
// make a single contiguous data buffer (useful for mmapping)
// For each cluster
for (size_t i = 0; i < ails->nlist; i++) {
    size_t n = ails->ids[i].size();
    // Only write non empty clusters
    if (n > 0) {
        // Write all vectors
        WRITEANDCHECK(ails->codes[i].data(), n * ails->code_size);
        // Write all ids
        WRITEANDCHECK(ails->ids[i].data(), n);
    }
}
```

## OnDiskInvertedLists

Mmapped, returns the lists directly

https://github.com/facebookresearch/faiss/blob/f72e759fe3d7b937215a328a5b8df58c83d450d1/faiss/invlists/OnDiskInvertedLists.cpp#L385-L400

```cpp
const uint8_t* OnDiskInvertedLists::get_codes(size_t list_no) const {
    if (lists[list_no].offset == INVALID_OFFSET) {
        return nullptr;
    }

    return ptr + lists[list_no].offset;
}

const idx_t* OnDiskInvertedLists::get_ids(size_t list_no) const {
    if (lists[list_no].offset == INVALID_OFFSET) {
        return nullptr;
    }

    return (const idx_t*)(ptr + lists[list_no].offset +
                          code_size * lists[list_no].capacity);
}
```

## io macros

Writes `std::vector`

- `WRITEVECTOR` first writes the size of the vector, then writes the data.
- `WRITEANDCHECK` writes the data using the overloaded operator on `IOWriter`
  - It get size of the element using `sizeof(*(ptr))` and write n elements

```cpp
#define WRITEVECTOR(vec)                   \
    {                                      \
        size_t size = (vec).size();        \
        WRITEANDCHECK(&size, 1);           \
        WRITEANDCHECK((vec).data(), size); \
    }
```

```cpp
#define WRITEANDCHECK(ptr, n)                         \
    {                                                 \
        size_t ret = (*f)(ptr, sizeof(*(ptr)), n);    \
        FAISS_THROW_IF_NOT_FMT(                       \
                ret == (n),                           \
                "write error in %s: %zd != %zd (%s)", \
                f->name.c_str(),                      \
                ret,                                  \
                size_t(n),                            \
                strerror(errno));                     \
    }
```