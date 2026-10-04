#pragma once

#include <simdjson.h>

#include <string>
#include <string_view>

namespace heimdall::lsp
{

    void QuoteJson(std::string_view value, std::string & out);
    void Send(std::string_view body);
    bool ReadMessage(std::string & body);
    bool GetString(simdjson::dom::object object, const char *key, std::string_view & output);
    bool GetObject(simdjson::dom::element element, const char *key, simdjson::dom::object & output);

} // namespace heimdall::lsp
