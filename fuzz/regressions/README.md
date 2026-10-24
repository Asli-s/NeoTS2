# Fuzz regressions

Inputs a fuzz target once crashed, hung or ran out of memory on, kept after the bug was fixed.
`vm_tests` ("fuzz regressions stay fixed") runs each one through its target's harness again, so the
bug stays fixed; an ASan build (`build-asan`) also checks them for memory errors.

After fixing a finding from `fuzz/findings`, minimise it and copy it here, into the folder named
after its target (the fuzz file's name without `_fuzz.cpp`), with a name that says what it does:

```
build-fuzz\RelWithDebInfo\<target>_fuzz.exe -minimize_crash=1 -runs=10000 fuzz\findings\<target>-crash-...
```
