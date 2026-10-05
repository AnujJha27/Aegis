#include "daemon/repository/index.h"
#include "repository_fixture.h"
#include <atomic>
#include <thread>
#include <iostream>
int main() {
    using namespace aegis::daemon::repository;
    RepositoryFixture fixture;
    fixture.write(".gitignore", "ignored/\n");
    fixture.write("ignored/secret.txt", "hidden");
    fixture.write("node_modules/pkg/a.js", "hidden");
    fixture.write(".aegis/private", "hidden");
    fixture.write("src/VirtualCodeView.tsx", "hello");
    fixture.write("deep/path/virtual-other.ts", "other");
    GitRepository git(fixture.root());
    RepositoryIndex index(git);
    assert(!index.snapshot()->indexing);
    if (index.snapshot()->files.size() != 3) { for (const auto &entry : index.snapshot()->files) std::cerr << entry.file.path << "\n"; }
    assert(index.snapshot()->files.size() == 3);
    assert(index.snapshot()->children.at("src").size() == 1);
    assert(index.search("VCV").entries.front().path == "src/VirtualCodeView.tsx");
    assert(index.search("virtual").entries.front().path == "src/VirtualCodeView.tsx");
    assert(index.search("zzzz").entries.empty());
    const auto before = index.snapshot();
    fixture.write("new.txt", "new");
    std::filesystem::remove(fixture.root() / "src/VirtualCodeView.tsx");
    std::jthread reader([&] { for (int i = 0; i < 1000; ++i) { const auto view = index.snapshot(); assert(view->version >= before->version); index.search("v"); } });
    index.rebuild();
    assert(index.snapshot()->version > before->version);
    assert(index.search("new.txt").entries.front().path == "new.txt");
    assert(index.search("VCV").entries.empty());
    for (int i = 0; i < 10000; ++i) fixture.write("many/File" + std::to_string(i) + ".cpp", "x");
    index.rebuild();
    assert(index.snapshot()->files.size() == 10003);
    assert(index.search("File").entries.size() == 60);
    assert(index.search("File9999.cpp").entries.front().path == "many/File9999.cpp");
}
