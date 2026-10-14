#include <Heimdall/SemanticModel.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace
{

    // Owns the source so the tree's views stay valid; not movable because the
    // model keeps a pointer to the tree.
    struct Bound
    {
        explicit Bound(std::string source) :
            text(std::make_shared<const std::string>(std::move(source))),
            tree(heimdall::ParseTree::Parse(*text)), model(heimdall::Binder::Bind(tree)) {}

        Bound(const Bound&) = delete;

        Bound& operator= (const Bound&) = delete;

        std::shared_ptr<const std::string> text;
        heimdall::ParseTree tree;
        heimdall::SemanticModel model;

        // First symbol with this spelling (optionally of a kind), or kNone.
        heimdall::SymbolId Find(std::string_view name, heimdall::SymbolKind kind) const
        {
            const auto& symbols = model.Symbols();
            for (heimdall::SymbolId i = 0; i < symbols.Size(); ++i)
            {
                if (symbols.kind[i] == kind && model.Names().Text(symbols.name[i]) == name)
                {
                    return i;
                }
            }

            return heimdall::kNone;
        }

        // Token index of the n-th identifier token spelled `name`.
        std::uint32_t Token(std::string_view name, std::size_t occurrence = 0) const
        {
            const auto& tokens = tree.Tokens();
            for (std::uint32_t i = 0; i < tokens.size(); ++i)
            {
                if (tokens[i].kind == heimdall::TokenKind::Identifier &&
                    tree.Text(tokens[i]) == name && occurrence--== 0)
                {
                    return i;
                }
            }

            return heimdall::kNone;
        }

        bool Has(heimdall::SymbolId symbol, std::uint32_t flag) const
        {
            return (model.Symbols().flags[symbol] & flag) != 0;
        }
    };

} // namespace

TEST(InternPool, EqualTextsShareAnIdAndUnknownTextsAreNotFound)
{
    heimdall::Arena arena;
    heimdall::InternPool pool(arena.Resource());
    const std::string source = "alpha beta alpha";
    const auto a = pool.Intern(std::string_view(source).substr(0, 5));
    const auto b = pool.Intern(std::string_view(source).substr(6, 4));
    const auto again = pool.Intern(std::string_view(source).substr(11, 5));
    EXPECT_EQ(a, again);
    EXPECT_NE(a, b);
    EXPECT_EQ(pool.Text(b), "beta");
    EXPECT_EQ(pool.Find("alpha"), a);
    EXPECT_EQ(pool.Find("gamma"), heimdall::kNone);
    EXPECT_EQ(pool.Size(), 2u);
}

TEST(Binder, RecordsTemplateParameterCategoriesDefaultsAndPacks)
{
    const Bound bound("template<class T = int, int N = 3, template<class> class C, class... Ts> struct Box {};");
    const auto& declarations = bound.model.Templates();
    const auto& parameters = bound.model.TemplateParameters();

    ASSERT_EQ(declarations.owner.size(), 1u);
    EXPECT_EQ(declarations.parameterCount[0], 4u);
    EXPECT_EQ(parameters.kind, (std::pmr::vector<heimdall::TemplateParameterKind> {
            heimdall::TemplateParameterKind::Type, heimdall::TemplateParameterKind::Value,
            heimdall::TemplateParameterKind::Template, heimdall::TemplateParameterKind::Type}));
    EXPECT_NE(parameters.defaultBegin[0], heimdall::kNone);
    EXPECT_NE(parameters.defaultBegin[1], heimdall::kNone);
    EXPECT_EQ(parameters.defaultBegin[2], heimdall::kNone);
    EXPECT_TRUE(parameters.pack[3]);
    EXPECT_EQ(bound.model.Scopes().kind[declarations.scope[0]], heimdall::ScopeKind::Template);
    EXPECT_EQ(bound.model.Lookup(0, bound.model.Names().Find("T")), heimdall::kNone);
    EXPECT_EQ(bound.model.Lookup(0, bound.model.Names().Find("N")), heimdall::kNone);
}

TEST(Binder, NestedTemplateParametersUseLexicalScopesWithoutLeaking)
{
    const Bound bound("template<class T> struct Outer { template<class U> U f(U value) { return value; } T field; }; int f(int value) { return value; }");
    const auto& declarations = bound.model.Templates();
    const auto& parameters = bound.model.TemplateParameters();

    ASSERT_EQ(declarations.owner.size(), 2u);
    ASSERT_EQ(parameters.symbol.size(), 2u);
    EXPECT_EQ(bound.model.Lookup(declarations.scope[1], bound.model.Names().Find("T")),
              parameters.symbol[0]);
    EXPECT_EQ(bound.model.Lookup(declarations.scope[0], bound.model.Names().Find("U")),
              heimdall::kNone);
    EXPECT_EQ(bound.model.Lookup(0, bound.model.Names().Find("U")), heimdall::kNone);
}

TEST(Binder, TemplateParameterShadowsOuterNamesAndQualifiedUsesStayDependent)
{
    const Bound bound("struct T {}; template<class T> int f() { return T::value; } int g() { T value; return 0; }");
    const auto parameter = bound.Find("T", heimdall::SymbolKind::TemplateParameter);

    EXPECT_EQ(bound.model.ResolveToken(bound.Token("T", 2)), parameter);
    EXPECT_EQ(bound.model.ReferenceState(bound.Token("value")), heimdall::RefState::Dependent);
    EXPECT_EQ(bound.model.Lookup(0, bound.model.Names().Find("T")),
              bound.Find("T", heimdall::SymbolKind::Class));
}

TEST(Binder, ParenthesizedValueDefaultsDoNotCreateExtraParameters)
{
    const Bound bound("template<int N = (1 > 0, 2), class T = int> struct Box {};");

    ASSERT_EQ(bound.model.Templates().parameterCount.size(), 1u);
    EXPECT_EQ(bound.model.Templates().parameterCount[0], 2u);
}

TEST(Binder, LaterTemplateParametersAreNotVisibleInEarlierDefaults)
{
    const Bound bound("struct U {}; template<class T = U, class U = int> struct Box {};");
    const auto defaultToken = bound.model.TemplateParameters().defaultBegin[0];
    const auto scope = bound.model.Templates().scope[0];

    EXPECT_EQ(bound.model.Lookup(scope, bound.model.Names().Find("U"), defaultToken),
              bound.Find("U", heimdall::SymbolKind::Class));
}

TEST(Binder, UnnamedQualifiedValueTypesDoNotDeclareTheirTypeNamesAsParameters)
{
    const Bound bound("template<std::size_t, const Type, template<class...> class C> struct Box {};");
    const auto& parameters = bound.model.TemplateParameters();

    ASSERT_EQ(parameters.symbol.size(), 3u);
    EXPECT_EQ(parameters.symbol[0], heimdall::kNone);
    EXPECT_EQ(parameters.symbol[1], heimdall::kNone);
    EXPECT_NE(parameters.symbol[2], heimdall::kNone);
    EXPECT_FALSE(parameters.pack[2]);
}

TEST(InternPool, CopiedTextOutlivesItsSource)
{
    heimdall::Arena arena;
    heimdall::InternPool pool(arena.Resource());
    heimdall::NameId id;
    {
        std::string temporary = "operator==";
        id = pool.InternCopy(temporary);
        temporary.assign(temporary.size(), 'x');
    }

    EXPECT_EQ(pool.Text(id), "operator==");
    EXPECT_EQ(pool.Find("operator=="), id);
}

TEST(Arena, ResourceAllocationsAreAccountedFor)
{
    heimdall::Arena arena;
    const auto before = arena.Used();
    std::pmr::vector<int> values(arena.Resource());
    values.assign(1000, 7);
    EXPECT_GE(arena.Used(), before + 1000 * sizeof(int));
}

TEST(SemanticModel, ModelAllocatesFromItsArena)
{
    Bound bound("namespace n { class A { int x; void f(); }; }\n");
    EXPECT_GT(bound.model.ArenaBytes(), 0u);
}

TEST(Binder, BuildsNamespaceAndClassScopes)
{
    Bound bound("namespace outer { namespace inner { class Widget { int size; }; } }\n");
    const auto& scopes = bound.model.Scopes();
    const auto outer = bound.Find("outer", heimdall::SymbolKind::Namespace);
    const auto inner = bound.Find("inner", heimdall::SymbolKind::Namespace);
    const auto widget = bound.Find("Widget", heimdall::SymbolKind::Class);
    const auto size = bound.Find("size", heimdall::SymbolKind::Variable);
    ASSERT_NE(outer, heimdall::kNone);
    ASSERT_NE(inner, heimdall::kNone);
    ASSERT_NE(widget, heimdall::kNone);
    ASSERT_NE(size, heimdall::kNone);

    const auto& symbols = bound.model.Symbols();
    EXPECT_EQ(scopes.kind[symbols.member_scope[outer]], heimdall::ScopeKind::Namespace);
    EXPECT_EQ(scopes.parent[symbols.member_scope[inner]], symbols.member_scope[outer]);
    EXPECT_EQ(scopes.parent[symbols.member_scope[widget]], symbols.member_scope[inner]);
    EXPECT_EQ(scopes.kind[symbols.member_scope[widget]], heimdall::ScopeKind::Class);
    EXPECT_EQ(scopes.owner[symbols.member_scope[widget]], widget);
    EXPECT_EQ(symbols.scope[size], symbols.member_scope[widget]);
    EXPECT_EQ(bound.model.LookupMember(inner, bound.model.Names().Find("Widget")), widget);
}

TEST(Binder, ModelsTemplateParameterScopeAndDependentReferenceState)
{
    Bound bound("template<class T, int N> int f(int value, T object) "
        "{ return object.member + value + N; }");
    const auto typeParameter = bound.Find("T", heimdall::SymbolKind::TemplateParameter);
    const auto valueParameter = bound.Find("N", heimdall::SymbolKind::TemplateParameter);
    ASSERT_NE(typeParameter, heimdall::kNone);
    ASSERT_NE(valueParameter, heimdall::kNone);
    const auto& symbols = bound.model.Symbols();
    const auto value = bound.Find("value", heimdall::SymbolKind::Parameter);
    ASSERT_NE(value, heimdall::kNone);
    EXPECT_EQ(bound.tree.NodesSoA().Kind(symbols.decl_node[typeParameter]),
              heimdall::GrammarKind::TemplateDeclaration);
    EXPECT_EQ(bound.model.ResolveToken(bound.Token("N", 1)), valueParameter);
    EXPECT_EQ(bound.model.ReferenceState(bound.Token("member")), heimdall::RefState::Dependent);
}

TEST(Binder, ReopenedNamespaceSharesItsScope)
{
    Bound bound("namespace n { int a; }\nnamespace n { int b; }\n");
    const auto n = bound.Find("n", heimdall::SymbolKind::Namespace);
    ASSERT_NE(n, heimdall::kNone);
    const auto& names = bound.model.Names();
    EXPECT_NE(bound.model.LookupMember(n, names.Find("a")), heimdall::kNone);
    EXPECT_NE(bound.model.LookupMember(n, names.Find("b")), heimdall::kNone);
    // One symbol for the namespace, however many times it is opened.
    std::size_t count = 0;
    for (heimdall::SymbolId i = 0; i < bound.model.Symbols().Size(); ++i)
    {
        count += bound.model.Symbols().kind[i] == heimdall::SymbolKind::Namespace ? 1 : 0;
    }

    EXPECT_EQ(count, 1u);
}

TEST(Binder, RecordsMemberFunctionFlags)
{
    Bound bound(
        "struct S {\n"
        "    virtual void a(int x) const;\n"
        "    void b() override;\n"
        "    void c() final;\n"
        "    virtual void d() = 0;\n"
        "    static int e();\n"
        "    S();\n"
        "    ~S();\n"
        "    void f() {}\n"
        "};\n");
    const auto kind = heimdall::SymbolKind::Function;
    const auto a = bound.Find("a", kind);
    const auto b = bound.Find("b", kind);
    const auto c = bound.Find("c", kind);
    const auto d = bound.Find("d", kind);
    const auto e = bound.Find("e", kind);
    const auto f = bound.Find("f", kind);
    ASSERT_NE(a, heimdall::kNone);
    ASSERT_NE(b, heimdall::kNone);
    ASSERT_NE(c, heimdall::kNone);
    ASSERT_NE(d, heimdall::kNone);
    ASSERT_NE(e, heimdall::kNone);
    ASSERT_NE(f, heimdall::kNone);

    EXPECT_TRUE(bound.Has(a, heimdall::SymbolFlag::Virtual));
    EXPECT_TRUE(bound.Has(a, heimdall::SymbolFlag::Const));
    EXPECT_FALSE(bound.Has(a, heimdall::SymbolFlag::Override));
    EXPECT_TRUE(bound.Has(b, heimdall::SymbolFlag::Override));
    EXPECT_FALSE(bound.Has(b, heimdall::SymbolFlag::Virtual));
    EXPECT_TRUE(bound.Has(c, heimdall::SymbolFlag::Final));
    EXPECT_TRUE(bound.Has(d, heimdall::SymbolFlag::Pure));
    EXPECT_TRUE(bound.Has(e, heimdall::SymbolFlag::Static));
    EXPECT_TRUE(bound.Has(f, heimdall::SymbolFlag::Definition));
    EXPECT_FALSE(bound.Has(f, heimdall::SymbolFlag::Const));
}

TEST(Binder, ConstructorsAndDestructorsAreFlagged)
{
    Bound bound("struct S { S(); S(int x); ~S(); };\n");
    std::size_t constructors = 0;
    std::size_t destructors = 0;
    const auto& symbols = bound.model.Symbols();
    for (heimdall::SymbolId i = 0; i < symbols.Size(); ++i)
    {
        if (symbols.kind[i] != heimdall::SymbolKind::Function)
        {
            continue;
        }

        constructors += bound.Has(i, heimdall::SymbolFlag::Constructor) ? 1 : 0;
        destructors += bound.Has(i, heimdall::SymbolFlag::Destructor) ? 1 : 0;
    }

    EXPECT_EQ(constructors, 2u);
    EXPECT_EQ(destructors, 1u);
}

TEST(Binder, SignatureIgnoresParameterNamesAndDefaults)
{
    Bound bound("struct B { virtual void f(int a, const char* s); virtual void g(int a = 1); };\n"
        "struct D : B { void f(int renamed, const char* text); void g(int); };\n");
    const auto& symbols = bound.model.Symbols();
    const auto& names = bound.model.Names();
    const auto signature =[&](std::string_view name, std::size_t occurrence)
    {
        for (heimdall::SymbolId i = 0; i < symbols.Size(); ++i)
        {
            if (symbols.kind[i] == heimdall::SymbolKind::Function &&
                names.Text(symbols.name[i]) == name && occurrence--== 0)
            {
                return symbols.signature[i];
            }
        }

        return std::uint64_t {0};
    };

    EXPECT_NE(signature("f", 0), 0u);
    EXPECT_EQ(signature("f", 0), signature("f", 1));
    EXPECT_EQ(signature("g", 0), signature("g", 1));
    EXPECT_NE(signature("f", 0), signature("g", 0));
}

TEST(Binder, SignatureSeparatesTypesAndQualifiers)
{
    Bound bound("struct S { void f(int); void f(long); void f(int) const; void f(int&); void "
        "h(void); void h(); };\n");
    const auto& symbols = bound.model.Symbols();
    std::vector<std::uint64_t> f;
    std::vector<std::uint64_t> h;
    for (heimdall::SymbolId i = 0; i < symbols.Size(); ++i)
    {
        if (symbols.kind[i] != heimdall::SymbolKind::Function)
        {
            continue;
        }

        const auto text = bound.model.Names().Text(symbols.name[i]);
        (text == "f" ? f : h).push_back(symbols.signature[i]);
    }

    ASSERT_EQ(f.size(), 4u);
    for (std::size_t i = 0; i < f.size(); ++i)
    {
        for (std::size_t j = i + 1; j < f.size(); ++j)
        {
            EXPECT_NE(f[i], f[j]) << i << " vs " << j;
        }
    }

    ASSERT_EQ(h.size(), 2u);
    EXPECT_EQ(h[0], h[1]);
}

TEST(Binder, ResolvesBasesIncludingQualifiedOnes)
{
    Bound bound(
        "namespace lib { struct Base {}; }\n"
        "struct Local {};\n"
        "struct Derived : public lib::Base, private Local, virtual protected Missing {};\n");
    const auto derived = bound.Find("Derived", heimdall::SymbolKind::Class);
    ASSERT_NE(derived, heimdall::kNone);
    const auto& symbols = bound.model.Symbols();
    const auto& bases = bound.model.Bases();
    ASSERT_EQ(symbols.base_count[derived], 3u);
    const auto first = symbols.first_base[derived];
    EXPECT_EQ(bases.target[first], bound.Find("Base", heimdall::SymbolKind::Class));
    EXPECT_EQ(bases.target[first + 1], bound.Find("Local", heimdall::SymbolKind::Class));
    EXPECT_EQ(bases.target[first + 2], heimdall::kNone);
    EXPECT_EQ(bound.model.Names().Text(bases.name[first + 2]), "Missing");
}

TEST(Binder, TemplateIdBasesStayUnresolved)
{
    Bound bound("template <class T> struct Box {};\nstruct D : Box<int> {};\n");
    const auto derived = bound.Find("D", heimdall::SymbolKind::Class);
    ASSERT_NE(derived, heimdall::kNone);
    ASSERT_EQ(bound.model.Symbols().base_count[derived], 1u);
    EXPECT_EQ(bound.model.Bases().target[bound.model.Symbols().first_base[derived]],
              heimdall::kNone);
}

TEST(Binder, ClassNamedLikeItselfIsNotItsOwnBase)
{
    Bound bound("struct A : A {};\n");
    const auto a = bound.Find("A", heimdall::SymbolKind::Class);
    ASSERT_NE(a, heimdall::kNone);
    ASSERT_EQ(bound.model.Symbols().base_count[a], 1u);
    EXPECT_EQ(bound.model.Bases().target[bound.model.Symbols().first_base[a]], heimdall::kNone);
}

TEST(Binder, MemberLookupSearchesBases)
{
    Bound bound("struct B { int inherited; };\nstruct D : B { int own; };\n");
    const auto d = bound.Find("D", heimdall::SymbolKind::Class);
    const auto b = bound.Find("B", heimdall::SymbolKind::Class);
    const auto& names = bound.model.Names();
    EXPECT_EQ(bound.model.LookupMember(d, names.Find("inherited")),
              bound.Find("inherited", heimdall::SymbolKind::Variable));
    EXPECT_EQ(bound.model.LookupMember(b, names.Find("own")), heimdall::kNone);
}

TEST(Binder, CyclicInheritanceDoesNotHang)
{
    Bound bound("struct A : B {};\nstruct B : A {};\n");
    const auto a = bound.Find("A", heimdall::SymbolKind::Class);
    ASSERT_NE(a, heimdall::kNone);
    EXPECT_EQ(bound.model.LookupMember(a, bound.model.Names().Find("A")), heimdall::kNone);
}

TEST(Binder, ResolvesLocalsParametersAndGlobals)
{
    Bound bound("int g;\n"
        "int f(int p) {\n"
        "    int a = p;\n"
        "    return a + g;\n"
        "}\n");
    const auto p = bound.Find("p", heimdall::SymbolKind::Parameter);
    const auto a = bound.Find("a", heimdall::SymbolKind::Variable);
    const auto g = bound.Find("g", heimdall::SymbolKind::Variable);
    ASSERT_NE(p, heimdall::kNone);
    ASSERT_NE(a, heimdall::kNone);
    ASSERT_NE(g, heimdall::kNone);
    // Second spelling of each name is the use.
    EXPECT_EQ(bound.model.ResolveToken(bound.Token("p", 1)), p);
    EXPECT_EQ(bound.model.ResolveToken(bound.Token("a", 1)), a);
    EXPECT_EQ(bound.model.ResolveToken(bound.Token("g", 1)), g);
}

TEST(Binder, ConceptDefinitionsDeclareConceptSymbols)
{
    Bound bound("template <typename T> concept Small = sizeof(T) > 1;\n");
    const auto small = bound.Find("Small", heimdall::SymbolKind::Concept);

    ASSERT_NE(small, heimdall::kNone);
    EXPECT_TRUE(bound.Has(small, heimdall::SymbolFlag::Template));
    EXPECT_EQ(bound.model.Symbols().scope[small], heimdall::SemanticModel::TranslationUnitScope);
}

TEST(Binder, ConstrainedTemplateParametersDeclareTheirNames)
{
    Bound bound("template <Hashable T> void f(T t) {}\n");

    EXPECT_NE(bound.Find("T", heimdall::SymbolKind::TemplateParameter), heimdall::kNone);
    EXPECT_NE(bound.Find("t", heimdall::SymbolKind::Parameter), heimdall::kNone);
    EXPECT_NE(bound.Find("f", heimdall::SymbolKind::Function), heimdall::kNone);
}

TEST(Binder, RequiresLocalsResolveInsideTheirRequirements)
{
    Bound bound("template <typename T> concept C = requires (T a, T b) { a + b; };\n");
    const auto a = bound.Find("a", heimdall::SymbolKind::Parameter);
    const auto b = bound.Find("b", heimdall::SymbolKind::Parameter);

    ASSERT_NE(a, heimdall::kNone);
    ASSERT_NE(b, heimdall::kNone);
    // First spelling of each name is the declaration; the second is the use.
    EXPECT_EQ(bound.model.ResolveToken(bound.Token("a", 1)), a);
    EXPECT_EQ(bound.model.ResolveToken(bound.Token("b", 1)), b);
    // Locals live in the requirement's own scope, not the enclosing one.
    EXPECT_NE(bound.model.Symbols().scope[a], heimdall::SemanticModel::TranslationUnitScope);
}

TEST(Binder, InnerBlockShadowsOuterDeclaration)
{
    Bound bound("void f() {\n"
        "    int x = 1;\n"
        "    {\n"
        "        int x = 2;\n"
        "        x = 3;\n"
        "    }\n"
        "    x = 4;\n"
        "}\n");
    const auto& symbols = bound.model.Symbols();
    heimdall::SymbolId outer = heimdall::kNone;
    heimdall::SymbolId inner = heimdall::kNone;
    for (heimdall::SymbolId i = 0; i < symbols.Size(); ++i)
    {
        if (symbols.kind[i] == heimdall::SymbolKind::Variable)
        {
            (outer == heimdall::kNone ? outer : inner) = i;
        }
    }

    ASSERT_NE(inner, heimdall::kNone);
    EXPECT_EQ(bound.model.ResolveToken(bound.Token("x", 2)), inner);
    EXPECT_EQ(bound.model.ResolveToken(bound.Token("x", 3)), outer);
}

TEST(Binder, LocalIsNotVisibleBeforeItsDeclaration)
{
    Bound bound("int later;\n"
        "void f() {\n"
        "    later = 1;\n"
        "    int later = 2;\n"
        "}\n");
    const auto& symbols = bound.model.Symbols();
    heimdall::SymbolId global = heimdall::kNone;
    for (heimdall::SymbolId i = 0; i < symbols.Size(); ++i)
    {
        if (symbols.kind[i] == heimdall::SymbolKind::Variable)
        {
            global = i;
            break;
        }
    }

    ASSERT_NE(global, heimdall::kNone);
    // The first use sees the global, not the local declared after it.
    EXPECT_EQ(bound.model.ResolveToken(bound.Token("later", 1)), global);
}

TEST(Binder, OutOfClassDefinitionSeesClassMembers)
{
    Bound bound("struct S { int m; void h(int p); };\n"
        "void S::h(int p) {\n"
        "    int a = p + m;\n"
        "}\n");
    const auto m = bound.Find("m", heimdall::SymbolKind::Variable);
    ASSERT_NE(m, heimdall::kNone);
    EXPECT_EQ(bound.model.ResolveToken(bound.Token("m", 1)), m);
}

TEST(Binder, QualifiedDefinitionIsNotFoundByUnqualifiedLookup)
{
    Bound bound("struct S { void h(); };\n"
        "void S::h() {}\n");
    const auto& symbols = bound.model.Symbols();
    std::size_t qualified = 0;
    for (heimdall::SymbolId i = 0; i < symbols.Size(); ++i)
    {
        if (symbols.kind[i] == heimdall::SymbolKind::Function &&
            bound.Has(i, heimdall::SymbolFlag::Qualified))
        {
            ++qualified;
            EXPECT_TRUE(bound.Has(i, heimdall::SymbolFlag::Definition));
        }
    }

    EXPECT_EQ(qualified, 1u);
    // `h` at translation-unit scope still resolves to the member declaration only
    // through the class, never to the qualified definition.
    EXPECT_EQ(bound.model.Lookup(heimdall::SemanticModel::TranslationUnitScope,
                                 bound.model.Names().Find("h")),
        heimdall::kNone);
}

TEST(Binder, QualifiedNamesResolveThroughNamespaces)
{
    Bound bound("namespace n { int v; namespace deep { int w; } }\n"
        "void f() {\n"
        "    n::v = 1;\n"
        "    n::deep::w = 2;\n"
        "}\n");
    EXPECT_EQ(bound.model.ResolveToken(bound.Token("v", 1)),
              bound.Find("v", heimdall::SymbolKind::Variable));
    EXPECT_EQ(bound.model.ResolveToken(bound.Token("w", 1)),
              bound.Find("w", heimdall::SymbolKind::Variable));
    EXPECT_EQ(bound.model.ResolveToken(bound.Token("n", 1)),
              bound.Find("n", heimdall::SymbolKind::Namespace));
}

TEST(Binder, MemberAccessThroughObjectsStaysUnresolved)
{
    Bound bound("struct S { int m; };\n"
        "int m;\n"
        "void f(S obj) {\n"
        "    obj.m = 1;\n"
        "}\n");
    // `obj.m` needs the type of `obj`: it must not fall back to the global `m`.
    EXPECT_EQ(bound.model.ResolveToken(bound.Token("m", 2)), heimdall::kNone);
    EXPECT_EQ(bound.model.ResolveToken(bound.Token("obj", 1)),
              bound.Find("obj", heimdall::SymbolKind::Parameter));
}

TEST(Binder, UnknownNamesStayUnresolved)
{
    Bound bound("void f() { missing = 1; }\n");
    EXPECT_EQ(bound.model.ResolveToken(bound.Token("missing")), heimdall::kNone);
}

TEST(Binder, EnumeratorsLiveInTheEnclosingScopeUnlessScoped)
{
    Bound bound("enum Plain { A, B };\n"
        "enum class Strong { C, D };\n"
        "int f() { return A + C; }\n");
    const auto& names = bound.model.Names();
    EXPECT_NE(
        bound.model.LookupLocal(heimdall::SemanticModel::TranslationUnitScope, names.Find("A")),
        heimdall::kNone);
    EXPECT_EQ(
        bound.model.LookupLocal(heimdall::SemanticModel::TranslationUnitScope, names.Find("C")),
        heimdall::kNone);
    EXPECT_EQ(bound.model.ResolveToken(bound.Token("A", 1)),
              bound.Find("A", heimdall::SymbolKind::Enumerator));
    EXPECT_EQ(bound.model.ResolveToken(bound.Token("C", 1)), heimdall::kNone);
}

TEST(Binder, TypeAliasesAreSymbols)
{
    Bound bound("typedef int Old;\nusing New = long;\nusing namespace std;\n");
    EXPECT_NE(bound.Find("Old", heimdall::SymbolKind::TypeAlias), heimdall::kNone);
    EXPECT_NE(bound.Find("New", heimdall::SymbolKind::TypeAlias), heimdall::kNone);
}

TEST(Binder, ToleratesBrokenInput)
{
    for (const char* source :
        {
            "", "struct", "struct S : {", "namespace {", "void f(", "class A { virtual void",
            "void S::", "enum class", "} } }", "int f() { { { {"
    })
    {
        Bound bound(source);
        EXPECT_GE(bound.model.Scopes().Size(), 1u) << source;
    }
}

TEST(Binder, DeclarationsOfParametersAreNotBound)
{
    Bound bound("struct S { void f(int ignored); };\n");
    EXPECT_EQ(bound.Find("ignored", heimdall::SymbolKind::Parameter), heimdall::kNone);
}
