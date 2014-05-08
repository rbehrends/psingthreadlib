SING=$1
shift
g++ -I$SING -I$SING/factory/include -I$SING/libpolys -O2 -c zmq.cc -DOM_NDEBUG -DNDEBUG -fPIC -DPIC -o zmq.o && \
libtool -dynamic -twolevel_namespace -weak_reference_mismatches weak -undefined dynamic_lookup -o zmq_core.so zmq.o -lzmq "$@"
