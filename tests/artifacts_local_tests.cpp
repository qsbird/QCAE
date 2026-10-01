#include "qcae/artifacts_local.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class Work> void rejects(Work work) {
    bool failed = false;
    try {
        work();
    } catch (const std::exception&) {
        failed = true;
    }
    require(failed, "Invalid artifact operation succeeded");
}
} // namespace
int main() {
    using namespace qcae;
    auto directory = std::filesystem::temp_directory_path() /
                     ("qcae-artifacts-" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        require(artifact_sha256("") ==
                    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                "SHA256 empty vector failed");
        require(artifact_sha256("abc") ==
                    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                "SHA256 short vector failed");
        require(artifact_sha256(std::string(1000000, 'a')) ==
                    "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
                "SHA256 multiple blocks vector failed");
        std::filesystem::create_directory(directory);
        ArtifactPlan plan{"model.bdf",
                          {{"model.bdf", "BEGIN BULK\nINCLUDE 'parts/nodes.bdf'\nENDDATA\n"},
                           {"parts/nodes.bdf", "GRID,1,,0.,0.,0.\n"}},
                          {},
                          EntityId("analysis"),
                          {}};
        LocalArtifactIntent intent{"artifact",
                                   "task",
                                   directory / "complete",
                                   plan.root_resource,
                                   artifact_file_digests(plan),
                                   "{\"complete\":true,\"task_id\":\"task\"}\n"};
        LocalArtifactStore store;
        rejects([&] { (void)store.verify(intent); });
        const auto readback = store.stage(intent, plan.resources, [] {});
        require(readback.size() == 2 && readback[1].text == plan.resources[1].text,
                "Staged file readback differs");
        rejects([&] { (void)store.verify(intent); });
        store.publish(intent);
        require(store.verify(intent).front().text == plan.resources.front().text,
                "Published readback differs");
        rejects([&] { (void)store.stage(intent, plan.resources, [] {}); });
        auto wrong = intent;
        wrong.manifest[2] = 'X';
        rejects([&] { (void)store.verify(wrong); });
        {
            std::ofstream corrupt(intent.directory / "parts/nodes.bdf", std::ios::binary);
            corrupt << std::string(plan.resources[1].text.size(), 'x');
        }
        rejects([&] { (void)store.verify(intent); });
        std::filesystem::remove(intent.directory / "parts/nodes.bdf");
        rejects([&] { (void)store.verify(intent); });
        auto interrupted = intent;
        interrupted.directory = directory / "interrupted";
        unsigned checkpoints{};
        rejects([&] {
            (void)store.stage(interrupted, plan.resources, [&] {
                if (++checkpoints == 3)
                    throw std::runtime_error("cancelled");
            });
        });
        rejects([&] { (void)store.verify(interrupted); });
        require(!std::filesystem::exists(interrupted.directory / "manifest.json"),
                "Cancelled operation published a completion marker");
        auto unsafe = intent;
        unsafe.directory = directory / "unsafe";
        unsafe.files.front().path = "../escape.bdf";
        rejects([&] { (void)store.stage(unsafe, plan.resources, [] {}); });
        require(!std::filesystem::exists(unsafe.directory),
                "Invalid manifest mutated the filesystem");
        std::filesystem::remove_all(directory);
        std::cout << "PASS: durable staging, completion marker and independent hash verification\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove_all(directory);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
