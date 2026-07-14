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
#include <concepts>
#include <type_traits>

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
//  BookPrice  –  shared price type used by both OrderBook<> and DynamicOrderBook
//
//  Carries both the raw integer tick and the scaleFactor so value() always
//  produces the correct double regardless of how the Price was constructed.
//
//  OrderBook<>::Price extends BookPrice with a compile-time single-argument
//  constructor for ergonomic use:  Price p(99.50);
// ─────────────────────────────────────────────────────────────────────────────
struct BookPrice {
    BookPrice() = default;
    BookPrice(double p, uint32_t sf)
    : raw_(static_cast<PTick>(std::llround(p * sf))), sf_(sf) {}

    static BookPrice fromTick(PTick tick, uint32_t sf) {
        BookPrice p; p.raw_ = tick; p.sf_ = sf; return p;
    }

    double value() const { return static_cast<double>(raw_) / sf_; }
    PTick  tick()  const { return raw_; }

    bool operator==(const BookPrice& o) const { return raw_ == o.raw_; }
    auto operator<=>(const BookPrice& o) const { return raw_ <=> o.raw_; }
    friend std::ostream& operator<<(std::ostream& os, const BookPrice& p) {
        return os << p.value();
    }

protected:
    // Protected so that OrderBook<>::Price can set both fields in its fromTick.
    PTick    raw_ = 0;
    uint32_t sf_  = 1;
};

// ── Helper concept used by the container checks below ────────────────────────
//  True when C (possibly a reference type) is a container whose value_type is T.
template<typename C, typename T>
concept container_of =
    std::is_same_v<typename std::remove_reference_t<C>::value_type, T>;

// ─────────────────────────────────────────────────────────────────────────────
//  OrderBookImpl concept  –  guards the CRTP contract for OrderBookBase<Derived>
//
//  A type D satisfies OrderBookImpl when it exposes the three sizing queries
//  that OrderBookBase needs to drive its algorithms.  The private container
//  members (bids_, asks_, pool_) are verified separately inside the
//  OrderBookBase constructor where friend access makes them reachable.
//
//  NOTE: the concept cannot be used directly as a template-parameter constraint
//  on OrderBookBase because Derived is an incomplete type at the point the base
//  is named in the derived class's base-list (standard CRTP limitation).  It is
//  therefore applied via static_assert inside the protected constructor, which
//  is instantiated only after Derived is fully defined.
// ─────────────────────────────────────────────────────────────────────────────
template<typename D>
concept OrderBookImpl = requires(const D& cd) {
    { cd.scaleFactor()  } -> std::convertible_to<uint32_t>;
    { cd.maxPriceTick() } -> std::convertible_to<PTick>;
    { cd.poolCap()      } -> std::convertible_to<uint32_t>;
};

// ─────────────────────────────────────────────────────────────────────────────
//  OrderBookBase<Derived>  –  CRTP mixin that owns all order-matching logic
//
//  Every method is written once here.  The two concrete classes below differ
//  only in their storage (std::array vs std::vector) and whether their sizing
//  parameters are compile-time constants or runtime values.
//
//  Contract on Derived
//  ───────────────────
//  • Declares  friend class OrderBookBase<Derived>;  so this base can reach
//    the private containers bids_, asks_, pool_.
//  • Exposes   scaleFactor()  maxPriceTick()  poolCap()  (static constexpr
//    or const instance methods – both work because they are called on the
//    derived instance via D()).
//  • bids_ and asks_ must be random-access containers of PriceLevel.
//  • pool_ must be a random-access container of OrderSlot.
//  • Sets  this->bestAsk_ = maxPriceTick() + 1  in its constructor.
// ─────────────────────────────────────────────────────────────────────────────
template<class Derived>
class OrderBookBase {
    Derived&       D()       { return static_cast<Derived&>(*this); }
    const Derived& D() const { return static_cast<const Derived&>(*this); }

protected:
    // ── CRTP contract guard ───────────────────────────────────────────────────
    //  Checked here rather than on the template parameter because Derived is an
    //  incomplete type when OrderBookBase<Derived> is first named in the base-
    //  list.  By the time this constructor runs, Derived is fully defined and
    //  the friend declaration grants access to its private containers.
    OrderBookBase() {
        static_assert(OrderBookImpl<Derived>,
            "Derived must expose scaleFactor(), maxPriceTick(), and poolCap()");

        // Verify that bids_, asks_, pool_ are containers of the expected types.
        // (Accessible here because Derived declares  friend OrderBookBase<Derived>.)
        static_assert(container_of<decltype(std::declval<Derived>().bids_), PriceLevel>,
            "Derived::bids_ must be a container of PriceLevel");
        static_assert(container_of<decltype(std::declval<Derived>().asks_), PriceLevel>,
            "Derived::asks_ must be a container of PriceLevel");
        static_assert(container_of<decltype(std::declval<Derived>().pool_), OrderSlot>,
            "Derived::pool_ must be a container of OrderSlot");
    }
    // ── Shared state ──────────────────────────────────────────────────────────
    std::unordered_map<OrderId, OrderLookup> lookup_;
    PTick bestBid_   = 0;
    PTick bestAsk_   = 0;   // initialised to maxPriceTick()+1 in each derived ctor
    Idx   freeHead_  = INVALID_IDX;
    Idx   freeCount_ = 0;
    Idx   poolHwm_   = 0;

public:
    using Price = BookPrice;

    struct Trade {
        OrderId   passiveId;
        OrderId   aggressorId;
        BookPrice price;
        Qty       qty;
    };

    // ── Core API ──────────────────────────────────────────────────────────────

    std::vector<Trade> addOrder(OrderId id, Side side, double price, Qty qty)
    {
        return addOrder(id, side, BookPrice(price, D().scaleFactor()), qty);
    }

    std::vector<Trade> addOrder(OrderId id, Side side, BookPrice price, Qty qty)
    {
        if (price.tick() > D().maxPriceTick()) throw std::out_of_range(
            "Price tick " + std::to_string(price.tick()) +
            " exceeds maxPriceTick " + std::to_string(D().maxPriceTick()));
        if (qty == 0)          throw std::invalid_argument("qty must be > 0");
        if (lookup_.count(id)) throw std::invalid_argument("duplicate order id");

        std::vector<Trade> trades;
        const bool market = price.tick() == 0;
        const PTick matchPrice = market && side == Side::Buy
            ? D().maxPriceTick()
            : price.tick();

        if (side == Side::Buy) {
            while (qty > 0 && bestAsk_ <= matchPrice) {
                fillHead(D().asks_[bestAsk_], qty, id, bestAsk_, trades);
                if (D().asks_[bestAsk_].totalQty == 0) updateBestAsk(bestAsk_);
            }
            if (qty > 0 && !market) {
                auto idx = allocSlot();
                D().pool_[idx] = {id, price.tick(), qty, side};
                enqueue(D().bids_[price.tick()], idx, qty);
                lookup_[id] = {idx, price.tick(), side};
                if (price.tick() > bestBid_) bestBid_ = price.tick();
            }
        } else {
            while (qty > 0 && bestBid_ >= matchPrice && bestBid_ > 0) {
                fillHead(D().bids_[bestBid_], qty, id, bestBid_, trades);
                if (D().bids_[bestBid_].totalQty == 0) updateBestBid(bestBid_);
            }
            if (qty > 0 && !market) {
                auto idx = allocSlot();
                D().pool_[idx] = {id, price.tick(), qty, side};
                enqueue(D().asks_[price.tick()], idx, qty);
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
        OrderSlot&         slot = D().pool_[ol.poolIdx];

        if (ol.side == Side::Buy) {
            D().bids_[ol.rawPrice].totalQty -= slot.qty;
            if (D().bids_[ol.rawPrice].totalQty == 0 && ol.rawPrice == bestBid_)
                updateBestBid(ol.rawPrice);
        } else {
            D().asks_[ol.rawPrice].totalQty -= slot.qty;
            if (D().asks_[ol.rawPrice].totalQty == 0 && ol.rawPrice == bestAsk_)
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

        OrderSlot& slot = D().pool_[it->second.poolIdx];
        if (reduceBy >= slot.qty) return false;

        slot.qty -= reduceBy;
        if (it->second.side == Side::Buy)
            D().bids_[it->second.rawPrice].totalQty -= reduceBy;
        else
            D().asks_[it->second.rawPrice].totalQty -= reduceBy;
        return true;
    }

    // ── Queries ───────────────────────────────────────────────────────────────

    std::optional<BookPrice> bestBid() const {
        return bestBid_ > 0
            ? std::optional<BookPrice>(BookPrice::fromTick(bestBid_, D().scaleFactor()))
            : std::nullopt;
    }
    std::optional<BookPrice> bestAsk() const {
        return bestAsk_ <= D().maxPriceTick()
            ? std::optional<BookPrice>(BookPrice::fromTick(bestAsk_, D().scaleFactor()))
            : std::nullopt;
    }
    std::optional<BookPrice> midPrice() const {
        if (bestBid_ > 0 && bestAsk_ <= D().maxPriceTick())
            return BookPrice::fromTick((bestBid_ + bestAsk_) / 2, D().scaleFactor());
        return std::nullopt;
    }
    std::optional<BookPrice> spread() const {
        if (bestBid_ > 0 && bestAsk_ <= D().maxPriceTick())
            return BookPrice::fromTick(bestAsk_ - bestBid_, D().scaleFactor());
        return std::nullopt;
    }

    Qty qtyAtPrice(Side side, BookPrice p) const {
        if (p.tick() > D().maxPriceTick()) return 0;
        return side == Side::Buy ? D().bids_[p.tick()].totalQty
                                 : D().asks_[p.tick()].totalQty;
    }

    Qty marketDepth(Side side, PTick depthLevels) const
    {
        Qty total = 0; PTick seen = 0;
        if (side == Side::Buy) {
            for (PTick p = bestBid_; p > 0 && seen < depthLevels; --p)
                if (D().bids_[p].totalQty > 0) { total += D().bids_[p].totalQty; ++seen; }
        } else {
            for (PTick p = bestAsk_; p <= D().maxPriceTick() && seen < depthLevels; ++p)
                if (D().asks_[p].totalQty > 0) { total += D().asks_[p].totalQty; ++seen; }
        }
        return total;
    }

    bool hasOrder(OrderId id) const { return lookup_.count(id) > 0; }

    Idx poolUsed()     const { return poolHwm_ - freeCount_; }
    Idx poolCapacity() const { return D().poolCap(); }

private:
    // ── Helpers ───────────────────────────────────────────────────────────────

    Idx allocSlot()
    {
        if (freeHead_ != INVALID_IDX) {
            auto idx  = freeHead_;
            freeHead_ = D().pool_[idx].next;
            --freeCount_;
            return idx;
        }
        if (poolHwm_ >= D().poolCap())
            throw std::runtime_error("OrderBook pool exhausted (poolCap=" +
                                     std::to_string(D().poolCap()) + ")");
        return poolHwm_++;
    }

    void freeSlot(Idx idx)
    {
        D().pool_[idx].active = false;
        D().pool_[idx].next   = freeHead_;
        freeHead_             = idx;
        ++freeCount_;
    }

    void enqueue(PriceLevel& level, Idx idx, Qty qty)
    {
        D().pool_[idx].next = INVALID_IDX;
        if (level.tail == INVALID_IDX) {
            level.head = level.tail = idx;
        } else {
            D().pool_[level.tail].next = idx;
            level.tail                 = idx;
        }
        level.totalQty += qty;
    }

    void fillHead(PriceLevel& level, Qty& want, OrderId aggressor,
                  PTick rawPx, std::vector<Trade>& out)
    {
        while (want > 0 && level.head != INVALID_IDX) {
            Idx        idx  = level.head;
            OrderSlot& slot = D().pool_[idx];

            // Skip lazily-cancelled slots and recycle them.
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

            out.push_back({slot.id, aggressor,
                           BookPrice::fromTick(rawPx, D().scaleFactor()), take});

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
            if (D().bids_[p].totalQty > 0) { bestBid_ = p; return; }
        bestBid_ = 0;
    }
    void updateBestAsk(PTick from)
    {
        for (PTick p = from; p <= D().maxPriceTick(); ++p)
            if (D().asks_[p].totalQty > 0) { bestAsk_ = p; return; }
        bestAsk_ = D().maxPriceTick() + 1;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  OrderBook<MaxPrice, ScaleFactor, PoolCap>
//
//  Compile-time–parameterised book.  Spine and pool are std::array – sizes are
//  baked in at compile time so there is no per-book heap allocation beyond the
//  object itself.
//  ⚠  Because bids_/asks_/pool_ are large, always heap-allocate:
//     auto ob = std::make_unique<OrderBook<...>>();
// ─────────────────────────────────────────────────────────────────────────────
template<uint32_t MaxPrice, uint32_t ScaleFactor, uint32_t PoolCap = 1'000'000>
class OrderBook : public OrderBookBase<OrderBook<MaxPrice, ScaleFactor, PoolCap>> {
    using Base = OrderBookBase<OrderBook>;
    friend Base;   // grants Base access to private bids_, asks_, pool_

public:
    constexpr static PTick MaxPriceTick = MaxPrice * ScaleFactor;
    static_assert(MaxPriceTick < UINT32_MAX);

    // ── Convenience Price with an implicit compile-time ScaleFactor ───────────
    //  Extends BookPrice so it is accepted anywhere BookPrice is expected.
    //  The single-argument constructor lets callers write  Price p(99.50);
    struct Price : BookPrice {
        Price() = default;
        explicit Price(double p) : BookPrice(p, ScaleFactor) {}
        // Single-argument fromTick for callers that know the scale at compile time.
        static Price fromTick(PTick tick) {
            Price p; p.raw_ = tick; p.sf_ = ScaleFactor; return p;
        }
    };

    // ── Constructor ───────────────────────────────────────────────────────────
    OrderBook() { this->bestAsk_ = MaxPriceTick + 1; }

    // ── Static metadata ───────────────────────────────────────────────────────
    static constexpr uint32_t scaleFactor()  { return ScaleFactor; }
    static constexpr PTick    maxPriceTick() { return MaxPriceTick; }
    static constexpr uint32_t poolCap()      { return PoolCap; }

private:
    std::array<PriceLevel, MaxPriceTick + 1> bids_;
    std::array<PriceLevel, MaxPriceTick + 1> asks_;
    std::array<OrderSlot,  PoolCap>          pool_;
};

// ─────────────────────────────────────────────────────────────────────────────
//  DynamicOrderBook
//
//  Runtime-parameterised book.  Spine and pool are std::vector – sizes are
//  determined at construction time so the engine can create a book from last
//  price and tick size received from market data:
//
//    scaleFactor = round(1 / tickSize)        e.g. tickSize 0.01  → 100
//    maxPrice    = ceil(lastPrice * headroom)  e.g. lastPrice 500  → 2500
//
//    auto book = std::make_unique<DynamicOrderBook>(maxPrice, scaleFactor);
//
//  One heap allocation is made in the constructor; no further allocations
//  during order matching.  The public API is identical to OrderBook<>.
// ─────────────────────────────────────────────────────────────────────────────
class DynamicOrderBook : public OrderBookBase<DynamicOrderBook> {
    using Base = OrderBookBase<DynamicOrderBook>;
    friend Base;   // grants Base access to private bids_, asks_, pool_

public:
    using Price = BookPrice;   // runtime sf is embedded in every BookPrice instance

    // ── Constructor ───────────────────────────────────────────────────────────
    //  maxPrice    – ceiling price value (pre-scale), e.g. 2500
    //  scaleFactor – double × scaleFactor → integer tick, e.g. 100
    //  poolCap     – max simultaneous live orders
    DynamicOrderBook(uint32_t maxPrice, uint32_t scaleFactor, uint32_t poolCap = 1'000'000)
    : scaleFactor_(scaleFactor)
    , maxPriceTick_(computeMaxTick(maxPrice, scaleFactor))
    , poolCap_(poolCap)
    , bids_(maxPriceTick_ + 1)
    , asks_(maxPriceTick_ + 1)
    , pool_(poolCap)
    { this->bestAsk_ = maxPriceTick_ + 1; }

    // ── Runtime metadata ──────────────────────────────────────────────────────
    uint32_t scaleFactor()  const { return scaleFactor_; }
    PTick    maxPriceTick() const { return maxPriceTick_; }
    uint32_t poolCap()      const { return poolCap_; }

private:
    uint32_t scaleFactor_;
    PTick    maxPriceTick_;
    uint32_t poolCap_;

    std::vector<PriceLevel> bids_;
    std::vector<PriceLevel> asks_;
    std::vector<OrderSlot>  pool_;

    static PTick computeMaxTick(uint32_t maxPrice, uint32_t scaleFactor) {
        uint64_t t = static_cast<uint64_t>(maxPrice) * scaleFactor;
        if (t >= UINT32_MAX)
            throw std::invalid_argument(
                "maxPrice(" + std::to_string(maxPrice) +
                ") * scaleFactor(" + std::to_string(scaleFactor) +
                ") overflows uint32_t");
        return static_cast<PTick>(t);
    }
};
