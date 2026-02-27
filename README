# HW6 - TDMM Memory Allocator

Name: Shrey Parekh
EID: sp52584

## what i implemented: 
- `t_init(alloc_strat_e strat)`
- `t_malloc(size_t size)`
- `t_free(void *ptr)`

the allocator supports:
- 4-byte alignment
- `FIRST_FIT`, `BEST_FIT`, `WORST_FIT`
- block splitting
- coalescing adjacent free blocks
- `mmap`-backed heap growth

## Files
- `libtdmm/tdmm.c`: allocator implementation
- `libtdmm/tdmm.h`: allocator API + stats getters used by tests
- `main.c`: test driver for policy comparison
- `REPORT.md`: report writeup/results

## Build
If `cmake` is installed:
```bash
bash build.sh
```

If `cmake` is not installed, compile directly:
```bash
cc -std=c99 -Ilibtdmm main.c libtdmm/tdmm.c -o hw6
```

## Run
```bash
./hw6
```

The program prints results for `FIRST_FIT`, `BEST_FIT`, and `WORST_FIT`.
