#include "orderbook.hpp"

#include <chrono>
#include <iostream>
#include <random>
#include <cassert>
#include <numeric>

#include <benchmark/benchmark.h>

static constexpr Price MID   = 50'000;
static constexpr Price RANGE = 200;

struct TestOrders {
    std::vector<OrderId> ids;
    std::vector<Side>    sides;
    std::vector<Price>   prices;
    std::vector<Qty>     qtys;

    TestOrders(int n, uint32_t seed = 42) {
        std::mt19937 rng(seed);
        std::uniform_int_distribution<Price> pDist(MID - RANGE, MID + RANGE);
        std::uniform_int_distribution<Qty>   qDist(1, 100);
        std::uniform_int_distribution<int>   sDist(0, 1);
        ids.resize(n); sides.resize(n); prices.resize(n); qtys.resize(n);
        for (int i = 0; i < n; ++i) {
            ids[i]    = i + 1;
            sides[i]  = sDist(rng) ? Side::Buy : Side::Sell;
            prices[i] = pDist(rng);
            qtys[i]   = qDist(rng);
        }
    }
};

// Coefficient of variation statistic for ComputeStatistics

// ── BM_AddOrder: mixed add + matching (realistic) ────────────────────────────

static void BM_AddOrder(benchmark::State& state)
{
    const int N = static_cast<int>(state.range(0));
    TestOrders d(N);

    for (auto _ : state) {
        state.PauseTiming();
        OrderBook ob(200'000);
        for (int i = 0; i < std::min(N / 10, 500); ++i) {
            Side  s = (i % 2 == 0) ? Side::Buy : Side::Sell;
            Price p = (s == Side::Buy) ? MID - 20 : MID + 20;
            ob.addOrder(2'000'000 + i, s, p, 50);
        }
        state.ResumeTiming();

        for (int i = 0; i < N; ++i)
            benchmark::DoNotOptimize(
                ob.addOrder(d.ids[i], d.sides[i], d.prices[i], d.qtys[i]));
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
    std::vector<std::tuple<OrderId,Side,Price,Qty>> orders;
    orders.reserve(N);
    {
        std::mt19937 rng(7);
        std::uniform_int_distribution<Qty> qDist(1, 50);
        for (int i = 0; i < N; ++i) {
            Side  s = (i % 2 == 0) ? Side::Buy : Side::Sell;
            Price p = (s == Side::Buy) ? MID - 2000 - (i % 500)
                                       : MID + 2000 + (i % 500);
            orders.emplace_back(i + 1, s, p, qDist(rng));
        }
    }

    for (auto _ : state) {
        state.PauseTiming();
        OrderBook ob(200'000);
        state.ResumeTiming();

        for (auto& [id, s, p, q] : orders)
            benchmark::DoNotOptimize(ob.addOrder(id, s, p, q));
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
        OrderBook ob(200'000);
        std::vector<OrderId> ids(N);
        for (int i = 0; i < N; ++i) {
            Side  s = (i % 2 == 0) ? Side::Buy : Side::Sell;
            Price p = (s == Side::Buy) ? MID - 20 : MID + 20;
            ob.addOrder(i + 1, s, p, 10);
            ids[i] = i + 1;
        }
        std::shuffle(ids.begin(), ids.end(), std::mt19937(13));
        state.ResumeTiming();

        for (OrderId id : ids)
            benchmark::DoNotOptimize(ob.cancelOrder(id));
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
        OrderBook ob(200'000);
        for (int i = 0; i < N; ++i)
            ob.addOrder(i + 1, Side::Sell, MID + 1 + i, 1);
        state.ResumeTiming();

        benchmark::DoNotOptimize(
            ob.addOrder(999'999, Side::Buy, MID + N + 1, (Qty)N));
    }
    state.SetItemsProcessed(state.iterations() * N);
}
BENCHMARK(BM_MarketSweep)
    ->Arg(500)
    ->Unit(benchmark::kNanosecond)
    ->Repetitions(5)
    ->DisplayAggregatesOnly(true);

// ── BM_BestBidAsk: hot-path query ────────────────────────────────────────────

static void BM_BestBidAsk(benchmark::State& state)
{
    OrderBook ob(200'000);
    ob.addOrder(1, Side::Buy,  MID - 10, 100);
    ob.addOrder(2, Side::Sell, MID + 10, 100);

    for (auto _ : state) {
        benchmark::DoNotOptimize(ob.bestBid());
        benchmark::DoNotOptimize(ob.bestAsk());
    }
}
BENCHMARK(BM_BestBidAsk)
    ->Unit(benchmark::kNanosecond)
    ->Repetitions(5)
    ->DisplayAggregatesOnly(true);

BENCHMARK_MAIN();