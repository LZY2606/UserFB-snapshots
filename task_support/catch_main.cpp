// Fixed execution gate for the upstream Catch2 runner, not a semantic oracle.
#include "catch2/catch_amalgamated.hpp"
#include <cstdio>
static bool ended=false, allNonempty=true;
static unsigned long long cases=0, assertions=0;
class CompletionGate : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;
    void testCaseEnded(Catch::TestCaseStats const& s) override {
        ++cases; assertions+=s.totals.assertions.passed;
        const auto& a=s.totals.assertions;
        if(!a.passed || a.failed || a.failedButOk || a.skipped || s.aborting) allNonempty=false;
    }
    void testRunEnded(Catch::TestRunStats const& s) override {
        ended=true;
        const auto& c=s.totals.testCases;
        if(c.failed || c.failedButOk || c.skipped || s.aborting) allNonempty=false;
    }
};
CATCH_REGISTER_LISTENER(CompletionGate)
int main(int argc,char** argv) {
    const int code=Catch::Session().run(argc,argv);
    if(code || !ended || !allNonempty || !cases || !assertions) {
        std::fprintf(stderr,"TEST_RUN_INCOMPLETE code=%d cases=%llu assertions=%llu\n",code,cases,assertions);
        return code?code:79;
    }
    std::printf("TEST_RUN_COMPLETE cases=%llu assertions=%llu\n",cases,assertions);
    return 0;
}
