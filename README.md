# Usage
In the root folder, create a folder named `datasets` and place all the datasets you want to compress and decompress there.

Adjust launch parameters in `lzss.conf`:

```ini
dataset_dir=datasets
window_size=65536
min_match_length=4
max_match_length=258
parse_mode=lazy
hash_mode=hash4
block_size=8388608
max_workers=8
```

Use `parse_mode=optimal` to build tANS tables from the bounded lazy pass and
then encode a second exhaustive optimal parse with those frozen tables.

Compile the project using `make` and run the `lzss` executable (`lzss.exe` for Windows).
