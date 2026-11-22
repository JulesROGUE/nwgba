LZMA-JS 2.3.2 (https://github.com/LZMA-JS/LZMA-JS), by Nathan Rugg, MIT licence (see LICENSE):
the compressor only (src/lzma-c.js). One change, marked "gba2nw" in the file: a compression
mode can be given whole, as {s: log2 of the dictionary, f: fast bytes, m: match finder}.
Used by lib/pack.js to pack groups of ROM harder than deflate does.
