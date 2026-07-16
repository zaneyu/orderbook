#pragma once
// Tiny zero-dependency test framework: TEST(name){...}, CHECK(cond), CHECK_EQ(a,b).
// Self-registering; main() calls tf::run(). Keeps the repo buildable from a clean
// clone with nothing but a C++20 compiler.
#include <cstdio>
#include <type_traits>
#include <vector>

namespace tf {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}
inline int& failures() {
    static int f = 0;
    return f;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

// Print the actual values on CHECK_EQ failure when they're printable — a bare
// source-text echo makes a fuzz failure in CI undebuggable. (Template so the
// integral branch is discarded, not just skipped, for non-printable types.)
template <typename A, typename B>
inline void print_actual(const A& a, const B& b) {
    if constexpr (std::is_integral_v<A> && std::is_integral_v<B>)
        std::printf("      actual: %lld vs %lld\n",
                    static_cast<long long>(a), static_cast<long long>(b));
}

inline int run() {
    int total_fail = 0;
    for (const auto& c : registry()) {
        const int before = failures();
        c.fn();
        const bool ok = failures() == before;
        total_fail += failures() - before;
        std::printf("%s %s\n", ok ? "[ ok ]" : "[FAIL]", c.name);
    }
    std::printf("\n%s: %d check(s) failed across %zu tests\n",
                total_fail == 0 ? "SUCCESS" : "FAILURE", total_fail, registry().size());
    return total_fail == 0 ? 0 : 1;
}

}  // namespace tf

#define TEST(name)                                       \
    static void name();                                  \
    static tf::Registrar tf_reg_##name(#name, &name);    \
    static void name()

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                              \
            ++tf::failures();                                                        \
            std::printf("    CHECK failed: %s  (%s:%d)\n", #cond, __FILE__, __LINE__); \
        }                                                                            \
    } while (0)

#define CHECK_EQ(a, b)                                                                       \
    do {                                                                                     \
        const auto _va = (a);                                                                \
        const auto _vb = (b);                                                                \
        if (!(_va == _vb)) {                                                                 \
            ++tf::failures();                                                                 \
            std::printf("    CHECK_EQ failed: %s == %s  (%s:%d)\n", #a, #b, __FILE__, __LINE__); \
            tf::print_actual(_va, _vb);                                                      \
        }                                                                                    \
    } while (0)
