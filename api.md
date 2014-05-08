# libzmqSingular

This library provides a simple interface to the ZeroMQ library.

# Usage

To use the library from Singular, use:

    LIB "zmq.lib";

This will load both the C++ library and the Singular code that builds on
top of the library.

# Socket creation and destruction

ZeroMQ sockets are represented as links. It is recommended that one uses
the `zmq_xxx_socket()` constructors instead of a string, e.g.:

    link zout = zmq_push_socket("tcp://127.0.0.1:7777");

Each of the socket constructors takes a ZeroMQ URI as an argument and
returns a socket bound to the URI. You can also prefix the URI with a
plus sign to connect to an address rather than binding to it, e.g.:

    link zin = zmq_pull_socket("+tcp://127.0.0.1:7777");

The following socket constructors are supported:

    zmq_push_socket(); zmq_pull_socket(); zmq_req_socket();
    zmq_rep_socket(); zmq_dealer_socket(); zmq_router_socket();
    zmq_pub_socket(); zmq_sub_socket();

Please consult the ZeroMQ documentation for the differences between
socket types and for permissible URIs. The most general socket types are
push and pull sockets (for unidirectional pipes) and dealer sockets (for
bidirectional pipes).

To close a socket, use the `close()` function.

# Sending and receiving data

The usual `write()` and `read()` functions can be used to send data to
and receive data from ZeroMQ sockets. Here, `write()` takes one or more
strings or lists of strings and sends them across the connection. At the
other end, each call to `read()` will return either a list or a string.

# Testing the status of a socket

To wait for a socket to be ready to receive data, one can use the
`zmq_poll()` function. This function takes one or more socket and string
parameters; the strings define whether the code is waiting for them to
become available for reading(`"r"`) or writing(`"w"`). For example,
consider the call:

    i = zmq_poll("r", zin1, zin2, "w", zout1, zout2, "rw", zmain);

This call waits until `zin1` or `zin2` can be read from, `zout1` or
`zout2` can be written to, or `zmain` can be either read or written. The
function returns an index corresponding to the position of the available
socket in the argument list, starting at 0 and ignoring string arguments
(in the above example, 0 for `zin1`, 1 for `zin2`, 2 for `zout1`, 3 for
`zout2`, and 4 for `zmain`).

If one simply wishes to know whether a socket can be read or written
without blocking on the query, the `status()` function can be used as
always.

