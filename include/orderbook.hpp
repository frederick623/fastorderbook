#pragma once

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <optional>
#include <stdexcept>
#include <cassert>
#include <iostream>
#include <iomanip>

using OrderId = uint64_t;
using Price   = uint32_t;
using Qty     = uint32_t;
enum class Side : uint8_t { Buy, Sell };

// ── OrderSlot ─────────────────────────────────────────────────────────────────
// Lives in a flat pool vector.  Links are uint32_t indices, not pointers.
// 32 bytes → 2 slots per 64-byte cache line.

static constexpr uint32_t INVALID_IDX = ~uint32_t(0);

struct OrderSlot {
    OrderId  id         = 0;
    uint32_t next       = INVALID_IDX;  // next in price-level FIFO chain
    Price    price      = 0;
    Qty      qty        = 0;
    Qty      initialQty = 0;
    Side     side       = Side::Buy;
    bool     active     = false;        // false = lazily deleted
    uint8_t  _pad[6]    = {};
};
static_assert(sizeof(OrderSlot) == 32);

struct Trade {
    OrderId passiveId, aggressorId;
    Price   price;
    Qty     qty;
};

// ── PriceLevel ────────────────────────────────────────────────────────────────
// FIFO singly-linked chain of pool indices; no heap allocation.

struct PriceLevel {
    uint32_t head     = INVALID_IDX;
    uint32_t tail     = INVALID_IDX;
    Qty      totalQty = 0;
    bool     empty()  const { return head == INVALID_IDX; }
};

struct OrderLookup {
    uint32_t poolIdx;
    Price    price;
    Side     side;
};

// ── OrderBook ─────────────────────────────────────────────────────────────────

class OrderBook {
public:
    explicit OrderBook(Price maxPrice = 1'000'000, uint32_t poolCap = 1'000'000)
        : maxPrice_(maxPrice)
        , bids_(maxPrice + 1)
        , asks_(maxPrice + 1)
        , bestBid_(0)
        , bestAsk_(maxPrice + 1)
    {
        pool_.reserve(poolCap);
    }

    std::vector<Trade> addOrder(OrderId id, Side side, Price price, Qty qty)
    {
        if (price > maxPrice_) throw std::out_of_range("price > maxPrice");
        if (qty == 0)          throw std::invalid_argument("qty must be > 0");
        if (lookup_.count(id)) throw std::invalid_argument("duplicate order id");

        std::vector<Trade> trades;

        if (side == Side::Buy) {
            // Match against resting asks
            while (qty > 0 && bestAsk_ <= price) {
                PriceLevel& level = asks_[bestAsk_];
                fillHead(level, qty, id, bestAsk_, trades);
                if (level.totalQty == 0) updateBestAsk(bestAsk_);
            }
            // Rest any unfilled qty
            if (qty > 0) {
                uint32_t idx = allocSlot();
                pool_[idx]   = {id, INVALID_IDX, price, qty, qty, side, true, {}};
                enqueue(bids_[price], idx, qty);
                lookup_[id]  = {idx, price, side};
                if (price > bestBid_) bestBid_ = price;
            }
        } else {
            // Match against resting bids
            while (qty > 0 && bestBid_ >= price && bestBid_ > 0) {
                PriceLevel& level = bids_[bestBid_];
                fillHead(level, qty, id, bestBid_, trades);
                if (level.totalQty == 0) updateBestBid(bestBid_);
            }
            // Rest any unfilled qty
            if (qty > 0) {
                uint32_t idx = allocSlot();
                pool_[idx]   = {id, INVALID_IDX, price, qty, qty, side, true, {}};
                enqueue(asks_[price], idx, qty);
                lookup_[id]  = {idx, price, side};
                if (price < bestAsk_) bestAsk_ = price;
            }
        }

        return trades;
    }

    bool cancelOrder(OrderId id)
    {
        auto it = lookup_.find(id);
        if (it == lookup_.end()) return false;

        const OrderLookup& ol   = it->second;
        OrderSlot&         slot = pool_[ol.poolIdx];

        if (ol.side == Side::Buy) {
            bids_[ol.price].totalQty -= slot.qty;
            if (bids_[ol.price].totalQty == 0 && ol.price == bestBid_)
                updateBestBid(ol.price);
        } else {
            asks_[ol.price].totalQty -= slot.qty;
            if (asks_[ol.price].totalQty == 0 && ol.price == bestAsk_)
                updateBestAsk(ol.price);
        }

        slot.active = false;   // fillHead will recycle this slot on next visit
        lookup_.erase(it);
        return true;
    }

    bool reduceOrder(OrderId id, Qty reduceBy)
    {
        auto it = lookup_.find(id);
        if (it == lookup_.end()) return false;

        OrderSlot& slot = pool_[it->second.poolIdx];
        if (reduceBy >= slot.qty) return false;

        slot.qty -= reduceBy;
        if (it->second.side == Side::Buy)
            bids_[it->second.price].totalQty -= reduceBy;
        else
            asks_[it->second.price].totalQty -= reduceBy;
        return true;
    }

    std::optional<Price> bestBid()  const { 
        return bestBid_>0?std::optional<Price>(bestBid_):std::nullopt; 
    }
    std::optional<Price> bestAsk()  const { 
        return bestAsk_<=maxPrice_?std::optional<Price>(bestAsk_):std::nullopt; 
    }
    std::optional<Price> midPrice() const { 
        return (bestBid_>0 && bestAsk_<= maxPrice_)?std::optional<Price>((bestBid_+bestAsk_)/2):std::nullopt; 
    }
    std::optional<Price> spread()   const { 
        return (bestBid_>0 && bestAsk_<=maxPrice_)?std::optional<Price>(bestAsk_-bestBid_):std::nullopt; 
    }

    Qty  qtyAtPrice(Side side, Price p) const 
    {
        if (p > maxPrice_) return 0;
        return side == Side::Buy ? bids_[p].totalQty : asks_[p].totalQty;
    }

    Qty  marketDepth(Side side, uint32_t depthLevels) const
    {
        Qty total = 0; uint32_t seen = 0;
        if (side == Side::Buy) {
            for (Price p = bestBid_; p > 0 && seen < depthLevels; --p)
                if (bids_[p].totalQty > 0) { total += bids_[p].totalQty; ++seen; }
        } else {
            for (Price p = bestAsk_; p <= maxPrice_ && seen < depthLevels; ++p)
                if (asks_[p].totalQty > 0) { total += asks_[p].totalQty; ++seen; }
        }
        return total;
    }

    bool hasOrder(OrderId id) const { return lookup_.count(id) > 0; }
    void printBook(uint32_t levels = 5) const;

    uint32_t poolSize()     const { return static_cast<uint32_t>(pool_.size()); }
    uint32_t freeListSize() const { return freeCount_; }

private:
    Price                                    maxPrice_;
    std::vector<PriceLevel>                  bids_, asks_;
    std::vector<OrderSlot>                   pool_;
    uint32_t                                 freeHead_  = INVALID_IDX;
    uint32_t                                 freeCount_ = 0;
    std::unordered_map<OrderId, OrderLookup> lookup_;
    Price                                    bestBid_, bestAsk_;

    uint32_t allocSlot()
    {
        if (freeHead_ != INVALID_IDX) {
            uint32_t idx = freeHead_;
            freeHead_ = pool_[idx].next;
            --freeCount_;
            return idx;
        }
        uint32_t idx = static_cast<uint32_t>(pool_.size());
        pool_.emplace_back();
        return idx;
    }

    void freeSlot(uint32_t idx)
    {
        pool_[idx].active = false;
        pool_[idx].next   = freeHead_;
        freeHead_         = idx;
        ++freeCount_;
    }

    void enqueue(PriceLevel& level, uint32_t idx, Qty qty)
    {
        pool_[idx].next = INVALID_IDX;
        if (level.tail == INVALID_IDX) {
            level.head = level.tail = idx;
        } else {
            pool_[level.tail].next = idx;
            level.tail             = idx;
        }
        level.totalQty += qty;
    }

    void fillHead(PriceLevel& level, Qty& want, OrderId aggressor, Price px, std::vector<Trade>& out)
    {
        while (want > 0 && level.head != INVALID_IDX) {
            uint32_t   idx  = level.head;
            OrderSlot& slot = pool_[idx];

            // Skip lazily-cancelled slots
            if (!slot.active) {
                level.head = slot.next;
                if (level.head == INVALID_IDX) level.tail = INVALID_IDX;
                freeSlot(idx);
                continue;
            }

            Qty take        = std::min(want, slot.qty);
            slot.qty       -= take;
            level.totalQty -= take;
            want           -= take;

            out.push_back({slot.id, aggressor, px, take});

            if (slot.qty == 0) {
                lookup_.erase(slot.id);
                level.head = slot.next;
                if (level.head == INVALID_IDX) level.tail = INVALID_IDX;
                freeSlot(idx);
            }
        }
    }

    void     updateBestBid(Price from)
    {
        for (Price p = from; p > 0; --p)
            if (bids_[p].totalQty > 0) { bestBid_ = p; return; }
        bestBid_ = 0;
    }

    void     updateBestAsk(Price from)
    {
        for (Price p = from; p <= maxPrice_; ++p)
            if (asks_[p].totalQty > 0) { bestAsk_ = p; return; }
        bestAsk_ = maxPrice_ + 1;
    }

};