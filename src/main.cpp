#include "http_client.h"
#include "url_scanner.h"
#include "version.h"

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

// ==================== 配置项 ====================
// Default extensions, matching the Python version. `--all-exts` widens this to
// every extension found in the cache.
const std::vector<std::string> kDefaultExtensions = {"webp", "webm"};

constexpr std::size_t kReadChunk = 1 << 20;
constexpr std::size_t kUrlLengthLimit = 10; // Python: len(decoded) > 10
constexpr std::size_t kFileNameLimit = 100; // Python: filename[:100]

// `%APPDATA%\miHoYo\HYP\<version>\fedata\Cache\Cache_Data\data_1`
constexpr const char* kDefaultInputTemplate = "%APPDATA%/miHoYo/HYP/1_1/fedata/Cache/Cache_Data/data_1";

constexpr const char* kColorReset = "\x1b[0m";
constexpr const char* kColorDim = "\x1b[2m";
constexpr const char* kColorRed = "\x1b[31m";
constexpr const char* kColorGreen = "\x1b[32m";
constexpr const char* kColorYellow = "\x1b[33m";
constexpr const char* kColorCyan = "\x1b[36m";

const char* kHelpTemplate = R"(dmbg - 米哈游启动器背景资源下载器 (C++ 版)

从米哈游启动器 (HYP) 的 Chromium 磁盘缓存中提取资源 URL，
再从网络下载完整的背景图片与背景视频。

用法:
  dmbg [选项]

选项:
  -o, --output-dir <目录>   输出根目录（未指定则弹出目录选择框）
  -f, --folder <名称>       输出子文件夹名（默认 downloads_mhy）
  -i, --input <文件>        指定启动器缓存文件
                            默认: @DEFAULT_INPUT@
  -t, --threads <数量>      并发下载线程数（默认 5，范围 1-64）
      --all-exts            下载缓存里所有扩展名，不再只限 webp/webm
      --ext <列表>          自定义扩展名，逗号分隔，如 webp,webm,png
      --force               已存在的文件也重新下载覆盖（默认跳过，避免堆出 _1 副本）
  -ot, --output-txt         把提取到的全部 URL 写入 all_urls.txt
  -h, --help                显示本帮助
  -v, --version             显示版本

示例:
  dmbg                                  # 弹框选目录，下载 webp/webm
  dmbg -o D:\bg -f hoyo -t 8            # 指定目录与线程数
  dmbg --ext png,jpg -ot -o D:\bg       # 只要图片，并导出 URL 清单

退出码: 0 成功 / 1 失败（无可用输入、无匹配资源等）/ 2 参数错误
)";

bool g_utf8_console = false;

void enable_utf8_console() {
    g_utf8_console = SetConsoleOutputCP(CP_UTF8) != 0;
    if (!g_utf8_console) {
        return;
    }
    // Nothing to reconfigure beyond the code page: writes are already routed
    // through WriteConsoleW below, so they cannot be lost to stream state.
}

void write_stdout(const std::string& utf8) {
    if (g_utf8_console) {
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                                               nullptr, 0);
        if (out != nullptr && out != INVALID_HANDLE_VALUE && needed > 0) {
            std::wstring wide(static_cast<std::size_t>(needed), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                                wide.data(), needed);
            DWORD written = 0;
            if (WriteConsoleW(out, wide.c_str(), static_cast<DWORD>(wide.size()), &written, nullptr) &&
                written == static_cast<DWORD>(wide.size())) {
                return;
            }
        }
    }
    std::fwrite(utf8.data(), 1, utf8.size(), stdout);
    std::fflush(stdout);
}

void log_line(const std::string& text, const char* color = nullptr) {
    std::string line;
    if (color != nullptr && g_utf8_console) {
        line = color;
        line += text;
        line += kColorReset;
    } else {
        line = text;
    }
    line += "\r\n";
    write_stdout(line);
}

// ==================== 小工具 ====================
std::string to_utf8(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                        utf8.data(), needed, nullptr, nullptr);
    return utf8;
}

std::string path_to_utf8(const fs::path& path) {
    return to_utf8(path.native());
}

std::wstring to_wide(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                                           nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), wide.data(), needed);
    return wide;
}

bool starts_with(const std::string& value, const char* prefix) {
    const std::size_t length = std::strlen(prefix);
    return value.size() >= length && value.compare(0, length, prefix) == 0;
}

// Expands %VAR% references like the Python `os.path.expandvars`.
fs::path expand_env(const std::string& text) {
    std::wstring expanded;
    expanded.reserve(text.size() + 64);

    const std::wstring wide = to_wide(text);
    for (std::size_t i = 0; i < wide.size();) {
        if (wide[i] != L'%') {
            expanded.push_back(wide[i]);
            ++i;
            continue;
        }
        const std::size_t close = wide.find(L'%', i + 1);
        if (close == std::wstring::npos || close == i + 1) {
            expanded.push_back(wide[i]);
            ++i;
            continue;
        }
        const std::wstring name = wide.substr(i + 1, close - i - 1);
        const DWORD needed = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
        if (needed == 0) {
            expanded.append(wide, i, close - i + 1); // leave unknown names untouched
        } else {
            std::wstring value(needed, L'\0');
            const DWORD written = GetEnvironmentVariableW(name.c_str(), value.data(), needed);
            value.resize(written);
            expanded += value;
        }
        i = close + 1;
    }

    return fs::path(expanded);
}

std::wstring with_long_path_prefix(const fs::path& path) {
    std::wstring native = path.native();
    if (starts_with(to_utf8(native), "\\\\?\\")) {
        return native;
    }
    if (native.size() >= 2 && native[1] == L':' && native.size() >= 260) {
        return L"\\\\?\\" + native;
    }
    return native;
}

// Python's `hash(str)`: SipHash-2-4 with a fixed key. Only used for the rare
// fallback filename, but keeping it identical means both versions produce the
// same names for the same URLs.
std::uint64_t rotate_left(std::uint64_t value, int bits) {
    return (value << bits) | (value >> (64 - bits));
}

void siphash_round(std::uint64_t& v0, std::uint64_t& v1, std::uint64_t& v2, std::uint64_t& v3) {
    v0 += v1;
    v1 = rotate_left(v1, 13);
    v1 ^= v0;
    v0 = rotate_left(v0, 32);
    v2 += v3;
    v3 = rotate_left(v3, 16);
    v3 ^= v2;
    v0 += v3;
    v3 = rotate_left(v3, 21);
    v3 ^= v0;
    v2 += v1;
    v1 = rotate_left(v1, 17);
    v1 ^= v2;
    v2 = rotate_left(v2, 32);
}

std::uint64_t python_string_hash(const std::string& text) {
    const std::uint64_t k0 = 0x736f6d6570736575ULL;
    const std::uint64_t k1 = 0x646f72616e646f6dULL;
    const std::uint64_t k2 = 0x6c7967656e657261ULL;
    const std::uint64_t k3 = 0x7465646279746573ULL;

    const auto* bytes = reinterpret_cast<const std::uint8_t*>(text.data());
    const std::size_t length = text.size();
    const std::size_t blocks = length / 8;

    std::uint64_t v0 = k0 ^ 0x0000000000000000ULL; // key[0]
    std::uint64_t v1 = k1 ^ 0x0000000000000000ULL; // key[1]
    std::uint64_t v2 = k2 ^ 0x0000000000000000ULL;
    std::uint64_t v3 = k3 ^ 0x0000000000000000ULL;
    v1 ^= 0xee;

    for (std::size_t i = 0; i < blocks; ++i) {
        std::uint64_t block = 0;
        for (std::size_t j = 0; j < 8; ++j) {
            block |= static_cast<std::uint64_t>(bytes[i * 8 + j]) << (8 * j);
        }
        v3 ^= block;
        siphash_round(v0, v1, v2, v3);
        siphash_round(v0, v1, v2, v3);
        v0 ^= block;
    }

    std::uint64_t tail = static_cast<std::uint64_t>(length & 0xff) << 56;
    for (std::size_t j = 0; j < length % 8; ++j) {
        tail |= static_cast<std::uint64_t>(bytes[blocks * 8 + j]) << (8 * j);
    }
    v3 ^= tail;
    siphash_round(v0, v1, v2, v3);
    siphash_round(v0, v1, v2, v3);
    v0 ^= tail;

    v2 ^= 0xff;
    siphash_round(v0, v1, v2, v3);
    siphash_round(v0, v1, v2, v3);
    siphash_round(v0, v1, v2, v3);
    siphash_round(v0, v1, v2, v3);

    return v0 ^ v1 ^ v2 ^ v3;
}

// ==================== 文件名生成 ====================
struct UrlParts {
    std::string host;
    std::string path;
};

UrlParts split_url(const std::string& url) {
    UrlParts parts;
    const std::size_t scheme_end = url.find("://");
    const std::size_t authority_start = scheme_end == std::string::npos ? 0 : scheme_end + 3;

    std::size_t path_start = url.find('/', authority_start);
    if (path_start == std::string::npos) {
        parts.host = url.substr(authority_start);
        return parts;
    }

    parts.host = url.substr(authority_start, path_start - authority_start);

    std::size_t path_end = url.size();
    const std::size_t query = url.find_first_of("?#", path_start);
    if (query != std::string::npos) {
        path_end = query;
    }
    parts.path = url.substr(path_start, path_end - path_start);
    return parts;
}

std::string base_name_of(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Python: `"".join(c for c in filename if c.isalnum() or c in "._-")`, which is
// byte-oriented and covers exactly ASCII [0-9A-Za-z._-].
std::string sanitize_file_name(const std::string& name) {
    std::string cleaned;
    cleaned.reserve(name.size());
    for (const char c : name) {
        const bool keep = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          c == '.' || c == '_' || c == '-';
        if (keep) {
            cleaned.push_back(c);
        }
    }
    return cleaned;
}

// Truncates to the same 100-character budget as the Python version, but keeps
// the extension intact so the saved file is still recognizable.
std::string truncate_file_name(const std::string& name) {
    if (name.size() <= kFileNameLimit) {
        return name;
    }
    const std::size_t dot = name.find_last_of('.');
    const bool has_extension = dot != std::string::npos && dot > 0 && name.size() - dot <= 12;
    if (!has_extension) {
        std::string cut = name.substr(0, kFileNameLimit);
        while (!cut.empty() && cut.back() == '.') {
            cut.pop_back();
        }
        return cut;
    }
    const std::string extension = name.substr(dot);
    const std::size_t room = kFileNameLimit - extension.size();
    std::string cut = name.substr(0, room) + extension;
    while (!cut.empty() && cut.back() == '.') {
        cut.pop_back();
    }
    return cut;
}

// Python: `filename = f"{parsed.hostname}_{abs(hash(url)) % 10000}"`.
std::string fallback_file_name(const std::string& url, const UrlParts& parts) {
    const std::uint64_t hash = python_string_hash(url);
    const std::uint64_t magnitude = hash == static_cast<std::uint64_t>(-1) ? 1 : (hash > 0x7fffffffffffffffULL
                                                                                     ? (~hash + 1ULL)
                                                                                     : hash);
    return parts.host + "_" + std::to_string(magnitude % 10000);
}

std::string extension_from_content_type(const std::string& content_type) {
    if (content_type.empty()) {
        return {};
    }
    if (content_type.find("html") != std::string::npos) {
        return "html";
    }
    if (content_type.find("json") != std::string::npos) {
        return "json";
    }
    if (content_type.find("image") != std::string::npos) {
        const std::size_t slash = content_type.find('/');
        if (slash == std::string::npos) {
            return {};
        }
        const std::size_t semicolon = content_type.find(';', slash);
        std::string subtype = content_type.substr(slash + 1, semicolon == std::string::npos
                                                                   ? std::string::npos
                                                                   : semicolon - slash - 1);
        while (!subtype.empty() && (subtype.back() == ' ' || subtype.back() == '\t')) {
            subtype.pop_back();
        }
        return subtype;
    }
    return "bin";
}

// ==================== 命令行 ====================
struct Args {
    std::string input;
    std::string output_dir;
    std::string folder = "downloads_mhy";
    int threads = 5;
    bool output_txt = false;
    bool all_exts = false;
    bool force = false;
    std::vector<std::string> extensions = kDefaultExtensions;
    bool help = false;
    bool version = false;
};

std::string default_input_template() {
    return kDefaultInputTemplate;
}

std::string help_text() {
    const std::string template_text = kHelpTemplate;
    const std::string placeholder = "@DEFAULT_INPUT@";
    std::string text;
    text.reserve(template_text.size() + 128);

    std::size_t cursor = 0;
    for (;;) {
        const std::size_t at = template_text.find(placeholder, cursor);
        if (at == std::string::npos) {
            text.append(template_text, cursor, std::string::npos);
            break;
        }
        text.append(template_text, cursor, at - cursor);
        text += default_input_template();
        cursor = at + placeholder.size();
    }
    return text;
}

void print_usage_hint() {
    log_line("用法: dmbg [选项]，详情见 dmbg --help", kColorDim);
}

// Returns std::nullopt on bad usage (already reported through `error`).
std::optional<Args> parse_arguments(int argc, char** argv, std::string& error) {
    Args args;
    args.input = default_input_template();

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        auto take_value = [&](const std::string& name) -> std::optional<std::string> {
            if (i + 1 >= argc) {
                error = "选项 " + name + " 缺少参数";
                return std::nullopt;
            }
            return std::string(argv[++i]);
        };

        if (arg == "-h" || arg == "--help") {
            args.help = true;
        } else if (arg == "-v" || arg == "--version") {
            args.version = true;
        } else if (arg == "-o" || arg == "--output-dir") {
            const auto value = take_value(arg);
            if (!value) return std::nullopt;
            args.output_dir = *value;
        } else if (arg == "-f" || arg == "--folder") {
            const auto value = take_value(arg);
            if (!value) return std::nullopt;
            args.folder = *value;
        } else if (arg == "-i" || arg == "--input") {
            const auto value = take_value(arg);
            if (!value) return std::nullopt;
            args.input = *value;
        } else if (arg == "-t" || arg == "--threads") {
            const auto value = take_value(arg);
            if (!value) return std::nullopt;
            try {
                args.threads = std::stoi(*value);
            } catch (const std::exception&) {
                error = "线程数必须是整数: " + *value;
                return std::nullopt;
            }
            if (args.threads < 1 || args.threads > 64) {
                error = "线程数必须在 1-64 之间";
                return std::nullopt;
            }
        } else if (arg == "-ot" || arg == "--output-txt") {
            args.output_txt = true;
        } else if (arg == "--all-exts") {
            args.all_exts = true;
        } else if (arg == "--force") {
            args.force = true;
        } else if (arg == "--ext") {
            const auto value = take_value(arg);
            if (!value) return std::nullopt;
            std::vector<std::string> parsed;
            std::string current;
            for (const char c : *value) {
                if (c == ',') {
                    if (!current.empty()) {
                        parsed.push_back(current);
                        current.clear();
                    }
                } else {
                    current.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
                }
            }
            if (!current.empty()) {
                parsed.push_back(current);
            }
            if (parsed.empty()) {
                error = "--ext 需要一个逗号分隔的扩展名列表";
                return std::nullopt;
            }
            args.extensions = std::move(parsed);
            args.all_exts = false;
        } else {
            error = "未知选项: " + arg;
            return std::nullopt;
        }
    }

    return args;
}

// ==================== 目录选择框 ====================
struct FolderPickerEvents : IFileDialogEvents {
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IFileDialogEvents) {
            *ppv = static_cast<IFileDialogEvents*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return 2; }
    IFACEMETHODIMP_(ULONG) Release() override { return 1; }

    IFACEMETHODIMP OnFileOk(IFileDialog*) override { return S_OK; }
    IFACEMETHODIMP OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP OnFolderChange(IFileDialog*) override { return S_OK; }
    IFACEMETHODIMP OnSelectionChange(IFileDialog*) override { return S_OK; }
    IFACEMETHODIMP OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE*) override {
        return S_OK;
    }
    IFACEMETHODIMP OnTypeChange(IFileDialog*) override { return S_OK; }
    IFACEMETHODIMP OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE*) override { return S_OK; }
};

std::optional<fs::path> pick_folder_dialog() {
    FolderPickerEvents events;
    IFileDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&dialog));
    if (FAILED(hr)) {
        log_line("无法创建目录选择框，请改用 -o/--output-dir 指定输出目录", kColorRed);
        return std::nullopt;
    }

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    }
    dialog->SetTitle(L"选择下载目录");

    DWORD cookie = 0;
    dialog->Advise(&events, &cookie);

    PWSTR initial = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DEFAULT, nullptr, &initial)) &&
        initial != nullptr) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial, nullptr, IID_PPV_ARGS(&item)))) {
            dialog->SetFolder(item);
            item->Release();
        }
        CoTaskMemFree(initial);
    }

    std::optional<fs::path> result;
    if (SUCCEEDED(dialog->Show(nullptr))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
                const std::wstring selected(path);
                CoTaskMemFree(path);
                // "This PC" and drive roots come back without a usable file path.
                if (selected.size() >= 3 && selected[1] == L':') {
                    result = fs::path(selected);
                } else {
                    log_line("请选择具体文件夹（当前选择的不是可写入的目录）", kColorYellow);
                }
            }
            item->Release();
        }
    }

    if (cookie != 0) {
        dialog->Unadvise(cookie);
    }
    dialog->Release();
    return result;
}

// ==================== 输入文件读取 ====================
struct ReadResult {
    std::vector<std::uint8_t> bytes;
    fs::path staging; // non-empty when the data came from a temp copy
    bool from_copy = false;
    std::string error;
};

// The launcher keeps `data_1` open; Python worked around that by copying the
// file to %TEMP%. A plain shared read handles the common case, and the copy
// stays as the fallback for when the launcher holds it exclusively.
ReadResult read_input_file(const fs::path& input) {
    ReadResult result;

    std::error_code size_error;
    const std::uintmax_t reported = fs::file_size(input, size_error);

    HANDLE file = CreateFileW(with_long_path_prefix(input).c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        const std::size_t reserve =
            !size_error && reported > 0 && reported < (64ull << 20) ? static_cast<std::size_t>(reported) : 0;
        result.bytes.reserve(reserve);
        std::vector<std::uint8_t> buffer(kReadChunk);
        DWORD read = 0;
        for (;;) {
            if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
                result.error = "读取文件失败 (Win32 " + std::to_string(GetLastError()) + ")";
                result.bytes.clear();
                CloseHandle(file);
                return result;
            }
            if (read == 0) {
                break;
            }
            result.bytes.insert(result.bytes.end(), buffer.begin(),
                                buffer.begin() + static_cast<std::ptrdiff_t>(read));
        }
        CloseHandle(file);
        return result;
    }

    const DWORD direct_error = GetLastError();

    // Fallback: shadow-copy into %TEMP% and read that. Sharing flags cannot be
    // widened on an existing handle, so this only helps when the launcher's
    // sharing mode accepts a fresh open for read.
    wchar_t temp_dir[MAX_PATH]{};
    const DWORD temp_length = GetTempPathW(MAX_PATH, temp_dir);
    if (temp_length == 0 || temp_length >= MAX_PATH) {
        result.error = "无法定位临时目录 (Win32 " + std::to_string(direct_error) + ")";
        return result;
    }

    const fs::path staging = fs::path(temp_dir) /
                             ("dmbg_cache_" + std::to_string(GetCurrentProcessId()) + ".tmp");
    std::error_code copy_error;
    fs::copy_file(input, staging, fs::copy_options::overwrite_existing, copy_error);
    if (copy_error) {
        result.error = "启动器缓存文件被占用且无法复制 (Win32 " + std::to_string(direct_error) +
                       ", 复制失败: " + copy_error.message() + ")";
        return result;
    }

    result.staging = staging;
    result.from_copy = true;

    std::ifstream stream(staging, std::ios::binary);
    if (!stream) {
        result.error = "无法打开临时副本: " + path_to_utf8(staging);
        return result;
    }
    std::vector<char> buffer(kReadChunk);
    while (stream) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = stream.gcount();
        if (got <= 0) {
            break;
        }
        result.bytes.insert(result.bytes.end(), buffer.begin(),
                            buffer.begin() + static_cast<std::ptrdiff_t>(got));
    }
    return result;
}

// Result of reserving an output name for one URL.
struct FileClaim {
    std::string path;
    bool already_complete = false; // an identical file is already there
};

// ==================== 进度条 ====================
class ProgressBar {
public:
    ProgressBar(std::size_t total, bool enabled) : total_(total), enabled_(enabled) {}

    std::size_t advance() {
        std::lock_guard<std::mutex> guard(mutex_);
        ++done_;
        render();
        return done_;
    }

    void finish() {
        std::lock_guard<std::mutex> guard(mutex_);
        done_ = total_;
        render();
        std::string clear = "\r";
        clear.append(4 + 48, ' ');
        clear += "\r";
        write_stdout(clear);
    }

private:
    void render() {
        if (!enabled_) {
            return;
        }
        const std::size_t width = 28;
        const int percent =
            total_ == 0 ? 100 : static_cast<int>(done_ * 100 / total_);
        const std::size_t filled =
            total_ == 0 ? width : (done_ * width / total_);
        const char* frames = "|/-\\";
        const char frame = frames[spin_++ % 4];

        std::string line = "\r";
        line += frame;
        line += " [";
        line.append(filled, '#');
        line.append(width - filled, '-');
        line += "] ";
        line += std::to_string(percent);
        line += "% (";
        line += std::to_string(done_);
        line += "/";
        line += std::to_string(total_);
        line += ")  ";
        write_stdout(line);
    }

    std::size_t total_ = 0;
    std::size_t done_ = 0;
    std::size_t spin_ = 0;
    bool enabled_ = false;
    std::mutex mutex_;
};

// ==================== 应用 ====================
class Application {
public:
    explicit Application(Args args) : args_(std::move(args)) {}

    int run();

private:
    fs::path resolve_input() const;
    bool prepare_output_directory();
    bool scan_and_report_urls();
    bool write_url_list() const;
    void start_workers();
    void download_one(std::size_t index);
    void report_errors() const;
    void cleanup_staging() const;
    FileClaim claim_output_path(const std::string& url, const std::string& content_type) const;
    void open_output_directory() const;

    Args args_;
    fs::path input_path_;
    fs::path output_root_;
    fs::path output_dir_;
    fs::path staging_;

    std::vector<std::string> urls_;
    std::vector<std::string> downloads_;

    std::atomic<std::size_t> next_{0};
    std::size_t success_ = 0;
    std::size_t failed_ = 0;
    std::size_t skipped_ = 0;
    std::size_t already_present_ = 0;
    long long bytes_total_ = 0;
    mutable std::mutex report_mutex_;
    std::vector<std::string> errors_;
};

fs::path Application::resolve_input() const {
    return fs::absolute(expand_env(args_.input));
}

bool Application::prepare_output_directory() {
    if (!args_.output_dir.empty()) {
        output_root_ = fs::absolute(expand_env(args_.output_dir));
    } else {
        log_line("请选择输出目录...", kColorCyan);
        const auto picked = pick_folder_dialog();
        if (!picked) {
            log_line("用户取消目录选择。", kColorYellow);
            return false;
        }
        output_root_ = *picked;
    }

    const std::wstring folder = to_wide(args_.folder);
    output_dir_ = folder.empty() ? output_root_ : output_root_ / fs::path(folder);

    std::error_code error;
    fs::create_directories(output_dir_, error);
    if (error) {
        // Creating the root can fail while the subfolder already exists; only
        // treat it as fatal if the subfolder really is unusable.
        if (!fs::exists(output_dir_, error)) {
            log_line("无法创建输出目录: " + path_to_utf8(output_dir_) + " (" + error.message() + ")",
                     kColorRed);
            return false;
        }
    }

    log_line("输出目录: " + path_to_utf8(output_dir_));
    return true;
}

bool Application::scan_and_report_urls() {
    ReadResult read = read_input_file(input_path_);
    if (!read.error.empty()) {
        log_line(read.error, kColorRed);
        log_line("请确认路径是否正确，或先用 -i/--input 指定缓存文件。", kColorDim);
        return false;
    }
    staging_ = read.staging;

    if (read.from_copy) {
        log_line("缓存文件被启动器占用，已先复制到临时目录再读取。", kColorDim);
    }
    log_line("读取 " + path_to_utf8(input_path_) + " (" + std::to_string(read.bytes.size()) + " 字节)",
             kColorDim);
    log_line("正在提取 URL...", kColorCyan);

    urls_ = dmbg::scan_urls(read.bytes);
    if (urls_.empty()) {
        log_line("未提取到有效 URL。", kColorYellow);
        return false;
    }
    log_line("共提取到 " + std::to_string(urls_.size()) + " 个唯一 URL");

    std::vector<std::pair<std::string, std::size_t>> by_extension;
    std::size_t cache_entries = 0;
    for (const std::string& url : urls_) {
        // Chromium range-request bookkeeping ("clip.webm:2fba...:3"), not a real
        // resource: the server answers 404 for every one of them.
        if (dmbg::url_is_cache_stream_entry(url)) {
            ++cache_entries;
            ++skipped_;
            continue;
        }
        const std::string extension = dmbg::url_extension(url);
        const bool wanted =
            args_.all_exts ||
            std::find(args_.extensions.begin(), args_.extensions.end(), extension) != args_.extensions.end();
        if (wanted) {
            downloads_.push_back(url);
            continue;
        }
        auto known = std::find_if(by_extension.begin(), by_extension.end(),
                                  [&](const auto& entry) { return entry.first == extension; });
        if (known == by_extension.end()) {
            by_extension.emplace_back(extension, 1);
        } else {
            ++known->second;
        }
        ++skipped_;
    }

    if (cache_entries != 0) {
        log_line("跳过 " + std::to_string(cache_entries) + " 个缓存分片条目（非真实资源，下载必 404）",
                 kColorDim);
    }

    if (!by_extension.empty()) {
        std::sort(by_extension.begin(), by_extension.end(),
                  [](const auto& left, const auto& right) { return left.second > right.second; });
        std::string summary;
        for (std::size_t i = 0; i < by_extension.size() && i < 8; ++i) {
            if (i != 0) {
                summary += ", ";
            }
            summary += by_extension[i].first.empty() ? "(无扩展名)" : ("." + by_extension[i].first);
            summary += " " + std::to_string(by_extension[i].second);
        }
        std::size_t listed = 0;
        for (const auto& entry : by_extension) {
            listed += entry.second;
        }
        log_line("跳过 " + std::to_string(listed) + " 个扩展名不匹配的 URL: " + summary, kColorDim);
    }

    if (downloads_.empty()) {
        log_line("没有符合扩展名的 URL。可用 --all-exts 或 --ext 放宽筛选。", kColorYellow);
        return false;
    }
    return true;
}

void Application::cleanup_staging() const {
    if (staging_.empty()) {
        return;
    }
    std::error_code error;
    fs::remove(staging_, error);
    if (!error) {
        log_line("已清理临时文件: " + path_to_utf8(staging_), kColorDim);
    }
}

bool Application::write_url_list() const {
    const fs::path list_path = output_dir_ / "all_urls.txt";
    std::ofstream stream(list_path, std::ios::binary);
    if (!stream) {
        log_line("无法写入 URL 清单: " + path_to_utf8(list_path), kColorRed);
        return false;
    }
    for (const std::string& url : urls_) {
        stream << url << "\r\n"; // the Python version joined with "\n"
    }
    log_line("URL 列表已保存: " + path_to_utf8(list_path));
    return true;
}

FileClaim Application::claim_output_path(const std::string& url, const std::string& content_type) const {
    const UrlParts parts = split_url(url);

    std::string name = base_name_of(parts.path);
    if (name.empty() || name.find('.') == std::string::npos) {
        name = fallback_file_name(url, parts);
    }
    name = sanitize_file_name(name);
    if (name.find('.') == std::string::npos) {
        // The common branch above always yields a dotted name; this keeps the
        // Python content-type fallback meaningful in the rare odd case.
        const std::string extension = extension_from_content_type(content_type);
        if (!extension.empty() && !name.empty()) {
            name += "." + extension;
        }
    }
    name = truncate_file_name(name);
    if (name.empty() || name == "." || name == "..") {
        name = fallback_file_name(url, parts);
    }

    for (int counter = 0; counter < 100'000; ++counter) {
        const fs::path candidate =
            counter == 0 ? output_dir_ / fs::path(to_wide(name))
                         : output_dir_ / fs::path(to_wide(name + "_" + std::to_string(counter)));

        if (args_.force) {
            // Overwrite in place: --force means "replace what is there", so the
            // first candidate is taken whether or not it already exists.
            // Continuing past an existing file would emit "name_1.webp" instead,
            // which is exactly the pile-up --force exists to avoid.
            if (counter == 0) {
                return FileClaim{path_to_utf8(candidate), false};
            }
        } else {
            // The name is derived only from the URL, so an existing file that is
            // not empty is this very resource and re-downloading it would just
            // spawn "name_1.webp" copies on every run. A zero-byte file is a
            // leftover placeholder from an interrupted run, so it is replaced.
            //
            // Keep looking when the name is taken by something unusable (a
            // directory, or a file we cannot read): the loop then falls through
            // to the next "_N" candidate, which is the old behaviour.
            std::error_code error;
            const bool exists = fs::exists(candidate, error);
            if (!error && exists && !fs::is_directory(candidate, error) && !error) {
                const std::uintmax_t size = fs::file_size(candidate, error);
                if (!error) {
                    if (size > 0) {
                        return FileClaim{path_to_utf8(candidate), true};
                    }
                    std::error_code ignored;
                    fs::remove(candidate, ignored); // discard the empty leftover
                }
            }
        }

        // CREATE_NEW is atomic, so concurrent workers cannot pick the same name.
        HANDLE handle = CreateFileW(with_long_path_prefix(candidate).c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            return FileClaim{path_to_utf8(candidate), false};
        }
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) {
            return FileClaim{};
        }
    }
    return FileClaim{};
}

void Application::download_one(std::size_t index) {
    const std::string& url = downloads_[index];
    if (args_.output_txt) {
        log_line("  [" + std::to_string(index + 1) + "/" + std::to_string(downloads_.size()) + "] " +
                 url);
    }

    const FileClaim claim = claim_output_path(url, {});
    if (claim.already_complete) {
        std::lock_guard<std::mutex> guard(report_mutex_);
        ++already_present_;
        return;
    }
    const std::string& output_path = claim.path;
    if (output_path.empty()) {
        std::lock_guard<std::mutex> guard(report_mutex_);
        ++failed_;
        errors_.push_back(url + " -> 无法创建输出文件");
        return;
    }

    const fs::path partial = fs::path(to_wide(output_path)).native() + L".part";
    HANDLE file = CreateFileW(partial.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        std::error_code ignored;
        fs::remove(fs::path(to_wide(output_path)), ignored); // release the claimed name
        std::lock_guard<std::mutex> guard(report_mutex_);
        ++failed_;
        errors_.push_back(url + " -> 无法写入 " + path_to_utf8(partial));
        return;
    }

    long long written = 0;
    bool io_ok = true;
    auto on_data = [&](const char* data, std::size_t length) -> bool {
        DWORD offset = 0;
        while (offset < length) {
            const DWORD want =
                (length - offset) > 0x7fffffffULL ? 0x7fffffff : static_cast<DWORD>(length - offset);
            DWORD chunk_written = 0;
            if (!WriteFile(file, data + offset, want, &chunk_written, nullptr) || chunk_written == 0) {
                io_ok = false;
                return false;
            }
            offset += chunk_written;
        }
        written += static_cast<long long>(length);
        return true;
    };

    dmbg::HttpResponse response;
    std::string error;
    const bool ok = dmbg::http_get(to_wide(url), on_data, response, error);
    CloseHandle(file);

    if (!ok && error.empty()) {
        error = "下载失败";
    }
    if (ok && !io_ok) {
        error = "写入磁盘失败（磁盘空间不足或路径过长）";
    }

    if (!ok || !io_ok) {
        // Drop both the partial body and the empty placeholder that
        // claim_output_path() created to reserve the name, so a failed download
        // never leaves a stray file behind.
        std::error_code ignored;
        fs::remove(partial, ignored);
        fs::remove(fs::path(to_wide(output_path)), ignored);
        std::lock_guard<std::mutex> guard(report_mutex_);
        ++failed_;
        errors_.push_back(url + " -> " + (error.empty() ? std::string("未知错误") : error));
        return;
    }

    std::error_code rename_error;
    fs::rename(partial, fs::path(to_wide(output_path)), rename_error);
    if (rename_error) {
        std::error_code ignored;
        fs::remove(partial, ignored);
        fs::remove(fs::path(to_wide(output_path)), ignored);
        std::lock_guard<std::mutex> guard(report_mutex_);
        ++failed_;
        errors_.push_back(url + " -> 重命名失败: " + rename_error.message());
        return;
    }

    std::lock_guard<std::mutex> guard(report_mutex_);
    ++success_;
    bytes_total_ += written;
}

void Application::start_workers() {
    const std::size_t total = downloads_.size();
    const int thread_count = std::max(1, std::min<int>(args_.threads, static_cast<int>(total)));
    ProgressBar bar(total, g_utf8_console);

    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(thread_count));

    for (int i = 0; i < thread_count; ++i) {
        workers.emplace_back([&]() {
            for (;;) {
                const std::size_t index = next_.fetch_add(1);
                if (index >= total) {
                    break;
                }
                download_one(index);
                bar.advance();
            }
            dmbg::http_release_thread_resources();
        });
    }

    for (std::thread& worker : workers) {
        worker.join();
    }
    bar.finish();
}

void Application::report_errors() const {
    if (errors_.empty()) {
        return;
    }
    log_line("失败明细:", kColorRed);
    for (const std::string& error : errors_) {
        log_line("  - " + error, kColorRed);
    }
}

void Application::open_output_directory() const {
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", output_dir_.c_str(), nullptr, nullptr,
                                           SW_SHOWNORMAL);
    if (reinterpret_cast<std::intptr_t>(result) <= 32) {
        log_line("无法自动打开输出目录（可手动前往）", kColorDim);
    }
}

int Application::run() {
    input_path_ = resolve_input();
    {
        std::error_code error;
        if (!fs::exists(input_path_, error)) {
            log_line("输入文件不存在: " + path_to_utf8(input_path_), kColorRed);
            log_line("请确认已运行启动器，或用 -i/--input 指定缓存文件。", kColorDim);
            return 1;
        }
    }

    if (!prepare_output_directory()) {
        return 1;
    }

    if (!scan_and_report_urls()) {
        cleanup_staging();
        return 1;
    }

    if (args_.output_txt && !write_url_list()) {
        cleanup_staging();
        return 1;
    }

    log_line("开始下载 " + std::to_string(downloads_.size()) + " 个文件（" +
                 std::to_string(args_.threads) + " 线程）...\n",
             kColorCyan);

    const auto started = std::chrono::steady_clock::now();
    start_workers();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count() / 1000.0;

    report_errors();

    char summary[320];
    if (already_present_ != 0) {
        std::snprintf(summary, sizeof(summary),
                      "下载完成: 新下载 %zu 已存在 %zu 失败 %zu 跳过 %zu，本次写入 %.1f MB，用时 %.1f 秒",
                      success_, already_present_, failed_, skipped_,
                      static_cast<double>(bytes_total_) / (1024.0 * 1024.0), elapsed);
    } else {
        std::snprintf(summary, sizeof(summary),
                      "下载完成: 成功 %zu 失败 %zu 跳过 %zu，共 %.1f MB，用时 %.1f 秒", success_,
                      failed_, skipped_, static_cast<double>(bytes_total_) / (1024.0 * 1024.0), elapsed);
    }
    log_line(summary, failed_ == 0 ? kColorGreen : kColorYellow);
    log_line("文件保存路径: " + path_to_utf8(output_dir_));

    cleanup_staging();
    open_output_directory();
    return failed_ == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    enable_utf8_console();

    std::string parse_error;
    const auto args = parse_arguments(argc, argv, parse_error);
    if (!args) {
        log_line(parse_error, kColorRed);
        print_usage_hint();
        return 2;
    }
    if (args->help) {
        log_line(help_text());
        return 0;
    }
    if (args->version) {
        log_line(std::string("dmbg ") + DMBG_VERSION);
        return 0;
    }

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    int exit_code = 1;
    try {
        Application app(*args);
        exit_code = app.run();
    } catch (const std::exception& error) {
        log_line(std::string("发生未预期的错误: ") + error.what(), kColorRed);
        exit_code = 1;
    }

    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
    return exit_code;
}
