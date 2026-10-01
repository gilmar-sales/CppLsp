#include <benchmark/benchmark.h>

#include <CppLsp/Arena.hpp>
#include <CppLsp/Buffer.hpp>
#include <CppLsp/LineTable.hpp>

#include <filesystem>
#include <string>

namespace
{

// Simulates CST-node-sized traffic: many small aligned allocations,
// arena reset between iterations (per-file / per-TU lifetime).
void BM_ArenaAlloc64B(benchmark::State& state)
{
    cpplsp::Arena arena;
    for (auto _ : state)
    {
        for (int i = 0; i < 4096; ++i)
        {
            benchmark::DoNotOptimize(arena.Allocate(64, 8));
        }
        state.PauseTiming();
        arena.Reset();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * 4096);
}

BENCHMARK(BM_ArenaAlloc64B);

void BM_LineTableBuild(benchmark::State& state)
{
    std::string corpus;
    for (const auto& entry : std::filesystem::directory_iterator(CPPLSP_CORPUS_DIR))
    {
        if (entry.is_regular_file())
        {
            auto buffer = cpplsp::MappedBuffer::Open(entry.path().string());
            if (!buffer)
            {
                state.SkipWithError(buffer.error().c_str());
                return;
            }
            corpus.append(buffer->view());
        }
    }
    if (corpus.empty())
    {
        state.SkipWithError("empty corpus");
        return;
    }

    cpplsp::LineTable table;
    for (auto _ : state)
    {
        table.Build(corpus);
        benchmark::DoNotOptimize(table.LineCount());
    }
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * corpus.size()));
}

BENCHMARK(BM_LineTableBuild);

} // namespace
