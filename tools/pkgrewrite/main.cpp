// pkgrewrite: saving's first step, tried on real files. Writes packages again with nothing changed
// (see content::rewritePackage), checking each against the original, so the result can be loaded
// in the game to show NeoTS2's package writing keeps saves intact.
//
//   pkgrewrite <package or folder>
//
// A folder is walked for every .package in it. Each file is replaced only after its check passes,
// and the first rewrite keeps the original beside it as "<file>.neots2bak". Run it on a COPY of a
// save folder, never on the only one.

#include "content/package_rewrite.hpp"

#include <cctype>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

bool isPackage(const fs::path& p) {
    std::string ext = p.extension().string();
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext == ".package";
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: pkgrewrite <package or folder>\n"
                             "Writes each package again unchanged and checks it. Use a COPY of a save folder.\n");
        return 2;
    }
    const fs::path target(argv[1]);
    std::vector<fs::path> files;
    std::error_code error;
    if (fs::is_directory(target, error)) {
        for (auto it = fs::recursive_directory_iterator(target, error); !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
            if (it->is_regular_file(error) && isPackage(it->path())) {
                files.push_back(it->path());
            }
        }
    } else if (fs::is_regular_file(target, error)) {
        files.push_back(target);
    }
    if (files.empty()) {
        std::fprintf(stderr, "no packages found at %s\n", target.string().c_str());
        return 1;
    }
    int failed = 0;
    std::size_t resources = 0;
    for (const fs::path& file : files) {
        try {
            const auto report = ts2::content::rewritePackage(file);
            resources += report.copied;
            std::printf("ok    %s (%zu resources%s)\n", file.string().c_str(), report.copied, report.madeBackup ? ", backed up" : "");
        } catch (const std::exception& e) {
            ++failed;
            std::printf("FAIL  %s: %s\n", file.string().c_str(), e.what());
        }
    }
    std::printf("\n%zu packages rewritten (%zu resources), %d failed\n", files.size() - static_cast<std::size_t>(failed), resources, failed);
    return failed == 0 ? 0 : 1;
}
