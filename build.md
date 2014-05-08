# Building on Linux

Call the build script `build-linux.sh` with:

    sh build-linux.sh $SINGULAR_PATH $LINKER_OPTIONS

Here, `$SINGULAR_PATH` denotes the path to the root directory of
the Singular installation (which is how the build process finds
include files). If you don't have ZeroMQ installed in a default
location, you will also have to add `-L<dir>` and `-Wl,-rpath=<dir>`
in lieu of `$LINKER_OPTIONS` above, where `<dir>` is the directory
in which the library resides.

If the build process can't find the `zmq.h` include file, create
a link from the include directory where it is to a `include` directory
in the `libzmqsingular` directory.

# Building on OS X

The library is built in the same fashion on OS X, except that the
script is called `build-darwin.sh` and that OS X does not have the
`-WL,-rpath=<dir>` linker option.
