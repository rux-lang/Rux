#include "OptimizerTestSupport.h"
#include "Parallel/WorkerPool.h"

#include <atomic>
#include <format>
#include <map>
#include <stdexcept>

using namespace Rux;
using namespace Rux::Testing::OptimizerTestSupport;

namespace {
/// Everything the optimizer may change in a package, spelled out so two runs can be compared.
std::string Fingerprint(const LirPackage &package) {
    std::string text;
    for (const auto &module : package.modules) {
        for (const auto &function : module.funcs) {
            text += std::format("func {} {}\n", function.name, function.blocks.size());
            for (const auto &block : function.blocks) {
                text += std::format("  {}\n", block.label);
                for (const auto &instruction : block.instrs) {
                    text += std::format("    {} {} {}", static_cast<int>(instruction.op), instruction.dst,
                                        instruction.strArg);
                    for (const auto source : instruction.srcs)
                        text += std::format(" {}", source);
                    for (const auto &[source, predecessor] : instruction.phiPreds)
                        text += std::format(" [{} {}]", source, predecessor);
                    text += '\n';
                }
            }
        }
    }
    return text;
}

LirFunc EmptyFunction(std::string name) {
    LirFunc function;
    function.name = std::move(name);
    function.blocks.resize(1);
    function.blocks[0].label = "entry";
    LirTerminator terminator;
    terminator.kind = LirTermKind::Return;
    function.blocks[0].term = std::move(terminator);
    return function;
}

/// Changes each function a set number of times, then leaves it alone, and counts the functions it was shown.
class CountdownPass final : public Optimization::LirFunctionPass {
public:
    explicit CountdownPass(std::map<std::string, int> inputRemaining)
        : remaining(std::move(inputRemaining)) {
        for (const auto &[name, count] : remaining)
            visits[name] = 0;
    }

    [[nodiscard]] std::string_view Name() const noexcept override {
        return "countdown";
    }

    [[nodiscard]] Optimization::PassChange RunFunction(LirFunc &function, std::size_t,
                                                       const Optimization::PassContext &) const override {
        ++visits.at(function.name);
        auto &count = remaining.at(function.name);
        if (count == 0)
            return Optimization::PassChange::None;
        --count;
        function.blocks[0].instrs.push_back(LirInstr{});
        return Optimization::PassChange::Changed;
    }

    mutable std::map<std::string, int> remaining;
    mutable std::map<std::string, std::atomic<int>> visits;
};
} // namespace

TEST_CASE("worker pools run every index once on any number of threads") {
    for (const std::size_t jobs : {std::size_t{1}, std::size_t{4}}) {
        Parallel::WorkerPool pool(jobs);
        CHECK(pool.Jobs() == jobs);
        for (int repeat = 0; repeat < 3; ++repeat) {
            std::vector<std::atomic<int>> hits(1000);
            pool.ForEach(hits.size(), [&](const std::size_t index) { ++hits[index]; });
            CHECK(std::ranges::all_of(hits, [](const auto &hit) { return hit.load() == 1; }));
        }
        pool.ForEach(0, [](std::size_t) { FAIL("an empty loop runs no body"); });
    }
}

TEST_CASE("worker pools rethrow a body's exception after the loop drains") {
    Parallel::WorkerPool pool(4);
    std::atomic<int> started = 0;
    CHECK_THROWS_AS(pool.ForEach(64,
                                 [&](const std::size_t index) {
                                     ++started;
                                     if (index == 3)
                                         throw std::runtime_error("body failed");
                                 }),
                    std::runtime_error);
    CHECK(started.load() >= 1);
    std::atomic<int> after = 0;
    pool.ForEach(8, [&](std::size_t) { ++after; });
    CHECK(after.load() == 8);
}

TEST_CASE("LIR function passes skip functions that settled in an earlier iteration") {
    LirModule module;
    for (const auto *name : {"Settled", "Twice", "Four"})
        module.funcs.push_back(EmptyFunction(name));
    LirPackage package;
    package.modules.push_back(std::move(module));

    Parallel::WorkerPool pool(4);
    Optimization::LirPassPipeline pipeline(BuildProfile::Release, 8);
    auto pass = std::make_unique<CountdownPass>(std::map<std::string, int>{{"Settled", 0}, {"Twice", 2}, {"Four", 4}});
    auto *countdown = pass.get();
    pipeline.Add(std::move(pass));
    pipeline.SetWorkerPool(&pool);
    const auto report = pipeline.Run(package);

    CHECK(report.reachedFixedPoint);
    CHECK(report.iterations == 5);
    // A function is shown again in the iteration after its last change, which proves it settled.
    CHECK(countdown->visits.at("Settled").load() == 1);
    CHECK(countdown->visits.at("Twice").load() == 3);
    CHECK(countdown->visits.at("Four").load() == 5);
    CHECK(package.modules[0].funcs[1].blocks[0].instrs.size() == 2);
    CHECK(package.modules[0].funcs[2].blocks[0].instrs.size() == 4);
}

TEST_CASE("HIR-to-LIR lowering produces the same package on one thread and on many") {
    // Several modules, so the lowering has modules to spread across threads; helper requests and the panic printer
    // must come out in the order a serial lowering produces them.
    std::vector<std::string> sources;
    for (int index = 0; index < 6; ++index) {
        sources.push_back(std::format(R"(
            const Limit{0}: uint32 = {0}u32;
            func Scale{0}(x: float32, y: float64) -> float64 {{ return (x as float64) * y + {0}.5; }}
            func Check{0}(values: uint32[..], index: uint) -> uint32 {{ return values[index] / (Limit{0} + 1u32); }}
        )",
                                      index));
    }
    const auto build = [&] {
        HirPackage package;
        for (const auto &source : sources) {
            auto module = CompileToHir(source);
            REQUIRE(module.modules.size() == 1);
            package.modules.push_back(std::move(module.modules[0]));
        }
        return package;
    };

    HirToLirLowering serialLowering(build(), TargetContext::CreateNative());
    const auto serial = serialLowering.Generate();
    Parallel::WorkerPool pool(4);
    HirToLirLowering parallelLowering(build(), TargetContext::CreateNative());
    parallelLowering.SetWorkerPool(&pool);
    const auto parallel = parallelLowering.Generate();

    CHECK(serialLowering.Diagnostics().size() == parallelLowering.Diagnostics().size());
    REQUIRE(serial.modules.size() == 6);
    CHECK(Fingerprint(serial) == Fingerprint(parallel));
}

TEST_CASE("the release LIR pipeline produces the same package on one thread and on many") {
    const auto source = R"(
        const Table: uint32[..] = [3u32, 1u32, 4u32, 1u32, 5u32, 9u32, 2u32, 6u32];
        func Mix(x: uint32, y: uint32) -> uint32 { return (x << 5) ^ (y >> 3) ^ (x + y); }
        func Sum(n: uint32) -> uint32 {
            var total: uint32 = 0;
            for i in 0u32..n { total = Mix(total, i) + Table[(i & 7) as uint]; }
            return total;
        }
        func Choose(flag: bool, n: uint32) -> uint32 {
            if flag { return Sum(n); }
            return Mix(n, 7u32);
        }
        func Fold(n: uint32) -> uint32 {
            var value: uint32 = 1;
            var i: uint32 = 0;
            while i < n { value = Choose(i % 2u32 == 0u32, value); i += 1; }
            return value;
        }
    )";
    auto serial = CompileToLir(source, BuildProfile::Debug);
    auto parallel = CompileToLir(source, BuildProfile::Debug);

    auto serialPipeline = Optimization::OptimizationPipeline::ForProfile(BuildProfile::Release);
    const auto serialReport = serialPipeline.RunLir(serial);
    Parallel::WorkerPool pool(8);
    auto parallelPipeline = Optimization::OptimizationPipeline::ForProfile(BuildProfile::Release);
    parallelPipeline.SetWorkerPool(&pool);
    const auto parallelReport = parallelPipeline.RunLir(parallel);

    REQUIRE(serialReport.reachedFixedPoint);
    REQUIRE(parallelReport.reachedFixedPoint);
    CHECK(serialReport.iterations == parallelReport.iterations);
    CHECK(Fingerprint(serial) == Fingerprint(parallel));
}
