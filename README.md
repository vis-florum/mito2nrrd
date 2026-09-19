# mito2nrrd

Convert Microtec reconstructed CT TIFFs to raw NRRD volumes and
`_ScanSettings.xml` sidecars. Supports slice pages and slices stacked into one
tall image, preserving voxel type and the original Python converter's axis order.

## Build

Requires C++17, CMake ≥ 3.16, OpenMP, libtiff, libxml2, and zlib.
On Debian/Ubuntu:

```sh
sudo apt install build-essential cmake libtiff-dev libxml2-dev zlib1g-dev
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
```

For dependencies in a custom prefix, add `-DCMAKE_PREFIX_PATH=/path/to/prefix`
to the configure command.

## Use

```sh
./build/release/mito_unpack scan.tiff
./build/release/mito_unpack --output-dir converted /path/to/*.tiff
./build/release/mito_unpack --workers 6 --compress /path/to/*.tiff
./build/release/mito_unpack --help
```

Defaults: **6 concurrent files**, **1 decode thread per file**, **raw NRRD**.
Outputs go alongside each input unless `--output-dir` is set. Existing outputs
require `--force` to replace.

| Option | Meaning |
| --- | --- |
| `--workers N` | Concurrent files, capped by input count (default 6) |
| `--threads N` | Decode threads per file (default 1) |
| `--compress` | Gzip output, level 6 |
| `--compression-level N` | Gzip level 0–9 |
| `--batch-mb N` | Output buffer MiB per file (default 64; at least one slice) |
| `--output-dir DIR` | Destination directory |
| `--force` | Replace existing outputs |

The last TIFF page must contain scanning XML. Tall images need
`PageNumber=(slice rows, slice count)`; `(0,1)` denotes one slice. Supported
metadata roots are `CT` and `logInfo`. Metadata spaces become underscores and
square brackets are removed, following the legacy convention. NRRD axes are
`(rows, columns, slices)`, with rows varying fastest. Spacings follow the original
converter's convention; origins remain in the XML sidecar. RGB images and
sinograms are unsupported.

## License

Copyright © 2026 Johannes Huber. Licensed under the
[PolyForm Noncommercial License 1.0.0](LICENSE.md).

Personal and noncommercial research use is free of charge; the license also
covers educational institutions and public research organizations. Retain the
required notice and credit `mito2nrrd` and Johannes Huber in publications,
documentation, or acknowledgements; GitHub citation metadata is provided in
`CITATION.cff`.

Commercial use, including incorporation into commercial software, requires a
separate paid license. Contact [johannes.huber@ltu.se](mailto:johannes.huber@ltu.se).
This is a source-available license, rather than an OSI-approved open-source license.
