#pragma once
#include "qcae/model_codec.hpp"
#include <functional>
#include <filesystem>
#include <span>

namespace qcae {
struct ArtifactFileDigest {
    std::string path;
    std::uint64_t byte_length{};
    std::string sha256;
    bool operator==(const ArtifactFileDigest&) const = default;
};
struct LocalArtifactIntent {
    std::string artifact_id, task_id;
    std::filesystem::path directory;
    std::string root_resource;
    std::vector<ArtifactFileDigest> files;
    // Exact, canonical UTF-8 manifest bytes frozen and persisted before filesystem writes.
    std::string manifest;
};
[[nodiscard]] std::string artifact_sha256(std::string_view);
[[nodiscard]] std::vector<ArtifactFileDigest> artifact_file_digests(const ArtifactPlan&);
// The new directory belongs to one persisted task. The manifest is the completion marker;
// files and SQLite facts are reconciled explicitly, never treated as one atomic transaction.
class LocalArtifactStore {
  public:
    using Checkpoint = std::function<void()>;
    [[nodiscard]] std::vector<TextResource>
    stage(const LocalArtifactIntent&, std::span<const TextResource>, const Checkpoint&) const;
    void publish(const LocalArtifactIntent&) const;
    [[nodiscard]] std::vector<TextResource> verify(const LocalArtifactIntent&) const;

  private:
    [[nodiscard]] std::vector<TextResource> read(const LocalArtifactIntent&, bool staged) const;
};
} // namespace qcae
