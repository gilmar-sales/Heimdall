#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    std::string Read(const fs::path& path)
    {
        std::ifstream input(path);
        std::ostringstream output;
        output << input.rdbuf();
        return output.str();
    }

    std::vector<std::string> Headers(const fs::path& directory)
    {
        std::vector<std::string> result;
        for (const auto& entry : fs::directory_iterator(directory))
        {
            if (entry.path().extension() == ".hpp")
            {
                result.push_back(entry.path().filename().string());
            }
        }

        return result;
    }

    void NoIncludes(const fs::path& directory, const std::vector<std::string>& forbidden)
    {
        for (const auto& entry : fs::recursive_directory_iterator(directory))
        {
            if (entry.path().extension() != ".cpp" && entry.path().extension() != ".hpp")
            {
                continue;
            }

            std::istringstream code(Read(entry.path()));
            std::string line;
            while (std::getline(code, line))
            {
                const auto first = line.find_first_not_of(" \t");
                if (first == std::string::npos || line.compare(first, 8, "#include") != 0)
                {
                    continue;
                }

                for (const auto& header : forbidden)
                {
                    EXPECT_EQ(line.find("Heimdall/" + header), std::string::npos) << entry.path() << ": " << line;
                }
            }
        }
    }

    TEST(ArchitecturePolicy, CoreAndSemanticDoNotDependOnAnalysis)
    {
        const fs::path root(HEIMDALL_SOURCE_DIR);
        const auto analysis = Headers(root / "analysis/include/Heimdall");
        NoIncludes(root / "core", analysis);
        NoIncludes(root / "semantic", analysis);
        NoIncludes(root / "core", Headers(root / "semantic/include/Heimdall"));
    }

    TEST(ArchitecturePolicy, PublicPluginApiDoesNotExposeInternalRepresentations)
    {
        const auto api = Read(fs::path(HEIMDALL_SOURCE_DIR) / "analysis/include/Heimdall/PluginApi.hpp");
        EXPECT_EQ(api.find("#include <Heimdall/ParseTree"), std::string::npos);
        EXPECT_EQ(api.find("#include <Heimdall/SemanticModel"), std::string::npos);
        EXPECT_EQ(api.find("#include <Heimdall/TypeModel"), std::string::npos);
        EXPECT_EQ(api.find("#include <Heimdall/AnalysisContext"), std::string::npos);
        EXPECT_EQ(api.find("simdjson.h"), std::string::npos);
    }
}
