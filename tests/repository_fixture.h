#pragma once

#include "daemon/process/process.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

class RepositoryFixture final {
public:
    RepositoryFixture() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path() /
            ("aegis-git-fixture-" + std::to_string(getpid()) + "-" + std::to_string(nonce));
        std::filesystem::create_directories(root_);
        git({"init", "--quiet"});
        git({"config", "user.name", "Aegis Test"});
        git({"config", "user.email", "aegis-test@example.invalid"});
    }

    ~RepositoryFixture() { std::filesystem::remove_all(root_); }

    const std::filesystem::path &root() const { return root_; }

    void write(const std::string &path, const std::string &contents) const {
        const auto file = root_ / path;
        std::filesystem::create_directories(file.parent_path());
        std::ofstream output(file, std::ios::binary);
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        assert(output.good());
    }

    std::string git(const std::vector<std::string> &arguments) const {
        auto command = std::vector<std::string>{"git"};
        command.insert(command.end(), arguments.begin(), arguments.end());
        const auto result = aegis::daemon::process::run(command, root_);
        assert(result.exitCode == 0);
        return result.output;
    }

    void commit(const std::string &message) const {
        git({"add", "--all"});
        git({"commit", "--quiet", "-m", message});
    }

private:
    std::filesystem::path root_;
};
