# Third-party notices

Fast Sigma Raw includes modified portions of X3F Tools under its BSD-style
license. The complete notice is distributed in `vendor/x3f-tools/LICENSE` and
in every binary release.

The lossless-JPEG DNG encoder is adapted from
[x3fuse-core](https://github.com/sagwaco/x3fuse-core), copyright 2026 Sang
Lee, under the Apache License, Version 2.0. The implementation was modified to
accept strided image buffers and avoid retaining a full-frame difference
buffer. The complete license is distributed in
`packaging/licenses/X3FUSE_CORE_APACHE_LICENSE.txt` and in every binary
release.

Binary releases link or bundle LibTIFF and, depending on the platform build,
its compression/image dependencies: zlib, libjpeg-turbo, zstd, XZ/liblzma,
libwebp, and libdeflate. LibTIFF's complete notice is distributed in
`packaging/licenses/LIBTIFF_LICENSE.md` and in every binary release. These
libraries are available under their respective permissive or weak-copyleft
licenses from their upstream projects.

Windows archives also carry the GCC and winpthreads runtime DLLs supplied by
MSYS2. They are redistributed under the GCC Runtime Library Exception and the
applicable MinGW-w64 permissive licenses.

The Rust dependency graph is recorded in `Cargo.lock`. Crate names, versions,
sources, and declared licenses can be inspected with:

```sh
cargo metadata --locked --format-version 1
```

The graphical interface uses eframe/egui (MIT OR Apache-2.0) and rfd (MIT).
