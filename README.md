# ASH0-tools
ASH0-tools are simple command line tools for compressing and decompressing Nintendo's ASH0 format found on the Wii, written in C.

## Decompressor
To use the decompressor, the syntax is as follows:
```shell
ashdec <infile> [optional arguments]
```
Generally, optional arguments aren't necessary to decompress a file. One important exemption is ASH0 files found inside My Pokémon Ranch, which will fail to decompress with the default options. To make these work, you'll need to use the argument `-d 15` to set the distance tree bits to 15.

The full list of arguments can be found below:
```shell
-o <file path> Specify output file path
-d <int> Specify distance tree bits  (default: 11)
-l <int> Specify length tree bits    (default:  9)
```

### Credits
All credit to the base code used for compression/decompression goes to [@Garhoogin](https://github.com/Garhoogin), who put a lot of time into figuring out the compression algorithm used by ASH0 files to create modern and much more cleanly written tools for them.
