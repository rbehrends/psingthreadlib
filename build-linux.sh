SING=$1
shift
g++ -Iinclude -I$SING -I$SING/factory/include -I$SING/libpolys -O2 -c zmq.cc -DOM_NDEBUG -DNDEBUG -fPIC -DPIC -o zmq.o && \
g++ -shared -o zmq_core.so zmq.o -Llib -lzmq "$@"
