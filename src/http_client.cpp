#include "http_client.h"

#include <windows.h>
#include <winhttp.h>

#include <cctype>
#include <cstdio>

namespace dmbg {
namespace {

// Python used a single 30 s timeout for the whole request. WinHTTP splits that
// into connect/send/receive phases; receive gets a larger budget so a 15 MB
// background video over a slow link is not killed mid-transfer.
constexpr int kResolveTimeoutMs = 30'000;
constexpr int kConnectTimeoutMs = 30'000;
constexpr int kSendTimeoutMs = 30'000;
constexpr int kReceiveTimeoutMs = 300'000;

constexpr const wchar_t* kUserAgent =
    L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36";

// Session + connection live together: a WinHTTP connection handle is only valid
// while its session is open, so they must be created and closed as a pair.
struct ThreadContext {
    HINTERNET session = nullptr;
    HINTERNET connection = nullptr;
    std::wstring host;
    INTERNET_PORT port = 0;
};

// WinHTTP handles are not safe to share across threads, so each worker keeps its
// own context. Worker threads are long-lived, which still buys keep-alive.
thread_local ThreadContext g_context;

// A failure raised from inside the streaming loop. Thrown rather than returned
// because the loop nests two deep; every path is caught in http_get().
struct TransferFailure {
    DWORD code = 0;
    std::string message;
};

std::string win32_message(DWORD code) {
    LPWSTR buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);

    std::string message;
    if (length != 0 && buffer != nullptr) {
        std::wstring wide(buffer, length);
        while (!wide.empty() && (wide.back() == L'\r' || wide.back() == L'\n' || wide.back() == L' ')) {
            wide.pop_back();
        }
        const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                               nullptr, 0, nullptr, nullptr);
        if (needed > 0) {
            message.resize(static_cast<std::size_t>(needed));
            WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                message.data(), needed, nullptr, nullptr);
        }
    }
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    return message;
}

std::string winhttp_error(const char* stage, DWORD code) {
    std::string text = stage;
    text += " 失败 (WinHTTP ";
    text += std::to_string(static_cast<unsigned long>(code));
    const std::string detail = win32_message(code);
    if (!detail.empty()) {
        text += ": ";
        text += detail;
    }
    text += ")";
    return text;
}

void close_context() {
    if (g_context.connection != nullptr) {
        WinHttpCloseHandle(g_context.connection);
        g_context.connection = nullptr;
    }
    if (g_context.session != nullptr) {
        WinHttpCloseHandle(g_context.session);
        g_context.session = nullptr;
    }
    g_context.host.clear();
    g_context.port = 0;
}

HINTERNET thread_session() {
    if (g_context.session != nullptr) {
        return g_context.session;
    }

    HINTERNET session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) {
        return nullptr;
    }

    // WinHTTP defaults to TLS 1.0 only; without this the HTTPS handshake fails.
    DWORD secure_protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    secure_protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &secure_protocols, sizeof(secure_protocols));
    WinHttpSetTimeouts(session, kResolveTimeoutMs, kConnectTimeoutMs, kSendTimeoutMs, kReceiveTimeoutMs);

    g_context.session = session;
    return session;
}

HINTERNET thread_connection(HINTERNET session, const std::wstring& host, INTERNET_PORT port) {
    if (g_context.connection != nullptr && g_context.host == host && g_context.port == port) {
        return g_context.connection;
    }
    if (g_context.connection != nullptr) {
        WinHttpCloseHandle(g_context.connection);
        g_context.connection = nullptr;
    }

    g_context.connection = WinHttpConnect(session, host.c_str(), port, 0);
    if (g_context.connection == nullptr) {
        return nullptr;
    }
    g_context.host = host;
    g_context.port = port;
    return g_context.connection;
}

std::string to_utf8_lower(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string text(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                        text.data(), needed, nullptr, nullptr);
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

std::string read_content_type(HINTERNET request) {
    DWORD size = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX,
                        WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);
    if (size <= sizeof(wchar_t) || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return {};
    }

    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX,
                             value.data(), &size, WINHTTP_NO_HEADER_INDEX)) {
        return {};
    }
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    const std::size_t semicolon = value.find(L';'); // drop "; charset=..."
    if (semicolon != std::wstring::npos) {
        value.resize(semicolon);
    }
    while (!value.empty() && value.back() == L' ') {
        value.pop_back();
    }
    return to_utf8_lower(value);
}

void transfer_body(HINTERNET request,
                   const std::function<bool(const char*, std::size_t)>& on_data,
                   HttpResponse& response) {
    char buffer[64 * 1024];
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            throw TransferFailure{GetLastError(), {}};
        }
        if (available == 0) {
            return; // clean end of body
        }

        DWORD offset = 0;
        while (offset < available) {
            const DWORD remaining = available - offset;
            const DWORD want = remaining < sizeof(buffer) ? remaining : static_cast<DWORD>(sizeof(buffer));
            DWORD read = 0;
            if (!WinHttpReadData(request, buffer + offset, want, &read) || read == 0) {
                throw TransferFailure{GetLastError(), {}};
            }
            offset += read;
            response.bytes_received += static_cast<long long>(read);
            if (on_data && !on_data(buffer, read)) {
                throw TransferFailure{ERROR_WRITE_FAULT, "写入失败或被中止"};
            }
        }
    }
}

} // namespace

bool http_get(const std::wstring& url,
              const std::function<bool(const char*, std::size_t)>& on_data,
              HttpResponse& response,
              std::string& error) {
    response = HttpResponse{};
    error.clear();

    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &components)) {
        error = winhttp_error("解析 URL", GetLastError());
        return false;
    }

    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring target(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength > 0) {
        target.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (target.empty()) {
        target = L"/";
    }

    HINTERNET request = nullptr;
    bool ok = false;

    try {
        HINTERNET session = thread_session();
        if (session == nullptr) {
            error = winhttp_error("创建会话", GetLastError());
            throw TransferFailure{};
        }

        HINTERNET connection = thread_connection(session, host, components.nPort);
        if (connection == nullptr) {
            // A dead keep-alive connection reports a connect failure; drop it so
            // the next attempt starts from a fresh socket.
            const DWORD code = GetLastError();
            close_context();
            error = winhttp_error("连接服务器", code);
            throw TransferFailure{};
        }

        request = WinHttpOpenRequest(connection, L"GET", target.c_str(), nullptr,
                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        if (request == nullptr) {
            const DWORD code = GetLastError();
            close_context();
            error = winhttp_error("创建请求", code);
            throw TransferFailure{};
        }

        // Asking for identity encoding keeps the body byte-exact: the files we
        // want (webp/webm) are already compressed, so nothing is lost.
        if (!WinHttpAddRequestHeaders(request, L"Accept-Encoding: identity",
                                      static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD)) {
            error = winhttp_error("设置请求头", GetLastError());
            throw TransferFailure{};
        }

        if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            error = winhttp_error("发送请求", GetLastError());
            throw TransferFailure{};
        }

        if (!WinHttpReceiveResponse(request, nullptr)) {
            const DWORD code = GetLastError();
            close_context();
            error = winhttp_error("接收响应", code);
            throw TransferFailure{};
        }

        DWORD status_code = 0;
        DWORD status_size = sizeof(status_code);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_size,
                            WINHTTP_NO_HEADER_INDEX);
        if (status_code < 200 || status_code >= 300) {
            error = "HTTP " + std::to_string(static_cast<unsigned long>(status_code));
            throw TransferFailure{};
        }

        DWORD content_length = 0;
        DWORD length_size = sizeof(content_length);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &content_length, &length_size,
                                WINHTTP_NO_HEADER_INDEX)) {
            response.content_length = static_cast<long long>(content_length);
        }
        response.content_type = read_content_type(request);

        transfer_body(request, on_data, response);
        ok = true;
    } catch (const TransferFailure& failure) {
        if (error.empty()) {
            error = failure.message.empty() ? winhttp_error("传输数据", failure.code) : failure.message;
        }
    }

    if (request != nullptr) {
        WinHttpCloseHandle(request);
    }
    return ok;
}

void http_release_thread_resources() {
    close_context();
}

} // namespace dmbg
