# ASH0-tools
ASH0-tools are simple command line tools for compressing and decompressing Nintendo's ASH format found on the Wii, written in C.

## Compressor
To use the compressor, the syntax is as follows:
```shell
ashcomp <infile> [optional arguments]
```
This will compress a single file into an ASH file. If no output name is specified, this will default to naming the file `<input file>.ash`.

Generally, optional arguments aren't necessary to compress a file. One exemption to this is when re-compressing ASH files for My Pokémon Ranch, which will not work if compressed with the default options. You'll need to specify the argument `-d 15` to set the distance tree leaf size to 15.

The full list of arguments can be found below:
```shell
-o <file path> Specify output file path
-d <int> Specify distance tree bits  (default: 11)
-l <int> Specify length tree bits    (default:  9)
```

## Decompressor
To use the decompressor, the syntax is as follows:
```shell
ashdec <infile> [optional arguments]
```
This will decompress an ASH file to its single contained file. If no output name is specified, this will default to naming the file `<input file>.arc`.

Generally, optional arguments aren't necessary to decompress a file. One important exemption is ASH files found inside My Pokémon Ranch, which will fail to decompress with the default options. To make these work, you'll need to use the argument `-d 15` to set the distance tree leaf size to 15.

The full list of arguments can be found below:
```shell
-o <file path> Specify output file path
-d <int> Specify distance tree bits  (default: 11)
-l <int> Specify length tree bits    (default:  9)
```

### Credits
All credit to the base code used for compression/decompression goes to [@Garhoogin](https://github.com/Garhoogin), who put a lot of time into figuring out the compression algorithm used by ASH files to create modern and much more cleanly written tools for them.
