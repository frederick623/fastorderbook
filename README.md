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
Performed in MacBook Pro M4
```
-----------------------------------------------------------------------------------------------------
Benchmark                                           Time             CPU   Iterations UserCounters...
-----------------------------------------------------------------------------------------------------
BM_AddOrder/10000/repeats:5_mean              3700171 ns      3699848 ns            5 items_per_second=2.70301M/s
BM_AddOrder/10000/repeats:5_median            3705004 ns      3704807 ns            5 items_per_second=2.6992M/s
BM_AddOrder/10000/repeats:5_stddev              35181 ns        34902 ns            5 items_per_second=25.5055k/s
BM_AddOrder/10000/repeats:5_cv                   0.95 %          0.94 %             5 items_per_second=0.94%
BM_AddOrder_NoMatch/10000/repeats:5_mean      2075265 ns      2075166 ns            5 items_per_second=4.81907M/s
BM_AddOrder_NoMatch/10000/repeats:5_median    2074964 ns      2074880 ns            5 items_per_second=4.81955M/s
BM_AddOrder_NoMatch/10000/repeats:5_stddev      13965 ns        13982 ns            5 items_per_second=32.4234k/s
BM_AddOrder_NoMatch/10000/repeats:5_cv           0.67 %          0.67 %             5 items_per_second=0.67%
BM_CancelOrder/10000/repeats:5_mean           2091387 ns      2091259 ns            5 items_per_second=4.78187M/s
BM_CancelOrder/10000/repeats:5_median         2094853 ns      2094688 ns            5 items_per_second=4.77398M/s
BM_CancelOrder/10000/repeats:5_stddev            8163 ns         8170 ns            5 items_per_second=18.7351k/s
BM_CancelOrder/10000/repeats:5_cv                0.39 %          0.39 %             5 items_per_second=0.39%
BM_MarketSweep/100/repeats:5_mean              730298 ns       730241 ns            5 items_per_second=136.955k/s
BM_MarketSweep/100/repeats:5_median            729110 ns       729035 ns            5 items_per_second=137.168k/s
BM_MarketSweep/100/repeats:5_stddev              8226 ns         8198 ns            5 items_per_second=1.5255k/s
BM_MarketSweep/100/repeats:5_cv                  1.13 %          1.12 %             5 items_per_second=1.11%
BM_BestBidAsk/repeats:5_mean                     33.6 ns         33.6 ns            5
BM_BestBidAsk/repeats:5_median                   33.6 ns         33.6 ns            5
BM_BestBidAsk/repeats:5_stddev                  0.123 ns        0.124 ns            5
BM_BestBidAsk/repeats:5_cv                       0.37 %          0.37 %             5
```
