// Runs every test file's tests. With an argument, only those whose names contain it. With
// "--group k/n", only every n-th test starting at the k-th (k from 1), so n processes can share the
// tests (ctest runs the groups side by side).
#include "test_support.hpp"

#include <charconv>

int main(int argc, char** argv) {
    std::string_view filter;
    std::size_t group = 0, groups = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--group" && i + 1 < argc) {
            const std::string_view value = argv[++i];
            const auto slash = value.find('/');
            const bool read = slash != std::string_view::npos &&
                              std::from_chars(value.data(), value.data() + slash, group).ec == std::errc{} &&
                              std::from_chars(value.data() + slash + 1, value.data() + value.size(), groups).ec == std::errc{};
            if (!read || groups == 0 || group == 0 || group > groups) {
                std::printf("--group needs k/n with 1 <= k <= n, not %.*s\n", static_cast<int>(value.size()), value.data());
                return 2;
            }
            --group;
        } else {
            filter = arg;
        }
    }
    std::vector<Test> tests;
    for (auto& t : vmCoreTests()) tests.push_back(t);
    for (auto& t : vmObjectTests()) tests.push_back(t);
    for (auto& t : vmInteractionTests()) tests.push_back(t);
    for (auto& t : vmScriptTests()) tests.push_back(t);
    for (auto& t : simulationTests()) tests.push_back(t);
    for (auto& t : routePlanningTests()) tests.push_back(t);
    for (auto& t : vmWalkingTests()) tests.push_back(t);
    for (auto& t : vmSetToNextTests()) tests.push_back(t);
    for (auto& t : vmAnimationTests()) tests.push_back(t);
    for (auto& t : formatTests()) tests.push_back(t);
    for (auto& t : modelAndImageTests()) tests.push_back(t);
    for (auto& t : sceneTests()) tests.push_back(t);
    for (auto& t : assetTests()) tests.push_back(t);
    for (auto& t : lightingTests()) tests.push_back(t);
    for (auto& t : renderPlanTests()) tests.push_back(t);
    for (auto& t : contentTests()) tests.push_back(t);
    for (auto& t : uiAndDiagTests()) tests.push_back(t);
#ifdef NEOTS2_TEXT_LAYOUT_TESTS
    for (auto& t : textLayoutTests()) tests.push_back(t);
#endif
    for (auto& t : pathsTests()) tests.push_back(t);
    for (auto& t : fuzzRegressionTests()) tests.push_back(t);

    std::size_t run = 0;
    for (std::size_t i = 0; i < tests.size(); ++i) {
        const auto& [name, test] = tests[i];
        if (i % groups != group || (!filter.empty() && std::string_view(name).find(filter) == std::string_view::npos)) {
            continue;
        }
        ++run;
        const int before = failures;
#if defined(_MSC_VER) && defined(_DEBUG)
        // In debug builds, memory a test leaves allocated is a failure.
        _CrtMemState start, end, change;
        _CrtMemCheckpoint(&start);
        test();
        _CrtMemCheckpoint(&end);
        if (_CrtMemDifference(&change, &start, &end) && change.lCounts[_NORMAL_BLOCK] > 0) {
            std::printf("  leaked %zu blocks, %zu bytes\n", change.lCounts[_NORMAL_BLOCK], change.lSizes[_NORMAL_BLOCK]);
            _CrtMemDumpAllObjectsSince(&start);
            ++failures;
        }
#else
        test();
#endif
        std::printf("%s %s\n", failures == before ? "[pass]" : "[FAIL]", name);
    }
    // A group counts only its own tests.
    std::printf("\n%zu tests, %d failed checks\n", groups == 1 ? tests.size() : run, failures);
    return failures == 0 ? 0 : 1;
}
