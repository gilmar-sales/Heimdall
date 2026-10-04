#pragma once

#include <Heimdall/ParseTree.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace heimdall
{

    enum class PointerAlignment
    {
        // `int* x` (`*`/`&` bind to the type).
        Left,
        // `int *x` (`*`/`&` bind to the declarator name).
        Right,
    };

    enum class BraceStyle
    {
        // `if (x) {` — opening brace stays on the code line.
        Attach,
        // `if (x)\n{` — opening brace on its own line.
        Allman,
    };

    // How single-statement control blocks (`if`/`else`/`for`/`while`/`do`
    // with one statement and no braces) are treated.
    enum class SingleLineStyle
    {
        // Leave them as written (default).
        Keep,
        // Collapse onto one line: `if (x) {\n    return;\n}` and
        // `if (x)\n    return;` both become `if (x) return;`.
        SingleLine,
        // Body on its own indented line, no braces added:
        // `if (x) return;` becomes `if (x)\n    return;`.
        Indent,
        // Braces plus indented body: `if (x) return;` becomes
        // `if (x) {\n    return;\n}`.
        IndentWithBraces,
    };

    struct FormatOptions
    {
        std::size_t indent_width = 4;
        bool use_tabs = false;
        std::size_t max_empty_lines = 1;
        // Maximum line width before comma-driven breaks; 0 disables breaking.
        std::size_t column_limit = 100;
        PointerAlignment pointer_alignment = PointerAlignment::Right;
        BraceStyle brace_style = BraceStyle::Allman;
        // Single-statement control blocks: Keep (default), SingleLine, Indent or
        // IndentWithBraces. Only `if`/`else`/`for`/`while`/`do` bodies are touched;
        // function, record, namespace, lambda and switch/case blocks never are.
        SingleLineStyle single_line_style = SingleLineStyle::IndentWithBraces;
        // Sort contiguous `#include` runs by header name; other directives stay
        // verbatim. Off by default: sorting reorders source lines.
        bool sort_includes = false;
        // Insert an empty line after the closing `}` of an `if`/`else`/`for`/
        // `while`/`do`/`switch`/`try`/`catch` body when more code follows in the
        // same scope (never before `}`, `else`, `catch` or a label).
        bool blank_line_after_control_block = true;
        // Insert an empty line after a `class`/`struct`/`union`/`enum` definition
        // (`};`) when more code follows in the same scope (never before `}` or a
        // label such as `public:`).
        bool blank_line_after_type_definition = true;
        // Space before the colon of a base clause or enum underlying type:
        // `enum class E : std::uint8_t`, `class A : public B` (false: `E: T`).
        bool space_before_inheritance_colon = true;
        // Pad trailing `//` comments of adjacent lines to one column.
        bool align_trailing_comments = true;
    };

    // One replacement hunk over the source buffer, line-oriented: lines
    // [start_line, end_line) are replaced by `replacement` (which carries its own
    // newlines). Offsets are provided so hosts can map to positions without
    // re-scanning; line numbers are 0-based.
    struct FormatEdit
    {
        std::size_t start_line = 0;
        std::size_t end_line = 0;
        std::size_t start_offset = 0;
        std::size_t end_offset = 0;
        std::string replacement;
    };

    class Formatter
    {
    public:
        explicit Formatter(FormatOptions options = {}) : m_options(options) {}

        std::string Format(std::string_view source) const;
        std::string Format(const ParseTree &tree) const;
        // Minimal line-diff between `source` and `Format(source)`: empty when the
        // file is already formatted. Edits are disjoint and ascending.
        std::vector<FormatEdit> FormatEdits(std::string_view source) const;
        // Format the whole buffer but only apply edits overlapping
        // [start_line, end_line); lines outside are byte-identical.
        std::string FormatRange(std::string_view source, std::size_t start_line,
            std::size_t end_line) const;

    private:
        std::string FormatImpl(std::string_view source, const std::vector<Token> & tokens,
            const std::vector<PreprocessorDirective> & directives) const;
        std::string FormatShaped(std::string_view source, const std::vector<Token> & tokens,
            const std::vector<PreprocessorDirective> & directives) const;
        FormatOptions m_options;
    };

} // namespace heimdall
