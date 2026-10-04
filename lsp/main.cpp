#include "Server.hpp"

#include <iostream>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

int main()
{
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    heimdall::lsp::LanguageServer server;
    return server.Run() ? 0 : 1;
}
