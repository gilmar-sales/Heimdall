#include <Heimdall/RuleEngine.hpp>
#include <Heimdall/SemanticRules.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace
{

    // Owns every layer so the references between them stay valid.
    struct Typed
    {
        explicit Typed(std::string text) :
            source(std::move(text)), tree(heimdall::ParseTree::Parse(source)),
            model(heimdall::Binder::Bind(tree)), types(heimdall::Typer::Type(model))
        {
        }

        Typed(const Typed&) = delete;

        Typed& operator=(const Typed&) = delete;

        heimdall::SymbolId Find(const std::string& name) const
        {
            const auto id = model.Names().Find(name);
            for (heimdall::SymbolId symbol = 0; symbol < model.Symbols().Size(); ++symbol)
            {
                if (model.Symbols().name[symbol] == id)
                {
                    return symbol;
                }
            }

            return heimdall::kNone;
        }

        std::string Of(const std::string& name) const
        {
            const auto symbol = Find(name);
            return symbol == heimdall::kNone ? "<no symbol>"
                                             : types.Spell(types.SymbolType(symbol));
        }

        std::string             source;
        heimdall::ParseTree     tree;
        heimdall::SemanticModel model;
        heimdall::TypeModel     types;
    };

    std::string Body(const std::string& statements)
    {
        return "void wrapper() {\n" + statements + "\n}\n";
    }

    std::vector<heimdall::Diagnostic> ImplicitBool(const std::string& source)
    {
        const Typed typed(source);
        return heimdall::SemanticRules::AnalyzeImplicitBool(typed.types);
    }

    std::vector<heimdall::Diagnostic> RangeLoop(const std::string& source)
    {
        const Typed typed(source);
        return heimdall::SemanticRules::AnalyzeRangeLoop(typed.types);
    }

    std::vector<heimdall::Diagnostic> LoopConvert(const std::string& source)
    {
        const Typed typed(source);
        return heimdall::SemanticRules::AnalyzeLoopConvert(typed.types);
    }

    // Applies the only fix, replacing the loop it covers.
    std::string ApplyFix(std::string source, const heimdall::Diagnostic& diagnostic)
    {
        source.replace(diagnostic.fix.offset, diagnostic.fix.length, diagnostic.fix.replacement);
        return source;
    }

} // namespace

// ---- TypeTable -----------------------------------------------------------------

TEST(TypeTable, HashConsesEqualTypes)
{
    heimdall::Arena     arena;
    heimdall::TypeTable table(arena.Resource());
    const auto          i = table.Builtin(heimdall::BuiltinType::Int);
    EXPECT_EQ(i, table.Builtin(heimdall::BuiltinType::Int));
    EXPECT_NE(i, table.Builtin(heimdall::BuiltinType::UInt));
    EXPECT_EQ(table.Pointer(i), table.Pointer(i));
    EXPECT_NE(table.Pointer(i), table.Pointer(table.Pointer(i)));
    EXPECT_EQ(table.Const(table.Const(i)), table.Const(i));
    EXPECT_EQ(table.Array(i, 3), table.Array(i, 3));
    EXPECT_NE(table.Array(i, 3), table.Array(i, 4));
    EXPECT_EQ(table.LRef(table.LRef(i)), table.LRef(i));
}

TEST(TypeTable, UnknownStaysUnknownWhereNothingIsLearned)
{
    heimdall::Arena     arena;
    heimdall::TypeTable table(arena.Resource());
    EXPECT_EQ(table.Const(heimdall::TypeTable::Unknown), heimdall::TypeTable::Unknown);
    EXPECT_EQ(table.LRef(heimdall::TypeTable::Unknown), heimdall::TypeTable::Unknown);
    EXPECT_EQ(table.Class(heimdall::kNone), heimdall::TypeTable::Unknown);
    // ...but a pointer to something unknown is still a pointer.
    EXPECT_TRUE(table.IsPointer(table.Pointer(heimdall::TypeTable::Unknown)));
    EXPECT_FALSE(table.IsKnown(heimdall::TypeTable::Unknown));
}

TEST(TypeTable, ConstOfAnArrayQualifiesItsElements)
{
    heimdall::Arena     arena;
    heimdall::TypeTable table(arena.Resource());
    const auto          i = table.Builtin(heimdall::BuiltinType::Int);
    EXPECT_EQ(table.Const(table.Array(i, 2)), table.Array(table.Const(i), 2));
    EXPECT_TRUE(table.IsConstQualified(table.Array(table.Const(i), 2)));
    EXPECT_FALSE(table.IsConstQualified(table.Array(i, 2)));
}

TEST(TypeTable, ClassifiesBuiltinTypes)
{
    heimdall::Arena     arena;
    heimdall::TypeTable table(arena.Resource());
    const auto          of = [&](heimdall::BuiltinType type) { return table.Builtin(type); };
    EXPECT_TRUE(table.IsInteger(of(heimdall::BuiltinType::Char)));
    EXPECT_TRUE(table.IsInteger(of(heimdall::BuiltinType::SizeT)));
    EXPECT_FALSE(table.IsInteger(of(heimdall::BuiltinType::Bool)));
    EXPECT_TRUE(table.IsBool(of(heimdall::BuiltinType::Bool)));
    EXPECT_TRUE(table.IsFloating(of(heimdall::BuiltinType::LongDouble)));
    EXPECT_TRUE(table.IsArithmetic(of(heimdall::BuiltinType::Bool)));
    EXPECT_FALSE(table.IsArithmetic(of(heimdall::BuiltinType::Void)));
    EXPECT_FALSE(table.IsArithmetic(of(heimdall::BuiltinType::NullptrT)));
}

// ---- declared types --------------------------------------------------------------

TEST(Typer, ReadsBuiltinTypes)
{
    const Typed typed(Body("int a; unsigned long b; const char* c; long long d; unsigned char e; "
                           "bool f; double g; float h;\n"
                           "unsigned u; short s; long double ld; signed char sc; unsigned short "
                           "us; char8_t c8; wchar_t w;\n"
                           "static const int k = 1; constexpr unsigned long long big = 1;"));
    EXPECT_EQ(typed.Of("a"), "int");
    EXPECT_EQ(typed.Of("b"), "unsigned long");
    EXPECT_EQ(typed.Of("c"), "const char*");
    EXPECT_EQ(typed.Of("d"), "long long");
    EXPECT_EQ(typed.Of("e"), "unsigned char");
    EXPECT_EQ(typed.Of("f"), "bool");
    EXPECT_EQ(typed.Of("g"), "double");
    EXPECT_EQ(typed.Of("h"), "float");
    EXPECT_EQ(typed.Of("u"), "unsigned int");
    EXPECT_EQ(typed.Of("s"), "short");
    EXPECT_EQ(typed.Of("ld"), "long double");
    EXPECT_EQ(typed.Of("sc"), "signed char");
    EXPECT_EQ(typed.Of("us"), "unsigned short");
    EXPECT_EQ(typed.Of("c8"), "char8_t");
    EXPECT_EQ(typed.Of("w"), "wchar_t");
    EXPECT_EQ(typed.Of("k"), "const int");
    EXPECT_EQ(typed.Of("big"), "unsigned long long");
}

TEST(Typer, ReadsPointersReferencesAndArrays)
{
    const Typed typed(Body("int* p; int** pp; int x = 0; const int& r = x; int a[3]; char* "
                           "names[2]; int* const cp = 0;\n"
                           "int m[2][3]; int&& rr = 1; const char* const s = 0;"));
    EXPECT_EQ(typed.Of("p"), "int*");
    EXPECT_EQ(typed.Of("pp"), "int**");
    EXPECT_EQ(typed.Of("r"), "const int&");
    EXPECT_EQ(typed.Of("a"), "int[3]");
    EXPECT_EQ(typed.Of("names"), "char*[2]");
    EXPECT_EQ(typed.Of("cp"), "int* const");
    EXPECT_EQ(typed.Of("m"), "int[2][3]");
    EXPECT_EQ(typed.Of("rr"), "int&&");
    EXPECT_EQ(typed.Of("s"), "const char* const");
}

TEST(Typer, ParametersDecayArraysToPointers)
{
    const Typed typed("void f(int a[], char b[4], const int c[2]) {}\n");
    EXPECT_EQ(typed.Of("a"), "int*");
    EXPECT_EQ(typed.Of("b"), "char*");
    EXPECT_EQ(typed.Of("c"), "const int*");
}

TEST(Typer, ResolvesClassesEnumsAndAliases)
{
    const Typed typed(
        "struct S {};\n"
        "enum E { A, B };\n"
        "enum class F { X };\n"
        "using U = unsigned;\n"
        "typedef long L;\n"
        "using P = S*;\n"
        "using V = std::size_t;\n"
        "namespace ns { struct T {}; }\n"
        "void f() { S s; E e; F g; U u; L l; P p; V v; ns::T t; S* sp; }\n");
    EXPECT_EQ(typed.Of("s"), "S");
    EXPECT_EQ(typed.Of("e"), "E");
    EXPECT_EQ(typed.Of("g"), "F");
    EXPECT_EQ(typed.Of("u"), "unsigned int");
    EXPECT_EQ(typed.Of("l"), "long");
    EXPECT_EQ(typed.Of("p"), "S*");
    EXPECT_EQ(typed.Of("v"), "size_t");
    EXPECT_EQ(typed.Of("t"), "T");
    EXPECT_EQ(typed.Of("sp"), "S*");
}

TEST(Typer, KeepsLibraryTypesAsExternal)
{
    const Typed typed(Body("std::vector<int> v; std::string s; std::map<int, std::vector<int>> m; "
                           "std::size_t n; size_t k;\n"
                           "std::unique_ptr<int> up;"));
    EXPECT_EQ(typed.Of("v"), "std::vector<int>");
    EXPECT_EQ(typed.Of("s"), "std::string");
    EXPECT_EQ(typed.Of("m"), "std::map<int,std::vector<int>>");
    EXPECT_EQ(typed.Of("n"), "size_t");
    EXPECT_EQ(typed.Of("k"), "size_t");
    EXPECT_EQ(typed.Of("up"), "std::unique_ptr<int>");
    EXPECT_EQ(typed.types.ExternalHead(typed.types.SymbolType(typed.Find("m"))), "std::map");
}

TEST(Typer, UnknownWhenTheTypeIsNotDeclaredHere)
{
    const Typed typed("template <class T> void g(T a, T* b, T& c) {}\n"
                      "void f() { Foo x; ns::Bar y; Box<int> z; typename T::type w; }\n");
    EXPECT_EQ(typed.Of("a"), "<unknown>");
    EXPECT_EQ(typed.Of("b"), "<unknown>*");
    EXPECT_EQ(typed.Of("c"), "<unknown>");
    EXPECT_EQ(typed.Of("x"), "<unknown>");
    EXPECT_EQ(typed.Of("y"), "<unknown>");
    EXPECT_EQ(typed.Of("z"), "<unknown>");
    EXPECT_EQ(typed.Of("w"), "<unknown>");
}

TEST(Typer, TemplateClassesAndTemplateAliasesAreUnknown)
{
    const Typed typed("template <class T> struct Box {};\n"
                      "template <class T> using Vec = std::vector<T>;\n"
                      "void f() { Box<int> b; Vec<int> v; }\n");
    EXPECT_EQ(typed.Of("b"), "<unknown>");
    EXPECT_EQ(typed.Of("v"), "<unknown>");
}

TEST(Typer, CyclesTerminateAsUnknown)
{
    const Typed typed("using A = A;\n"
                      "using B = C;\n"
                      "using C = B;\n"
                      "void f() { A a; B b; auto x = x; }\n");
    EXPECT_EQ(typed.Of("a"), "<unknown>");
    EXPECT_EQ(typed.Of("b"), "<unknown>");
    EXPECT_EQ(typed.Of("x"), "<unknown>");
}

// ---- auto and literals ---------------------------------------------------------------

TEST(Typer, TypesLiterals)
{
    const Typed typed(Body("auto a = 1; auto b = 1u; auto c = 1.5; auto d = 1.5f; auto e = 'x'; "
                           "auto f = true; auto g = \"s\";\n"
                           "auto h = nullptr; auto i = 5L; auto j = 5ull; auto k = 0x10; auto l = "
                           "3000000000; auto m = 1e3;\n"
                           "auto n = 2.0L; auto o = L'x'; auto p = u8'x'; auto q = 0b101; auto r = "
                           "010; auto s = 1'000;\n"
                           "auto t = 5ul; auto u = 5lu; auto v = 5ll; auto w = 5lul;"));
    EXPECT_EQ(typed.Of("a"), "int");
    EXPECT_EQ(typed.Of("b"), "unsigned int");
    EXPECT_EQ(typed.Of("c"), "double");
    EXPECT_EQ(typed.Of("d"), "float");
    EXPECT_EQ(typed.Of("e"), "char");
    EXPECT_EQ(typed.Of("f"), "bool");
    EXPECT_EQ(typed.Of("g"), "const char*");
    EXPECT_EQ(typed.Of("h"), "decltype(nullptr)");
    EXPECT_EQ(typed.Of("i"), "long");
    EXPECT_EQ(typed.Of("j"), "unsigned long long");
    EXPECT_EQ(typed.Of("k"), "int");
    EXPECT_EQ(typed.Of("l"), "<unknown>"); // long or long long depending on the platform
    EXPECT_EQ(typed.Of("m"), "double");
    EXPECT_EQ(typed.Of("n"), "long double");
    EXPECT_EQ(typed.Of("o"), "wchar_t");
    EXPECT_EQ(typed.Of("p"), "char8_t");
    EXPECT_EQ(typed.Of("q"), "int");
    EXPECT_EQ(typed.Of("r"), "int");
    EXPECT_EQ(typed.Of("s"), "int");
    EXPECT_EQ(typed.Of("t"), "unsigned long");
    EXPECT_EQ(typed.Of("u"), "unsigned long");
    EXPECT_EQ(typed.Of("v"), "long long");
    EXPECT_EQ(typed.Of("w"), "<unknown>");
}

TEST(Typer, AutoDropsReferencesAndTopLevelConst)
{
    const Typed typed(Body("int x = 0; const int cx = 0; int& rx = x; int arr[3] = {};\n"
                           "auto a = x; auto b = cx; auto c = rx; auto d = arr; const auto e = x; "
                           "auto& f = x; const auto& g = x;\n"
                           "auto& h = cx;"));
    EXPECT_EQ(typed.Of("a"), "int");
    EXPECT_EQ(typed.Of("b"), "int");
    EXPECT_EQ(typed.Of("c"), "int");
    EXPECT_EQ(typed.Of("d"), "int*");
    EXPECT_EQ(typed.Of("e"), "const int");
    EXPECT_EQ(typed.Of("f"), "int&");
    EXPECT_EQ(typed.Of("g"), "const int&");
    EXPECT_EQ(typed.Of("h"), "const int&");
}

TEST(Typer, AutoPointerDeduction)
{
    const Typed typed(Body("int x = 0; const int cx = 0; int arr[3] = {};\n"
                           "auto* p = &x; const auto* q = &x; auto* r = arr; auto* const s = &x; "
                           "auto* t = &cx; auto* u = x;"));
    EXPECT_EQ(typed.Of("p"), "int*");
    EXPECT_EQ(typed.Of("q"), "const int*");
    EXPECT_EQ(typed.Of("r"), "int*");
    EXPECT_EQ(typed.Of("s"), "int* const");
    EXPECT_EQ(typed.Of("t"), "const int*");
    EXPECT_EQ(typed.Of("u"), "<unknown>"); // ill-formed: not a pointer
}

TEST(Typer, AutoForwardingReferences)
{
    const Typed typed(
        Body("int x = 0; const int cx = 0;\n"
             "auto&& a = x; auto&& b = cx; auto&& c = 1; auto&& d = x + 1; const auto&& e = 2;"));
    EXPECT_EQ(typed.Of("a"), "int&");
    EXPECT_EQ(typed.Of("b"), "const int&");
    EXPECT_EQ(typed.Of("c"), "int&&");
    EXPECT_EQ(typed.Of("d"), "int&&");
    EXPECT_EQ(typed.Of("e"), "const int&&");
}

TEST(Typer, AutoWithDirectAndBraceInitialization)
{
    const Typed typed(Body("int x = 0; double d = 0;\n"
                           "auto a{x}; auto b{d}; auto c = {x}; auto e{1, 2}; auto& f{x};"));
    EXPECT_EQ(typed.Of("a"), "int");
    EXPECT_EQ(typed.Of("b"), "double");
    EXPECT_EQ(typed.Of("c"), "<unknown>"); // std::initializer_list
    EXPECT_EQ(typed.Of("e"), "<unknown>"); // ill-formed
    EXPECT_EQ(typed.Of("f"), "int&");
}

TEST(Typer, AutoInRangeFor)
{
    const Typed typed(Body(
        "int arr[3] = {}; const int carr[2] = {};\n"
        "for (auto a : arr) {}\n"
        "for (auto& b : arr) {}\n"
        "for (const auto& c : carr) {}\n"
        "for (auto&& d : arr) {}\n"
        "for (auto e : carr) {}"));
    EXPECT_EQ(typed.Of("a"), "int");
    EXPECT_EQ(typed.Of("b"), "int&");
    EXPECT_EQ(typed.Of("c"), "const int&");
    EXPECT_EQ(typed.Of("d"), "int&");
    EXPECT_EQ(typed.Of("e"), "int");
}

TEST(Typer, AutoReturnTypeIsDeducedFromTheBody)
{
    const Typed typed(
        "auto f1() { return 1; }\n"
        "auto f2() { int x = 0; return x; }\n"
        "const auto f3() { return 1.5; }\n"
        "auto f4(bool b) { if (b) return 1; return 2; }\n"
        "auto f5(bool b) { if (b) return 1; return 2.0; }\n"
        "auto f6() { auto l = [] { return 3.0; }; return 1; }\n"
        "auto f7() { }\n"
        "auto f8() { return f1(); }\n"
        "auto f9() -> long { return 1; }\n"
        "void use() { auto v = f1(); auto w = f8(); }\n");
    EXPECT_EQ(typed.Of("f1"), "int");
    EXPECT_EQ(typed.Of("f2"), "int");
    EXPECT_EQ(typed.Of("f3"), "const double");
    EXPECT_EQ(typed.Of("f4"), "int");
    EXPECT_EQ(typed.Of("f5"), "<unknown>"); // conflicting deductions: ill-formed
    EXPECT_EQ(typed.Of("f6"), "int");       // the lambda's return is not ours
    EXPECT_EQ(typed.Of("f7"), "void");
    EXPECT_EQ(typed.Of("f8"), "int");
    EXPECT_EQ(typed.Of("f9"), "long");
    EXPECT_EQ(typed.Of("v"), "int");
    EXPECT_EQ(typed.Of("w"), "int");
}

// ---- expressions ------------------------------------------------------------------------

TEST(Typer, AppliesTheUsualArithmeticConversions)
{
    const Typed typed(Body("int i = 0; long l = 0; double d = 0; unsigned u = 0; char c = 0; float "
                           "fl = 0; short sh = 0; bool b = true;\n"
                           "unsigned long ul = 0; long long ll = 0; std::size_t sz = 0;\n"
                           "auto s1 = i + l; auto s2 = i + d; auto s3 = i + u; auto s4 = c + c; "
                           "auto s5 = fl * i; auto s6 = sh - sh;\n"
                           "auto s7 = b + b; auto s8 = l + ul; auto s9 = ll + u; auto s10 = l + u; "
                           "auto s11 = sz - i; auto s12 = d / fl;\n"
                           "auto s13 = i % l; auto s14 = d % i; auto s15 = i & u; auto s16 = i << "
                           "l; auto s17 = i ^ c;"));
    EXPECT_EQ(typed.Of("s1"), "long");
    EXPECT_EQ(typed.Of("s2"), "double");
    EXPECT_EQ(typed.Of("s3"), "unsigned int");
    EXPECT_EQ(typed.Of("s4"), "int");
    EXPECT_EQ(typed.Of("s5"), "float");
    EXPECT_EQ(typed.Of("s6"), "int");
    EXPECT_EQ(typed.Of("s7"), "int");
    EXPECT_EQ(typed.Of("s8"), "unsigned long");
    EXPECT_EQ(typed.Of("s9"), "long long");
    EXPECT_EQ(typed.Of("s10"), "<unknown>"); // long vs unsigned depends on the platform
    EXPECT_EQ(typed.Of("s11"), "size_t");
    EXPECT_EQ(typed.Of("s12"), "double");
    EXPECT_EQ(typed.Of("s13"), "long");
    EXPECT_EQ(typed.Of("s14"), "<unknown>");
    EXPECT_EQ(typed.Of("s15"), "unsigned int");
    EXPECT_EQ(typed.Of("s16"), "int");
    EXPECT_EQ(typed.Of("s17"), "int");
}

TEST(Typer, TypesComparisonsAndLogicalOperators)
{
    const Typed typed(Body("int i = 0; long l = 0; int* p = 0; double d = 0;\n"
                           "auto c1 = i < l; auto c2 = p == nullptr; auto c3 = d != i; auto c4 = i "
                           "&& l; auto c5 = !i; auto c6 = p || d;\n"
                           "auto c7 = !p;"));
    for (const char* name : { "c1", "c2", "c3", "c4", "c5", "c6", "c7" })
    {
        EXPECT_EQ(typed.Of(name), "bool") << name;
    }
}

TEST(Typer, TypesUnaryOperators)
{
    const Typed typed(Body(
        "int i = 0; char c = 0; int* p = 0; int a[2] = {}; unsigned u = 0; const int* cp = 0; "
        "double d = 0;\n"
        "auto u1 = -c; auto u2 = +u; auto u3 = ~c; auto u4 = &i; auto u5 = *p; auto u6 = *a; auto "
        "u7 = ++i;\n"
        "auto u8 = i--; auto u9 = *cp; auto u10 = ~d; auto u11 = &a; auto u12 = -d;"));
    EXPECT_EQ(typed.Of("u1"), "int");
    EXPECT_EQ(typed.Of("u2"), "unsigned int");
    EXPECT_EQ(typed.Of("u3"), "int");
    EXPECT_EQ(typed.Of("u4"), "int*");
    EXPECT_EQ(typed.Of("u5"), "int");
    EXPECT_EQ(typed.Of("u6"), "int");
    EXPECT_EQ(typed.Of("u7"), "int");
    EXPECT_EQ(typed.Of("u8"), "int");
    EXPECT_EQ(typed.Of("u9"), "int"); // auto drops the top-level const
    EXPECT_EQ(typed.Of("u10"), "<unknown>");
    EXPECT_EQ(typed.Of("u11"), "int(*)[2]");
    EXPECT_EQ(typed.Of("u12"), "double");
}

TEST(Typer, TypesPointerArithmeticSubscriptsAndConditionals)
{
    const Typed typed(Body("int i = 0; long l = 0; int* p = 0; int a[4] = {}; char* s = 0;\n"
                           "auto p1 = p + 1; auto p2 = 1 + p; auto p3 = p - 1; auto p4 = a[1]; "
                           "auto p5 = p[i]; auto p6 = s[0];\n"
                           "auto p7 = i ? l : i; auto p8 = i ? p : nullptr; auto p9 = i ? p : s; "
                           "auto p10 = (i); auto p11 = a + 1;\n"
                           "auto p12 = p - p;"));
    EXPECT_EQ(typed.Of("p1"), "int*");
    EXPECT_EQ(typed.Of("p2"), "int*");
    EXPECT_EQ(typed.Of("p3"), "int*");
    EXPECT_EQ(typed.Of("p4"), "int");
    EXPECT_EQ(typed.Of("p5"), "int");
    EXPECT_EQ(typed.Of("p6"), "char");
    EXPECT_EQ(typed.Of("p7"), "long");
    EXPECT_EQ(typed.Of("p8"), "int*");
    EXPECT_EQ(typed.Of("p9"), "<unknown>");
    EXPECT_EQ(typed.Of("p10"), "int");
    EXPECT_EQ(typed.Of("p11"), "int*");
    EXPECT_EQ(typed.Of("p12"), "<unknown>");
}

TEST(Typer, TypesAssignmentsAndCasts)
{
    const Typed typed(Body("int i = 0; long l = 0; double d = 0;\n"
                           "auto a1 = (i = 2); auto a2 = (l += i); auto a3 = static_cast<char>(i); "
                           "auto a4 = static_cast<double>(i);\n"
                           "auto a5 = reinterpret_cast<long*>(&i); auto a6 = static_cast<const "
                           "int&>(i); auto a7 = int(d);\n"
                           "auto a8 = sizeof(i); auto a9 = static_cast<Foo>(i); auto a10 = "
                           "static_cast<std::vector<int>>(i);"));
    EXPECT_EQ(typed.Of("a1"), "int");
    EXPECT_EQ(typed.Of("a2"), "long");
    EXPECT_EQ(typed.Of("a3"), "char");
    EXPECT_EQ(typed.Of("a4"), "double");
    EXPECT_EQ(typed.Of("a5"), "long*");
    EXPECT_EQ(typed.Of("a6"), "int");
    EXPECT_EQ(typed.Of("a7"), "int");
    EXPECT_EQ(typed.Of("a8"), "size_t");
    EXPECT_EQ(typed.Of("a9"), "<unknown>");
}

TEST(Typer, TypesMembersAndCalls)
{
    const Typed typed(
        "struct P { int x; double y; int* q; int get() const; static long make(); P* self(); };\n"
        "long free_fn();\n"
        "int* mk();\n"
        "auto tr() -> long;\n"
        "void f(P p, P* pp) {\n"
        "    auto a = p.x; auto b = pp->y; auto c = p.get(); auto d = P::make(); auto e = "
        "free_fn();\n"
        "    auto g = pp->q; auto h = *pp->q; auto i = p.self(); auto j = mk(); auto k = tr(); "
        "auto m = P();\n"
        "}\n");
    EXPECT_EQ(typed.Of("a"), "int");
    EXPECT_EQ(typed.Of("b"), "double");
    EXPECT_EQ(typed.Of("c"), "int");
    EXPECT_EQ(typed.Of("d"), "long");
    EXPECT_EQ(typed.Of("e"), "long");
    EXPECT_EQ(typed.Of("g"), "int*");
    EXPECT_EQ(typed.Of("h"), "int");
    EXPECT_EQ(typed.Of("i"), "P*");
    EXPECT_EQ(typed.Of("j"), "int*");
    EXPECT_EQ(typed.Of("k"), "long");
    EXPECT_EQ(typed.Of("m"), "P");
}

TEST(Typer, FindsInheritedMembers)
{
    const Typed typed("struct B { int bx; long bf(); };\n"
                      "struct D : B { double dx; };\n"
                      "void f(D d, D* dp) { auto a = d.bx; auto b = d.bf(); auto c = dp->dx; auto "
                      "e = dp->bx; }\n");
    EXPECT_EQ(typed.Of("a"), "int");
    EXPECT_EQ(typed.Of("b"), "long");
    EXPECT_EQ(typed.Of("c"), "double");
    EXPECT_EQ(typed.Of("e"), "int");
}

TEST(Typer, TypesThisAndImplicitMembers)
{
    const Typed typed("struct S {\n"
                      "    int v;\n"
                      "    int m() { auto t = this; auto w = v; auto z = this->v; return w; }\n"
                      "};\n"
                      "int S::n() { auto q = this; return 0; }\n");
    EXPECT_EQ(typed.Of("t"), "S*");
    EXPECT_EQ(typed.Of("w"), "int");
    EXPECT_EQ(typed.Of("z"), "int");
    EXPECT_EQ(typed.Of("q"), "S*");
}

TEST(Typer, OverloadsShareAReturnTypeOrAreUnknown)
{
    const Typed typed("int same(int); int same(double);\n"
                      "int mixed(); long mixed(int);\n"
                      "template <class T> int generic(T);\n"
                      "void f() { auto a = same(1); auto b = mixed(); auto c = generic(1); }\n");
    EXPECT_EQ(typed.Of("a"), "int");
    EXPECT_EQ(typed.Of("b"), "<unknown>");
    EXPECT_EQ(typed.Of("c"), "<unknown>");
}

TEST(Typer, KnowsStandardContainerObservers)
{
    const Typed typed(Body("std::vector<int> v; std::string s; std::deque<int> d; std::array<int, "
                           "3> a; std::list<int> l;\n"
                           "auto n = v.size(); auto e = v.empty(); auto len = s.length(); auto c = "
                           "d.size(); auto f = a.size();\n"
                           "auto cap = v.capacity(); auto b = v.begin(); auto x = l.length();"));
    EXPECT_EQ(typed.Of("n"), "size_t");
    EXPECT_EQ(typed.Of("e"), "bool");
    EXPECT_EQ(typed.Of("len"), "size_t");
    EXPECT_EQ(typed.Of("c"), "size_t");
    EXPECT_EQ(typed.Of("f"), "size_t");
    EXPECT_EQ(typed.Of("cap"), "size_t");
    EXPECT_EQ(typed.Of("b"), "<unknown>");
    EXPECT_EQ(typed.Of("x"), "<unknown>");
}

TEST(Typer, UsesTheOperatorSubscriptOfAClass)
{
    const Typed typed("struct Grid { long operator[](int); };\n"
                      "void f(Grid g) { auto a = g[1]; }\n");
    EXPECT_EQ(typed.Of("a"), "long");
}

TEST(Typer, UnknownPropagatesThroughExpressions)
{
    const Typed typed(Body("int i = 0; auto a = missing(); auto b = a + 1; auto c = i + a; auto d "
                           "= unknown.field; auto e = i < a;\n"
                           "auto f = a ? i : i; auto g = -a; auto h = *a; auto m = a[0]; auto n = "
                           "i + unknown_global;"));
    EXPECT_EQ(typed.Of("a"), "<unknown>");
    EXPECT_EQ(typed.Of("b"), "<unknown>");
    EXPECT_EQ(typed.Of("c"), "<unknown>");
    EXPECT_EQ(typed.Of("d"), "<unknown>");
    EXPECT_EQ(typed.Of("e"), "<unknown>");
    EXPECT_EQ(typed.Of("f"), "int"); // both branches agree whatever the condition is
    EXPECT_EQ(typed.Of("g"), "<unknown>");
    EXPECT_EQ(typed.Of("h"), "<unknown>");
    EXPECT_EQ(typed.Of("m"), "<unknown>");
    EXPECT_EQ(typed.Of("n"), "<unknown>");
}

TEST(Typer, ClassOperatorsAreNotGuessed)
{
    const Typed typed("struct V {};\n"
                      "void f(V a, V b) { auto s = a + b; auto c = a < b; auto n = !a; auto p = "
                      "&a; auto m = a && b; }\n");
    EXPECT_EQ(typed.Of("s"), "<unknown>");
    EXPECT_EQ(typed.Of("c"), "<unknown>");
    EXPECT_EQ(typed.Of("n"), "<unknown>");
    EXPECT_EQ(typed.Of("p"), "<unknown>"); // operator& may be overloaded
    EXPECT_EQ(typed.Of("m"), "<unknown>");
}

TEST(Typer, BuiltinCastsTypeTheirWholeExpression)
{
    // Before the parser knew `(int)` was a cast these read as the operand's type, so
    // the Typer answered Unknown to stay safe; now the cast is its own node.
    const Typed typed(Body("double d = 0; int i = 0;\n"
                           "auto a = (int)d; auto b = (double)i / 2; auto c = (long)i + 1;"));
    EXPECT_EQ(typed.Of("a"), "int");
    EXPECT_EQ(typed.Of("b"), "double");
    EXPECT_EQ(typed.Of("c"), "long");
}

TEST(Typer, NodeTypeCoversEveryExpression)
{
    const Typed typed(Body("int i = 0; long l = 0; auto x = i + l;"));
    const auto& nodes  = typed.tree.Nodes();
    std::size_t binary = 0;
    for (std::uint32_t node = 0; node < nodes.size(); ++node)
    {
        if (nodes[node].kind == heimdall::GrammarKind::BinaryExpression)
        {
            ++binary;
            EXPECT_EQ(typed.types.Spell(typed.types.NodeType(node)), "long");
        }
        else if (nodes[node].kind == heimdall::GrammarKind::RecordDefinition)
        {
            EXPECT_EQ(typed.types.NodeType(node), heimdall::TypeTable::Unknown);
        }
    }

    EXPECT_EQ(binary, 1u);
    EXPECT_EQ(typed.types.NodeType(~0u), heimdall::TypeTable::Unknown);
    EXPECT_EQ(typed.types.SymbolType(~0u), heimdall::TypeTable::Unknown);
}

TEST(Typer, HandlesVeryLongExpressionChains)
{
    std::string left  = "int a = 1";
    std::string right = "int b; void f() { int c = 0; c = ";
    for (int i = 0; i < 3000; ++i)
    {
        left += " + 1";
    }

    // Right-associative chains recurse in the typer (and the grammar): the depth guard keeps it
    // bounded.
    for (int i = 0; i < 400; ++i)
    {
        right += "c = ";
    }

    const Typed deep_left(left + ";\n");
    EXPECT_EQ(deep_left.Of("a"), "int");
    const Typed deep_right(right + "1; }\n");
    EXPECT_NO_FATAL_FAILURE((void) deep_right.Of("c"));
}

TEST(Typer, ToleratesBrokenInput)
{
    const std::string sample =
        "struct P { int x; int* q; int get(); };\n"
        "void f(P p, std::vector<int>& v) {\n"
        "    auto a = p.x + (p.get() * 2);\n"
        "    for (size_t i = 0; i < v.size(); ++i) { if (!v[i] && p.q) {} }\n"
        "    auto b = static_cast<long>(a) ? p.q : nullptr;\n"
        "}\n";
    for (std::size_t length = 0; length <= sample.size(); ++length)
    {
        const std::string cut = sample.substr(0, length);
        const Typed       typed(cut);
        EXPECT_NO_FATAL_FAILURE((void) heimdall::SemanticRules::Analyze(typed.model, typed.types))
            << cut;
    }

    for (const char* source :
         { "auto x = ;", "int [", "void f( { int a = (", "a b c d", "auto = 1;", "int a[",
           "using = int;", "struct { int x; } s; auto y = s.x;", "std::vector<", "static_cast<>(",
           "x = y ? : z;" })
    {
        const Typed typed(source);
        EXPECT_NO_FATAL_FAILURE((void) heimdall::SemanticRules::Analyze(typed.model, typed.types))
            << source;
    }
}

TEST(Typer, IgnoresInactiveCode)
{
    const Typed typed("#if 0\n"
                      "int dead = 1;\n"
                      "#endif\n"
                      "void f() { int live = 1; }\n");
    EXPECT_EQ(typed.Of("live"), "int");
}

// ---- cpp/no-implicit-bool-conversion ------------------------------------------------------------

TEST(ImplicitBool, ReportsIntegersFloatsAndPointersInConditions)
{
    const std::string source = Body(
        "int count = 0; double ratio = 0; int* p = 0; char c = 0; std::size_t n = 0;\n"
        "if (count) {}\n"
        "while (ratio) {}\n"
        "if (p) {}\n"
        "if (c) {}\n"
        "for (; n;) {}\n"
        "bool b = count ? true : false;");
    const auto diagnostics = ImplicitBool(source);
    ASSERT_EQ(diagnostics.size(), 6u);
    EXPECT_EQ(diagnostics[0].code, "cpp/no-implicit-bool-conversion");
    EXPECT_EQ(diagnostics[0].rule, heimdall::RuleId::NoImplicitBoolConversion);
    EXPECT_NE(diagnostics[0].message.find("'int'"), std::string::npos);
    EXPECT_NE(diagnostics[0].message.find("an 'if' condition"), std::string::npos);
    EXPECT_NE(diagnostics[1].message.find("'double'"), std::string::npos);
    EXPECT_NE(diagnostics[1].message.find("a 'while' condition"), std::string::npos);
    EXPECT_NE(diagnostics[2].message.find("'int*'"), std::string::npos);
    EXPECT_NE(diagnostics[2].message.find("nullptr"), std::string::npos);
    EXPECT_NE(diagnostics[3].message.find("'char'"), std::string::npos);
    EXPECT_NE(diagnostics[4].message.find("'size_t'"), std::string::npos);
    EXPECT_NE(diagnostics[4].message.find("a 'for' condition"), std::string::npos);
    EXPECT_NE(diagnostics[5].message.find("'?:'"), std::string::npos);
    for (const auto& diagnostic : diagnostics)
    {
        EXPECT_FALSE(diagnostic.has_fix);
    }
}

TEST(ImplicitBool, ReportsOperandsOfLogicalOperators)
{
    const std::string source =
        Body("int a = 0; int* p = 0; bool ok = true;\n"
             "if (!a) {}\n"
             "if (ok && p) {}\n"
             "if (a || ok) {}\n"
             "bool r = !p;");
    const auto diagnostics = ImplicitBool(source);
    ASSERT_EQ(diagnostics.size(), 4u);
    EXPECT_EQ(source.substr(diagnostics[0].offset, diagnostics[0].length), "a");
    EXPECT_NE(diagnostics[0].message.find("the operand of '!'"), std::string::npos);
    EXPECT_EQ(source.substr(diagnostics[1].offset, diagnostics[1].length), "p");
    EXPECT_NE(diagnostics[1].message.find("an operand of '&&'"), std::string::npos);
    EXPECT_EQ(source.substr(diagnostics[2].offset, diagnostics[2].length), "a");
    EXPECT_NE(diagnostics[2].message.find("an operand of '||'"), std::string::npos);
    EXPECT_EQ(source.substr(diagnostics[3].offset, diagnostics[3].length), "p");
}

TEST(ImplicitBool, ReportsTheLocationOfTheOperand)
{
    const std::string source      = "void f(int n) {\n    if (n + 1) {}\n}\n";
    const auto        diagnostics = ImplicitBool(source);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(source.substr(diagnostics[0].offset, diagnostics[0].length), "n + 1");
    EXPECT_EQ(diagnostics[0].line, 2u); // 1-based
}

TEST(ImplicitBool, SilentForBooleansAndComparisons)
{
    const std::string source = Body(
        "bool ok = true; int a = 0; int* p = 0;\n"
        "if (ok) {}\n"
        "if (a > 0) {}\n"
        "if (p != nullptr) {}\n"
        "if (a == 0 && p) {}\n"
        "if (a < 1 || ok) {}\n"
        "while (!ok) {}\n"
        "bool b = static_cast<bool>(a);\n"
        "if (static_cast<bool>(a)) {}");
    const auto diagnostics = ImplicitBool(source);
    // only `p` in `a == 0 && p` is a conversion
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(source.substr(diagnostics[0].offset, diagnostics[0].length), "p");
}

TEST(ImplicitBool, SilentWhenTheTypeIsUnknown)
{
    const std::string source =
        "template <class T> void g(T value, T* ptr) { if (value) {} if (ptr) {} }\n"
        "void f() {\n"
        "    auto x = missing();\n"
        "    if (x) {}\n"
        "    if (unknown_name) {}\n"
        "    if (!unknown_name) {}\n"
        "    std::unique_ptr<int> up;\n"
        "    if (up) {}\n"
        "    std::optional<int> o;\n"
        "    if (o && x) {}\n"
        "}\n";
    const auto diagnostics = ImplicitBool(source);
    // `T*` is known to be a pointer even though T is not
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(source.substr(diagnostics[0].offset, diagnostics[0].length), "ptr");
}

TEST(ImplicitBool, SilentForClassesAndEnums)
{
    const std::string source =
        "struct Flag { explicit operator bool() const; };\n"
        "enum Mode { Off, On };\n"
        "void f(Flag flag, Mode mode, Flag* none) {\n"
        "    if (flag) {}\n"
        "    if (mode) {}\n"
        "    if (!flag) {}\n"
        "}\n";
    EXPECT_TRUE(ImplicitBool(source).empty());
}

TEST(ImplicitBool, SilentForLiteralConditionsAndDoubleNegation)
{
    const std::string source = Body(
        "int x = 0;\n"
        "while (1) {}\n"
        "for (;;) {}\n"
        "if (0) {}\n"
        "bool b = !!x;\n"
        "if (!!x) {}");
    EXPECT_TRUE(ImplicitBool(source).empty());
}

TEST(ImplicitBool, SilentForDeclarationConditionsAndInitStatements)
{
    const std::string source =
        Body("int* next();\n"
             "while (int* p = next()) {}\n"
             "if (int* q = next()) {}");
    EXPECT_TRUE(ImplicitBool(source).empty());
    // The grammar gives `if (init; cond)` no node for the condition: silent.
    const std::string with_init = Body("int a = 0;\nif (int b = 1; a) {}");
    EXPECT_TRUE(ImplicitBool(with_init).empty());
}

TEST(ImplicitBool, SilentForCStyleCasts)
{
    const std::string source =
        Body("int a = 0;\nif ((bool)a) {}\nif (!(bool)a) {}\nbool b = (bool)a && a > 0;");
    EXPECT_TRUE(ImplicitBool(source).empty());
}

TEST(ImplicitBool, ReportsUseOfMembersAndCalls)
{
    const std::string source =
        "struct S { int n; int* p; int size() const; };\n"
        "int* find();\n"
        "void f(S s, S* sp) {\n"
        "    if (s.n) {}\n"
        "    if (sp->p) {}\n"
        "    if (s.size()) {}\n"
        "    if (find()) {}\n"
        "}\n";
    EXPECT_EQ(ImplicitBool(source).size(), 4u);
}

TEST(ImplicitBool, IgnoresInactiveCode)
{
    const std::string source =
        "void f(int n) {\n"
        "#if 0\n"
        "    if (n) {}\n"
        "#endif\n"
        "}\n";
    EXPECT_TRUE(ImplicitBool(source).empty());
}

TEST(ImplicitBool, ToleratesBrokenInput)
{
    for (const char* source :
         { "void f(int n) { if (n", "void f() { while (", "void f(int a) { if (a &&",
           "void f() { for (;", "void f(int a) { a ? : ; }", "void f(int a) { if (a ? b : ) }",
           "void f(int a) { !", "if ()" })
    {
        EXPECT_NO_FATAL_FAILURE(ImplicitBool(source)) << source;
    }
}

// ---- cpp/modernize-range-loop -----------------------------------------------------------------

TEST(RangeLoop, ConvertsAnIndexLoopOverAVector)
{
    const std::string source =
        "void f(std::vector<int>& v) {\n"
        "    for (int i = 0; i < v.size(); ++i) {\n"
        "        use(v[i]);\n"
        "    }\n"
        "}\n";
    const auto diagnostics = RangeLoop(source);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "cpp/modernize-range-loop");
    EXPECT_EQ(diagnostics[0].rule, heimdall::RuleId::ModernizeRangeLoop);
    EXPECT_EQ(source.substr(diagnostics[0].offset, diagnostics[0].length),
              "for (int i = 0; i < v.size(); ++i)");
    EXPECT_TRUE(diagnostics[0].has_fix);
    EXPECT_FALSE(diagnostics[0].fix_is_safe);
    EXPECT_EQ(ApplyFix(source, diagnostics[0]),
              "void f(std::vector<int>& v) {\n"
              "    for (auto& element : v) {\n"
              "        use(element);\n"
              "    }\n"
              "}\n");
}

TEST(RangeLoop, ConvertsLoopsOverArrays)
{
    const std::string source =
        "void f() {\n"
        "    int a[8];\n"
        "    for (unsigned i = 0; i < 8; i++) a[i] = 0;\n"
        "    for (size_t j = 0; j < std::size(a); j += 1) { a[j] += a[j]; }\n"
        "}\n";
    const auto diagnostics = RangeLoop(source);
    ASSERT_EQ(diagnostics.size(), 2u);
    EXPECT_EQ(ApplyFix(source, diagnostics[0]),
              "void f() {\n"
              "    int a[8];\n"
              "    for (auto& element : a) element = 0;\n"
              "    for (size_t j = 0; j < std::size(a); j += 1) { a[j] += a[j]; }\n"
              "}\n");
    EXPECT_EQ(ApplyFix(source, diagnostics[1]),
              "void f() {\n"
              "    int a[8];\n"
              "    for (unsigned i = 0; i < 8; i++) a[i] = 0;\n"
              "    for (auto& element : a) { element += element; }\n"
              "}\n");
}

TEST(RangeLoop, ConvertsLoopsOverStrings)
{
    const std::string source =
        "int f(const std::string& s) {\n"
        "    int n = 0;\n"
        "    for (std::size_t i = 0; i < s.size(); ++i) { if (s[i] == 'a') ++n; }\n"
        "    return n;\n"
        "}\n";
    const auto diagnostics = RangeLoop(source);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(ApplyFix(source, diagnostics[0]),
              "int f(const std::string& s) {\n"
              "    int n = 0;\n"
              "    for (const auto& element : s) { if (element == 'a') ++n; }\n"
              "    return n;\n"
              "}\n");
}

TEST(RangeLoop, KeepsConstOfConstArrays)
{
    const std::string source =
        "int f() {\n"
        "    const int a[2] = {1, 2};\n"
        "    int sum = 0;\n"
        "    for (int i = 0; i < 2; ++i) sum += a[i];\n"
        "    return sum;\n"
        "}\n";
    const auto diagnostics = RangeLoop(source);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_NE(diagnostics[0].fix.replacement.find("const auto& element : a"), std::string::npos);
}

TEST(RangeLoop, PicksANameThatIsNotInUse)
{
    const std::string source =
        "void f(std::vector<int>& v) {\n"
        "    int element = 0;\n"
        "    for (int i = 0; i < v.size(); ++i) { element += v[i]; }\n"
        "}\n";
    const auto diagnostics = RangeLoop(source);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_NE(diagnostics[0].fix.replacement.find("auto& item : v"), std::string::npos);
    EXPECT_NE(diagnostics[0].fix.replacement.find("element += item;"), std::string::npos);
}

TEST(RangeLoop, SilentWhenTheIndexIsUsedForAnythingElse)
{
    const std::string source =
        "void f(std::vector<int>& v, int* out) {\n"
        "    for (int i = 0; i < v.size(); ++i) { out[i] = v[i]; }\n"
        "    for (int i = 0; i < v.size(); ++i) { use(i, v[i]); }\n"
        "    for (int i = 0; i < v.size(); ++i) { use(v[i + 1]); }\n"
        "    for (int i = 0; i < v.size(); ++i) { use(v[i], v[i - 1]); }\n"
        "    for (int i = 0; i < v.size(); ++i) { }\n"
        "}\n";
    EXPECT_TRUE(RangeLoop(source).empty());
}

TEST(RangeLoop, SilentWhenTheContainerIsUsedOtherwise)
{
    const std::string source =
        "void f(std::vector<int>& v) {\n"
        "    for (int i = 0; i < v.size(); ++i) { if (v[i]) v.push_back(1); }\n"
        "    for (int i = 0; i < v.size(); ++i) { use(v, v[i]); }\n"
        "    for (int i = 0; i < v.size(); ++i) { use(v.back(), v[i]); }\n"
        "}\n";
    EXPECT_TRUE(RangeLoop(source).empty());
}

TEST(RangeLoop, SilentWhenTheContainerTypeIsUnknown)
{
    const std::string source =
        "void f(Foo& foo, int* raw, int n) {\n"
        "    for (int i = 0; i < foo.size(); ++i) { use(foo[i]); }\n"
        "    for (int i = 0; i < n; ++i) { use(raw[i]); }\n"
        "    for (int i = 0; i < unknown.size(); ++i) { use(unknown[i]); }\n"
        "    for (int i = 0; i < std::size(raw); ++i) { use(raw[i]); }\n"
        "}\n"
        "template <class C> void g(C& c) { for (int i = 0; i < c.size(); ++i) { use(c[i]); } }\n";
    EXPECT_TRUE(RangeLoop(source).empty());
}

TEST(RangeLoop, SilentWhenTheArrayBoundDiffers)
{
    const std::string source =
        "void f() {\n"
        "    int a[8];\n"
        "    for (int i = 0; i < 4; ++i) use(a[i]);\n"
        "    for (int i = 0; i < 9; ++i) use(a[i]);\n"
        "}\n";
    EXPECT_TRUE(RangeLoop(source).empty());
}

TEST(RangeLoop, SilentForVectorOfBool)
{
    const std::string source =
        "void f(std::vector<bool>& v) { for (int i = 0; i < v.size(); ++i) { use(v[i]); } }\n";
    EXPECT_TRUE(RangeLoop(source).empty());
}

TEST(RangeLoop, SilentForOtherLoopShapes)
{
    const std::string source =
        "void f(std::vector<int>& v) {\n"
        "    for (int i = 1; i < v.size(); ++i) use(v[i]);\n"
        "    for (int i = 0; i <= v.size(); ++i) use(v[i]);\n"
        "    for (int i = 0; i < v.size(); i += 2) use(v[i]);\n"
        "    for (int i = 0; i < v.size(); --i) use(v[i]);\n"
        "    for (double d = 0; d < v.size(); ++d) use(v[d]);\n"
        "    for (int i = 0, j = 0; i < v.size(); ++i) use(v[i]);\n"
        "    for (auto& x: v) use(x);\n"
        "    for (int i = 0; i < v.size();) use(v[i++]);\n"
        "}\n";
    EXPECT_TRUE(RangeLoop(source).empty());
}

TEST(RangeLoop, SilentWhenALambdaCouldCaptureTheIndex)
{
    const std::string source =
        "void f(std::vector<int>& v) {\n"
        "    for (int i = 0; i < v.size(); ++i) { run([&] { use(v[i]); }); }\n"
        "}\n";
    EXPECT_TRUE(RangeLoop(source).empty());
}

TEST(RangeLoop, SilentForClassMembers)
{
    const std::string source =
        "struct S {\n"
        "    std::vector<int> items;\n"
        "    void f() { for (int i = 0; i < items.size(); ++i) { use(items[i]); } }\n"
        "};\n";
    EXPECT_TRUE(RangeLoop(source).empty());
}

TEST(RangeLoop, SilentWhenTheIndexIsShadowed)
{
    const std::string source =
        "void f(std::vector<int>& v) {\n"
        "    for (int i = 0; i < v.size(); ++i) { for (int i = 0; i < 2; ++i) { use(v[i]); } }\n"
        "}\n";
    EXPECT_TRUE(RangeLoop(source).empty());
}

TEST(RangeLoop, IgnoresInactiveCode)
{
    const std::string source =
        "void f(std::vector<int>& v) {\n"
        "#if 0\n"
        "    for (int i = 0; i < v.size(); ++i) { use(v[i]); }\n"
        "#endif\n"
        "}\n";
    EXPECT_TRUE(RangeLoop(source).empty());
}

TEST(RangeLoop, ToleratesBrokenInput)
{
    const std::string sample =
        "void f(std::vector<int>& v) {\n"
        "    for (int i = 0; i < v.size(); ++i) { use(v[i]); }\n"
        "}\n";
    for (std::size_t length = 0; length <= sample.size(); ++length)
    {
        EXPECT_NO_FATAL_FAILURE(RangeLoop(sample.substr(0, length))) << length;
    }
}

// ---- cpp/modernize-loop-convert
// -------------------------------------------------------------------

TEST(LoopConvert, ConvertsAnIteratorLoop)
{
    const std::string source =
        "void f(std::vector<int>& v) {\n"
        "    for (auto it = v.begin(); it != v.end(); ++it) {\n"
        "        use(*it);\n"
        "    }\n"
        "}\n";
    const auto diagnostics = LoopConvert(source);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "cpp/modernize-loop-convert");
    EXPECT_EQ(diagnostics[0].rule, heimdall::RuleId::ModernizeLoopConvert);
    EXPECT_EQ(source.substr(diagnostics[0].offset, diagnostics[0].length),
              "for (auto it = v.begin(); it != v.end(); ++it)");
    EXPECT_FALSE(diagnostics[0].fix_is_safe);
    EXPECT_EQ(ApplyFix(source, diagnostics[0]),
              "void f(std::vector<int>& v) {\n"
              "    for (auto& element : v) {\n"
              "        use(element);\n"
              "    }\n"
              "}\n");
}

TEST(LoopConvert, ConvertsMemberAccessAndMapIteration)
{
    const std::string source =
        "void f(std::map<int, std::string>& m, std::list<Foo>& l) {\n"
        "    for (auto it = m.begin(); it != m.end(); it++) use(it->first, it->second);\n"
        "    for (auto it = l.begin(); it != l.end(); ++it) { it->run(); (*it).stop(); }\n"
        "}\n";
    const auto diagnostics = LoopConvert(source);
    ASSERT_EQ(diagnostics.size(), 2u);
    EXPECT_EQ(ApplyFix(source, diagnostics[0]),
              "void f(std::map<int, std::string>& m, std::list<Foo>& l) {\n"
              "    for (auto& element : m) use(element.first, element.second);\n"
              "    for (auto it = l.begin(); it != l.end(); ++it) { it->run(); (*it).stop(); }\n"
              "}\n");
    EXPECT_EQ(ApplyFix(source, diagnostics[1]),
              "void f(std::map<int, std::string>& m, std::list<Foo>& l) {\n"
              "    for (auto it = m.begin(); it != m.end(); it++) use(it->first, it->second);\n"
              "    for (auto& element : l) { element.run(); (element).stop(); }\n"
              "}\n");
}

TEST(LoopConvert, UsesConstForConstContainersAndCbegin)
{
    const std::string source =
        "void f(const std::vector<int>& v, std::vector<int>& w) {\n"
        "    for (auto it = v.begin(); it != v.end(); ++it) use(*it);\n"
        "    for (auto it = w.cbegin(); it != w.cend(); ++it) use(*it);\n"
        "}\n";
    const auto diagnostics = LoopConvert(source);
    ASSERT_EQ(diagnostics.size(), 2u);
    EXPECT_NE(diagnostics[0].fix.replacement.find("const auto& element : v"), std::string::npos);
    EXPECT_NE(diagnostics[1].fix.replacement.find("const auto& element : w"), std::string::npos);
}

TEST(LoopConvert, ConvertsFreeBeginEndOverArraysAndContainers)
{
    const std::string source =
        "void f(std::vector<int>& v) {\n"
        "    int a[4];\n"
        "    for (auto it = std::begin(a); it != std::end(a); ++it) *it = 0;\n"
        "    for (auto it = std::begin(v); it != std::end(v); ++it) use(*it);\n"
        "}\n";
    const auto diagnostics = LoopConvert(source);
    ASSERT_EQ(diagnostics.size(), 2u);
    EXPECT_NE(diagnostics[0].fix.replacement.find("for (auto& element : a) element = 0;"),
              std::string::npos);
    EXPECT_NE(diagnostics[1].fix.replacement.find("for (auto& element : v) use(element);"),
              std::string::npos);
}

TEST(LoopConvert, MultiplicationIsNotADereference)
{
    const std::string source =
        "void f(std::vector<int>& v, int k) {\n"
        "    for (auto it = v.begin(); it != v.end(); ++it) { use(k * it); }\n"
        "}\n";
    EXPECT_TRUE(LoopConvert(source).empty());
}

TEST(LoopConvert, SilentWhenTheIteratorEscapes)
{
    const std::string source =
        "void f(std::vector<int>& v) {\n"
        "    for (auto it = v.begin(); it != v.end(); ++it) { use(it); }\n"
        "    for (auto it = v.begin(); it != v.end(); ++it) { use(*(it + 1)); }\n"
        "    for (auto it = v.begin(); it != v.end(); ++it) { v.erase(it); }\n"
        "    for (auto it = v.begin(); it != v.end(); ++it) { auto next = it; }\n"
        "    for (auto it = v.begin(); it != v.end(); ++it) { }\n"
        "}\n";
    EXPECT_TRUE(LoopConvert(source).empty());
}

TEST(LoopConvert, SilentWhenTheContainerIsTouched)
{
    const std::string source =
        "void f(std::vector<int>& v) {\n"
        "    for (auto it = v.begin(); it != v.end(); ++it) { if (*it) v.push_back(1); }\n"
        "    for (auto it = v.begin(); it != v.end(); ++it) { use(v.size(), *it); }\n"
        "}\n";
    EXPECT_TRUE(LoopConvert(source).empty());
}

TEST(LoopConvert, SilentForUnknownContainers)
{
    const std::string source =
        "void f(Foo& foo, int* raw) {\n"
        "    for (auto it = foo.begin(); it != foo.end(); ++it) use(*it);\n"
        "    for (auto it = unknown.begin(); it != unknown.end(); ++it) use(*it);\n"
        "    for (auto it = std::begin(raw); it != std::end(raw); ++it) use(*it);\n"
        "}\n"
        "template <class C> void g(C& c) { for (auto it = c.begin(); it != c.end(); ++it) "
        "use(*it); }\n";
    EXPECT_TRUE(LoopConvert(source).empty());
}

TEST(LoopConvert, SilentForOtherLoopShapes)
{
    const std::string source =
        "void f(std::vector<int>& v, std::vector<int>& w) {\n"
        "    for (auto it = v.begin(); it != w.end(); ++it) use(*it);\n"
        "    for (auto it = v.begin(); it != v.end(); it += 2) use(*it);\n"
        "    for (auto it = v.begin() + 1; it != v.end(); ++it) use(*it);\n"
        "    for (auto it = v.rbegin(); it != v.rend(); ++it) use(*it);\n"
        "    for (std::vector<int>::iterator it = v.begin(); it != v.end(); ++it) use(*it);\n"
        "    for (auto it = v.begin(); it < v.end(); ++it) use(*it);\n"
        "    for (auto it = v.cbegin(); it != v.end(); ++it) use(*it);\n"
        "}\n";
    EXPECT_TRUE(LoopConvert(source).empty());
}

TEST(LoopConvert, SilentForVectorOfBoolAndMembers)
{
    const std::string source =
        "struct S {\n"
        "    std::vector<int> items;\n"
        "    void f() { for (auto it = items.begin(); it != items.end(); ++it) use(*it); }\n"
        "};\n"
        "void g(std::vector<bool>& v) { for (auto it = v.begin(); it != v.end(); ++it) use(*it); "
        "}\n";
    EXPECT_TRUE(LoopConvert(source).empty());
}

TEST(LoopConvert, SilentWhenALambdaCouldCaptureTheIterator)
{
    const std::string source =
        "void f(std::vector<int>& v) {\n"
        "    for (auto it = v.begin(); it != v.end(); ++it) { run([&] { use(*it); }); }\n"
        "}\n";
    EXPECT_TRUE(LoopConvert(source).empty());
}

TEST(LoopConvert, IgnoresInactiveCode)
{
    const std::string source =
        "void f(std::vector<int>& v) {\n"
        "#if 0\n"
        "    for (auto it = v.begin(); it != v.end(); ++it) use(*it);\n"
        "#endif\n"
        "}\n";
    EXPECT_TRUE(LoopConvert(source).empty());
}

TEST(LoopConvert, ToleratesBrokenInput)
{
    const std::string sample =
        "void f(std::vector<int>& v) {\n"
        "    for (auto it = v.begin(); it != v.end(); ++it) { use(*it); it->x; }\n"
        "}\n";
    for (std::size_t length = 0; length <= sample.size(); ++length)
    {
        EXPECT_NO_FATAL_FAILURE(LoopConvert(sample.substr(0, length))) << length;
    }
}

// ---- integration
// ----------------------------------------------------------------------------------

TEST(TyperRules, AreInTheCatalog)
{
    for (const char* code : { "cpp/no-implicit-bool-conversion", "cpp/modernize-range-loop",
                              "cpp/modernize-loop-convert" })
    {
        EXPECT_TRUE(heimdall::IsKnownRuleCode(code)) << code;
    }

    EXPECT_EQ(heimdall::FindRuleByCode("cpp/no-implicit-bool-conversion")->id,
              heimdall::RuleId::NoImplicitBoolConversion);
    EXPECT_EQ(heimdall::FindRuleByCode("cpp/modernize-range-loop")->id,
              heimdall::RuleId::ModernizeRangeLoop);
    EXPECT_EQ(heimdall::FindRuleByCode("cpp/modernize-loop-convert")->id,
              heimdall::RuleId::ModernizeLoopConvert);
}

TEST(TyperRules, AnalyzeRunsThemAllSortedByOffset)
{
    const std::string source =
        "void f(std::vector<int>& v, int n) {\n"
        "    if (n) {}\n"
        "    for (int i = 0; i < v.size(); ++i) { use(v[i]); }\n"
        "    for (auto it = v.begin(); it != v.end(); ++it) { use(*it); }\n"
        "    void* p = (void*)0;\n"
        "}\n";
    const Typed typed(source);
    const auto  with_types = heimdall::SemanticRules::Analyze(typed.model, typed.types);
    const auto  standalone = heimdall::SemanticRules::Analyze(typed.model);
    ASSERT_EQ(with_types.size(), 4u);
    ASSERT_EQ(standalone.size(), with_types.size());
    EXPECT_EQ(with_types[0].code, "cpp/no-implicit-bool-conversion");
    EXPECT_EQ(with_types[1].code, "cpp/modernize-range-loop");
    EXPECT_EQ(with_types[2].code, "cpp/modernize-loop-convert");
    EXPECT_EQ(with_types[3].code, "cpp/modernize-nullptr");
    for (std::size_t i = 1; i < with_types.size(); ++i)
    {
        EXPECT_LE(with_types[i - 1].offset, with_types[i].offset);
    }
}

TEST(TyperRules, PolicyCanDisableAndSuppressEachRule)
{
    const std::string source =
        "void f(std::vector<int>& v, int n) {\n"
        "    if (n) {} // heimdall-disable-line cpp/no-implicit-bool-conversion\n"
        "    for (int i = 0; i < v.size(); ++i) { use(v[i]); }\n"
        "    for (auto it = v.begin(); it != v.end(); ++it) { use(*it); }\n"
        "}\n";
    const Typed typed(source);
    const auto  raw = heimdall::SemanticRules::Analyze(typed.model, typed.types);
    ASSERT_EQ(raw.size(), 3u);

    heimdall::RuleOptions options;
    options.overrides.push_back({ "cpp/modernize-range-loop", false, heimdall::Severity::Warning });
    const auto filtered = heimdall::RuleEngine(options).ApplyPolicy(raw, typed.tree);
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0].code, "cpp/modernize-loop-convert");
}

TEST(TyperRules, TypeModelIsIndependentOfTheRuleThatAsksForIt)
{
    // Two models over the same source agree: the Typer is deterministic.
    const std::string source = Body("int a = 1; long b = a + 2; auto c = &b;");
    const Typed       first(source);
    const Typed       second(source);
    ASSERT_EQ(first.types.Types().Size(), second.types.Types().Size());
    for (const char* name : { "a", "b", "c" })
    {
        EXPECT_EQ(first.Of(name), second.Of(name));
    }

    EXPECT_GT(first.types.ArenaBytes(), 0u);
}

TEST(Typer, CStyleCastsTakeTheCastType)
{
    const Typed typed("struct Foo { int v; };\n"
                      "void f(void* p, double d) {\n"
                      "    auto a = (Foo*)p; auto b = (int)d; auto c = (unsigned long)d; auto e = "
                      "(const Foo*)p;\n"
                      "    auto g = (Unknown*)p;\n"
                      "}\n");
    EXPECT_EQ(typed.Of("a"), "Foo*");
    EXPECT_EQ(typed.Of("b"), "int");
    EXPECT_EQ(typed.Of("c"), "unsigned long");
    EXPECT_EQ(typed.Of("e"), "const Foo*");
    EXPECT_EQ(typed.Of("g"), "<unknown>"); // not known to be a type: still a parenthesized group
}

TEST(Typer, ProductsOfVariablesAreTypedAsExpressions)
{
    const Typed typed("void f(int a, long b) { auto x = a * b; a * b; }\n");
    EXPECT_EQ(typed.Of("x"), "long");
}
