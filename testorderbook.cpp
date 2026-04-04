#include <benchmark/benchmark.h>
#include "orderbook.hpp"
#include <random>
#include <cmath>
#include <memory>

// ─────────────────────────────────────────────────────────────────────────────
//  Instantiate the book once so the type alias is readable everywhere.
//  ScaleFactor=100 → prices in whole cents  ($99.50 → tick 9950)
// ─────────────────────────────────────────────────────────────────────────────

using Book  = OrderBook<200'000, 500'000, 100>;
using Price = Book::Price;
using Trade = Book::Trade;

static constexpr double MID   = 500.00;   // $500.00
static constexpr double RANGE =   2.00;   // ±$2.00 = ±200 ticks

// Snap a double to the nearest cent (avoids rounding noise in Price ctor)
static inline double snapCent(double p) {
    return std::round(p * 100.0) / 100.0;
}

// Pre-generated test vectors (built once per process)
struct TestData {
    std::vector<OrderId> ids;
    std::vector<Side>    sides;
    std::vector<Price>   prices;
    std::vector<Qty>     qtys;

    TestData(int n, uint32_t seed = 42) {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<double> pDist(MID - RANGE, MID + RANGE);
        std::uniform_int_distribution<Qty>     qDist(1, 100);
        std::uniform_int_distribution<int>     sDist(0, 1);
        ids.resize(n); sides.resize(n); prices.resize(n); qtys.resize(n);
        for (int i = 0; i < n; ++i) {
            ids[i]    = i + 1;
            sides[i]  = sDist(rng) ? Side::Buy : Side::Sell;
            prices[i] = Price(snapCent(pDist(rng)));
            qtys[i]   = qDist(rng);
        }
    }
};

static const TestData& testData(int n) {
    static std::unordered_map<int, TestData> cache;
    auto it = cache.find(n);
    if (it == cache.end()) it = cache.emplace(n, TestData(n)).first;
    return it->second;
}

// ── BM_AddOrder: mixed add + matching (realistic) ────────────────────────────

static void BM_AddOrder(benchmark::State& state)
{
    const int N  = static_cast<int>(state.range(0));
    const auto& d = testData(N);

    for (auto _ : state) {
        state.PauseTiming();
        auto ob = std::make_unique<Book>();
        // Seed some resting liquidity so matching occurs
        for (int i = 0; i < std::min(N / 10, 500); ++i) {
            Side  s = (i % 2 == 0) ? Side::Buy : Side::Sell;
            Price p = Price((s == Side::Buy) ? MID - 0.20 : MID + 0.20);
            ob->addOrder(2'000'000 + i, s, p, 50);
        }
        state.ResumeTiming();

        for (int i = 0; i < N; ++i)
            benchmark::DoNotOptimize(
                ob->addOrder(d.ids[i], d.sides[i], d.prices[i], d.qtys[i]));
    }
    state.SetItemsProcessed(state.iterations() * N);
}
BENCHMARK(BM_AddOrder)
    ->Arg(10'000)
    ->Unit(benchmark::kNanosecond)
    ->Repetitions(5)
    ->DisplayAggregatesOnly(true);

// ── BM_AddOrder_NoMatch: pure insert, no crossing orders ─────────────────────

static void BM_AddOrder_NoMatch(benchmark::State& state)
{
    const int N = static_cast<int>(state.range(0));

    // Bids far below mid, asks far above → guaranteed no match
    std::vector<std::tuple<OrderId,Side,Price,Qty>> orders;
    orders.reserve(N);
    {
        std::mt19937 rng(7);
        std::uniform_int_distribution<Qty> qDist(1, 50);
        for (int i = 0; i < N; ++i) {
            Side  s = (i % 2 == 0) ? Side::Buy : Side::Sell;
            Price p = Price((s == Side::Buy) ? MID - 20.0 - (i % 100) * 0.01
                                             : MID + 20.0 + (i % 100) * 0.01);
            orders.emplace_back(i + 1, s, p, qDist(rng));
        }
    }

    for (auto _ : state) {
        state.PauseTiming();
        auto ob = std::make_unique<Book>();
        state.ResumeTiming();

        for (auto& [id, s, p, q] : orders)
            benchmark::DoNotOptimize(ob->addOrder(id, s, p, q));
    }
    state.SetItemsProcessed(state.iterations() * N);
}
BENCHMARK(BM_AddOrder_NoMatch)
    ->Arg(10'000)
    ->Unit(benchmark::kNanosecond)
    ->Repetitions(5)
    ->DisplayAggregatesOnly(true);

// ── BM_CancelOrder: cancel resting orders in random order ────────────────────

static void BM_CancelOrder(benchmark::State& state)
{
    const int N = static_cast<int>(state.range(0));

    for (auto _ : state) {
        state.PauseTiming();
        auto ob = std::make_unique<Book>();
        std::vector<OrderId> ids(N);
        for (int i = 0; i < N; ++i) {
            Side  s = (i % 2 == 0) ? Side::Buy : Side::Sell;
            Price p = Price((s == Side::Buy) ? MID - 0.20 : MID + 0.20);
            ob->addOrder(i + 1, s, p, 10);
            ids[i] = i + 1;
        }
        std::shuffle(ids.begin(), ids.end(), std::mt19937(13));
        state.ResumeTiming();

        for (OrderId id : ids)
            benchmark::DoNotOptimize(ob->cancelOrder(id));
    }
    state.SetItemsProcessed(state.iterations() * N);
}
BENCHMARK(BM_CancelOrder)
    ->Arg(10'000)
    ->Unit(benchmark::kNanosecond)
    ->Repetitions(5)
    ->DisplayAggregatesOnly(true);

// ── BM_MarketSweep: one aggressive order sweeps N resting levels ──────────────

static void BM_MarketSweep(benchmark::State& state)
{
    const int N = static_cast<int>(state.range(0));

    for (auto _ : state) {
        state.PauseTiming();
        auto ob = std::make_unique<Book>();
        // Place N sell levels 1 cent apart starting at MID+0.01
        for (int i = 0; i < N; ++i)
            ob->addOrder(i + 1, Side::Sell, Price(MID + 0.01 * (i + 1)), 1);
        state.ResumeTiming();

        // One buy sweeps all N levels
        benchmark::DoNotOptimize(
            ob->addOrder(999'999, Side::Buy, Price(MID + 0.01 * (N + 1)), (Qty)N));
    }
    state.SetItemsProcessed(state.iterations() * N);
}
BENCHMARK(BM_MarketSweep)
    ->Arg(500)
    ->Unit(benchmark::kNanosecond)
    ->Repetitions(5)
    ->DisplayAggregatesOnly(true);

// ── BM_BestBidAsk: hot-path integer load ─────────────────────────────────────

static void BM_BestBidAsk(benchmark::State& state)
{
    auto ob = std::make_unique<Book>();
    ob->addOrder(1, Side::Buy,  Price(MID - 0.10), 100);
    ob->addOrder(2, Side::Sell, Price(MID + 0.10), 100);

    for (auto _ : state) {
        benchmark::DoNotOptimize(ob->bestBid());
        benchmark::DoNotOptimize(ob->bestAsk());
    }
}
BENCHMARK(BM_BestBidAsk)
    ->Unit(benchmark::kNanosecond)
    ->Repetitions(5)
    ->DisplayAggregatesOnly(true);

BENCHMARK_MAIN();
