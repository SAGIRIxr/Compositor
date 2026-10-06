#include "test.h"
#ifdef _WIN32
#include <windows.h>
#endif

std::vector<TestCase>& testRegistry() {
    static std::vector<TestCase> tests;
    return tests;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::string filter = argc > 1 ? argv[1] : "";
    int failed = 0, run = 0;
    for (const auto& test : testRegistry()) {
        if (!filter.empty() && std::string(test.name).find(filter) == std::string::npos) continue;
        ++run;
        try {
            test.body();
            std::cout << "[通过] " << test.name << "\n";
        } catch (const TestFailure& failure) {
            ++failed;
            std::cout << "[失败] " << test.name << "\n    " << failure.message << "\n";
        } catch (const std::exception& e) {
            ++failed;
            std::cout << "[失败] " << test.name << "\n    异常：" << e.what() << "\n";
        }
    }
    std::cout << "\n共 " << run << " 项，失败 " << failed << " 项。\n";
    return failed ? 1 : 0;
}
