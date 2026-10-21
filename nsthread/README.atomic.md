# Atomic integer regression tests

The public atomic types and accessors are declared in `include/nsthread.h`;
`nsthread/atomic.c` implements them in libnsthread. Callers must initialize an
object before sharing it and destroy it only after all users have finished.
Relaxed operations do not publish unrelated memory; use the acquire/release
accessors when an atomic flag protects other state.

After configuring NaviServer, run from its source root:

```sh
make test-atomic
```

`make test` also runs this target. The rules are part of `nsthread/Makefile`;
`make -C nsthread check` runs them directly.

`atomictest` links the configured implementation from libnsthread and includes
only the public header. `atomic-fallback-test` runs the same checks against a
private build of atomic.c with `NS_ATOMIC_MUTEX_FALLBACK` defined. Its accessor
symbols are renamed to prevent interposition on the library's native functions.
Neither test changes the configured production backend.

The checks cover every accessor, returned previous values, high bits and unsigned
addition wraparound, destruction/reinitialization, concurrent 32/64-bit counters
and acquire-release publication of ordinary payload fields. The forced fallback
also checks that both widths actually use initialized mutexes. Publication waits
have a deadline to turn broken synchronization into a test failure.

Clean the standalone test executables and objects with:

```sh
make -C nsthread clean-atomic
```

The C test sources use the NaviServer thread API and also support MSVC's
interlocked backend. The integrated test targets use GNU make and Unix shell
syntax; normal `make clean` also removes the test executables and objects. The normal Unix, nmake and Visual Studio library builds compile atomic.c
as part of libnsthread.

`NS_ATOMIC_MUTEX_FALLBACK` affects the public structure layout. If used outside
these private tests, rebuild the library and every caller with the same setting;
do not apply it only to a module using an existing native library.
