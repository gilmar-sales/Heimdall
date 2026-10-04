#include "JsonRpc.hpp"

#include <iostream>
#include <mutex>

#include <simdjson.h>

namespace heimdall::lsp
{

    void QuoteJson(std::string_view value, std::string& out)
    {
        constexpr char hex[] = "0123456789abcdef";
        out += '"';
        for (const unsigned char c: value)
        {
            switch (c)
            {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20)
                {
                    out += "\\u00";
                    out += hex[c >> 4];
                    out += hex[c & 0x0f];
                }
                else
                {
                    out += static_cast<char>(c);
                }
            }
        }

        out += '"';
    }

    void Send(std::string_view body)
    {
        static std::mutex send_mutex;
        const std::lock_guard<std::mutex> lock(send_mutex);

        std::cout << "Content-Length: " << body.size() << "\r\n\r\n";
        std::cout.write(body.data(), static_cast<std::streamsize>(body.size()));
        std::cout.flush();
    }

    bool ReadMessage(std::string& body)
    {
        std::string line;
        std::size_t length = 0;
        bool got_length = false;

        while (std::getline(std::cin, line))
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }

            if (line.empty())
            {
                break;
            }

            constexpr std::string_view prefix = "Content-Length:";
            if (line.starts_with(prefix))
            {
                const auto value = std::string_view(line).substr(prefix.size());
                try
                {
                    length = static_cast<std::size_t>(std::stoull(std::string(value)));
                    got_length = true;
                }
                catch (...)
                {
                    return false;
                }
            }
        }

        if (!std::cin ||!got_length)
        {
            return false;
        }

        body.resize(length);
        std::cin.read(body.data(), static_cast<std::streamsize>(length));

        return static_cast<std::size_t>(std::cin.gcount()) == length;
    }

    bool GetString(simdjson::dom::object object, const char* key, std::string_view& output)
    {
        return!object[key].get_string().get(output);
    }

    bool GetObject(simdjson::dom::element element, const char* key, simdjson::dom::object& output)
    {
        return!element[key].get_object().get(output);
    }

} // namespace heimdall::lsp
