#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dmbg {

// Extracts every `http://` / `https://` URL from an arbitrary byte buffer.
//
// This is the exact equivalent of the Python one-liner
// `re.findall(rb'https?://[!-~]+', data)`: a URL runs from the scheme up to the
// first byte that printable ASCII cannot represent (space, CR/LF, NUL, high
// bytes, and the backslash that minified JS uses to escape quotes).
//
// Results are deduplicated and sorted, matching `sorted(set(...))`. Only URLs
// longer than 10 bytes are kept, matching the Python length guard.
std::vector<std::string> scan_urls(const std::vector<std::uint8_t>& data);

// Best-effort file extension of a URL, lowercased, without the dot.
// The query string and fragment are discarded first, so
// ".../clip.webm?token=abc" yields "webm" and ".../clip.webm#t=5" yields "webm".
// Returns an empty string when there is no extension.
std::string url_extension(const std::string& url);

// Case-insensitive "does this URL end with this extension" test, using the same
// normalization as url_extension().
bool url_has_extension(const std::string& url, const std::string& ext);

// True for entries that only exist inside the launcher's cache bookkeeping and
// can never be fetched. Chromium records range requests for a cached resource as
// "name.webm:2fba45501d8d99:5"; scanning the raw cache surfaces those as if they
// were URLs, but the server answers 404 for all of them.
//
// The two colon-separated hex/decimal segments make the pattern specific enough
// not to catch a real file name, and a real URL cannot contain a colon after the
// authority anyway (it would start the port).
bool url_is_cache_stream_entry(const std::string& url);

} // namespace dmbg
