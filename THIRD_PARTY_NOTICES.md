# Third-Party Notices

MiniKV uses external open-source libraries. RPC transport, serialization, and
the storage engine are provided by the projects below, not implemented by
MiniKV. MiniKV's application source, tests, and scripts were written for this
project; no upstream example source files have been vendored into `src/`.

The Apache-2.0 license at the repository root applies to MiniKV's own work.
It does not replace any third-party license or copyright notice.

## Direct Dependencies

| Component | Verified version | License / selected option | Role |
| --- | --- | --- | --- |
| [Apache bRPC](https://github.com/apache/brpc) | 1.18.0, commit `94f1bbd32845a45f0218dfe43e25791252bdcb72` | Apache-2.0; bundled components have additional notices in its LICENSE | RPC transport and dispatch |
| [RocksDB](https://github.com/facebook/rocksdb) | Ubuntu `8.9.1-2` | Dual GPL-2.0 / Apache-2.0; MiniKV uses the Apache-2.0 option for the library | Persistent key-value storage |
| [Protocol Buffers](https://github.com/protocolbuffers/protobuf) | Ubuntu `3.21.12-8.2ubuntu0.3` | BSD-3-Clause for this version | Messages, generated bindings, runtime |
| [gflags](https://github.com/gflags/gflags) | Ubuntu `2.2.2-2build1` | BSD-3-Clause | Command-line flags |

bRPC is built unmodified from the pinned upstream commit by
`scripts/bootstrap.sh`. Other libraries are installed from Ubuntu packages.
Protobuf bindings are generated during the build and are not checked in.
License terms can change across dependency versions; these records describe
the versions above, not every past or future release.

## Linked Transitive Libraries

The following libraries appeared in `ldd minikv_server` on the verified Ubuntu
24.04 build. The list records that build, not a universal dependency closure.

| Component | Ubuntu package version | Main library license / option |
| --- | --- | --- |
| [LevelDB](https://github.com/google/leveldb) | `1.23-5build1` | BSD-3-Clause |
| [Snappy](https://github.com/google/snappy) | `1.1.10-1build1` | BSD-3-Clause |
| [OpenSSL](https://github.com/openssl/openssl) | `3.0.13-0ubuntu3.15` | Apache-2.0 |
| [zlib](https://zlib.net/) | `1:1.3.dfsg-3.1ubuntu2.2` | Zlib |
| [LZ4](https://github.com/lz4/lz4) | `1.9.4-1build1.1` | BSD-2-Clause for the library |
| [Zstandard](https://github.com/facebook/zstd) | `1.5.5+dfsg2-2build1.1` | Dual BSD-3-Clause / GPL-2.0; BSD-3-Clause option |
| [bzip2](https://sourceware.org/bzip2/) | `1.0.8-5.1ubuntu0.1` | bzip2-1.0.6 license (BSD-style variant) |
| [GCC runtime](https://gcc.gnu.org/) (`libstdc++6`, `libgcc-s1`) | `14.2.0-4ubuntu2~24.04.1` | GPL-3.0 with GCC Runtime Library Exception for applicable runtime files; see package notices |
| [glibc](https://www.gnu.org/software/libc/) (`libc6`, `libm`, loader) | `2.39-0ubuntu8.9` | LGPL-2.1-or-later for the main library; file-specific terms also apply |

## Preserved License Records

[`third_party/licenses/`](third_party/licenses/) contains verbatim records:

- `brpc-LICENSE` and `brpc-NOTICE` from the pinned bRPC source, including its
  bundled-component attributions. They have not been replaced by a short label.
- `<package>.copyright` copied from `/usr/share/doc/<package>/copyright` for
  each listed Ubuntu dependency, preserving copyright holders and file-specific
  notices, including RocksDB's LevelDB attribution.
- `Apache-2.0`, `GPL-2`, `GPL-3`, and `LGPL-2.1` contain the common license texts
  referenced by package records.

Package copyright records also describe upstream tools, tests and Debian
packaging that MiniKV does not ship. For example, the GPL terms on RocksDB's
Debian packaging do not describe the selected Apache-2.0 library option, and
LZ4 command-line tools have different terms from its library. Consult each
record's file patterns rather than applying one label to the entire package.

## Distribution Scope

This repository distributes MiniKV source and attribution records, not
third-party source archives, shared libraries, generated binaries, or an OS
image. Dependencies are fetched or installed at build time.

A future binary, container, static-link or bundled-dependency release needs
its own inventory and license review against the actual shipped artifacts,
including applicable notices, license texts, source or relinking obligations.
These source-release records are not a blanket compliance certification for
such distributions.
