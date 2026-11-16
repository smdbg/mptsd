# Source provenance

This is a fork of [gfto/mptsd](https://github.com/gfto/mptsd), originally by
Georgi Chorbadzhiyski. The original Git history and copyright notices are retained.
The integration is based on upstream revision `a2b6e4d057e1f68c6a1f9d1b9c80732fa4ef9198`.

Standalone packaging on 2026-11-16 imports the native changes developed in
https://git.promet.ad/mitev/tsdecrypt at revision `525d6cca6a23db65213ad6929ca664059de74a11`.
See [CHANGELOG.md](../CHANGELOG.md) for the changes. Web management, deployment
configuration and real customer/card data are not part of this repository.

The original submodules are included as ordinary source directories, so GitHub
source archives and regular clones build without submodule downloads:

| Component | Original repository | Pinned revision | License |
| --- | --- | --- | --- |
| libfuncs | https://github.com/gfto/libfuncs | `48f7087fe79b3ff54c6c31e07dc18e0fdf5bf0f5` | MIT |
| libtsfuncs | https://github.com/gfto/libtsfuncs | `8c8a28d1b013a61051fd970b90aa81c533bb37db` | MIT |

The main program is distributed under GPL v2; see [COPYING](../COPYING).
Each bundled library retains its own `LICENSE-MIT.txt` and original notices.

Local library changes are in libfuncs `cbuf.c/h` and libtsfuncs `crc.c`,
`nit_desc.c`, `sdt_desc.c`, `time.c` and `tsfuncs.h`; they support atomic
PSI insertion, table metadata and the documented correctness fixes.
