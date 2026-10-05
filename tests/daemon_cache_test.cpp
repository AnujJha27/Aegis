#include "daemon/repository/cache.h"
#include "daemon/repository/files.h"
#include "daemon/repository/watcher.h"
#include "repository_fixture.h"
#include <cassert>
int main() {
    using namespace aegis::daemon::repository;
    ReviewCache<std::string> cache(800, 2);
    cache.put("a", "a", 1); cache.put("b", "b", 1);
    assert(cache.get("a")); cache.put("c", "c", 1);
    assert(!cache.get("b")); assert(cache.get("a")); assert(cache.stats().entries == 2);
    cache.put("huge", "ignored", 801); assert(!cache.get("huge")); assert(cache.stats().bytes <= 800);
    RepositoryFixture fixture;
    fixture.write("a.txt", "old\n"); fixture.commit("base");
    GitRepository git(fixture.root()); Files files(git);
    assert(files.read("a.txt", FileSource::head).content == "old\n");
    assert(files.read("a.txt", FileSource::head).content == "old\n");
    const auto metadata = git.metadataKey();
    assert(files.read("a.txt", FileSource::worktree).content == "old\n");
    fixture.write("a.txt", "new\n");
    assert(files.read("a.txt", FileSource::worktree).content == "new\n");
    assert(files.compare("a.txt").modified.content == "new\n");
    assert(files.compare("a.txt").modified.content == "new\n");
    fixture.write("a.txt", "end\n");
    assert(files.compare("a.txt").modified.content == "end\n");
    fixture.git({"add", "a.txt"}); assert(git.metadataKey() != metadata);
    assert(files.read("a.txt", FileSource::index).content == "end\n");
    fixture.commit("updated"); assert(files.read("a.txt", FileSource::head).content == "end\n");
    std::filesystem::remove(fixture.root() / "a.txt");
    assert(!files.read("a.txt", FileSource::worktree).exists);
    std::filesystem::create_symlink("/etc/passwd", fixture.root() / "a.txt");
    bool rejected = false;
    try { files.read("a.txt", FileSource::worktree); } catch (const std::invalid_argument &) { rejected = true; }
    assert(rejected);
    std::filesystem::remove(fixture.root() / "a.txt"); fixture.write("a.txt", "end\n");
    aegis::daemon::EventHub hub; auto subscription = hub.subscribe();
    RepositoryWatcher watcher(git, files, hub, [] { return true; });
    aegis::daemon::AgentEvent event;
    assert(hub.wait(subscription, event, std::chrono::seconds(4))); assert(event.type == "repository.changed");
    fixture.write("external.txt", "new file\n");
    bool seen = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (hub.wait(subscription, event, std::chrono::milliseconds(500)) && event.content.find("external.txt") != std::string::npos) { seen = true; break; }
    }
    assert(seen);
    const auto oldKey = git.metadataKey();
    fixture.git({"add", "external.txt"});
    assert(git.metadataKey() != oldKey);
    seen = false;
    const auto stagedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < stagedDeadline) {
        if (hub.wait(subscription, event, std::chrono::milliseconds(500)) && event.content.find("\"invalidate_all\":true") != std::string::npos) { seen = true; break; }
    }
    assert(seen);
}
