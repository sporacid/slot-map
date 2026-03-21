# Slot map

A header-only, C++20 slot map implementation with a hierarchical free list
based [on this post](https://jakubtomsu.github.io/posts/bit_pools/).

## What is a slot map?

A slot map is a data structure that provides stable and versioned keys to stored values. Inserting into the map creates
and return a unique key, which stays valid until the slot is explicitly freed.

- O(1) insert, lookup, and erase
- Stable keys
- Automatic detection of use-after-free via version fields
- Cache-friendly, page-aligned storage

My use case for this specialized container was a RHI resource manager.

## Features

- Single header
- No external dependencies
- Thread-safe variant with lock-free synchronization on hot paths

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
for (auto value : map) { }

// use typed key to prevent errors
struct my_key : slot_key {};
slot_map_st<my_key, uint32_t, 1024> map;
```

## License

[Boost Software License 1.0](LICENSE)
