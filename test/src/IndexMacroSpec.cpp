#include <Heimdall/IndexSnapshot.hpp>
#include <Heimdall/MacroEnvironment.hpp>
#include <Heimdall/MacroResolver.hpp>
#include <Heimdall/MacroTable.hpp>
#include <Heimdall/ShardBuilder.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    namespace idx = heimdall::index;

    // Macros the "headers" define, every one live everywhere.
    class FixedResolver final : public heimdall::MacroResolver
    {
    public:
        struct Entry
        {
            std::string_view definition;
            std::string_view file;
        };

        explicit FixedResolver(std::initializer_list<Entry> entries)
        {
            for (const auto& entry : entries)
            {
                auto parsed = heimdall::MacroTable::ParseDefinition(entry.definition);
                EXPECT_TRUE(parsed.has_value()) << entry.definition;
                if (parsed)
                {
                    parsed->file = std::string(entry.file);
                    mTable.Define(std::move(*parsed));
                }
            }
        }

        const heimdall::MacroDef* Find(std::string_view name, std::size_t) const noexcept override
        {
            return mTable.Find(name);
        }

        std::uint64_t Fingerprint() const noexcept override
        {
            return mTable.Size();
        }

    private:
        heimdall::MacroTable mTable;
    };

    std::shared_ptr<const idx::FileShard> Shard(
        std::string path, std::string source, std::shared_ptr<const heimdall::MacroResolver> resolver = {})
    {
        const auto text = std::make_shared<const std::string>(std::move(source));
        heimdall::ParserOptions options;
        options.macro_resolver = std::move(resolver);
        const auto tree = heimdall::ParseTree::Parse(*text, options);
        const auto model = heimdall::Binder::Bind(tree);
        return idx::ShardBuilder::Build(model, std::move(path));
    }

    std::uint32_t MacroSymbol(const idx::FileShard& shard, std::string_view name)
    {
        for (std::uint32_t i = 0; i < shard.Symbols().Size(); ++i)
        {
            if (shard.Name(i) == name && shard.Symbols().kind[i] == heimdall::SymbolKind::Macro)
            {
                return i;
            }
        }

        return idx::kInvalid;
    }

    std::size_t OccurrencesAt(const idx::FileShard& shard, std::uint32_t offset)
    {
        std::size_t count = 0;
        for (const auto at : shard.Occurrences().offset)
        {
            count += at == offset;
        }

        return count;
    }
} // namespace

TEST(IndexMacros, DefinesBecomeMacroSymbolsAndSameFileUsesBindToThem)
{
    const std::string source = "#define LIMIT 8\n#define SQUARE(x) ((x) * (x))\nint a = LIMIT;\nint b = SQUARE(2);\n";

    const auto shard = Shard("a.cpp", source);

    const auto limit = MacroSymbol(*shard, "LIMIT");
    const auto square = MacroSymbol(*shard, "SQUARE");
    ASSERT_NE(limit, idx::kInvalid);
    ASSERT_NE(square, idx::kInvalid);
    EXPECT_EQ(shard->Symbols().role[limit], idx::Role::Definition);
    EXPECT_EQ(shard->Symbols().paramMin[square], 1);
    EXPECT_EQ(shard->Symbols().paramMax[square], 1);
    EXPECT_EQ(shard->OccurrencesOf(limit).size(), 2u);
    EXPECT_EQ(shard->OccurrencesOf(square).size(), 2u);
    EXPECT_EQ(shard->SymbolAt(static_cast<std::uint32_t>(source.rfind("LIMIT"))), limit);
}

TEST(IndexMacros, UsesInOtherFilesResolveThroughTheIncludeClosure)
{
    const auto resolver = std::make_shared<FixedResolver>(std::initializer_list<FixedResolver::Entry> {
            {
                "LIMIT 8", "config.h"
            }, {"SQUARE(x) ((x) * (x))", "config.h"}});
    const auto header = Shard("config.h", "#define LIMIT 8\n#define SQUARE(x) ((x) * (x))\n");
    const auto source = Shard("main.cpp",
        "#include \"config.h\"\nint a = LIMIT;\nint b = SQUARE(LIMIT);\n", resolver);

    const auto snapshot = idx::IndexSnapshot::Build({header, source});

    const auto hash = header->Symbols().hash[MacroSymbol(*header, "LIMIT")];
    const auto references = snapshot->References(hash);
    std::size_t inMain = 0;
    for (const auto& location : references)
    {
        inMain += snapshot->Shards()[location.shard]->Path() == "main.cpp";
    }

    EXPECT_EQ(inMain, 2u);
    EXPECT_EQ(references.size(), 3u);
    EXPECT_EQ(snapshot->Definitions(hash).size(), 1u);
}

TEST(IndexMacros, MentionsInConditionsAndOtherMacroBodiesAreReferences)
{
    const auto resolver = std::make_shared<FixedResolver>(std::initializer_list<FixedResolver::Entry> {
            {
                "FEATURE 1", "config.h"
    }});
    const std::string source = "#include \"config.h\"\n#ifdef FEATURE\nint a;\n#endif\n"
        "#if defined(FEATURE) && FEATURE\nint b;\n#endif\n#define ON FEATURE\n";

    const auto shard = Shard("main.cpp", source, resolver);

    std::size_t count = 0;
    for (std::uint32_t i = 0; i < shard->Occurrences().Size(); ++i)
    {
        count += shard->Strings().Text(shard->Occurrences().name[i]) == "FEATURE";
    }

    EXPECT_EQ(count, 4u);
}

TEST(IndexMacros, TheMacroOwnsTheTokenWhateverTheBinderMadeOfIt)
{
    const auto resolver = std::make_shared<FixedResolver>(std::initializer_list<FixedResolver::Entry> {
            {
                "value 42", "config.h"
    }});
    const std::string source = "#include \"config.h\"\nint f() { return value; }\n";

    const auto shard = Shard("main.cpp", source, resolver);

    EXPECT_EQ(OccurrencesAt(*shard, static_cast<std::uint32_t>(source.rfind("value"))), 1u);
}

TEST(IndexMacros, ANameThatIsNotAMacroHereNeverResolvesToOne)
{
    const auto header = Shard("config.h", "#define LIMIT 8\n");
    const auto source = Shard("main.cpp",
        "#include \"config.h\"\nint LIMIT_other;\nint f(int LIMIT) { return LIMIT; }\n");

    const auto snapshot = idx::IndexSnapshot::Build({header, source});

    const auto hash = header->Symbols().hash[MacroSymbol(*header, "LIMIT")];
    EXPECT_EQ(snapshot->References(hash).size(), 1u);
}

TEST(IndexMacros, UsesInInactiveCodeAreNotRecorded)
{
    const auto resolver = std::make_shared<FixedResolver>(std::initializer_list<FixedResolver::Entry> {
            {
                "LIMIT 8", "config.h"
    }});
    const std::string source = "#include \"config.h\"\n#if 0\nint a = LIMIT;\n#endif\nint b = LIMIT;\n";

    const auto shard = Shard("main.cpp", source, resolver);

    std::size_t count = 0;
    for (std::uint32_t i = 0; i < shard->Occurrences().Size(); ++i)
    {
        count += shard->Strings().Text(shard->Occurrences().name[i]) == "LIMIT";
    }

    EXPECT_EQ(count, 1u);
}

TEST(IndexMacros, RedefinitionsShareOneIdentity)
{
    const auto shard = Shard("a.cpp", "#define MODE 1\n#undef MODE\n#define MODE 2\n");

    std::size_t count = 0;
    idx::SymbolHash first = 0;
    for (std::uint32_t i = 0; i < shard->Symbols().Size(); ++i)
    {
        if (shard->Name(i) == "MODE")
        {
            first = count++== 0 ? shard->Symbols().hash[i] : first;
            EXPECT_EQ(shard->Symbols().hash[i], first);
        }
    }

    EXPECT_EQ(count, 2u);
}

TEST(IndexMacros, UsesOfAHeaderMacroParsedThroughTheMacroEnvironmentAreIndexed)
{
    const auto root = std::filesystem::temp_directory_path() / "heimdall-index-macro-env";
    std::filesystem::create_directories(root);
    {
        std::ofstream(root / "config.h") << "#pragma once\n#define LIMIT 8\n";
    }

    const std::string source = "#include \"config.h\"\nint a = LIMIT;\n";
    const auto main = root / "main.cpp";
    {
        std::ofstream(main) << source;
    }

    heimdall::CompileCommand command;
    command.directory = root;
    heimdall::MacroEnvironmentOptions environmentOptions;
    environmentOptions.probeCompiler = false;
    auto environment = std::make_shared<const heimdall::MacroEnvironment>(
        heimdall::MacroEnvironment::Build(main, source, &command, environmentOptions));
    heimdall::ParserOptions options;
    options.macro_resolver = std::make_shared<heimdall::MacroEnvironmentResolver>(environment);
    const auto text = std::make_shared<const std::string>(source);

    const auto tree = heimdall::ParseTree::Parse(*text, options);
    const auto model = heimdall::Binder::Bind(tree);
    const auto shard = idx::ShardBuilder::Build(model, main.generic_string());

    std::size_t count = 0;
    for (std::uint32_t i = 0; i < shard->Occurrences().Size(); ++i)
    {
        count += shard->Strings().Text(shard->Occurrences().name[i]) == "LIMIT" &&
            (shard->Occurrences().flags[i] & idx::kOccurrenceMacro) != 0;
    }

    EXPECT_EQ(count, 1u);
    std::filesystem::remove_all(root);
}

TEST(IndexMacros, ConditionsOnHeaderMacrosAreEvaluatedWithTheTranslationUnitsMacros)
{
    const auto root = std::filesystem::temp_directory_path() / "heimdall-index-macro-conditions";
    std::filesystem::create_directories(root);
    {
        std::ofstream(root / "config.h") << "#pragma once\n#define FEATURE 1\n#define LEVEL(x) (x + 1)\n";
    }

    const std::string source = "#include \"config.h\"\n#ifdef FEATURE\nint enabled();\n#endif\n"
        "#ifndef FEATURE\nint disabled();\n#endif\n"
        "#if LEVEL(1) == 2\nint computed();\n#endif\n";
    const auto main = root / "main.cpp";
    {
        std::ofstream(main) << source;
    }

    heimdall::CompileCommand command;
    command.directory = root;
    heimdall::MacroEnvironmentOptions environmentOptions;
    environmentOptions.probeCompiler = false;
    auto environment = std::make_shared<const heimdall::MacroEnvironment>(
        heimdall::MacroEnvironment::Build(main, source, &command, environmentOptions));
    heimdall::ParserOptions withEnvironment;
    withEnvironment.macro_resolver = std::make_shared<heimdall::MacroEnvironmentResolver>(environment);

    const auto names =[&](const heimdall::ParserOptions& options)
    {
        const auto tree = heimdall::ParseTree::Parse(source, options);
        const auto model = heimdall::Binder::Bind(tree);
        const auto shard = idx::ShardBuilder::Build(model, main.generic_string());
        std::vector<std::string> found;
        for (std::uint32_t i = 0; i < shard->Symbols().Size(); ++i)
        {
            if (shard->Symbols().kind[i] == heimdall::SymbolKind::Function)
            {
                found.emplace_back(shard->Name(i));
            }
        }

        return found;
    };

    EXPECT_EQ(names(withEnvironment), (std::vector<std::string> {"enabled", "computed"}));
    EXPECT_EQ(names({}), (std::vector<std::string> {"disabled"}));
    std::filesystem::remove_all(root);
}
