# Building the Singular bindings for ZeroMQ

Build the library with:

    ./configure --singular=$SINGULAR_PATH --zmq=$ZEROMQ_PATH
    make

Here, `$SINGULAR_PATH` denotes the path to the root directory of the
Singular installation (which is how the build process finds include
files). Similarly, `$ZEROMQ_PATH` denotes the path to the ZeroMQ
installation (which must have `include` and `lib` subdirectories). The
`--zmq` option can be omitted if ZeroMQ can be found on the default
include and library paths.

Copy `zmq.lib` and `zmq_core.so` to a location in your Singular path.