// Runs every test file's tests. With an argument, only those whose names contain it.
#include "test_support.hpp"

int main(int argc, char** argv) {
    const std::string_view filter = argc > 1 ? argv[1] : "";
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

    for (const auto& [name, test] : tests) {
        if (!filter.empty() && std::string_view(name).find(filter) == std::string_view::npos) {
            continue;
        }
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
    std::printf("\n%zu tests, %d failed checks\n", tests.size(), failures);
    return failures == 0 ? 0 : 1;
}
