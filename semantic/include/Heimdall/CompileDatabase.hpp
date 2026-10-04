#pragma once

#include <expected>
#include <Heimdall/CppStandard.hpp>
#include <Heimdall/Preprocessor.hpp>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace heimdall
{

struct CompileCommand
{
    std::filesystem::path directory;
    std::filesystem::path file;
    std::vector<std::string> arguments;
    Preprocessor::MacroMap defines;
    std::vector<std::string> undefines;
    std::vector<std::filesystem::path> include_directories;
    // Quoted-include-only directories (-iquote); searched for "..." after the
    // including file's own directory, before the general include directories.
    std::vector<std::filesystem::path> quote_directories;
    CppStandard standard = CppStandard::Cpp20;
};

class CompileDatabase
{
  public:
    static std::expected<CompileDatabase, std::string> Load(const std::filesystem::path& path);

    const std::vector<CompileCommand>& Commands() const noexcept { return m_commands; }
    const CompileCommand* Find(std::filesystem::path file) const;

  private:
    std::vector<CompileCommand> m_commands;
};

} // namespace heimdall
