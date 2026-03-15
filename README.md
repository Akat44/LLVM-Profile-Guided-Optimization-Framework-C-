# CS 5544 Assignment 3

By Alexander Novotny (anovotny) & Arya Katoch (arya4)

## Building
The provided makefile will build the llvm plugin into the `build/` directory by default. By running `make tests`, the plugin will be compiled and automatically run on every test. Additionally, you can run `make diff`, and a diff of the pre-optimized and post-optimized statistics reported by `lli` will be displayed.