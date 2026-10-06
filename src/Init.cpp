#include "Init.hpp"

#include <Heimdall/RuleConfig.hpp>

#include <fstream>
#include <system_error>

namespace heimdall::cli
{

    std::string DefaultConfigText()
    {
        return R"({
  "root": true,
  "rules": {
    "cpp/no-null": "warning",
    "format/no-trailing-whitespace": "warning",
    "format/require-final-newline": "warning",
    "cpp/no-empty-catch": "warning",
    "cpp/no-duplicate-include": "warning",
    "cpp/modernize-using": "warning",
    "cpp/no-todo": "warning",
    "cpp/no-magic-numbers": "warning"
  },
  "suppressions": true,
  "include-order": {
    "groups": ["angle", "quote"],
    "case-insensitive": true
  }
}
)";
    }

    bool RunInit(const Options& options, std::string& error, std::filesystem::path& created_path)
    {
        std::filesystem::path directory;
        if (!options.inputs.empty())
        {
            directory = options.inputs.front();
        }
        else
        {
            std::error_code ec;
            directory = std::filesystem::current_path(ec);
            if (ec)
            {
                error = "cannot resolve current directory: " + ec.message();
                return false;
            }
        }

        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec)
        {
            error = "cannot create directory '" + directory.string() + "': " + ec.message();
            return false;
        }

        created_path = directory / std::string(heimdall::RuleConfigFileName);
        if (std::filesystem::exists(created_path, ec) && !ec && !options.force)
        {
            error = "config already exists: " + created_path.string() + " (use --force to overwrite)";
            return false;
        }

        std::ofstream out(created_path, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            error = "cannot write config file: " + created_path.string();
            return false;
        }

        const std::string text = DefaultConfigText();
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out)
        {
            error = "cannot write config file: " + created_path.string();
            return false;
        }

        return true;
    }

} // namespace heimdall::cli
