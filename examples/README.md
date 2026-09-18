`example.tiff` is a 9.65 MiB LZW-compressed crop of the original CT scan:
32 central slices, each 350 rows × 1,000 columns, with signed 16-bit voxels and
0.3 mm spacing. It contains original zero-based slices 1638–1669 out of 3308;
the full cross-section is preserved.

The final metadata page is retained, with the slice count changed to 32 and
the Z origin adjusted to -5937.000 mm. Other settings describe the original scan.

This sample is included in Git. Other TIFFs and generated NRRD/XML outputs are
ignored.

```sh
./build/release/mito_unpack --output-dir converted examples/example.tiff
```
