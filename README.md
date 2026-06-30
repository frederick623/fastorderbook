# fastorderbook
High performance orderbook achieving nanosecond magnitude and low variance of latency. Implementation loosely reference from https://www.youtube.com/watch?v=8uAW5FQtcvE and https://www.youtube.com/watch?v=sX2nF1fW7kI&t=3036s

This implementation of an OrderBook is designed for high-performance trading applications, specifically focusing on memory locality, zero-heap allocation during order matching, and cache efficiency.

## Memory Management: The Flat Pool

Unlike traditional order books that use std::list or raw pointers (which cause memory fragmentation and cache misses), this implementation uses a Flat Memory Pool.

Contiguous Storage: All orders are stored in a pre-allocated std::array<OrderSlot, PoolCap>.

Index-Based Linking: Instead of 64-bit pointers, the book uses uint32_t indices to link orders. This reduces the memory footprint and ensures the OrderSlot struct is exactly 32 bytes.

Cache Friendliness: By keeping slots at 32 bytes, exactly two orders fit perfectly into a standard 64-byte CPU cache line.

## Data Structures

Price-Level Array: The book uses a "Direct Map" approach for price levels (std::array<PriceLevel, MaxPrice + 1>). This allows for O(1) access to any price point without searching a tree.

Singly-Linked FIFO: Each price level maintains a head and tail index. New orders are appended to the tail, and matching occurs at the head, ensuring strict Time Priority.

Fast Lookup: An unordered_map maps OrderId to its position in the pool for O(1) cancellations and modifications.

## Key Optimization Strategies

Lazy Deletion: When an order is cancelled, it is marked as active = false. The memory is not immediately reclaimed. Instead, the matching engine "cleans" these slots the next time it traverses that price level, minimizing the work done on the critical path of a cancellation.

Free-List Recycling: When an order is fully filled or cleaned up, its index is added to a freeHead_ stack. New orders reuse these "holes" before the vector is forced to grow, keeping the memory footprint stable.

Best-Price Tracking: The book maintains bestBid_ and bestAsk_ variables. While it uses a linear search to update these after a level is exhausted, it starts the search from the previous best price, which is highly efficient in active markets.

## Complexity Analysis

| Operation | Complexity | Note |
| -------- | -------- | -------- |
| Add Order (Limit) | O(1) | Best case (no matching).
| Add Order (Market) | O(N) | Where N is the number of price levels crossed.
| Cancel Order | O(1) | Immediate lookup and lazy flag set.
| Reduce Order | O(1) | Direct modification via lookup map.
| Price/Qty Query | O(1) | Direct array access.

## To build and use
To build, simply 
```
cmake .
```
Or to build with benchmark test, 
```
cmake . -DBUILD_TEST=1
```
then 
```
make
```

Header only, just included by:
```cpp
#include "orderbook.hpp"
```
and link with the target orderbook

## Usage

```cpp
// Initialize book with max price 10,000
OrderBook book(10000);

// Add a Buy Order (Aggressor)
auto trades = book.addOrder(101, Side::Buy, 500, 10);

// The trades vector contains execution details if the order matched
for (const auto& trade : trades) {
    std::cout << "Matched " << trade.qty << " @ " << trade.price << std::endl;
}

// Check Top of Book
if (auto bid = book.bestBid()) {
    std::cout << "Best Bid: " << *bid << std::endl;
}
```

## Benchmark
Performed in MacBook Air M5
```
CPU Caches:
  L1 Data 64 KiB
  L1 Instruction 128 KiB
  L2 Unified 6144 KiB (x10)
Load Average: 1.46, 1.56, 1.57
---------------------------------------------------------------------------------------------------------
Benchmark                                               Time             CPU   Iterations UserCounters...
---------------------------------------------------------------------------------------------------------
BM_AddOrder/10000/repeats:5_mean                  2640136 ns      2639038 ns            5 items_per_second=3.7893M/s
BM_AddOrder/10000/repeats:5_median                2635104 ns      2633849 ns            5 items_per_second=3.79672M/s
BM_AddOrder/10000/repeats:5_stddev                   9692 ns         9659 ns            5 items_per_second=13.8391k/s
BM_AddOrder/10000/repeats:5_cv                       0.37 %          0.37 %             5 items_per_second=0.37%
BM_AddOrder_NoMatch/10000/repeats:5_mean          1389513 ns      1388889 ns            5 items_per_second=7.20003M/s
BM_AddOrder_NoMatch/10000/repeats:5_median        1388916 ns      1388268 ns            5 items_per_second=7.20322M/s
BM_AddOrder_NoMatch/10000/repeats:5_stddev           3093 ns         3057 ns            5 items_per_second=15.8514k/s
BM_AddOrder_NoMatch/10000/repeats:5_cv               0.22 %          0.22 %             5 items_per_second=0.22%
BM_CancelOrder/10000/repeats:5_mean               1311085 ns      1310480 ns            5 items_per_second=7.63089M/s
BM_CancelOrder/10000/repeats:5_median             1312327 ns      1311788 ns            5 items_per_second=7.62318M/s
BM_CancelOrder/10000/repeats:5_stddev                5321 ns         5427 ns            5 items_per_second=31.6579k/s
BM_CancelOrder/10000/repeats:5_cv                    0.41 %          0.41 %             5 items_per_second=0.41%
BM_MarketSweep/100/repeats:5_mean                  339429 ns       339285 ns            5 items_per_second=294.847k/s
BM_MarketSweep/100/repeats:5_median                338096 ns       337970 ns            5 items_per_second=295.884k/s
BM_MarketSweep/100/repeats:5_stddev                  7387 ns         7401 ns            5 items_per_second=6.30035k/s
BM_MarketSweep/100/repeats:5_cv                      2.18 %          2.18 %             5 items_per_second=2.14%
BM_BestBidAsk/repeats:5_mean                         28.6 ns         28.6 ns            5
BM_BestBidAsk/repeats:5_median                       28.6 ns         28.6 ns            5
BM_BestBidAsk/repeats:5_stddev                      0.024 ns        0.025 ns            5
BM_BestBidAsk/repeats:5_cv                           0.09 %          0.09 %             5
BM_Dyn_AddOrder/10000/repeats:5_mean              4469473 ns      4467513 ns            5 items_per_second=2.23839M/s
BM_Dyn_AddOrder/10000/repeats:5_median            4470254 ns      4468427 ns            5 items_per_second=2.23792M/s
BM_Dyn_AddOrder/10000/repeats:5_stddev               8432 ns         8424 ns            5 items_per_second=4.22216k/s
BM_Dyn_AddOrder/10000/repeats:5_cv                   0.19 %          0.19 %             5 items_per_second=0.19%
BM_Dyn_AddOrder_NoMatch/10000/repeats:5_mean      3167700 ns      3166193 ns            5 items_per_second=3.15837M/s
BM_Dyn_AddOrder_NoMatch/10000/repeats:5_median    3168019 ns      3166561 ns            5 items_per_second=3.158M/s
BM_Dyn_AddOrder_NoMatch/10000/repeats:5_stddev       2601 ns         2602 ns            5 items_per_second=2.59565k/s
BM_Dyn_AddOrder_NoMatch/10000/repeats:5_cv           0.08 %          0.08 %             5 items_per_second=0.08%
BM_Dyn_CancelOrder/10000/repeats:5_mean           2987755 ns      2986382 ns            5 items_per_second=3.34854M/s
BM_Dyn_CancelOrder/10000/repeats:5_median         2988247 ns      2987047 ns            5 items_per_second=3.34779M/s
BM_Dyn_CancelOrder/10000/repeats:5_stddev            3952 ns         4007 ns            5 items_per_second=4.49412k/s
BM_Dyn_CancelOrder/10000/repeats:5_cv                0.13 %          0.13 %             5 items_per_second=0.13%
BM_Dyn_MarketSweep/100/repeats:5_mean             2013905 ns      2013073 ns            5 items_per_second=49.6754k/s
BM_Dyn_MarketSweep/100/repeats:5_median           2014178 ns      2013319 ns            5 items_per_second=49.6692k/s
BM_Dyn_MarketSweep/100/repeats:5_stddev              2685 ns         2772 ns            5 items_per_second=68.4599/s
BM_Dyn_MarketSweep/100/repeats:5_cv                  0.13 %          0.14 %             5 items_per_second=0.14%
BM_Dyn_BestBidAsk/repeats:5_mean                     29.8 ns         29.7 ns            5
BM_Dyn_BestBidAsk/repeats:5_median                   29.7 ns         29.7 ns            5
BM_Dyn_BestBidAsk/repeats:5_stddev                  0.030 ns        0.031 ns            5
BM_Dyn_BestBidAsk/repeats:5_cv                       0.10 %          0.10 %             5
```
