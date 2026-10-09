#include "content/install.hpp"
#include "content/resource_index.hpp"
#include "formats/resource_types.hpp"

#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: ts2index <sims2-install-folder>\n");
        return 1;
    }

    try {
        const auto start = std::chrono::steady_clock::now();
        const auto products = ts2::content::findProducts(argv[1]);

        const bool diff = argc > 2 && std::string_view(argv[2]) == "--diff";

        ts2::content::ResourceIndex index;
        std::printf("%-4s %-38s %10s %10s%s\n", "", "Load order", "Entries", "Overrides",
                    diff ? "  Identical    Changed" : "");
        std::map<std::filesystem::path, const ts2::content::Product*> productOf;
        for (const auto& product : products) {
            for (const auto& path : product.objectPackages) {
                productOf[path] = &product;
            }
        }

        for (const auto& path : ts2::content::objectLoadOrder(products)) {
            const ts2::content::Product& product = *productOf.at(path);
            {
                std::size_t identical = 0;
                std::size_t changed = 0;
                if (diff) {
                    ts2::dbpf::Package incoming{path};
                    for (const auto& entry : incoming.entries()) {
                        const auto* existing = index.find(entry.key);
                        if (existing == nullptr) {
                            continue;
                        }
                        if (index.readRaw(*existing) == incoming.readRaw(entry)) {
                            ++identical;
                        } else {
                            ++changed;
                        }
                    }
                }

                const auto before = index.stats();
                index.addPackage(path);
                const auto after = index.stats();
                std::printf("%-4s %-38s %10zu %10zu", product.code.c_str(), product.name.c_str(),
                            after.entriesSeen - before.entriesSeen,
                            after.overridden - before.overridden);
                if (diff) {
                    std::printf(" %10zu %10zu", identical, changed);
                }
                std::printf("   %s\n",
                            reinterpret_cast<const char*>(path.filename().u8string().c_str()));
            }
        }

        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start);

        std::map<std::size_t, std::size_t> winnersPerPackage;
        std::map<std::uint32_t, std::size_t> types;
        for (const auto& r : index.resources()) {
            ++winnersPerPackage[r.package];
            ++types[r.entry.key.type];
        }

        std::printf("\nPackages         : %zu\n", index.packageCount());
        std::printf("Entries read     : %zu\n", index.stats().entriesSeen);
        std::printf("Overridden       : %zu\n", index.stats().overridden);
        std::printf("Unique resources : %zu\n", index.resources().size());
        std::printf("Load time        : %.0f ms\n", elapsed.count());

        std::printf("\nResources each package still provides after overrides:\n");
        for (const auto& [package, n] : winnersPerPackage) {
            const auto folder = index.packagePath(package).parent_path().parent_path()
                                    .parent_path().parent_path().u8string();
            std::printf("  %8zu  %s\n", n, reinterpret_cast<const char*>(folder.c_str()));
        }
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
    return 0;
}
