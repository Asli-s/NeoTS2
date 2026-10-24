#include "test_support.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>
#include <string>
#include <vector>

#define FUZZ_TARGET(name) extern "C" int neots2_fuzz_##name(const std::uint8_t* data, std::size_t size);
#include "fuzz_targets.inc"
#undef FUZZ_TARGET

namespace {

namespace fs = std::filesystem;

using FuzzEntry = int (*)(const std::uint8_t*, std::size_t);

struct FuzzTarget {
    std::string_view name;
    FuzzEntry entry;
};

constexpr FuzzTarget kFuzzTargets[] = {
#define FUZZ_TARGET(name) {#name, neots2_fuzz_##name},
#include "fuzz_targets.inc"
#undef FUZZ_TARGET
};

// The harness of the fuzz target with this name, or null.
FuzzEntry fuzzEntry(std::string_view name) {
    for (const auto& target : kFuzzTargets) {
        if (target.name == name) {
            return target.entry;
        }
    }
    return nullptr;
}

// Each input in fuzz/regressions/<target> (one a fuzzer crashed, hung or ran out of memory on, kept
// after the fix) runs through that target's harness again, so the bug stays fixed. An ASan build
// (build-asan) checks them for memory errors as the fuzzer did.
void fuzzRegressionsStayFixed() {
    const fs::path root = fs::path(NEOTS2_SOURCE_DIR) / "fuzz" / "regressions";
    CHECK(fs::is_directory(root));
    std::size_t replayed = 0;
    for (const auto& folder : fs::directory_iterator(root)) {
        if (!folder.is_directory()) {
            continue;
        }
        const FuzzEntry entry = fuzzEntry(folder.path().filename().string());
        CHECK(entry != nullptr);  // each folder is named after a fuzz target
        if (entry == nullptr) {
            continue;
        }
        for (const auto& file : fs::directory_iterator(folder.path())) {
            std::ifstream in(file.path(), std::ios::binary);
            const std::vector<std::uint8_t> data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
            entry(data.data(), data.size());
            ++replayed;
        }
    }
    CHECK(replayed > 0);
}

} // namespace

std::vector<Test> fuzzRegressionTests() {
    return {{"fuzz regressions stay fixed", fuzzRegressionsStayFixed}};
}
