# Slot map

A header-only, C++20 slot map implementation with a hierarchical free list
based [on this post](https://jakubtomsu.github.io/posts/bit_pools/).

## What is a slot map?

A slot map is a data structure that provides stable and versioned keys to stored values. Inserting into the map creates
and return a unique key, which stays valid until the slot is explicitly freed.

- O(1) insert, lookup, and erase
- Stable keys and references
- Automatic detection of use-after-free via version fields
- Cache-friendly

My use case for this specialized container was a RHI resource manager.

## Features

- Single header
- No external dependencies
- Thread-safe variant with lock-free synchronization on hot paths
- Static and dynamic storages

## Usage

```cpp
#include "spore/slot_map.hpp"

using namespace spore;

// create a single thread slot map with a capacity of 1024
slot_map_st<slot_key, uint32_t, 1024> map;

// insert a value
slot_key key = map.emplace(42);

// retrieve the value
auto value = map.at(key);

// erase the value
map.erase(key);

// iterate on values
for (const auto [key, value] : map) { }

// use typed key to prevent errors
struct my_key : slot_key { using slot_key::slot_key; };
slot_map_st<my_key, uint32_t, 1024> map;
```

## Benchmarks

### Test

The benchmark test does reads, writes, iterations and removal in succession, with a fixed number of thread, a fixed
value size, a fixed number of iterations, a random number of actions with a fixed seed and a read multiplier.

```cpp
struct bench_config
{
    size_t parallelism = 0;
    size_t iteration = 0;
    size_t action_min = 0;
    size_t action_max = 0;
    size_t read_num = 0;
    size_t seed = 0;
};
```

### Hardware

The tests were done with this hardware:

- Intel(R) Core(TM) i9-12900K, 3200 Mhz, 16 Core(s), 24 Logical Processor(s)
- 32 GB RAM
- Windows 11 Version 10.0.26100

The executable was built with these settings:

- Release mode
- Clang-cl compiler
- Maximum optimization and inlining settings ([exact compile options here](benchmarks/CMakeLists.txt))

### Single Thread

The tests were done with these configurations.

```cpp
bench_config configs[] {
    {
        .parallelism = 1,
        .iteration = 10,
        .action_min = 100'000,
        .action_max = 500'000,
        .read_num = 10,
    },
}
```

|             name | get (ms) | set (ms) | reset (ms) | iteration (ms) | total (ms) |
|-----------------:|---------:|---------:|-----------:|---------------:|-----------:|
|  static slot map |  20.1460 |  45.1114 |     7.6144 |         3.1787 |    76.0505 |
| dynamic slot map |  32.9478 |  61.7436 |     9.8881 |         5.3814 |   109.9609 |
|      plf::colony |  26.1153 |  90.7187 |    39.8732 |         5.7632 |   162.4704 |

### Multi-Thread

The tests were done with these configurations. `plf::colony` was excluded as it was not able to complete the tests.

```cpp
bench_config configs[] {
    {
        .parallelism = 2,
        .iteration = 5,
        .action_min = 40'000,
        .action_max = 240'000,
        .read_num = 5,
    },
    {
        .parallelism = 4,
        .iteration = 5,
        .action_min = 20'000,
        .action_max = 120'000,
        .read_num = 5,
    },
    {
        .parallelism = 8,
        .iteration = 5,
        .action_min = 10'000,
        .action_max = 60'000,
        .read_num = 5,
    },
    {
        .parallelism = 16,
        .iteration = 5,
        .action_min = 5'000,
        .action_max = 30'000,
        .read_num = 5,
    },
};
```

|             name | par | size | get (ms) |  set (ms) | reset (ms) | iteration (ms) | total (ms) 
|-----------------:|----:|-----:|---------:|----------:|-----------:|---------------:|-----------:
|  static slot map |   2 |  128 |   4.8379 |   87.3244 |    62.5641 |         1.6902 |   156.4166 
| dynamic slot map |   2 |  128 |   8.2121 |  130.6879 |    59.9673 |         4.5615 |   203.4288 
|  static slot map |   4 |  128 |   4.3377 |  226.7701 |   124.1663 |         2.9257 |   358.1998 
| dynamic slot map |   4 |  128 |   6.7841 |  242.7728 |   126.1847 |         5.7426 |   381.4842 
|  static slot map |   8 |  128 |   4.4607 |  775.3701 |   250.8486 |         5.5630 |  1036.2424 
| dynamic slot map |   8 |  128 |   6.9206 |  749.2363 |   305.0924 |        10.9117 |  1072.1610 
| dynamic slot map |  16 |  128 |  10.1594 | 2005.6542 |   498.4583 |        27.8018 |  2542.0735 
|  static slot map |  16 |  128 |   5.7311 | 2134.2031 |   510.5195 |        13.2010 |  2663.6545 

## License

[Boost Software License 1.0](LICENSE)
