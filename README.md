# Usage
In the root folder, create a folder named `datasets` and place all the datasets you want to compress and decompress there.

Adjust launch parameters in `lzss.conf`:

```ini
dataset_dir=datasets
entropy_codec=ac
distance_coding=class
window_size=65536
min_match_length=4
max_match_length=258
parse_mode=lazy
hash_mode=hash4
block_size=8388608
max_workers=8
```

Use `entropy_codec=tans` for the static tANS backend or `entropy_codec=ac`
for the Order-0 adaptive arithmetic backend.
For the adaptive arithmetic backend, use `distance_coding=class` for the
Deflate-style distance classes or `distance_coding=bit_tree` for the explicit
distance bit tree.

Use `parse_mode=optimal` to build tANS tables from the bounded lazy pass and
then encode a second exhaustive optimal parse with those frozen cost tables.

Compile the project using `make` and run the `lzss` executable (`lzss.exe` for Windows).
