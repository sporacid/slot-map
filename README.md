# Slot map

A header-only, C++20 slot map implementation with a hierarchical free list
based [on this post](https://jakubtomsu.github.io/posts/bit_pools/).

## What is a slot map?

A slot map is a dataructure that providesable and versioned keys toored values. Inserting into the map creates
and return a unique key, whichays valid until the slot is explicitly freed.

- O(1) insert, lookup, and erase
- Stable keys
- Automatic detection of use-after-free via version fields
- Cache-friendly, page-alignedorage

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

## Benchmarks

### Single Thread

 par | size | get (ms) | set (ms) | reset (ms) | total (ms) | name                  
-----|------|----------|----------|------------|------------|-----------------------
 1   | 1    | 1.6033   | 1.9744   | 2.5574     | 6.1351     | plf colony            
 1   | 1    | 1.9630   | 1.2313   | 0.6845     | 3.8788     | slot map (static)  
 1   | 1    | 2.3904   | 1.4254   | 0.8486     | 4.6644     | slot map (dynamic) 
 1   | 1    | 18.3825  | 18.6682  | 24.5353    | 61.5860    | plf colony            
 1   | 1    | 20.1650  | 12.1171  | 7.1957     | 39.4778    | slot map (static)  
 1   | 1    | 25.5529  | 14.8402  | 9.2628     | 49.6559    | slot map (dynamic) 

 par | size | get (ms) | set (ms) | reset (ms) | total (ms) | name                  
-----|------|----------|----------|------------|------------|-----------------------
 1   | 8    | 1.6569   | 2.3417   | 2.5185     | 6.5171     | plf colony            
 1   | 8    | 1.9435   | 1.3324   | 0.6521     | 3.9280     | slot map (static)  
 1   | 8    | 2.9947   | 1.5379   | 0.8843     | 5.4169     | slot map (dynamic) 
 1   | 8    | 18.7178  | 12.2265  | 7.1336     | 38.0779    | slot map (static)  
 1   | 8    | 31.5827  | 16.1043  | 9.7226     | 57.4096    | slot map (dynamic) 
 1   | 8    | 39.8862  | 35.1415  | 33.4870    | 108.5147   | plf colony            

 par | size | get (ms) | set (ms) | reset (ms) | total (ms) | name                  
-----|------|----------|----------|------------|------------|-----------------------
 1   | 32   | 1.6840   | 3.4253   | 2.9025     | 8.0118     | plf colony            
 1   | 32   | 1.8829   | 1.4324   | 0.6517     | 3.9670     | slot map (static)  
 1   | 32   | 3.4267   | 2.1293   | 0.9457     | 6.5017     | slot map (dynamic) 
 1   | 32   | 18.8597  | 13.2041  | 7.2840     | 39.3478    | slot map (static)  
 1   | 32   | 30.9946  | 22.2208  | 9.6056     | 62.8210    | slot map (dynamic) 
 1   | 32   | 42.9740  | 41.1388  | 47.1980    | 131.3108   | plf colony            

 par | size | get (ms) | set (ms) | reset (ms) | total (ms) | name                  
-----|------|----------|----------|------------|------------|-----------------------
 1   | 128  | 2.0155   | 2.4097   | 0.6410     | 5.0662     | slot map (static)  
 1   | 128  | 2.8376   | 10.6448  | 5.5376     | 19.0200    | plf colony            
 1   | 128  | 3.0594   | 4.0184   | 0.9772     | 8.0550     | slot map (dynamic) 
 1   | 128  | 20.0946  | 33.9728  | 6.6182     | 60.6856    | slot map (static)  
 1   | 128  | 34.3537  | 51.2137  | 9.9443     | 95.5117    | slot map (dynamic) 
 1   | 128  | 36.2506  | 100.3860 | 60.8060    | 197.4426   | plf colony            

 par | size | get (ms) | set (ms) | reset (ms) | total (ms) | name                  
-----|------|----------|----------|------------|------------|-----------------------
 1   | 512  | 1.7987   | 6.3322   | 0.6478     | 8.7787     | slot map (static)  
 1   | 512  | 2.0764   | 30.0784  | 7.3985     | 39.5533    | plf colony            
 1   | 512  | 3.3218   | 12.8762  | 1.0670     | 17.2650    | slot map (dynamic) 
 1   | 512  | 19.4792  | 147.4727 | 6.6404     | 173.5923   | slot map (static)  
 1   | 512  | 35.3396  | 244.4047 | 13.1798    | 292.9241   | slot map (dynamic) 
 1   | 512  | 40.5446  | 387.6783 | 110.8688   | 539.0917   | plf colony            

### Multi-Threads

 par | size | get (ms) | set (ms)  | reset (ms) | total (ms) | name                  
-----|------|----------|-----------|------------|------------|-----------------------
 2   | 1    | 8.8039   | 137.1966  | 105.1155   | 251.1160   | slot map (static)  
 2   | 1    | 13.0613  | 150.7179  | 85.3688    | 249.1480   | slot map (dynamic) 
 4   | 1    | 10.9457  | 357.1173  | 157.2347   | 525.2977   | slot map (dynamic) 
 4   | 1    | 28.7467  | 371.3181  | 253.3942   | 653.4590   | slot map (static)  
 8   | 1    | 9.5554   | 1242.4026 | 369.3803   | 1621.3383  | slot map (static)  
 8   | 1    | 12.9421  | 1175.5476 | 320.4153   | 1508.9050  | slot map (dynamic) 
 16  | 1    | 5.7144   | 1085.9509 | 289.6724   | 1381.3379  | slot map (static)  
 16  | 1    | 11.2534  | 1181.9905 | 267.3988   | 1460.6427  | slot map (dynamic) 

 par | size | get (ms) | set (ms)  | reset (ms) | total (ms) | name                  
-----|------|----------|-----------|------------|------------|-----------------------
 2   | 8    | 8.0391   | 117.9184  | 100.4907   | 226.4482   | slot map (static)  
 2   | 8    | 14.8256  | 127.0111  | 67.0339    | 208.8706   | slot map (dynamic) 
 4   | 8    | 7.2504   | 380.8837  | 164.2987   | 552.4327   | slot map (static)  
 4   | 8    | 14.1581  | 399.0996  | 168.8788   | 582.1365   | slot map (dynamic) 
 8   | 8    | 8.6825   | 1319.8109 | 352.6948   | 1681.1882  | slot map (static)  
 8   | 8    | 20.6412  | 1068.9221 | 357.2508   | 1446.8141  | slot map (dynamic) 
 16  | 8    | 8.8028   | 992.9959  | 224.8810   | 1226.6797  | slot map (dynamic) 
 16  | 8    | 14.1162  | 1043.7568 | 306.0575   | 1363.9305  | slot map (static)  

 par | size | get (ms) | set (ms)  | reset (ms) | total (ms) | name                  
-----|------|----------|-----------|------------|------------|-----------------------
 2   | 32   | 5.7384   | 78.0340   | 66.8569    | 150.6293   | slot map (static)  
 2   | 32   | 17.9340  | 153.7512  | 84.6611    | 256.3463   | slot map (dynamic) 
 4   | 32   | 9.7866   | 440.4753  | 231.7277   | 681.9896   | slot map (static)  
 4   | 32   | 15.4950  | 395.2914  | 176.7843   | 587.5707   | slot map (dynamic) 
 8   | 32   | 12.3172  | 1450.5510 | 367.7767   | 1830.6450  | slot map (static)  
 8   | 32   | 19.0922  | 1022.9577 | 366.0083   | 1408.0582  | slot map (dynamic) 
 16  | 32   | 9.7935   | 1083.8412 | 298.3657   | 1392.0002  | slot map (static)  
 16  | 32   | 10.4476  | 1062.6678 | 229.5939   | 1302.7094  | slot map (dynamic) 

 par | size | get (ms) | set (ms)  | reset (ms) | total (ms) | name                  
-----|------|----------|-----------|------------|------------|-----------------------
 2   | 128  | 8.5305   | 160.3594  | 81.9947    | 250.8846   | slot map (static)  
 2   | 128  | 15.2495  | 206.6740  | 95.7579    | 317.6814   | slot map (dynamic) 
 4   | 128  | 13.4125  | 418.7982  | 179.1756   | 611.3863   | slot map (static)  
 4   | 128  | 25.1392  | 340.0454  | 158.3206   | 523.5052   | slot map (dynamic) 
 8   | 128  | 10.1845  | 1343.3231 | 353.1908   | 1706.6984  | slot map (static)  
 8   | 128  | 19.5407  | 1284.3389 | 306.6128   | 1610.4923  | slot map (dynamic) 
 16  | 128  | 5.8552   | 1126.5140 | 295.3484   | 1427.7178  | slot map (static)  
 16  | 128  | 12.1530  | 1090.6366 | 218.5329   | 1321.3225  | slot map (dynamic) 

 par | size | get (ms) | set (ms)  | reset (ms) | total (ms) | name                  
-----|------|----------|-----------|------------|------------|-----------------------
 2   | 512  | 8.5398   | 294.0922  | 57.7382    | 360.3702   | slot map (dynamic) 
 2   | 512  | 10.7508  | 278.4164  | 75.3880    | 364.5552   | slot map (static)  
 4   | 512  | 9.3703   | 501.4918  | 155.7426   | 666.6047   | slot map (static)  
 4   | 512  | 18.4900  | 717.4370  | 166.2137   | 902.1406   | slot map (dynamic) 
 8   | 512  | 10.9592  | 1391.4069 | 306.9694   | 1709.3354  | slot map (static)  
 8   | 512  | 36.8883  | 1741.0305 | 289.4847   | 2067.4036  | slot map (dynamic) 
 16  | 512  | 6.8045   | 1232.6981 | 280.5061   | 1520.0087  | slot map (static)  
 16  | 512  | 13.0511  | 1541.3818 | 101.4130   | 1655.8461  | slot map (dynamic) 

## License

[Boost Software License 1.0](LICENSE)
