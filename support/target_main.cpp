#include "checks.h"
#include <iostream>
void projection_trace(Checks&);
void depth_intervals(Checks&);
void capture_and_composite(Checks&);
void render_integration(Checks&);
int main() {
    struct Case { const char* name; void (*run)(Checks&); };
    const Case cases[] = {
        {"projection_trace", projection_trace},
        {"depth_intervals", depth_intervals},
        {"capture_and_composite", capture_and_composite},
        {"render_integration", render_integration},
    };
    try {
        for (const auto& test : cases) {
            Checks c;
            test.run(c);
            if (!c.count()) {
                std::cerr << "ZERO_ASSERTIONS " << test.name << '\n';
                return 3;
            }
            std::cout << "PASS " << test.name << " checks=" << c.count() << '\n';
        }
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
    std::cout << "SSR_CASES_OK groups=4\n";
}
