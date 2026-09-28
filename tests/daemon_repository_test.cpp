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
    fixture.write("space name.cpp", "int spaced = 0;\n");
    fixture.write("tracked-binary.dat", std::string("a\0b", 3));
    fixture.commit("base");

    fixture.write("staged.cpp", "int staged = 1;\n");
    fixture.git({"add", "--", "staged.cpp"});
    fixture.write("mixed.cpp", "int mixed = 1;\n");
    fixture.git({"add", "--", "mixed.cpp"});
    fixture.write("mixed.cpp", "int mixed = 2;\n");
    fixture.write("unstaged.cpp", "int unstaged = 1;\n");
    fixture.write("space name.cpp", "int spaced = 1;\n");
    std::filesystem::remove(fixture.root() / "deleted.cpp");
    fixture.git({"mv", "--", "old name.cpp", "new name.cpp"});
    fixture.write("nested/new file.cpp", "int fresh = 1;\n");
    fixture.write("tracked-binary.dat", std::string("c\0d", 3));

    aegis::daemon::repository::GitRepository repository(fixture.root());
    const auto changes = repository.changes();
    const auto find = [&](const std::string &path) -> const aegis::daemon::GitChange * {
        const auto item = std::find_if(changes.begin(), changes.end(), [&](const auto &change) { return change.path == path; });
        return item == changes.end() ? nullptr : &*item;
    };
    assert(find("staged.cpp") && find("staged.cpp")->indexStatus == "M" && find("staged.cpp")->worktreeStatus == " ");
    assert(find("staged.cpp")->additions == 1 && find("staged.cpp")->deletions == 1);
    assert(find("mixed.cpp") && find("mixed.cpp")->indexStatus == "M" && find("mixed.cpp")->worktreeStatus == "M");
    assert(find("mixed.cpp")->additions == 1 && find("mixed.cpp")->deletions == 1);
    assert(find("space name.cpp") && find("space name.cpp")->additions == 1 && find("space name.cpp")->deletions == 1);
    assert(find("unstaged.cpp") && find("unstaged.cpp")->indexStatus == " " && find("unstaged.cpp")->worktreeStatus == "M");
    assert(find("deleted.cpp") && find("deleted.cpp")->worktreeStatus == "D" && find("deleted.cpp")->deletions == 1);
    assert(find("nested/new file.cpp") && find("nested/new file.cpp")->indexStatus == "?");
    assert(find("tracked-binary.dat") && find("tracked-binary.dat")->binary);

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
    fixture.write("large.txt", std::string(1024 * 1024 + 1, 'x'));

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
    const auto large = files.read("large.txt", aegis::daemon::repository::FileSource::worktree);
    assert(large.exists && large.truncated && large.content.empty());
    const auto explicitlyLoaded = files.read("large.txt", aegis::daemon::repository::FileSource::worktree, true);
    assert(explicitlyLoaded.exists && !explicitlyLoaded.truncated && explicitlyLoaded.content.size() == 1024 * 1024 + 1);

    const auto nested = files.list("nested", aegis::daemon::repository::FileScope::all);
    assert(std::any_of(nested.entries.begin(), nested.entries.end(), [](const auto &entry) { return entry.path == "nested/new file.cpp"; }));
    const auto unannotated = files.list("nested", aegis::daemon::repository::FileScope::all, 500, false, false);
    const auto unannotatedFile = std::find_if(unannotated.entries.begin(), unannotated.entries.end(), [](const auto &entry) { return entry.path == "nested/new file.cpp"; });
    assert(unannotatedFile != unannotated.entries.end() && !unannotatedFile->changed);
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

    RepositoryFixture history;
    history.write("old name.cpp", "int renamed = 0;\n");
    history.write("gone.cpp", "int gone = 0;\n");
    history.write("space name.cpp", "int value = 0;\n");
    history.commit("base snapshot");
    history.git({"mv", "--", "old name.cpp", "new name.cpp"});
    history.write("space name.cpp", "int value = 1;\n");
    history.write("new file.cpp", "int fresh = 1;\n");
    std::filesystem::remove(history.root() / "gone.cpp");
    history.commit("inspectable snapshot");
    auto commitId = history.git({"rev-parse", "HEAD"});
    while (!commitId.empty() && (commitId.back() == '\n' || commitId.back() == '\r')) commitId.pop_back();
    aegis::daemon::repository::GitRepository historyGit(history.root());
    const auto commits = historyGit.commits();
    assert(!commits.empty() && commits.front().id == commitId);
    assert(commits.front().parentId && commits.front().subject == "inspectable snapshot");
    const auto commitFiles = historyGit.commitFiles(commitId);
    const auto commitFile = [&](const std::string &path) -> const aegis::daemon::repository::CommitFile * {
        const auto item = std::find_if(commitFiles.begin(), commitFiles.end(), [&](const auto &entry) { return entry.path == path; });
        return item == commitFiles.end() ? nullptr : &*item;
    };
    assert(commitFile("new name.cpp") && commitFile("new name.cpp")->status == "R");
    assert(commitFile("new name.cpp")->oldPath == "old name.cpp");
    assert(commitFile("space name.cpp") && commitFile("space name.cpp")->status == "M");
    assert(commitFile("new file.cpp") && commitFile("new file.cpp")->status == "A");
    assert(commitFile("gone.cpp") && commitFile("gone.cpp")->status == "D");

    aegis::daemon::repository::Files historyFiles(historyGit);
    const auto commitComparison = historyFiles.compareCommit(commitId, "space name.cpp");
    assert(commitComparison.original.content == "int value = 0;\n");
    assert(commitComparison.modified.content == "int value = 1;\n");
    const auto commitRename = historyFiles.compareCommit(commitId, "new name.cpp");
    assert(commitRename.oldPath == "old name.cpp");
    assert(commitRename.original.content == "int renamed = 0;\n");
    assert(commitRename.modified.content == "int renamed = 0;\n");
    const auto commitDelete = historyFiles.compareCommit(commitId, "gone.cpp");
    assert(commitDelete.original.content == "int gone = 0;\n" && !commitDelete.modified.exists);
}
