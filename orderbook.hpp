#pragma once

#include <array>
#include <vector>
#include <unordered_map>
#include <optional>
#include <stdexcept>
#include <cstdint>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <algorithm>

// ── Non-template globals ──────────────────────────────────────────────────────

using OrderId = uint64_t;
using PTick   = uint32_t;
using Qty     = uint32_t;
using Idx     = uint32_t;
enum class Side : uint8_t { Buy, Sell };

static constexpr Idx INVALID_IDX = ~uint32_t(0);

// ── OrderSlot: one node in the flat pool array ────────────────────────────────
// price field stores the raw (scaled) integer tick.
// 32 bytes → 2 slots per 64-byte cache line.

struct OrderSlot {
    OrderSlot() = default;

    OrderSlot(OrderId oid, PTick px, Qty q, Side s, bool ac=true)
    : id(oid)
    , price(px)
    , qty(q)
    , initialQty(q)
    , side(s)
    , active(ac)
    {

    }

    OrderId  id         = 0;
    Idx      next       = INVALID_IDX; // next pool index in price-level FIFO chain
    PTick    price      = 0;           // raw scaled tick
    Qty      qty        = 0;
    Qty      initialQty = 0;
    Side     side       = Side::Buy;
    bool     active     = false;       // false = lazily deleted
    uint8_t  _pad[6]    = {};
};
static_assert(sizeof(OrderSlot) == 32, "OrderSlot must be 32 bytes");

// ── PriceLevel: FIFO chain of pool indices ────────────────────────────────────

struct PriceLevel {
    Idx head     = INVALID_IDX;
    Idx tail     = INVALID_IDX;
    Qty totalQty = 0;
};

// ── OrderLookup: payload stored per order in the hash map ─────────────────────

struct OrderLookup {
    Idx      poolIdx;
    PTick    rawPrice;
    Side     side;
};

// ─────────────────────────────────────────────────────────────────────────────
//  OrderBook<MaxPrice, ScaleFactor, PoolCap>
//
//  Template parameters
//  ───────────────────
//  MaxPrice    – largest raw (scaled) price tick accepted.
//                e.g. MaxPrice $2'000.00 with ScaleFactor=100 → MaxPriceTick = 200'000
//
//  ScaleFactor – multiplier applied to a double price to produce a raw tick.
//                e.g. ScaleFactor=100  → $99.50 becomes tick 9950
//                     ScaleFactor=1000 → $99.500 becomes tick 99500
//
//  PoolCap     – compile-time capacity of the flat order pool.
//                The pool is a std::array<OrderSlot, PoolCap> embedded inside
//                the object; no heap allocation after construction.
//                ⚠  Because bids_/asks_/pool_ are large, always heap-allocate
//                   the OrderBook (std::make_unique<OrderBook<...>>()).
//
//  Nested types
//  ────────────
//  OrderBook::Price  – public-facing price type.
//                      Constructed from a double; stores the scaled raw tick.
//                      All public API methods take and return Price objects.
//  OrderBook::Trade  – execution report returned by addOrder().
// ─────────────────────────────────────────────────────────────────────────────

template<uint32_t MaxPrice, uint32_t ScaleFactor, uint32_t PoolCap=1'000'000>
class OrderBook {
public:
    constexpr static PTick MaxPriceTick = MaxPrice*ScaleFactor;
    // ── Price ─────────────────────────────────────────────────────────────────
    //
    //  Wraps a double price into a scaled uint32_t tick.
    //  The raw tick is what the spine vectors and pool index on.
    //
    //  Usage:
    //    Price p(99.50);          // ctor from double
    //    p.value()  → 99.5        // back to double
    //    p.tick()   → 9950        // raw integer tick (ScaleFactor=100)
    //
    struct Price {
        Price() = default;

        // Primary constructor: scale a double into a tick
        explicit Price(double p)
        : raw(static_cast<PTick>(std::llround(p * ScaleFactor)))
        {}

        // Construct directly from a raw tick (used internally)
        static Price fromTick(PTick tick) { 
            auto p = Price();
            p.raw = tick;
            return p; 
        }

        inline double value() const { return static_cast<double>(raw) / ScaleFactor; }
        inline PTick tick()  const { return raw; }

        bool operator<=>(Price o) const { return raw<=>o.tick(); }
        friend std::ostream& operator<<(std::ostream& os, Price p) {
            return os << p.value();
        }

    private:
        PTick raw = 0;
    };

    // ── Trade ─────────────────────────────────────────────────────────────────

    struct Trade {
        OrderId passiveId;
        OrderId aggressorId;
        Price   price;
        Qty     qty;
    };

    // ── Constructor ───────────────────────────────────────────────────────────

    OrderBook() : bestBid_(0), bestAsk_(MaxPriceTick + 1) {}

    // ── Core API ──────────────────────────────────────────────────────────────

    std::vector<Trade> addOrder(OrderId id, Side side, double price, Qty qty)
    {
        return addOrder(id, side, Price(price), qty);
    }

    std::vector<Trade> addOrder(OrderId id, Side side, Price price, Qty qty)
    {
        if (price.tick() > MaxPriceTick)    throw std::out_of_range(
            "Price tick " + std::to_string(price.tick()) + " exceeds MaxPriceTick " + std::to_string(MaxPriceTick));
        if (qty == 0)          throw std::invalid_argument("qty must be > 0");
        if (lookup_.count(id)) throw std::invalid_argument("duplicate order id");

        std::vector<Trade> trades;

        if (side == Side::Buy) {
            while (qty > 0 && bestAsk_ <= price.tick()) {
                fillHead(asks_[bestAsk_], qty, id, bestAsk_, trades);
                if (asks_[bestAsk_].totalQty == 0) updateBestAsk(bestAsk_);
            }
            if (qty > 0) {
                auto idx = allocSlot();
                pool_[idx] = {id, price.tick(), qty, side};
                enqueue(bids_[price.tick()], idx, qty);
                lookup_[id] = {idx, price.tick(), side};
                if (price.tick() > bestBid_) bestBid_ = price.tick();
            }
        } else {
            while (qty > 0 && bestBid_ >= price.tick() && bestBid_ > 0) {
                fillHead(bids_[bestBid_], qty, id, bestBid_, trades);
                if (bids_[bestBid_].totalQty == 0) updateBestBid(bestBid_);
            }
            if (qty > 0) {
                auto idx = allocSlot();
                pool_[idx] = {id, price.tick(), qty, side};
                enqueue(asks_[price.tick()], idx, qty);
                lookup_[id] = {idx, price.tick(), side};
                if (price.tick() < bestAsk_) bestAsk_ = price.tick();
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
            bids_[ol.rawPrice].totalQty -= slot.qty;
            if (bids_[ol.rawPrice].totalQty == 0 && ol.rawPrice == bestBid_)
                updateBestBid(ol.rawPrice);
        } else {
            asks_[ol.rawPrice].totalQty -= slot.qty;
            if (asks_[ol.rawPrice].totalQty == 0 && ol.rawPrice == bestAsk_)
                updateBestAsk(ol.rawPrice);
        }

        slot.active = false;
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
            bids_[it->second.rawPrice].totalQty -= reduceBy;
        else
            asks_[it->second.rawPrice].totalQty -= reduceBy;
        return true;
    }

    // ── Queries ───────────────────────────────────────────────────────────────

    std::optional<Price> bestBid() const {
        return bestBid_ > 0
            ? std::optional<Price>(Price::fromTick(bestBid_)) : std::nullopt;
    }
    std::optional<Price> bestAsk() const {
        return bestAsk_ <= MaxPriceTick
            ? std::optional<Price>(Price::fromTick(bestAsk_)) : std::nullopt;
    }
    std::optional<Price> midPrice() const {
        if (bestBid_ > 0 && bestAsk_ <= MaxPriceTick)
            return Price::fromTick((bestBid_ + bestAsk_) / 2);
        return std::nullopt;
    }
    std::optional<Price> spread() const {
        if (bestBid_ > 0 && bestAsk_ <= MaxPriceTick)
            return Price::fromTick(bestAsk_ - bestBid_);
        return std::nullopt;
    }

    Qty qtyAtPrice(Side side, Price p) const {
        if (p.tick() > MaxPriceTick) return 0;
        return side == Side::Buy ? bids_[p.tick()].totalQty : asks_[p.tick()].totalQty;
    }

    Qty marketDepth(Side side, PTick depthLevels) const
    {
        Qty total = 0; PTick seen = 0;
        if (side == Side::Buy) {
            for (PTick p = bestBid_; p > 0 && seen < depthLevels; --p)
                if (bids_[p].totalQty > 0) { total += bids_[p].totalQty; ++seen; }
        } else {
            for (PTick p = bestAsk_; p <= MaxPriceTick && seen < depthLevels; ++p)
                if (asks_[p].totalQty > 0) { total += asks_[p].totalQty; ++seen; }
        }
        return total;
    }

    bool hasOrder(OrderId id) const { return lookup_.count(id) > 0; }

    // Pool diagnostics
    Idx poolUsed()     const { return poolHwm_ - freeCount_; }
    Idx poolCapacity() const { return PoolCap; }

    // Static metadata
    static constexpr uint32_t scaleFactor() { return ScaleFactor; }
    static constexpr PTick    maxPriceTick(){ return MaxPriceTick; }
    static constexpr uint32_t poolCap()     { return PoolCap; }

private:
    // ── The spine: two flat arrays indexed by raw price tick ──────────────────
    std::array<PriceLevel, MaxPriceTick + 1> bids_;
    std::array<PriceLevel, MaxPriceTick + 1> asks_;

    // ── The flat order pool: all orders in one contiguous buffer ──────────────
    std::array<OrderSlot, PoolCap>       pool_;
    Idx                                  freeHead_  = INVALID_IDX;
    Idx                                  freeCount_ = 0;
    Idx                                  poolHwm_   = 0; // high-water mark

    std::unordered_map<OrderId, OrderLookup> lookup_;
    PTick bestBid_;  // raw tick, 0 = no bid
    PTick bestAsk_;  // raw tick, MaxPriceTick+1 = no ask

    // ── Private helpers ───────────────────────────────────────────────────────

    Idx allocSlot()
    {
        if (freeHead_ != INVALID_IDX) {
            auto idx = freeHead_;
            freeHead_ = pool_[idx].next;
            --freeCount_;
            return idx;
        }
        if (poolHwm_ >= PoolCap)
            throw std::runtime_error("OrderBook pool exhausted (PoolCap=" +
                                    std::to_string(PoolCap) + ")");
        return poolHwm_++;
    }

    void freeSlot(Idx idx)
    {
        pool_[idx].active = false;
        pool_[idx].next   = freeHead_;
        freeHead_         = idx;
        ++freeCount_;
    }

    void enqueue(PriceLevel& level, Idx idx, Qty qty)
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

    void fillHead(PriceLevel& level, Qty& want, OrderId aggressor, PTick rawPx, std::vector<Trade>& out)
    {
        while (want > 0 && level.head != INVALID_IDX) {
            Idx idx  = level.head;
            OrderSlot& slot = pool_[idx];

            // Skip lazily-cancelled slots, recycle them
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

            out.push_back({slot.id, aggressor, Price::fromTick(rawPx), take});

            if (slot.qty == 0) {
                lookup_.erase(slot.id);
                level.head = slot.next;
                if (level.head == INVALID_IDX) level.tail = INVALID_IDX;
                freeSlot(idx);
            }
        }
    }
    void updateBestBid(PTick from)
    {
        for (PTick p = from; p > 0; --p)
            if (bids_[p].totalQty > 0) { bestBid_ = p; return; }
        bestBid_ = 0;
    }
    void updateBestAsk(PTick from)
    {
        for (PTick p = from; p <= MaxPriceTick; ++p)
            if (asks_[p].totalQty > 0) { bestAsk_ = p; return; }
        bestAsk_ = MaxPriceTick + 1;
    }
};
