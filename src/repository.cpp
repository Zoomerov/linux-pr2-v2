#include "repository.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

struct Json {
    enum class Type { Null, String, Array, Object } type = Type::Null;
    std::string value;
    std::vector<Json> array;
    std::map<std::string, Json> object;

    const Json &get(const std::string &key) const {
        static const Json missing;
        auto it = object.find(key);
        return it == object.end() ? missing : it->second;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string &text) : text_(text) {}

    Json parse() {
        Json result = readValue(0);
        skipSpace();
        if (position_ != text_.size()) fail();
        return result;
    }

private:
    const std::string &text_;
    size_t position_ = 0;

    [[noreturn]] void fail() const {
        throw std::runtime_error("Invalid JSON at byte " + std::to_string(position_));
    }

    void skipSpace() {
        while (position_ < text_.size() && std::isspace(
                   static_cast<unsigned char>(text_[position_]))) ++position_;
    }

    void expect(char ch) {
        skipSpace();
        if (position_ == text_.size() || text_[position_++] != ch) fail();
    }

    std::string readString() {
        expect('"');
        std::string value;
        while (position_ < text_.size()) {
            char ch = text_[position_++];
            if (ch == '"') return value;
            if (ch != '\\') {
                if (static_cast<unsigned char>(ch) < 0x20) fail();
                value += ch;
                continue;
            }
            if (position_ == text_.size()) fail();
            ch = text_[position_++];
            if (ch == 'n') value += '\n';
            else if (ch == 'r') value += '\r';
            else if (ch == 't') value += '\t';
            else if (ch == 'b') value += '\b';
            else if (ch == 'f') value += '\f';
            else if (ch == '"' || ch == '\\' || ch == '/') value += ch;
            else if (ch == 'u') {
                if (position_ + 4 > text_.size()) fail();
                const std::string hex = text_.substr(position_, 4);
                if (!std::all_of(hex.begin(), hex.end(), [](unsigned char c) {
                        return std::isxdigit(c);
                    })) fail();
                const unsigned code = std::stoul(hex, nullptr, 16);
                if (code < 0x80) value += static_cast<char>(code);
                else if (code < 0x800) {
                    value += static_cast<char>(0xc0 | (code >> 6));
                    value += static_cast<char>(0x80 | (code & 0x3f));
                } else {
                    value += static_cast<char>(0xe0 | (code >> 12));
                    value += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
                    value += static_cast<char>(0x80 | (code & 0x3f));
                }
                position_ += 4;
            } else fail();
        }
        fail();
    }

    Json readValue(int depth) {
        if (depth > 128) fail();
        skipSpace();
        if (position_ == text_.size()) fail();
        Json result;
        const char first = text_[position_];
        if (first == '"') {
            result.type = Json::Type::String;
            result.value = readString();
        } else if (first == '{') {
            result.type = Json::Type::Object;
            expect('{');
            skipSpace();
            if (position_ == text_.size()) fail();
            if (text_[position_] != '}') {
                do {
                    const std::string key = readString();
                    expect(':');
                    result.object[key] = readValue(depth + 1);
                    skipSpace();
                    if (position_ == text_.size()) fail();
                    if (text_[position_] != ',') break;
                    ++position_;
                } while (true);
            }
            expect('}');
        } else if (first == '[') {
            result.type = Json::Type::Array;
            expect('[');
            skipSpace();
            if (position_ == text_.size()) fail();
            if (text_[position_] != ']') {
                do {
                    result.array.push_back(readValue(depth + 1));
                    skipSpace();
                    if (position_ == text_.size()) fail();
                    if (text_[position_] != ',') break;
                    ++position_;
                } while (true);
            }
            expect(']');
        } else {
            while (position_ < text_.size() && text_[position_] != ',' &&
                   text_[position_] != '}' && text_[position_] != ']' &&
                   !std::isspace(static_cast<unsigned char>(text_[position_]))) {
                result.value += text_[position_++];
            }
            if (result.value.empty()) fail();
        }
        return result;
    }
};

std::string lower(std::string text) {
    for (char &ch : text) ch = static_cast<char>(std::tolower(
        static_cast<unsigned char>(ch)));
    return text;
}

std::string trim(std::string text) {
    auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

#ifdef _WIN32
std::wstring toWide(const std::string &text) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 0) throw std::runtime_error("Invalid UTF-8 URL");
    std::wstring wide(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), size);
    wide.pop_back();
    return wide;
}

std::string httpGet(const std::string &url) {
    const std::wstring wide = toWide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength =
        parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts)) {
        throw std::runtime_error("Invalid URL: " + url);
    }
    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    HINTERNET session = WinHttpOpen(L"nuget-graph/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connection = session ? WinHttpConnect(session, host.c_str(), parts.nPort, 0) : nullptr;
    HINTERNET request = connection ? WinHttpOpenRequest(
        connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    if (!request || !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                       WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        if (request) WinHttpCloseHandle(request);
        if (connection) WinHttpCloseHandle(connection);
        if (session) WinHttpCloseHandle(session);
        throw std::runtime_error("HTTP request failed: " + url);
    }
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        nullptr, &status, &size, nullptr);
    std::string body;
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(request, &available) && available) {
        const size_t offset = body.size();
        body.resize(offset + available);
        DWORD received = 0;
        if (!WinHttpReadData(request, body.data() + offset, available, &received)) break;
        body.resize(offset + received);
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    if (status < 200 || status >= 300) {
        throw std::runtime_error("HTTP " + std::to_string(status) + " for " + url);
    }
    return body;
}
#else
std::string httpGet(const std::string &url) {
    int pipefd[2];
    if (pipe(pipefd) != 0) throw std::runtime_error("Cannot start curl");
    const pid_t child = fork();
    if (child < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        throw std::runtime_error("Cannot start curl");
    }
    if (child == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        execlp("curl", "curl", "--fail", "--location", "--silent", "--show-error",
               "--max-time", "25", url.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    close(pipefd[1]);
    std::string body;
    char buffer[8192];
    ssize_t count = 0;
    while ((count = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
        body.append(buffer, static_cast<size_t>(count));
        if (body.size() > 32 * 1024 * 1024) break;
    }
    close(pipefd[0]);
    int status = 0;
    waitpid(child, &status, 0);
    if (body.size() > 32 * 1024 * 1024 || !WIFEXITED(status) || WEXITSTATUS(status)) {
        throw std::runtime_error("HTTP request failed: " + url);
    }
    return body;
}
#endif

bool isRegistrationType(const Json &type) {
    if (type.type == Json::Type::String) {
        return type.value.find("RegistrationsBaseUrl") != std::string::npos;
    }
    for (const auto &member : type.array) {
        if (isRegistrationType(member)) return true;
    }
    return false;
}

std::string registrationBase(const Json &index) {
    for (const auto &resource : index.get("resources").array) {
        if (isRegistrationType(resource.get("@type"))) {
            const std::string url = resource.get("@id").value;
            if (!url.empty()) return url.back() == '/' ? url : url + '/';
        }
    }
    throw std::runtime_error("NuGet service index has no registration endpoint");
}

std::vector<int> versionNumbers(const std::string &version) {
    std::vector<int> numbers;
    std::stringstream input(version.substr(0, version.find('-')));
    std::string part;
    while (std::getline(input, part, '.')) {
        try { numbers.push_back(std::stoi(part)); }
        catch (...) { numbers.push_back(0); }
    }
    return numbers;
}

bool newerVersion(const std::string &left, const std::string &right) {
    if (right.empty()) return true;
    const bool leftStable = left.find('-') == std::string::npos;
    const bool rightStable = right.find('-') == std::string::npos;
    if (leftStable != rightStable) return leftStable;
    auto a = versionNumbers(left), b = versionNumbers(right);
    a.resize(std::max(a.size(), b.size()));
    b.resize(a.size());
    if (a != b) return a > b;
    return left > right;
}

Json catalogEntry(const Json &leaf) {
    const Json &entry = leaf.get("catalogEntry");
    if (entry.type == Json::Type::Object) return entry;
    if (entry.type == Json::Type::String) return JsonParser(httpGet(entry.value)).parse();
    return {};
}

std::vector<std::string> remoteDependencies(const std::string &base,
                                             const std::string &package) {
    const Json index = JsonParser(httpGet(base + lower(package) + "/index.json")).parse();
    std::string latestVersion;
    Json latestEntry;
    for (const auto &page : index.get("items").array) {
        Json loaded = page;
        if (page.get("items").type != Json::Type::Array) {
            loaded = JsonParser(httpGet(page.get("@id").value)).parse();
        }
        for (const auto &leaf : loaded.get("items").array) {
            const Json entry = catalogEntry(leaf);
            std::string version = entry.get("version").value;
            if (version.empty()) version = leaf.get("catalogEntry").get("version").value;
            if (newerVersion(version, latestVersion)) {
                latestVersion = version;
                latestEntry = entry;
            }
        }
    }
    if (latestVersion.empty()) {
        throw std::runtime_error("No versions found for package " + package);
    }
    std::set<std::string> ids;
    for (const auto &group : latestEntry.get("dependencyGroups").array) {
        for (const auto &dependency : group.get("dependencies").array) {
            const std::string id = lower(dependency.get("id").value);
            if (!isNuGetId(id)) throw std::runtime_error("Invalid NuGet dependency ID");
            ids.insert(id);
        }
    }
    return {ids.begin(), ids.end()};
}

} // namespace

bool isTestPackage(const std::string &name) {
    return !name.empty() && std::all_of(name.begin(), name.end(), [](char ch) {
        return ch >= 'A' && ch <= 'Z';
    });
}

bool isNuGetId(const std::string &name) {
    return !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '.' || ch == '-' || ch == '_';
    });
}

Repository::Repository(const std::string &source, bool test) : test_(test) {
    if (!test_) {
        if (source.rfind("https://", 0) != 0 && source.rfind("http://", 0) != 0) {
            throw std::runtime_error("--repository must be an HTTP URL outside --test mode");
        }
        registrationBase_ = registrationBase(JsonParser(httpGet(source)).parse());
        return;
    }
    if (!fs::is_regular_file(fs::path(source))) {
        throw std::runtime_error("Test repository file not found: " + source);
    }
    std::ifstream input{fs::path(source)};
    std::string line;
    size_t number = 0;
    while (std::getline(input, line)) {
        ++number;
        line = trim(line.substr(0, line.find('#')));
        if (line.empty()) continue;
        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            throw std::runtime_error("Expected PACKAGE: DEPENDENCIES on line " +
                                     std::to_string(number));
        }
        const std::string name = trim(line.substr(0, colon));
        if (!isTestPackage(name) || testGraph_.contains(name)) {
            throw std::runtime_error("Invalid or duplicate package on line " +
                                     std::to_string(number));
        }
        std::string rest = line.substr(colon + 1);
        std::replace(rest.begin(), rest.end(), ',', ' ');
        std::stringstream stream(rest);
        std::set<std::string> unique;
        std::string dependency;
        while (stream >> dependency) {
            if (!isTestPackage(dependency)) {
                throw std::runtime_error("Invalid dependency on line " +
                                         std::to_string(number));
            }
            unique.insert(dependency);
        }
        testGraph_[name] = {unique.begin(), unique.end()};
    }
}

std::vector<std::string> Repository::dependencies(const std::string &package) {
    if (test_) {
        auto it = testGraph_.find(package);
        if (it == testGraph_.end()) {
            throw std::runtime_error("Package " + package + " is absent from test repository");
        }
        return it->second;
    }
    const std::string id = lower(package);
    auto it = cache_.find(id);
    if (it != cache_.end()) return it->second;
    return cache_[id] = remoteDependencies(registrationBase_, id);
}
