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

## Citation

**Citation is necessary for research use.** If `mito2nrrd` contributes to your
work, cite or credit it in the resulting publication, presentation, software,
or documentation:

> Johannes A. J. Huber. *mito2nrrd: a Microtec CT TIFF to NRRD converter.*
> [ORCID 0000-0001-9196-0370](https://orcid.org/0000-0001-9196-0370),
> [github.com/vis-florum/mito2nrrd](https://github.com/vis-florum/mito2nrrd).

GitHub-compatible citation metadata is provided in [`CITATION.cff`](CITATION.cff).

## License

Copyright © 2026 Johannes A. J. Huber. Licensed under the
[Apache License 2.0](LICENSE). Commercial and noncommercial use, modification,
and distribution are permitted under its terms. Preserve the license, copyright,
and `NOTICE` attribution where required. The citation request above expresses
the academic attribution expected for research use and does not add a restriction
to Apache-2.0.
