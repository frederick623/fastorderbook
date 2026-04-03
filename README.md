# fastorderbook
High performance orderbook

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
| Add Order (Limit) | O(1) | Best case (no matching).
| Add Order (Market) | O(N) | Where N is the number of price levels crossed.
| Cancel Order | O(1) | Immediate lookup and lazy flag set.
| Reduce Order | O(1) | Direct modification via lookup map.
| Price/Qty Query | O(1) | Direct array access.