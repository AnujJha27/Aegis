#include "daemon/api/static_files.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <vector>

namespace aegis::daemon::api {
namespace {

constexpr std::size_t maxStaticFileSize = 16 * 1024 * 1024;

class Descriptor final {
public:
    explicit Descriptor(int value) : value_(value) {}
    ~Descriptor() { if (value_ >= 0) ::close(value_); }
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
    int get() const { return value_; }
    void reset(int value) {
        if (value_ >= 0) ::close(value_);
        value_ = value;
    }

private:
    int value_;
};

Response statusResponse(boost::beast::http::status status) {
    Response response{status, 11};
    response.prepare_payload();
    return response;
}

}

Response staticFileResponse(const std::filesystem::path &webRoot, std::string_view target) {
    const auto path = target.substr(0, target.find('?'));
    std::string normalized(path);
    for (auto &character : normalized) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    if (path.empty() || path.front() != '/' || path.find('\0') != std::string_view::npos || normalized.find("%2e") != std::string::npos)
        return statusResponse(boost::beast::http::status::bad_request);
    if (webRoot.empty()) return statusResponse(boost::beast::http::status::not_found);

    const auto relative = path == "/" ? std::string("index.html") : std::string(path.substr(1));
    const std::filesystem::path relativePath(relative);
    if (relativePath.is_absolute() || relativePath.has_root_name())
        return statusResponse(boost::beast::http::status::bad_request);
    std::vector<std::string> components;
    for (const auto &component : relativePath) {
        if (component == "..") return statusResponse(boost::beast::http::status::bad_request);
        if (component != ".") components.push_back(component.string());
    }
    if (components.empty()) return statusResponse(boost::beast::http::status::not_found);

    std::error_code error;
    const auto root = std::filesystem::canonical(webRoot, error);
    if (error) return statusResponse(boost::beast::http::status::not_found);
    Descriptor directory(open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (directory.get() < 0) return statusResponse(boost::beast::http::status::not_found);
    for (std::size_t i = 0; i < components.size(); ++i) {
        const bool last = i + 1 == components.size();
        const auto flags = last ? O_RDONLY | O_CLOEXEC | O_NOFOLLOW
                                : O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
        const auto next = openat(directory.get(), components[i].c_str(), flags);
        if (next < 0) return statusResponse(boost::beast::http::status::not_found);
        directory.reset(next);
    }

    struct stat info {};
    if (fstat(directory.get(), &info) != 0 || !S_ISREG(info.st_mode))
        return statusResponse(boost::beast::http::status::not_found);
    if (info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) > maxStaticFileSize)
        return statusResponse(boost::beast::http::status::payload_too_large);

    std::string body;
    body.reserve(static_cast<std::size_t>(info.st_size));
    std::array<char, 16384> buffer{};
    while (body.size() <= maxStaticFileSize) {
        const auto request = std::min(buffer.size(), maxStaticFileSize + 1 - body.size());
        const auto count = ::read(directory.get(), buffer.data(), request);
        if (count < 0) {
            if (errno == EINTR) continue;
            return statusResponse(boost::beast::http::status::internal_server_error);
        }
        if (count == 0) break;
        body.append(buffer.data(), static_cast<std::size_t>(count));
    }
    if (body.size() > maxStaticFileSize) return statusResponse(boost::beast::http::status::payload_too_large);

    Response response{boost::beast::http::status::ok, 11};
    const auto extension = relativePath.extension().string();
    const auto contentType = extension == ".html" ? "text/html" : extension == ".js" ? "text/javascript" : extension == ".css" ? "text/css" : "application/octet-stream";
    response.set(boost::beast::http::field::content_type, contentType);
    response.set(boost::beast::http::field::cache_control,
                 path.starts_with("/assets/") ? "public, max-age=31536000, immutable" : "no-cache");
    response.body() = std::move(body);
    response.prepare_payload();
    return response;
}

}
