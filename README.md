# fastorderbook
High performance orderbook achieving nanosecond magnitude of latency and low variance

Implementation loosely reference from https://www.youtube.com/watch?v=8uAW5FQtcvE and https://www.youtube.com/watch?v=sX2nF1fW7kI&t=3036s

This implementation of an OrderBook is designed for high-performance trading applications, specifically focusing on memory locality, zero-heap allocation during order matching, and cache efficiency.

Here is a breakdown of the technical implementation you can include in your README.md.

Technical Overview: High-Performance Order Book
## Memory Management: The Flat Pool

Unlike traditional order books that use std::list or raw pointers (which cause memory fragmentation and cache misses), this implementation uses a Flat Memory Pool.

Contiguous Storage: All orders are stored in a pre-allocated std::vector<OrderSlot>.

Index-Based Linking: Instead of 64-bit pointers, the book uses uint32_t indices to link orders. This reduces the memory footprint and ensures the OrderSlot struct is exactly 32 bytes.

Cache Friendliness: By keeping slots at 32 bytes, exactly two orders fit perfectly into a standard 64-byte CPU cache line.

## Data Structures

Price-Level Array: The book uses a "Direct Map" approach for price levels (std::vector<PriceLevel>). This allows for O(1) access to any price point without searching a tree.

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
To build, simply cmake . 
Or to build with benchmark test, cmake . -DBUILD_TEST=1
then make

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
BM_AddOrder/10000/repeats:5_mean              3535346 ns      3535260 ns            5 items_per_second=2.82868M/s
BM_AddOrder/10000/repeats:5_median            3542737 ns      3542642 ns            5 items_per_second=2.82275M/s
BM_AddOrder/10000/repeats:5_stddev              13005 ns        13001 ns            5 items_per_second=10.416k/s
BM_AddOrder/10000/repeats:5_cv                   0.37 %          0.37 %             5 items_per_second=0.37%
BM_AddOrder_NoMatch/10000/repeats:5_mean      2622158 ns      2622082 ns            5 items_per_second=3.81385M/s
BM_AddOrder_NoMatch/10000/repeats:5_median    2626655 ns      2626616 ns            5 items_per_second=3.80718M/s
BM_AddOrder_NoMatch/10000/repeats:5_stddev      13629 ns        13608 ns            5 items_per_second=19.8195k/s
BM_AddOrder_NoMatch/10000/repeats:5_cv           0.52 %          0.52 %             5 items_per_second=0.52%
BM_CancelOrder/10000/repeats:5_mean           2143338 ns      2143276 ns            5 items_per_second=4.66584M/s
BM_CancelOrder/10000/repeats:5_median         2143954 ns      2143919 ns            5 items_per_second=4.66436M/s
BM_CancelOrder/10000/repeats:5_stddev           10159 ns        10103 ns            5 items_per_second=21.9875k/s
BM_CancelOrder/10000/repeats:5_cv                0.47 %          0.47 %             5 items_per_second=0.47%
BM_MarketSweep/500/repeats:5_mean              995350 ns       995251 ns            5 items_per_second=502.387k/s
BM_MarketSweep/500/repeats:5_median            994898 ns       994883 ns            5 items_per_second=502.571k/s
BM_MarketSweep/500/repeats:5_stddev              2069 ns         2046 ns            5 items_per_second=1.03092k/s
BM_MarketSweep/500/repeats:5_cv                  0.21 %          0.21 %             5 items_per_second=0.21%
BM_BestBidAsk/repeats:5_mean                     17.7 ns         17.7 ns            5
BM_BestBidAsk/repeats:5_median                   17.7 ns         17.7 ns            5
BM_BestBidAsk/repeats:5_stddev                  0.054 ns        0.054 ns            5
BM_BestBidAsk/repeats:5_cv                       0.31 %          0.31 %             5
```
