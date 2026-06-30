#include <gtest/gtest.h>
#include "orderbook.hpp"

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────

// Reference static book with identical parameters to the dynamic book used
// in each test: maxPrice=2000, scaleFactor=100, poolCap=50000.
using RefBook  = OrderBook<2000, 100, 50000>;
using RefPrice = RefBook::Price;

static constexpr uint32_t kMaxPrice    = 2000;
static constexpr uint32_t kScale       = 100;
static constexpr uint32_t kPoolCap     = 50000;
static constexpr double   kMid         = 500.00;

static DynamicOrderBook makeDynBook() {
    return DynamicOrderBook(kMaxPrice, kScale, kPoolCap);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Construction
// ─────────────────────────────────────────────────────────────────────────────

TEST(DynamicOrderBook, ConstructionBasic) {
    DynamicOrderBook ob(1000, 100, 10000);
    EXPECT_FALSE(ob.bestBid().has_value());
    EXPECT_FALSE(ob.bestAsk().has_value());
    EXPECT_FALSE(ob.midPrice().has_value());
    EXPECT_FALSE(ob.spread().has_value());
    EXPECT_EQ(ob.poolUsed(), 0u);
    EXPECT_EQ(ob.poolCapacity(), 10000u);
}

TEST(DynamicOrderBook, ConstructionOverflowThrows) {
    // maxPrice * scaleFactor must not overflow uint32_t
    EXPECT_THROW(DynamicOrderBook(100000u, 100000u), std::invalid_argument);
}

// ─────────────────────────────────────────────────────────────────────────────
//  addOrder – basic resting
// ─────────────────────────────────────────────────────────────────────────────

TEST(DynamicOrderBook, AddBuyRestingUpdates_BestBid) {
    auto ob = makeDynBook();
    auto trades = ob.addOrder(1, Side::Buy, kMid - 0.10, 100);
    EXPECT_TRUE(trades.empty());
    ASSERT_TRUE(ob.bestBid().has_value());
    EXPECT_DOUBLE_EQ(ob.bestBid()->value(), kMid - 0.10);
    EXPECT_FALSE(ob.bestAsk().has_value());
}

TEST(DynamicOrderBook, AddSellRestingUpdates_BestAsk) {
    auto ob = makeDynBook();
    auto trades = ob.addOrder(1, Side::Sell, kMid + 0.10, 100);
    EXPECT_TRUE(trades.empty());
    ASSERT_TRUE(ob.bestAsk().has_value());
    EXPECT_DOUBLE_EQ(ob.bestAsk()->value(), kMid + 0.10);
    EXPECT_FALSE(ob.bestBid().has_value());
}

TEST(DynamicOrderBook, AddOrderPriceOutOfRangeThrows) {
    auto ob = makeDynBook();
    double tooHigh = static_cast<double>(kMaxPrice) + 1.0;
    EXPECT_THROW(ob.addOrder(1, Side::Buy, tooHigh, 10), std::out_of_range);
}

TEST(DynamicOrderBook, AddOrderZeroQtyThrows) {
    auto ob = makeDynBook();
    EXPECT_THROW(ob.addOrder(1, Side::Buy, kMid, 0), std::invalid_argument);
}

TEST(DynamicOrderBook, AddOrderDuplicateIdThrows) {
    auto ob = makeDynBook();
    ob.addOrder(1, Side::Buy, kMid - 0.10, 10);
    EXPECT_THROW(ob.addOrder(1, Side::Buy, kMid - 0.20, 10), std::invalid_argument);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Matching / trades
// ─────────────────────────────────────────────────────────────────────────────

TEST(DynamicOrderBook, FullMatch_OneTrade) {
    auto ob = makeDynBook();
    ob.addOrder(1, Side::Sell, kMid + 0.10, 50);
    auto trades = ob.addOrder(2, Side::Buy, kMid + 0.10, 50);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].passiveId,   1u);
    EXPECT_EQ(trades[0].aggressorId, 2u);
    EXPECT_EQ(trades[0].qty,         50u);
    EXPECT_DOUBLE_EQ(trades[0].price.value(), kMid + 0.10);

    // Book should be empty after full fill
    EXPECT_FALSE(ob.bestBid().has_value());
    EXPECT_FALSE(ob.bestAsk().has_value());
}

TEST(DynamicOrderBook, PartialMatch_Aggressor) {
    auto ob = makeDynBook();
    ob.addOrder(1, Side::Sell, kMid + 0.10, 30);
    auto trades = ob.addOrder(2, Side::Buy, kMid + 0.10, 50);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].qty, 30u);

    // 20 qty of aggressor should rest as bid
    ASSERT_TRUE(ob.bestBid().has_value());
    EXPECT_DOUBLE_EQ(ob.bestBid()->value(), kMid + 0.10);
    EXPECT_EQ(ob.qtyAtPrice(Side::Buy, BookPrice(kMid + 0.10, kScale)), 20u);
    EXPECT_FALSE(ob.bestAsk().has_value());
}

TEST(DynamicOrderBook, PartialMatch_Passive) {
    auto ob = makeDynBook();
    ob.addOrder(1, Side::Sell, kMid + 0.10, 80);
    auto trades = ob.addOrder(2, Side::Buy, kMid + 0.10, 50);

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].qty, 50u);

    // 30 qty should remain on ask
    ASSERT_TRUE(ob.bestAsk().has_value());
    EXPECT_DOUBLE_EQ(ob.bestAsk()->value(), kMid + 0.10);
    EXPECT_EQ(ob.qtyAtPrice(Side::Sell, BookPrice(kMid + 0.10, kScale)), 30u);
    EXPECT_FALSE(ob.bestBid().has_value());
}

TEST(DynamicOrderBook, SweepMultipleLevels) {
    auto ob = makeDynBook();
    // Three sell levels
    ob.addOrder(1, Side::Sell, kMid + 0.01, 10);
    ob.addOrder(2, Side::Sell, kMid + 0.02, 10);
    ob.addOrder(3, Side::Sell, kMid + 0.03, 10);

    auto trades = ob.addOrder(4, Side::Buy, kMid + 0.03, 30);
    ASSERT_EQ(trades.size(), 3u);

    // Book should be empty
    EXPECT_FALSE(ob.bestBid().has_value());
    EXPECT_FALSE(ob.bestAsk().has_value());
}

TEST(DynamicOrderBook, BuyBelowBestAsk_DoesNotMatch) {
    auto ob = makeDynBook();
    ob.addOrder(1, Side::Sell, kMid + 0.50, 100);
    auto trades = ob.addOrder(2, Side::Buy, kMid + 0.10, 100);
    EXPECT_TRUE(trades.empty());
    EXPECT_TRUE(ob.bestBid().has_value());
    EXPECT_TRUE(ob.bestAsk().has_value());
}

// ─────────────────────────────────────────────────────────────────────────────
//  cancelOrder
// ─────────────────────────────────────────────────────────────────────────────

TEST(DynamicOrderBook, CancelBid_RemovesFromBook) {
    auto ob = makeDynBook();
    ob.addOrder(1, Side::Buy, kMid - 0.10, 100);
    EXPECT_TRUE(ob.cancelOrder(1));
    EXPECT_FALSE(ob.bestBid().has_value());
    EXPECT_FALSE(ob.hasOrder(1));
}

TEST(DynamicOrderBook, CancelAsk_RemovesFromBook) {
    auto ob = makeDynBook();
    ob.addOrder(1, Side::Sell, kMid + 0.10, 100);
    EXPECT_TRUE(ob.cancelOrder(1));
    EXPECT_FALSE(ob.bestAsk().has_value());
}

TEST(DynamicOrderBook, CancelNonExistent_ReturnsFalse) {
    auto ob = makeDynBook();
    EXPECT_FALSE(ob.cancelOrder(999));
}

TEST(DynamicOrderBook, CancelBestLevel_UpdatesBestBid) {
    auto ob = makeDynBook();
    ob.addOrder(1, Side::Buy, kMid - 0.10, 100);
    ob.addOrder(2, Side::Buy, kMid - 0.20, 100);

    ob.cancelOrder(1);  // removes the higher bid
    ASSERT_TRUE(ob.bestBid().has_value());
    EXPECT_DOUBLE_EQ(ob.bestBid()->value(), kMid - 0.20);
}

TEST(DynamicOrderBook, CancelBestLevel_UpdatesBestAsk) {
    auto ob = makeDynBook();
    ob.addOrder(1, Side::Sell, kMid + 0.10, 100);
    ob.addOrder(2, Side::Sell, kMid + 0.20, 100);

    ob.cancelOrder(1);  // removes the lower ask
    ASSERT_TRUE(ob.bestAsk().has_value());
    EXPECT_DOUBLE_EQ(ob.bestAsk()->value(), kMid + 0.20);
}

// ─────────────────────────────────────────────────────────────────────────────
//  reduceOrder
// ─────────────────────────────────────────────────────────────────────────────

TEST(DynamicOrderBook, ReduceOrder_PartialReduceSucceeds) {
    auto ob = makeDynBook();
    ob.addOrder(1, Side::Buy, kMid - 0.10, 100);
    EXPECT_TRUE(ob.reduceOrder(1, 40));
    EXPECT_EQ(ob.qtyAtPrice(Side::Buy, BookPrice(kMid - 0.10, kScale)), 60u);
}

TEST(DynamicOrderBook, ReduceOrder_ReduceToZeroReturnsFalse) {
    // reduceBy >= qty should return false (cannot fully fill via reduce)
    auto ob = makeDynBook();
    ob.addOrder(1, Side::Buy, kMid - 0.10, 100);
    EXPECT_FALSE(ob.reduceOrder(1, 100));
    EXPECT_FALSE(ob.reduceOrder(1, 200));
}

TEST(DynamicOrderBook, ReduceOrder_NonExistentReturnsFalse) {
    auto ob = makeDynBook();
    EXPECT_FALSE(ob.reduceOrder(999, 10));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Queries: midPrice, spread, qtyAtPrice, marketDepth, hasOrder
// ─────────────────────────────────────────────────────────────────────────────

TEST(DynamicOrderBook, MidPriceAndSpread) {
    auto ob = makeDynBook();
    ob.addOrder(1, Side::Buy,  kMid - 0.10, 100);
    ob.addOrder(2, Side::Sell, kMid + 0.10, 100);

    ASSERT_TRUE(ob.midPrice().has_value());
    ASSERT_TRUE(ob.spread().has_value());
    EXPECT_DOUBLE_EQ(ob.midPrice()->value(), kMid);
    EXPECT_DOUBLE_EQ(ob.spread()->value(), 0.20);
}

TEST(DynamicOrderBook, QtyAtPrice) {
    auto ob = makeDynBook();
    ob.addOrder(1, Side::Buy, kMid - 0.10, 40);
    ob.addOrder(2, Side::Buy, kMid - 0.10, 60);

    BookPrice p(kMid - 0.10, kScale);
    EXPECT_EQ(ob.qtyAtPrice(Side::Buy,  p), 100u);
    EXPECT_EQ(ob.qtyAtPrice(Side::Sell, p), 0u);
}

TEST(DynamicOrderBook, MarketDepth_BuyAndSell) {
    auto ob = makeDynBook();
    for (int i = 1; i <= 5; ++i)
        ob.addOrder(i,     Side::Buy,  kMid - i * 0.01, 10u * i);
    for (int i = 1; i <= 5; ++i)
        ob.addOrder(i + 5, Side::Sell, kMid + i * 0.01, 10u * i);

    // 3 best bid levels: 10+20+30 = 60
    EXPECT_EQ(ob.marketDepth(Side::Buy,  3), 60u);
    // 3 best ask levels: 10+20+30 = 60
    EXPECT_EQ(ob.marketDepth(Side::Sell, 3), 60u);
}

TEST(DynamicOrderBook, HasOrder) {
    auto ob = makeDynBook();
    EXPECT_FALSE(ob.hasOrder(1));
    ob.addOrder(1, Side::Buy, kMid - 0.10, 10);
    EXPECT_TRUE(ob.hasOrder(1));
    ob.cancelOrder(1);
    EXPECT_FALSE(ob.hasOrder(1));
}

TEST(DynamicOrderBook, QtyAtPriceOutOfRange) {
    auto ob = makeDynBook();
    double tooHigh = static_cast<double>(kMaxPrice) + 1.0;
    BookPrice p(tooHigh, kScale);  // tick > maxPriceTick
    EXPECT_EQ(ob.qtyAtPrice(Side::Buy, p), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Pool / slot recycling
// ─────────────────────────────────────────────────────────────────────────────

TEST(DynamicOrderBook, PoolUsedAndRecycled) {
    auto ob = makeDynBook();
    ob.addOrder(1, Side::Buy, kMid - 0.10, 10);
    ob.addOrder(2, Side::Buy, kMid - 0.20, 10);
    EXPECT_EQ(ob.poolUsed(), 2u);

    // cancelOrder uses lazy deletion: slot is marked inactive but NOT
    // immediately returned to the free-list.  poolUsed() stays at 2 until
    // a fill sweep recycles the slot.
    ob.cancelOrder(1);
    EXPECT_EQ(ob.poolUsed(), 2u);
    EXPECT_FALSE(ob.hasOrder(1));

    // A matching fill sweeps the level, recycles the inactive slot, and
    // allocSlot() reuses it.  After recycling + reallocation the net
    // poolUsed() stays at 2.
    ob.addOrder(3, Side::Sell, kMid - 0.20, 10);  // matches order 2 fully
    // order 2 was filled → slot freed → pool recycled; order 3 fully matched
    EXPECT_EQ(ob.poolUsed(), 1u);  // only the cancelled-but-not-swept slot 1 remains
}

TEST(DynamicOrderBook, PoolExhaustedThrows) {
    DynamicOrderBook ob(1000, 100, 3);  // tiny pool
    ob.addOrder(1, Side::Buy, 100.0, 1);
    ob.addOrder(2, Side::Buy, 101.0, 1);
    ob.addOrder(3, Side::Buy, 102.0, 1);
    EXPECT_THROW(ob.addOrder(4, Side::Buy, 103.0, 1), std::runtime_error);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Cross-validation: DynamicOrderBook vs OrderBook<> produce identical results
// ─────────────────────────────────────────────────────────────────────────────

TEST(DynamicOrderBook, CrossValidation_AddOrder) {
    auto dynOb = makeDynBook();
    auto refOb = std::make_unique<RefBook>();

    struct Op { OrderId id; Side side; double price; Qty qty; };
    const std::vector<Op> ops = {
        {1, Side::Buy,  kMid - 0.50, 100},
        {2, Side::Sell, kMid + 0.50, 100},
        {3, Side::Buy,  kMid + 0.50,  80},  // partial match
        {4, Side::Sell, kMid - 0.50,  80},  // partial match
        {5, Side::Buy,  kMid - 0.10,  50},
        {6, Side::Sell, kMid + 0.10,  50},
    };

    for (const auto& op : ops) {
        auto dynTrades = dynOb.addOrder(op.id, op.side, op.price, op.qty);
        auto refTrades = refOb->addOrder(op.id, op.side, RefPrice(op.price), op.qty);

        ASSERT_EQ(dynTrades.size(), refTrades.size())
            << "Trade count mismatch at orderId=" << op.id;
        for (size_t t = 0; t < dynTrades.size(); ++t) {
            EXPECT_EQ(dynTrades[t].passiveId,   refTrades[t].passiveId);
            EXPECT_EQ(dynTrades[t].aggressorId, refTrades[t].aggressorId);
            EXPECT_EQ(dynTrades[t].qty,         refTrades[t].qty);
            EXPECT_DOUBLE_EQ(dynTrades[t].price.value(), refTrades[t].price.value());
        }
    }

    // bestBid/bestAsk should match
    auto dynBid = dynOb.bestBid();
    auto refBid = refOb->bestBid();
    ASSERT_EQ(dynBid.has_value(), refBid.has_value());
    if (dynBid) EXPECT_DOUBLE_EQ(dynBid->value(), refBid->value());

    auto dynAsk = dynOb.bestAsk();
    auto refAsk = refOb->bestAsk();
    ASSERT_EQ(dynAsk.has_value(), refAsk.has_value());
    if (dynAsk) EXPECT_DOUBLE_EQ(dynAsk->value(), refAsk->value());
}

TEST(DynamicOrderBook, CrossValidation_CancelOrder) {
    auto dynOb = makeDynBook();
    auto refOb = std::make_unique<RefBook>();

    for (int i = 1; i <= 10; ++i) {
        double p = kMid + (i % 2 == 0 ? 1 : -1) * i * 0.05;
        Side   s = (i % 2 == 0) ? Side::Sell : Side::Buy;
        dynOb.addOrder(i, s, p, 10);
        refOb->addOrder(i, s, RefPrice(p), 10);
    }

    for (int i = 1; i <= 10; i += 2) {
        EXPECT_EQ(dynOb.cancelOrder(i), refOb->cancelOrder(i));
    }

    auto dynBid = dynOb.bestBid();
    auto refBid = refOb->bestBid();
    ASSERT_EQ(dynBid.has_value(), refBid.has_value());
    if (dynBid) EXPECT_DOUBLE_EQ(dynBid->value(), refBid->value());

    auto dynAsk = dynOb.bestAsk();
    auto refAsk = refOb->bestAsk();
    ASSERT_EQ(dynAsk.has_value(), refAsk.has_value());
    if (dynAsk) EXPECT_DOUBLE_EQ(dynAsk->value(), refAsk->value());
}

TEST(DynamicOrderBook, CrossValidation_MarketSweep) {
    auto dynOb = makeDynBook();
    auto refOb = std::make_unique<RefBook>();

    for (int i = 1; i <= 10; ++i) {
        double p = kMid + i * 0.01;
        dynOb.addOrder(i, Side::Sell, p, 5);
        refOb->addOrder(i, Side::Sell, RefPrice(p), 5);
    }

    auto dynTrades = dynOb.addOrder(100, Side::Buy, kMid + 0.11, 50);
    auto refTrades = refOb->addOrder(100, Side::Buy, RefPrice(kMid + 0.11), 50);

    ASSERT_EQ(dynTrades.size(), refTrades.size());
    for (size_t t = 0; t < dynTrades.size(); ++t) {
        EXPECT_EQ(dynTrades[t].passiveId,   refTrades[t].passiveId);
        EXPECT_EQ(dynTrades[t].qty,         refTrades[t].qty);
        EXPECT_DOUBLE_EQ(dynTrades[t].price.value(), refTrades[t].price.value());
    }

    EXPECT_EQ(dynOb.bestBid().has_value(), refOb->bestBid().has_value());
    EXPECT_EQ(dynOb.bestAsk().has_value(), refOb->bestAsk().has_value());
}
