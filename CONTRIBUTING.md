# Contributing

Issues and pull requests are welcome. Please keep reverse-engineering notes
separate from implementation changes and do not submit SIGMA binaries,
proprietary code, or copyrighted sample photographs to the repository.

Before opening a pull request, run:

```sh
cargo fmt --check
cargo clippy --all-targets -- -D warnings
cargo test --all-targets --locked
```

Changes to the Merrill image pipeline should include the camera model, ISO,
the affected stage, and repeatable numerical or crop-based validation. Small
X3F fixtures cannot currently be stored in the repository, so describe how a
maintainer can reproduce tests with independently obtained sample files.

The project code is MIT-licensed. Modified X3F Tools files remain under the
BSD-style terms in `vendor/x3f-tools/LICENSE`.
