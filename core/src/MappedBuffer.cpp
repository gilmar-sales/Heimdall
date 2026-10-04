#include <Heimdall/MappedBuffer.hpp>

#include <cstdio>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace heimdall
{

    namespace
    {

        std::expected<std::string, std::string> ReadAll(const char *path)
        {
            std::string out;
            FILE *file = std::fopen(path, "rb");
            if (file == nullptr)
            {
                return std::unexpected(std::string("cannot open file: ") + path);
            }

            char chunk[64 * 1024];
            std::size_t n = 0;
            while ((n = std::fread(chunk, 1, sizeof(chunk), file)) > 0)
            {
                out.append(chunk, n);
            }

            const int err = std::ferror(file);
            std::fclose(file);
            if (err != 0)
            {
                return std::unexpected(std::string("error reading file: ") + path);
            }

            return out;
        }

#if defined(_WIN32)

        std::expected<std::wstring, std::string> ToWide(const char *path)
        {
            const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0);
            if (needed <= 0)
            {
                return std::unexpected(std::string("invalid UTF-8 path: ") + path);
            }

            std::wstring wide(static_cast<std::size_t>(needed) - 1, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, path, -1, wide.data(), needed);
            return wide;
        }

#endif

    } // namespace

    void MappedBuffer::Release()
    {
#if defined(_WIN32)
        if (m_data != nullptr && m_data != m_owned.data() && m_size > 0)
        {
            UnmapViewOfFile(m_data);
        }

        if (m_map_handle != nullptr)
        {
            CloseHandle(m_map_handle);
        }

        if (m_file_handle != nullptr && m_file_handle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(m_file_handle);
        }
#else
        if (m_data != nullptr && m_data != m_owned.data() && m_size > 0)
        {
            munmap(const_cast<char * >(m_data), m_size);
        }

        if (m_file_handle != nullptr)
        {
            close(static_cast<int>(reinterpret_cast<std::intptr_t>(m_file_handle)));
        }

        (void) m_map_handle;
#endif
        m_data = "";
        m_size = 0;
        m_file_handle = nullptr;
        m_map_handle = nullptr;
    }

    MappedBuffer::MappedBuffer(MappedBuffer &&other) noexcept
    {
        *this = std::move(other);
    }

    MappedBuffer & MappedBuffer::operator= (MappedBuffer &&other) noexcept
    {
        if (this != &other)
        {
            Release();
            m_data = std::exchange(other.m_data, "");
            m_size = std::exchange(other.m_size, 0);
            m_file_handle = std::exchange(other.m_file_handle, nullptr);
            m_map_handle = std::exchange(other.m_map_handle, nullptr);
            m_owned = std::move(other.m_owned);
            // Buffered-fallback data aliases owned storage; re-alias to ours
            // after the move (also fixes SSO, where the bytes were copied).
            if (!m_owned.empty())
            {
                m_data = m_owned.data();
            }
        }

        return *this;
    }

    MappedBuffer::~MappedBuffer()
    {
        Release();
    }

    std::expected<MappedBuffer, std::string> MappedBuffer::Open(const std::string & path)
    {
        return Open(path.c_str());
    }

    std::expected<MappedBuffer, std::string> MappedBuffer::Open(const char *path)
    {
        if (path == nullptr)
        {
            return std::unexpected(std::string("null path"));
        }

#if defined(_WIN32)
        auto wide = ToWide(path);
        if (!wide)
        {
            return std::unexpected(wide.error());
        }

        HANDLE file =
            CreateFileW(wide->c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return std::unexpected(std::string("cannot open file: ") + path);
        }

        LARGE_INTEGER size{};
        if (GetFileSizeEx(file, &size) == 0)
        {
            CloseHandle(file);
            return std::unexpected(std::string("cannot stat file: ") + path);
        }

        if (size.QuadPart == 0)
        {
            CloseHandle(file);
            return MappedBuffer{};
        }

        HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping == nullptr)
        {
            CloseHandle(file);
            return OpenBuffered(path);
        }

        const char *data = static_cast<const char * >(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
        if (data == nullptr)
        {
            CloseHandle(mapping);
            CloseHandle(file);
            return OpenBuffered(path);
        }

        MappedBuffer out;
        out.m_data = data;
        out.m_size = static_cast<std::size_t>(size.QuadPart);
        out.m_file_handle = file;
        out.m_map_handle = mapping;
        return out;
#else
        const int fd = open(path, O_RDONLY);
        if (fd < 0)
        {
            return std::unexpected(std::string("cannot open file: ") + path);
        }

        struct stat st
        {};
        if (fstat(fd, &st) != 0)
        {
            close(fd);
            return std::unexpected(std::string("cannot stat file: ") + path);
        }

        if (st.st_size == 0)
        {
            close(fd);
            return MappedBuffer{};
        }

        void *data = mmap(nullptr, static_cast<std::size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
        if (data == MAP_FAILED)
        {
            close(fd);
            return OpenBuffered(path);
        }

        MappedBuffer out;
        out.m_data = static_cast<const char * >(data);
        out.m_size = static_cast<std::size_t>(st.st_size);
        out.m_file_handle = reinterpret_cast<void * >(static_cast<std::intptr_t>(fd));
        return out;
#endif
    }

    std::expected<MappedBuffer, std::string> MappedBuffer::OpenBuffered(const char *path)
    {
        auto contents = ReadAll(path);
        if (!contents)
        {
            return std::unexpected(contents.error());
        }

        MappedBuffer out;
        out.m_owned = std::move(*contents);
        out.m_data = out.m_owned.data();
        out.m_size = out.m_owned.size();
        return out;
    }

} // namespace heimdall
