#include "daemon/protocol/json.h"
#include "daemon/repository/files.h"
#include "daemon/repository/git.h"
#include "repository_fixture.h"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <stdexcept>
#include <string>

int main() {
    RepositoryFixture fixture;
    fixture.write("staged.cpp", "int staged = 0;\n");
    fixture.write("mixed.cpp", "int mixed = 0;\n");
    fixture.write("unstaged.cpp", "int unstaged = 0;\n");
    fixture.write("deleted.cpp", "int deleted = 0;\n");
    fixture.write("old name.cpp", "int renamed = 0;\n");
    fixture.commit("base");

    fixture.write("staged.cpp", "int staged = 1;\n");
    fixture.git({"add", "--", "staged.cpp"});
    fixture.write("mixed.cpp", "int mixed = 1;\n");
    fixture.git({"add", "--", "mixed.cpp"});
    fixture.write("mixed.cpp", "int mixed = 2;\n");
    fixture.write("unstaged.cpp", "int unstaged = 1;\n");
    std::filesystem::remove(fixture.root() / "deleted.cpp");
    fixture.git({"mv", "--", "old name.cpp", "new name.cpp"});
    fixture.write("nested/new file.cpp", "int fresh = 1;\n");

    aegis::daemon::repository::GitRepository repository(fixture.root());
    const auto changes = repository.changes();
    const auto find = [&](const std::string &path) -> const aegis::daemon::GitChange * {
        const auto item = std::find_if(changes.begin(), changes.end(), [&](const auto &change) { return change.path == path; });
        return item == changes.end() ? nullptr : &*item;
    };
    assert(find("staged.cpp") && find("staged.cpp")->indexStatus == "M" && find("staged.cpp")->worktreeStatus == " ");
    assert(find("mixed.cpp") && find("mixed.cpp")->indexStatus == "M" && find("mixed.cpp")->worktreeStatus == "M");
    assert(find("unstaged.cpp") && find("unstaged.cpp")->indexStatus == " " && find("unstaged.cpp")->worktreeStatus == "M");
    assert(find("deleted.cpp") && find("deleted.cpp")->worktreeStatus == "D");
    assert(find("nested/new file.cpp") && find("nested/new file.cpp")->indexStatus == "?");

    const auto renamed = find("new name.cpp");
    assert(renamed && renamed->indexStatus == "R");
    const auto serializedRename = aegis::daemon::protocol::toJson(*renamed);
    assert(serializedRename.value("old_path", std::string{}) == "old name.cpp");

    fixture.write(".gitignore", "/.aegis/\n/node_modules/\n");
    fixture.git({"add", ".gitignore"});
    fixture.write(".aegis/session.sqlite", "private");
    fixture.write("node_modules/pkg/index.js", "ignored");
    fixture.write("binary.dat", std::string("x\0y", 3));
    fixture.write("empty.txt", "");

    aegis::daemon::repository::Files files(repository);
    const auto head = files.read("mixed.cpp", aegis::daemon::repository::FileSource::head);
    const auto index = files.read("mixed.cpp", aegis::daemon::repository::FileSource::index);
    const auto worktree = files.read("mixed.cpp", aegis::daemon::repository::FileSource::worktree);
    assert(head.content == "int mixed = 0;\n");
    assert(index.content == "int mixed = 1;\n");
    assert(worktree.content == "int mixed = 2;\n");

    const auto comparison = files.compare("mixed.cpp");
    assert(comparison.original.content == head.content);
    assert(comparison.modified.content == worktree.content);
    const auto unstaged = files.compare("mixed.cpp", aegis::daemon::repository::FileSource::index,
                                        aegis::daemon::repository::FileSource::worktree);
    assert(unstaged.original.content == index.content && unstaged.modified.content == worktree.content);

    const auto newFile = files.compare("nested/new file.cpp");
    assert(!newFile.original.exists && newFile.modified.content == "int fresh = 1;\n");
    const auto deletedFile = files.compare("deleted.cpp");
    assert(deletedFile.original.content == "int deleted = 0;\n" && !deletedFile.modified.exists);
    const auto renamedFile = files.compare("new name.cpp");
    assert(renamedFile.oldPath == "old name.cpp");
    assert(renamedFile.original.content == "int renamed = 0;\n");
    assert(renamedFile.modified.content == "int renamed = 0;\n");

    const auto binary = files.read("binary.dat", aegis::daemon::repository::FileSource::worktree);
    assert(binary.binary && binary.content.empty());
    const auto empty = files.read("empty.txt", aegis::daemon::repository::FileSource::worktree);
    assert(empty.exists && empty.content.empty());

    const auto nested = files.list("nested", aegis::daemon::repository::FileScope::all);
    assert(std::any_of(nested.entries.begin(), nested.entries.end(), [](const auto &entry) { return entry.path == "nested/new file.cpp"; }));
    const auto quickOpen = files.list("", aegis::daemon::repository::FileScope::all, 500, true);
    assert(std::any_of(quickOpen.entries.begin(), quickOpen.entries.end(), [](const auto &entry) { return entry.path == "nested/new file.cpp" && entry.kind == "file"; }));
    assert(std::none_of(quickOpen.entries.begin(), quickOpen.entries.end(), [](const auto &entry) { return entry.path.starts_with(".aegis/") || entry.path.starts_with("node_modules/"); }));
    const auto changed = files.list("", aegis::daemon::repository::FileScope::changed);
    assert(std::any_of(changed.entries.begin(), changed.entries.end(), [](const auto &entry) { return entry.path == "nested" && entry.changed; }));
    assert(std::none_of(changed.entries.begin(), changed.entries.end(), [](const auto &entry) { return entry.path.starts_with(".aegis/") || entry.path.starts_with("node_modules/"); }));
    assert(std::none_of(changed.entries.begin(), changed.entries.end(), [](const auto &entry) { return entry.path == ".git"; }));

    const auto outside = std::filesystem::temp_directory_path() / ("aegis-outside-" + std::to_string(getpid()));
    fixture.write("escape", "inside");
    std::ofstream(outside) << "outside";
    std::filesystem::remove(fixture.root() / "escape");
    std::filesystem::create_symlink(outside, fixture.root() / "escape");
    bool rejectedTraversal = false;
    bool rejectedSymlink = false;
    try { (void)files.read("../outside", aegis::daemon::repository::FileSource::worktree); }
    catch (const std::invalid_argument &) { rejectedTraversal = true; }
    try { (void)files.read("escape", aegis::daemon::repository::FileSource::worktree); }
    catch (const std::invalid_argument &) { rejectedSymlink = true; }
    std::filesystem::remove(outside);
    assert(rejectedTraversal && rejectedSymlink);

    RepositoryFixture emptyRepository;
    emptyRepository.write("first.cpp", "int first = 1;\n");
    aegis::daemon::repository::GitRepository emptyGit(emptyRepository.root());
    aegis::daemon::repository::Files emptyFiles(emptyGit);
    const auto firstCommitComparison = emptyFiles.compare("first.cpp");
    assert(!firstCommitComparison.original.exists && firstCommitComparison.modified.content == "int first = 1;\n");
}
