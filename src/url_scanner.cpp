#include "url_scanner.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace dmbg {
namespace {

// Python's bytes regex class `[!-~]` is printable ASCII 0x21..0x7E. We drop the
// backslash on top of that: it is the one printable byte that shows up glued to
// a URL ending inside minified JavaScript ("...a.webp\"}]), ") and it would
// otherwise corrupt the extracted URL. Observed real cache data closes URLs with
// `"`, `\`, `}` or end-of-record bytes, so this only ever trims junk.
bool is_url_char(std::uint8_t c) {
    return c >= 0x21 && c <= 0x7E && c != '\\';
}

bool starts_with_http_scheme(const std::vector<std::uint8_t>& data, std::size_t at) {
    static const char kHttp[] = "http://";
    static const char kHttps[] = "https://";
    const std::size_t remaining = data.size() - at;

    // sizeof(literal) - 1 drops the NUL terminator. Writing the count by hand is
    // what broke this the first time: memcmp(..., "https://", 8) compares the
    // literal's NUL against the URL's ninth byte ('l') and can never match.
    if (remaining >= sizeof(kHttp) - 1 && std::memcmp(data.data() + at, kHttp, sizeof(kHttp) - 1) == 0) {
        return true;
    }
    if (remaining >= sizeof(kHttps) - 1 &&
        std::memcmp(data.data() + at, kHttps, sizeof(kHttps) - 1) == 0) {
        return true;
    }
    return false;
}

// Mirrors `os.path.basename(urlparse(url).path)`. urlparse splits off the query
// before we ever look at the path, so the caller passes an already-stripped path.
std::string basename_of_path(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

std::vector<std::string> scan_urls(const std::vector<std::uint8_t>& data) {
    std::vector<std::string> found;
    std::size_t i = 0;

    while (i < data.size()) {
        if (!starts_with_http_scheme(data, i)) {
            ++i;
            continue;
        }

        std::size_t end = i;
        while (end < data.size() && is_url_char(data[end])) {
            ++end;
        }

        // Python keeps only hits longer than 10 characters.
        if (end - i > 10) {
            found.emplace_back(reinterpret_cast<const char*>(data.data() + i), end - i);
        }
        i = end > i ? end : i + 1;
    }

    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    return found;
}

std::string url_extension(const std::string& url) {
    // urlparse(url).path: everything after the authority up to '?' or '#'.
    const std::size_t scheme_end = url.find("://");
    const std::size_t authority_start = scheme_end == std::string::npos ? 0 : scheme_end + 3;
    const std::size_t path_start = url.find('/', authority_start);
    if (path_start == std::string::npos) {
        return {};
    }

    std::size_t path_end = url.size();
    const std::size_t query = url.find_first_of("?#", path_start);
    if (query != std::string::npos) {
        path_end = query;
    }

    const std::string name = basename_of_path(url.substr(path_start, path_end - path_start));
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) {
        return {};
    }

    std::string ext = name.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

bool url_has_extension(const std::string& url, const std::string& ext) {
    return !ext.empty() && url_extension(url) == ext;
}

bool url_is_cache_stream_entry(const std::string& url) {
    const std::size_t scheme_end = url.find("://");
    const std::size_t authority_start = scheme_end == std::string::npos ? 0 : scheme_end + 3;
    const std::size_t path_start = url.find('/', authority_start);
    if (path_start == std::string::npos) {
        return false;
    }

    std::size_t path_end = url.size();
    const std::size_t query = url.find_first_of("?#", path_start);
    if (query != std::string::npos) {
        path_end = query;
    }

    const std::string name = basename_of_path(url.substr(path_start, path_end - path_start));

    // "resource.ext:<hex>[b|e|a]>:<segment>" is Chromium's bookkeeping for a
    // range request; the real resource is fetched from the URL without the suffix.
    const std::size_t first = name.find(':');
    if (first == std::string::npos) {
        return false;
    }
    const std::size_t second = name.find(':', first + 1);
    if (second == std::string::npos || second == first + 1) {
        return false;
    }

    // The middle segment must look like a hex stream id, optionally with the
    // single flat/sparse marker letter Chromium appends.
    std::string stream = name.substr(first + 1, second - first - 1);
    if (stream.size() > 1) {
        const char last = static_cast<char>(std::tolower(static_cast<unsigned char>(stream.back())));
        if (last == 'b' || last == 'e' || last == 'a') {
            stream.pop_back();
        }
    }
    if (stream.empty() || stream.size() > 32) {
        return false;
    }
    return std::all_of(stream.begin(), stream.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

} // namespace dmbg
