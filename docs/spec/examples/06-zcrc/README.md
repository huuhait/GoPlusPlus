# 06 zcrc

Computes a CRC-32 of `input.txt` with zlib, whole and in chunks. Run: `go run main.go` (the Go version uses `hash/crc32`, the same IEEE CRC-32).

`gopp.mod` for the Go++ version (hashes elided; see the plan overview):

```
module example.com/zcrc

gopp 0.1

cdep github.com/madler/zlib v1.3.1 (
    commit   <full commit hash>
    mode     static
    provides ZLIB
)
```

## Spec rules exercised

- [FOR-1, FOR-2, FOR-3](../../14-foreign-surface.md): `import "github.com/madler/zlib/zlib.h"` is a header import; the package is `zlib`; names keep their C spelling (`zlib.crc32`).
- [FOR-6](../../14-foreign-surface.md): no conversions. C types are plain Go++ types (`uLong` is `uint64`, `uInt` is `uint32`). A `[]byte` passes to a C byte pointer parameter (`const Bytef*` or `Bytef*`) as its data pointer (P07 D7.7).
- [OWN-S2](../../04-ownership.md#44-slicing-and-shared-buffers-d16-adr-0032): `&data[i:j]` is an $O(1)$ view of part of `data`, a `*[]byte` that copies no bytes (plain `data[i:j]` is a new slice with a copy of them). `len(chunk)` needs no star; `*chunk` is the whole slice value passed to `crc32`.
- [OWN-R2](../../04-ownership.md): the `[]byte("123456789")` temporary lives to the end of the statement, so the pointer stays valid during the call.
- [ERR-6](../../09-errors-and-panics.md): `os.ReadFile!` panics in `main` on error.

## Differences from the Go version

- Go uses `crc32.ChecksumIEEE` and `crc32.Update`; Go++ calls zlib's C `crc32` directly.
- Output is identical (both are IEEE CRC-32).
- Not checked here: the exact Go++ type of `zlib.crc32`'s result (written `uint64` as in P07 D7.4: `uLong` on Linux).
