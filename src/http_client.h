#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace dmbg {

// One HTTP GET, executed on the calling thread.
//
// WinHTTP handles TLS (1.2/1.3 enforced), redirects and chunked responses.
// Every call uses its own session but keeps a per-thread connection handle, so
// a pool of worker threads reuses connections without sharing them.
struct HttpResponse {
    std::string content_type; // lowercased, parameters stripped
    long long content_length = -1;
    long long bytes_received = 0;
};

// Streams the response body through `on_data` (may be called many times).
// `on_data` returns false to abort the transfer, which is reported as failure.
// Returns true only for HTTP 2xx with a fully consumed body.
bool http_get(const std::wstring& url,
              const std::function<bool(const char*, std::size_t)>& on_data,
              HttpResponse& response,
              std::string& error);

// Frees the calling thread's cached connection (except the session).
// A worker thread should call this before it exits.
void http_release_thread_resources();

} // namespace dmbg
