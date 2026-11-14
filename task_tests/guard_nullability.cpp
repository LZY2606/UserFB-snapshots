// Task tests for one-directional guard nullability contracts.
//
// Fixture semantics (see kGuardCfg below, loaded through the real XML parser):
//   ready(p)    - pure guard; when it returns true, p is not null. A false result says nothing.
//   failed(p)   - pure guard; when it returns false, p is not null. A true result says nothing.
//   ready2(a,b) - pure guard; when it returns true, both a and b are not null.
//   observe(p)  - pure observation function; used to read ValueFlow facts on p without dereferencing it.
//   check(p)    - pure boolean observer; used on the right-hand side of short-circuit operators.
//   tweak(pp)   - deliberately unconfigured (unknown) function that may reseat the pointer
//                 through a mutable handle.
// Unknown pointers are only ever passed to observe()/check(), never dereferenced. Dereferences
// only appear under a real non-null guard, so all snippets are benign.

#include "task_support/check.h"

#include "lib/check.h"
#include "checknullpointer.h"
#include "errorlogger.h"
#include "library.h"
#include "settings.h"
#include "token.h"
#include "tokenize.h"
#include "vfvalue.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

const char kGuardCfgPath[] = ".task-build/guard_nullability.cfg";

const char kGuardCfg[] =
    "<?xml version=\"1.0\"?>\n"
    "<def format=\"2\">\n"
    "  <function name=\"ready\">\n"
    "    <pure/>\n"
    "    <arg nr=\"1\" direction=\"in\"><not-null-when result=\"true\"/></arg>\n"
    "  </function>\n"
    "  <function name=\"failed\">\n"
    "    <pure/>\n"
    "    <arg nr=\"1\" direction=\"in\"><not-null-when result=\"false\"/></arg>\n"
    "  </function>\n"
    "  <function name=\"ready2\">\n"
    "    <pure/>\n"
    "    <arg nr=\"1\" direction=\"in\"><not-null-when result=\"true\"/></arg>\n"
    "    <arg nr=\"2\" direction=\"in\"><not-null-when result=\"true\"/></arg>\n"
    "  </function>\n"
    "  <function name=\"readyInt\">\n"
    "    <pure/>\n"
    "    <arg nr=\"1\" direction=\"in\"><not-null-when result=\"true\"/></arg>\n"
    "  </function>\n"
    "  <function name=\"observe\"><pure/><arg nr=\"1\" direction=\"in\"/></function>\n"
    "  <function name=\"check\"><pure/><arg nr=\"1\" direction=\"in\"/></function>\n"
    "  <function name=\"probe\"><pure/><arg nr=\"1\" direction=\"in\"/></function>\n"
    "</def>\n";

// Declarations shared by the analysed snippets so the library functions bind.
const char kPrelude[] =
    "void observe(const int*);\n"
    "bool check(const int*);\n"
    "bool ready(const int*);\n"
    "bool failed(const int*);\n"
    "bool ready2(const int*, const int*);\n"
    "bool readyInt(int);\n"
    "void probe(int);\n"
    "void tweak(const int**);\n";

struct Logger : ErrorLogger {
    std::vector<ErrorMessage> errors;
    void reportOut(const std::string&, Color) override {}
    void reportErr(const ErrorMessage& e) override { errors.push_back(e); }
};

void require(bool ok, const char* why) {
    if (!ok)
        throw std::runtime_error(why);
}

std::string writeCfg(const char* name, const std::string& xml) {
    const std::string path = std::string(".task-build/") + name;
    std::ofstream out(path.c_str());
    out << xml;
    out.close();
    require(out.good(), "cfg file written");
    return path;
}

Library::Error loadXml(Settings& settings, const char* name, const std::string& xml) {
    return settings.library.load(nullptr, writeCfg(name, xml).c_str());
}

void ensureGuardCfg() {
    static bool written = false;
    if (!written) {
        writeCfg("guard_nullability.cfg", kGuardCfg);
        written = true;
    }
    (void)kGuardCfgPath;
}

// Facts observed on the pointer argument of one observe()/check() call.
struct Observation {
    std::string callee;
    bool impossibleZero = false;   // argument carries an impossible int value 0 (cannot be null)
    bool possibleZero = false;     // argument carries a possible/known int value 0
    bool conditionIsGuardCall = false; // the impossible fact points back at the guard call
    bool errorPathToGuard = false;     // the error path contains the guard call location
};

struct Analysis {
    std::vector<Observation> observations;
    bool ranChecker = false;
    bool checkerErrors = false;
};

bool isGuardCallToken(const Token* tok) {
    return tok && tok->str() == "(" && tok->previous() &&
           (tok->previous()->str() == "ready" || tok->previous()->str() == "failed" ||
            tok->previous()->str() == "ready2");
}

Analysis analyse(const std::string& code) {
    ensureGuardCfg();
    Settings settings;
    Logger logger;
    require(settings.library.load(nullptr, kGuardCfgPath).errorcode == Library::ErrorCode::OK, "guard cfg load");
    Tokenizer tokenizer(settings, logger);
    std::istringstream input(code);
    require(tokenizer.list.createTokens(input, "benign.cpp"), "create tokens");
    require(tokenizer.simplifyTokens1(""), "actual ValueFlow run");

    Analysis result;
    for (const Token* t = tokenizer.tokens(); t; t = t->next()) {
        if ((t->str() != "observe" && t->str() != "check" && t->str() != "probe") || !Token::Match(t->next(), "( %var% )"))
            continue;
        if (!t->next()->astOperand1())
            continue;
        const Token* pointer = t->tokAt(2);
        if (!pointer->variable() || !pointer->scope() || !pointer->exprId())
            continue;
        Observation obs;
        obs.callee = t->str();
        for (const auto& value : pointer->values()) {
            if (!value.isIntValue() || value.intvalue != 0)
                continue;
            if (value.isImpossible()) {
                obs.impossibleZero = true;
                if (isGuardCallToken(value.condition))
                    obs.conditionIsGuardCall = true;
                for (const auto& loc : value.errorPath) {
                    if (isGuardCallToken(loc.first))
                        obs.errorPathToGuard = true;
                }
            } else {
                obs.possibleZero = true;
            }
        }
        result.observations.push_back(obs);
    }
    require(!result.observations.empty(), "observation argument located");

    for (Check* checker : Check::instances()) {
        if (dynamic_cast<CheckNullPointer*>(checker)) {
            checker->runChecks(tokenizer, &logger);
            result.ranChecker = true;
        }
    }
    require(result.ranChecker, "registered NullPointer checker reached");
    for (const auto& error : logger.errors) {
        if (error.severity == Severity::error)
            result.checkerErrors = true;
    }
    return result;
}

std::string guardCfgWith(const std::string& functionBody) {
    return std::string("<?xml version=\"1.0\"?><def format=\"2\"><function name=\"ready\">") +
           functionBody + "</function></def>";
}

// ---------------- contracts ----------------

int contracts_load_valid() {
    Settings settings;
    ensureGuardCfg();
    const Library::Error err = settings.library.load(nullptr, kGuardCfgPath);
    TASK_CHECK(err.errorcode == Library::ErrorCode::OK);

    const std::unordered_map<std::string, Library::Function>& funcs = settings.library.functions();
    TASK_CHECK(funcs.at("ready").argumentChecks.at(1).notNullWhen == Library::ArgumentChecks::NotNullWhen::TRUE);
    TASK_CHECK(funcs.at("failed").argumentChecks.at(1).notNullWhen == Library::ArgumentChecks::NotNullWhen::FALSE);
    TASK_CHECK(funcs.at("ready2").argumentChecks.at(1).notNullWhen == Library::ArgumentChecks::NotNullWhen::TRUE);
    TASK_CHECK(funcs.at("ready2").argumentChecks.at(2).notNullWhen == Library::ArgumentChecks::NotNullWhen::TRUE);
    // stored separately from the unconditional <not-null/> contract
    TASK_CHECK(!funcs.at("ready").argumentChecks.at(1).notnull);
    TASK_CHECK(funcs.at("observe").argumentChecks.at(1).notNullWhen == Library::ArgumentChecks::NotNullWhen::NONE);
    TASK_CHECK(funcs.at("ready").ispure);

    // the pre-existing configuration still loads
    Settings oldSettings;
    TASK_CHECK(oldSettings.library.load(nullptr, "task_support/benign.cfg").errorcode == Library::ErrorCode::OK);

    // the query API resolves the contract through real tokens, separately from isnullargbad
    Logger logger;
    Tokenizer tokenizer(settings, logger);
    std::istringstream input(std::string(kPrelude) +
                             "void f(const int* p, const int* q) {\n"
                             "    if (ready(p) && failed(q)) {\n"
                             "    }\n"
                             "}\n");
    TASK_CHECK(tokenizer.list.createTokens(input, "benign.cpp"));
    TASK_CHECK(tokenizer.simplifyTokens1(""));
    bool seenReady = false, seenFailed = false;
    for (const Token* t = tokenizer.tokens(); t; t = t->next()) {
        if (t->str() == "ready" && Token::simpleMatch(t->next(), "(")) {
            TASK_CHECK(settings.library.notNullWhen(t, 1) == Library::ArgumentChecks::NotNullWhen::TRUE);
            TASK_CHECK(!settings.library.isnullargbad(t, 1));
            seenReady = true;
        }
        if (t->str() == "failed" && Token::simpleMatch(t->next(), "(")) {
            TASK_CHECK(settings.library.notNullWhen(t, 1) == Library::ArgumentChecks::NotNullWhen::FALSE);
            seenFailed = true;
        }
    }
    TASK_CHECK(seenReady);
    TASK_CHECK(seenFailed);
    return 0;
}

int contracts_missing_result() {
    Settings settings;
    const Library::Error err = loadXml(settings, "guard_missing_result.cfg",
                                       guardCfgWith("<pure/><arg nr=\"1\"><not-null-when/></arg>"));
    TASK_CHECK(err.errorcode == Library::ErrorCode::MISSING_ATTRIBUTE);
    TASK_CHECK(!err.reason.empty());
    return 0;
}

int contracts_bad_result() {
    Settings settings;
    const Library::Error err = loadXml(settings, "guard_bad_result.cfg",
                                       guardCfgWith("<pure/><arg nr=\"1\"><not-null-when result=\"maybe\"/></arg>"));
    TASK_CHECK(err.errorcode == Library::ErrorCode::BAD_ATTRIBUTE_VALUE);
    TASK_CHECK(err.reason == "maybe");
    return 0;
}

int contracts_duplicate_relation() {
    Settings settings;
    const Library::Error err = loadXml(settings, "guard_dup1.cfg",
                                       guardCfgWith("<pure/><arg nr=\"1\"><not-null-when result=\"true\"/><not-null-when result=\"false\"/></arg>"));
    TASK_CHECK(err.errorcode == Library::ErrorCode::BAD_ATTRIBUTE_VALUE);

    Settings settings2;
    const Library::Error err2 = loadXml(settings2, "guard_dup2.cfg",
                                        guardCfgWith("<pure/><arg nr=\"1\"><not-null-when result=\"true\"/></arg><arg nr=\"1\"><not-null-when result=\"true\"/></arg>"));
    TASK_CHECK(err2.errorcode == Library::ErrorCode::BAD_ATTRIBUTE_VALUE);
    return 0;
}

int contracts_bad_nr() {
    Settings settings;
    TASK_CHECK(loadXml(settings, "guard_nr0.cfg",
                       guardCfgWith("<pure/><arg nr=\"0\"><not-null-when result=\"true\"/></arg>")).errorcode == Library::ErrorCode::BAD_ATTRIBUTE_VALUE);
    Settings settings2;
    TASK_CHECK(loadXml(settings2, "guard_nrany.cfg",
                       guardCfgWith("<pure/><arg nr=\"any\"><not-null-when result=\"true\"/></arg>")).errorcode == Library::ErrorCode::BAD_ATTRIBUTE_VALUE);
    Settings settings3;
    TASK_CHECK(loadXml(settings3, "guard_nrneg.cfg",
                       guardCfgWith("<pure/><arg nr=\"-2\"><not-null-when result=\"true\"/></arg>")).errorcode == Library::ErrorCode::BAD_ATTRIBUTE_VALUE);
    return 0;
}

int contracts_requires_pure() {
    // not a pure function: rejected
    Settings settings;
    TASK_CHECK(loadXml(settings, "guard_notpure.cfg",
                       guardCfgWith("<arg nr=\"1\"><not-null-when result=\"true\"/></arg>")).errorcode == Library::ErrorCode::BAD_ATTRIBUTE_VALUE);
    // tag order must not matter: <arg> before <pure/> is still valid
    Settings settings2;
    TASK_CHECK(loadXml(settings2, "guard_purelate.cfg",
                       guardCfgWith("<arg nr=\"1\"><not-null-when result=\"true\"/></arg><pure/>")).errorcode == Library::ErrorCode::OK);
    // <const/> implies pure
    Settings settings3;
    TASK_CHECK(loadXml(settings3, "guard_const.cfg",
                       guardCfgWith("<arg nr=\"1\"><not-null-when result=\"false\"/></arg><const/>")).errorcode == Library::ErrorCode::OK);
    return 0;
}

// ---------------- branches ----------------

int branches_if_guard() {
    const Analysis a = analyse(std::string(kPrelude) +
                               "void f(const int* p) {\n"
                               "    if (ready(p)) {\n"
                               "        observe(p);\n"
                               "    }\n"
                               "}\n");
    TASK_CHECK(a.observations.size() == 1);
    TASK_CHECK(a.observations[0].impossibleZero);
    TASK_CHECK(a.observations[0].conditionIsGuardCall);
    TASK_CHECK(a.observations[0].errorPathToGuard);
    TASK_CHECK(!a.checkerErrors);
    return 0;
}

int branches_negated_early_return() {
    const Analysis a = analyse(std::string(kPrelude) +
                               "void f(const int* p) {\n"
                               "    if (!ready(p)) {\n"
                               "        return;\n"
                               "    }\n"
                               "    observe(p);\n"
                               "}\n");
    TASK_CHECK(a.observations.size() == 1);
    TASK_CHECK(a.observations[0].impossibleZero);
    TASK_CHECK(a.observations[0].conditionIsGuardCall);
    TASK_CHECK(!a.checkerErrors);
    return 0;
}

int branches_other_side_unknown() {
    // the false side of a result="true" guard carries no fact, and nothing leaks past the merge
    const Analysis a = analyse(std::string(kPrelude) +
                               "void f(const int* p) {\n"
                               "    if (ready(p)) {\n"
                               "    } else {\n"
                               "        observe(p);\n"
                               "    }\n"
                               "    observe(p);\n"
                               "}\n");
    TASK_CHECK(a.observations.size() == 2);
    TASK_CHECK(!a.observations[0].impossibleZero);
    TASK_CHECK(!a.observations[1].impossibleZero);
    TASK_CHECK(!a.checkerErrors);
    return 0;
}

int branches_short_circuit() {
    const Analysis a = analyse(std::string(kPrelude) +
                               "void f(const int* p) {\n"
                               "    if (ready(p) && check(p)) {\n"
                               "    }\n"
                               "}\n"
                               "void g(const int* p) {\n"
                               "    if (!ready(p) || check(p)) {\n"
                               "    }\n"
                               "}\n");
    TASK_CHECK(a.observations.size() == 2);
    TASK_CHECK(a.observations[0].callee == "check");
    TASK_CHECK(a.observations[0].impossibleZero);
    TASK_CHECK(a.observations[1].callee == "check");
    TASK_CHECK(a.observations[1].impossibleZero);
    TASK_CHECK(!a.checkerErrors);
    return 0;
}

int branches_result_false() {
    const Analysis a = analyse(std::string(kPrelude) +
                               "void f(const int* p) {\n"
                               "    if (!failed(p)) {\n"
                               "        observe(p);\n"
                               "    }\n"
                               "}\n"
                               "void g(const int* p) {\n"
                               "    if (failed(p)) {\n"
                               "    } else {\n"
                               "        observe(p);\n"
                               "    }\n"
                               "}\n");
    TASK_CHECK(a.observations.size() == 2);
    TASK_CHECK(a.observations[0].impossibleZero);
    TASK_CHECK(a.observations[1].impossibleZero);
    TASK_CHECK(!a.checkerErrors);
    return 0;
}

int branches_multiple_args() {
    const Analysis a = analyse(std::string(kPrelude) +
                               "void f(const int* p, const int* q) {\n"
                               "    if (ready2(p, q)) {\n"
                               "        observe(p);\n"
                               "        observe(q);\n"
                               "    }\n"
                               "}\n");
    TASK_CHECK(a.observations.size() == 2);
    TASK_CHECK(a.observations[0].impossibleZero);
    TASK_CHECK(a.observations[1].impossibleZero);
    TASK_CHECK(!a.checkerErrors);
    return 0;
}

int branches_scoped_variables() {
    // same name in different scopes: the fact belongs to the inner variable only
    const Analysis a = analyse(std::string(kPrelude) +
                               "void f(const int* p, const int* q) {\n"
                               "    {\n"
                               "        const int* p = q;\n"
                               "        if (ready(p)) {\n"
                               "            observe(p);\n"
                               "        }\n"
                               "    }\n"
                               "    observe(p);\n"
                               "}\n");
    TASK_CHECK(a.observations.size() == 2);
    TASK_CHECK(a.observations[0].impossibleZero);
    TASK_CHECK(!a.observations[1].impossibleZero);
    TASK_CHECK(!a.checkerErrors);
    return 0;
}

int branches_deref_guarded() {
    // a real dereference, protected by a real guard; CheckNullPointer must stay quiet
    const Analysis a = analyse(std::string(kPrelude) +
                               "void f(int* p) {\n"
                               "    if (ready(p)) {\n"
                               "        *p = 1;\n"
                               "        observe(p);\n"
                               "    }\n"
                               "}\n");
    TASK_CHECK(a.observations.size() == 1);
    TASK_CHECK(a.observations[0].impossibleZero);
    TASK_CHECK(!a.checkerErrors);
    return 0;
}

int branches_stored_result_untracked() {
    // storing the result in a bool first is intentionally not followed
    const Analysis a = analyse(std::string(kPrelude) +
                               "void f(const int* p) {\n"
                               "    bool b = ready(p);\n"
                               "    if (b) {\n"
                               "        observe(p);\n"
                               "    }\n"
                               "}\n");
    TASK_CHECK(a.observations.size() == 1);
    TASK_CHECK(!a.observations[0].impossibleZero);
    TASK_CHECK(!a.checkerErrors);
    return 0;
}

int branches_no_contract() {
    // check() is configured but has no guard relation; unknownGuard() cannot be bound
    const Analysis a = analyse(std::string(kPrelude) +
                               "void f(const int* p) {\n"
                               "    if (check(p)) {\n"
                               "        observe(p);\n"
                               "    }\n"
                               "}\n"
                               "void g(const int* p) {\n"
                               "    if (unknownGuard(p)) {\n"
                               "        observe(p);\n"
                               "    }\n"
                               "}\n");
    // the check() call in the condition is itself an observation site, so three are expected
    TASK_CHECK(a.observations.size() == 3);
    TASK_CHECK(a.observations[0].callee == "check");
    TASK_CHECK(!a.observations[0].impossibleZero);
    TASK_CHECK(!a.observations[1].impossibleZero);
    TASK_CHECK(!a.observations[2].impossibleZero);
    TASK_CHECK(!a.checkerErrors);
    return 0;
}

int branches_non_pointer_arg() {
    // a guard relation on a non-pointer argument is ignored by the ValueFlow handler
    const Analysis a = analyse(std::string(kPrelude) +
                               "void f(int i) {\n"
                               "    if (readyInt(i)) {\n"
                               "        probe(i);\n"
                               "    }\n"
                               "}\n");
    TASK_CHECK(a.observations.size() == 1);
    TASK_CHECK(!a.observations[0].impossibleZero);
    TASK_CHECK(!a.checkerErrors);
    return 0;
}

// ---------------- invalidation ----------------

int invalidation_rebind() {
    const Analysis a = analyse(std::string(kPrelude) +
                               "void f(const int* p, const int* q) {\n"
                               "    if (ready(p)) {\n"
                               "        p = q;\n"
                               "        observe(p);\n"
                               "    }\n"
                               "}\n");
    TASK_CHECK(a.observations.size() == 1);
    TASK_CHECK(!a.observations[0].impossibleZero);
    TASK_CHECK(!a.checkerErrors);
    return 0;
}

int invalidation_unknown_call() {
    // an unknown function may reseat the pointer through the mutable handle
    const Analysis a = analyse(std::string(kPrelude) +
                               "void f(const int* p) {\n"
                               "    if (ready(p)) {\n"
                               "        tweak(&p);\n"
                               "        observe(p);\n"
                               "    }\n"
                               "}\n");
    TASK_CHECK(a.observations.size() == 1);
    TASK_CHECK(!a.observations[0].impossibleZero);
    TASK_CHECK(!a.checkerErrors);
    return 0;
}

int invalidation_reproof() {
    // after the old proof is destroyed, a new guard establishes a new proof
    const Analysis a = analyse(std::string(kPrelude) +
                               "void f(const int* p, const int* q) {\n"
                               "    if (ready(p)) {\n"
                               "        p = q;\n"
                               "    }\n"
                               "    if (ready(p)) {\n"
                               "        observe(p);\n"
                               "    }\n"
                               "}\n");
    TASK_CHECK(a.observations.size() == 1);
    TASK_CHECK(a.observations[0].impossibleZero);
    TASK_CHECK(a.observations[0].conditionIsGuardCall);
    TASK_CHECK(!a.checkerErrors);
    return 0;
}

} // namespace

const struct task_case task_cases[] = {
    {"contracts", "load_valid", contracts_load_valid},
    {"contracts", "missing_result", contracts_missing_result},
    {"contracts", "bad_result", contracts_bad_result},
    {"contracts", "duplicate_relation", contracts_duplicate_relation},
    {"contracts", "bad_nr", contracts_bad_nr},
    {"contracts", "requires_pure", contracts_requires_pure},
    {"branches", "if_guard", branches_if_guard},
    {"branches", "negated_early_return", branches_negated_early_return},
    {"branches", "other_side_unknown", branches_other_side_unknown},
    {"branches", "short_circuit", branches_short_circuit},
    {"branches", "result_false", branches_result_false},
    {"branches", "multiple_args", branches_multiple_args},
    {"branches", "scoped_variables", branches_scoped_variables},
    {"branches", "deref_guarded", branches_deref_guarded},
    {"branches", "stored_result_untracked", branches_stored_result_untracked},
    {"branches", "no_contract", branches_no_contract},
    {"branches", "non_pointer_arg", branches_non_pointer_arg},
    {"invalidation", "rebind", invalidation_rebind},
    {"invalidation", "unknown_call", invalidation_unknown_call},
    {"invalidation", "reproof", invalidation_reproof},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
