#include "qcae/artifacts_local.hpp"
#include <array>
#include <bit>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace qcae {
namespace {
constexpr std::uint64_t max_bytes = 16 * 1024 * 1024;
constexpr std::string_view stage_name = ".qcae-stage";
void require(bool ok, std::string_view message) {
    if (!ok)
        throw std::runtime_error(std::string(message));
}
std::filesystem::path safe_path(std::string_view text) {
    require(!text.empty() && text.size() <= 1024 && text.find('\0') == std::string_view::npos,
            "Invalid artifact resource path");
    const std::filesystem::path path(text);
    require(!path.is_absolute() && path.has_filename(), "Artifact resource must be relative");
    for (const auto& item : path)
        require(item != ".." && item != "." && item != "" && item != stage_name &&
                    item != "manifest.json" && item != ".manifest.part",
                "Unsafe artifact resource path");
    return path;
}
void no_symlink(const std::filesystem::path& root, const std::filesystem::path& relative) {
    auto path = root;
    require(!std::filesystem::is_symlink(root), "Artifact directory is a symlink");
    for (const auto& item : relative) {
        path /= item;
        require(!std::filesystem::is_symlink(path), "Artifact resource is a symlink");
    }
}
void sync_directory(const std::filesystem::path& path) {
#if defined(__unix__) || defined(__APPLE__)
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    require(descriptor >= 0, "Cannot open artifact directory for synchronization");
    const int result = ::fsync(descriptor);
    ::close(descriptor);
    require(result == 0, "Cannot synchronize artifact directory");
#else
    (void)path;
    throw std::runtime_error("Durable artifact publication is unsupported on this platform");
#endif
}
void write_new(const std::filesystem::path& path, std::string_view bytes) {
#if defined(__unix__) || defined(__APPLE__)
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    require(descriptor >= 0, "Cannot exclusively create artifact file");
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count <= 0) {
            ::close(descriptor);
            throw std::runtime_error("Cannot write complete artifact file");
        }
        offset += static_cast<std::size_t>(count);
    }
    const int result = ::fsync(descriptor);
    ::close(descriptor);
    require(result == 0, "Cannot synchronize artifact file");
#else
    (void)path;
    (void)bytes;
    throw std::runtime_error("Durable artifact publication is unsupported on this platform");
#endif
}
std::string read_file(const std::filesystem::path& path, std::uint64_t expected) {
    require(expected <= max_bytes && std::filesystem::is_regular_file(path) &&
                !std::filesystem::is_symlink(path) && std::filesystem::file_size(path) == expected,
            "Artifact file is missing, partial or has an invalid length");
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "Cannot open artifact file");
    std::string bytes(static_cast<std::size_t>(expected), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(static_cast<std::uint64_t>(input.gcount()) == expected && !input.bad(),
            "Cannot independently read complete artifact file");
    return bytes;
}
void validate(const LocalArtifactIntent& intent) {
    require(!intent.artifact_id.empty() && !intent.task_id.empty() &&
                intent.directory.is_absolute() && !intent.manifest.empty() &&
                intent.manifest.size() <= max_bytes && !intent.files.empty(),
            "Artifact intent is incomplete");
    const auto root = safe_path(intent.root_resource).generic_string();
    std::set<std::string> paths;
    std::uint64_t bytes = 0;
    for (const auto& file : intent.files) {
        const auto path = safe_path(file.path).generic_string();
        require(path == file.path && paths.insert(path).second && file.sha256.size() == 64 &&
                    file.sha256.find_first_not_of("0123456789abcdef") == std::string::npos &&
                    file.byte_length <= max_bytes - bytes,
                "Artifact file manifest is invalid");
        bytes += file.byte_length;
    }
    require(paths.contains(root), "Artifact root is not declared in the manifest");
}
} // namespace

std::string artifact_sha256(std::string_view bytes) {
    constexpr std::array<std::uint32_t, 64> constants{
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    std::array<std::uint32_t, 8> state{0x6a09e667,
                                       0xbb67ae85,
                                       0x3c6ef372,
                                       0xa54ff53a,
                                       0x510e527f,
                                       0x9b05688c,
                                       0x1f83d9ab,
                                       0x5be0cd19};
    const auto blocks = (bytes.size() + 9 + 63) / 64;
    for (std::size_t block = 0; block < blocks; ++block) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 64; ++index) {
            const auto position = block * 64 + index;
            std::uint8_t value{};
            if (position < bytes.size())
                value = static_cast<std::uint8_t>(bytes[position]);
            else if (position == bytes.size())
                value = 0x80;
            else if (position >= blocks * 64 - 8)
                value = static_cast<std::uint8_t>((static_cast<std::uint64_t>(bytes.size()) * 8) >>
                                                  ((blocks * 64 - position - 1) * 8));
            words[index / 4] |= static_cast<std::uint32_t>(value) << ((3 - index % 4) * 8);
        }
        for (std::size_t index = 16; index < words.size(); ++index) {
            const auto a = words[index - 15], b = words[index - 2];
            const auto s0 = std::rotr(a, 7) ^ std::rotr(a, 18) ^ (a >> 3);
            const auto s1 = std::rotr(b, 17) ^ std::rotr(b, 19) ^ (b >> 10);
            words[index] = words[index - 16] + s0 + words[index - 7] + s1;
        }
        auto work = state;
        for (std::size_t index = 0; index < words.size(); ++index) {
            const auto s1 = std::rotr(work[4], 6) ^ std::rotr(work[4], 11) ^ std::rotr(work[4], 25);
            const auto choose = (work[4] & work[5]) ^ (~work[4] & work[6]);
            const auto t1 = work[7] + s1 + choose + constants[index] + words[index];
            const auto s0 = std::rotr(work[0], 2) ^ std::rotr(work[0], 13) ^ std::rotr(work[0], 22);
            const auto majority = (work[0] & work[1]) ^ (work[0] & work[2]) ^ (work[1] & work[2]);
            const auto t2 = s0 + majority;
            for (std::size_t slot = 7; slot > 0; --slot)
                work[slot] = work[slot - 1];
            work[4] += t1;
            work[0] = t1 + t2;
        }
        for (std::size_t index = 0; index < state.size(); ++index)
            state[index] += work[index];
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto value : state)
        output << std::setw(8) << value;
    return output.str();
}
std::vector<ArtifactFileDigest> artifact_file_digests(const ArtifactPlan& plan) {
    std::vector<ArtifactFileDigest> files;
    for (const auto& resource : plan.resources)
        files.push_back({resource.path, resource.text.size(), artifact_sha256(resource.text)});
    return files;
}
std::vector<TextResource> LocalArtifactStore::stage(const LocalArtifactIntent& intent,
                                                    std::span<const TextResource> resources,
                                                    const Checkpoint& checkpoint) const {
    validate(intent);
    require(resources.size() == intent.files.size(), "Artifact resource list is incomplete");
    if (checkpoint)
        checkpoint();
    require(std::filesystem::is_directory(intent.directory.parent_path()),
            "Artifact destination parent does not exist");
    require(std::filesystem::create_directory(intent.directory),
            "Artifact destination already exists; publication never overwrites it");
    sync_directory(intent.directory.parent_path());
    const auto staging = intent.directory / stage_name;
    require(std::filesystem::create_directory(staging), "Cannot create artifact staging directory");
    std::set<std::string> written;
    for (const auto& resource : resources) {
        if (checkpoint)
            checkpoint();
        const auto path = safe_path(resource.path);
        require(written.insert(path.generic_string()).second, "Duplicate artifact resource");
        std::filesystem::create_directories((staging / path).parent_path());
        no_symlink(staging, path);
        write_new(staging / path, resource.text);
        sync_directory((staging / path).parent_path());
    }
    sync_directory(staging);
    if (checkpoint)
        checkpoint();
    return read(intent, true);
}
std::vector<TextResource> LocalArtifactStore::read(const LocalArtifactIntent& intent,
                                                   bool staged) const {
    validate(intent);
    const auto root = staged ? intent.directory / stage_name : intent.directory;
    require(std::filesystem::is_directory(root) && !std::filesystem::is_symlink(root),
            "Artifact directory is missing or invalid");
    std::vector<TextResource> resources;
    for (const auto& file : intent.files) {
        const auto path = safe_path(file.path);
        no_symlink(root, path);
        auto bytes = read_file(root / path, file.byte_length);
        require(artifact_sha256(bytes) == file.sha256, "Artifact SHA256 mismatch");
        resources.push_back({file.path, std::move(bytes)});
    }
    return resources;
}
void LocalArtifactStore::publish(const LocalArtifactIntent& intent) const {
    (void)read(intent, true);
    const auto staging = intent.directory / stage_name;
    require(!std::filesystem::exists(intent.directory / "manifest.json"),
            "Artifact completion manifest already exists");
    for (const auto& file : intent.files) {
        const auto path = safe_path(file.path);
        no_symlink(intent.directory, path);
        require(!std::filesystem::exists(intent.directory / path),
                "Artifact publication refuses to replace an existing file");
        std::filesystem::create_directories((intent.directory / path).parent_path());
        std::filesystem::rename(staging / path, intent.directory / path);
        sync_directory((intent.directory / path).parent_path());
    }
    std::filesystem::remove_all(staging);
    write_new(intent.directory / ".manifest.part", intent.manifest);
    std::filesystem::rename(intent.directory / ".manifest.part",
                            intent.directory / "manifest.json");
    sync_directory(intent.directory);
    (void)verify(intent);
}
std::vector<TextResource> LocalArtifactStore::verify(const LocalArtifactIntent& intent) const {
    validate(intent);
    const auto manifest = read_file(intent.directory / "manifest.json", intent.manifest.size());
    require(manifest == intent.manifest, "Artifact completion manifest disagrees with intent");
    return read(intent, false);
}
} // namespace qcae
