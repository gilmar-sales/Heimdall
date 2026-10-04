#pragma once

// Private bridge between ParseTree entry points (ParseTree.cpp) and the
// GrammarParser engine (GrammarParser.cpp). Not installed, not part of the public
// <Heimdall/...> API. Keeps the 1900-line grammar engine out of the header
// while letting ParseTree::Parse stay in its own TU for parallel builds.

#include <Heimdall/ParseTree.hpp>
#include <Heimdall/Preprocessor.hpp>

namespace heimdall::detail
{

    void ParseWithGrammar(ParseTree &tree, const PreprocessorResult &preprocessing);

} // namespace heimdall::detail
