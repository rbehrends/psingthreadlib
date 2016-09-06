# Building the library

Run:

        ./configure && make && make install

Use:

        ./configure --singular=/path/to/singular

instead if you want to use this with a different Singular installation.

The "make install" command will install `systhreads.so` and `systhreads.lib`
in the MOD and LIB directories.
