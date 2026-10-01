
#pragma once
#include "/Users/qs/Documents/ChatGPT/QCAE/build-c3-qt-observed/tranche8b-private/qcae-sdk-observer.hpp"
#include <limits>
namespace QcaeFtHash {
struct Scope;
inline thread_local Scope *active = nullptr;
struct Scope {
    Scope *previous;
    unsigned cache;
    std::uint64_t remaining;
    unsigned actions = 0;
    bool unknown = false;
    bool finished = false;
    QcaeQtSdkNoexceptScope sdk;
    explicit Scope(unsigned kind, std::uint64_t byteLimit = (std::numeric_limits<std::uint64_t>::max)())
        : previous(active), cache(kind), remaining(byteLimit), sdk(kind == 1 ? "ft_hash/glyph_operation" : "ft_hash/missing_operation")
    { active = this; }
    void finish() noexcept { finished = true; }
    ~Scope() noexcept {
        struct Restore { Scope *previous; ~Restore() noexcept { active = previous; } } restore{previous};
        if (qcae_qt_sdk_observer_status()) unknown = true;
        if (!finished || actions != 1)
            markUnknown("ft_hash/unseen_or_multiple_node_action");
        qcae_qt_sdk_observer_emit(cache == 1 ? "ft_hash/glyph_operation_complete" : "ft_hash/missing_operation_complete", unknown ? 3 : 0, 0);
    }
    void markUnknown(const char *site) noexcept {
        unknown = true;
        qcae_qt_sdk_observer_emit(site, 3, 0);
    }
};
inline void record(const char *site, unsigned kind, std::uint64_t bytes) {
    if (!active)
        return;
    if (bytes > active->remaining) {
        active->markUnknown("ft_hash/observer_byte_limit");
        return;
    }
    active->remaining -= bytes;
    qcae_qt_sdk_observer_emit(site, kind, bytes);
}
template<class Key, class = void> struct HasGlyphFields : std::false_type {};
template<class Key> struct HasGlyphFields<Key, std::void_t<decltype(std::declval<Key>().glyph), decltype(std::declval<Key>().subPixelPosition)>> : std::true_type {};
template<class Node, class Value, class = void> struct HasDirectValue : std::false_type {};
template<class Node, class Value> struct HasDirectValue<Node, Value, std::void_t<decltype(std::declval<Node>().value)>> : std::is_same<std::decay_t<decltype(std::declval<Node>().value)>, Value> {};
template<class Node> bool knownNode() {
    if (!active)
        return false;
    using Key = typename Node::KeyType;
    using Value = typename Node::ValueType;
    constexpr bool glyphShape = HasGlyphFields<Key>::value;
    constexpr bool trivialKey = std::is_trivially_copyable_v<Key> && std::is_standard_layout_v<Key>;
    constexpr bool directValue = HasDirectValue<Node, Value>::value;
    constexpr bool glyphNode = glyphShape && trivialKey && sizeof(Key) == 12 && std::is_pointer_v<Value> && directValue;
    constexpr bool missingNode = trivialKey && std::is_unsigned_v<Key> && sizeof(Key) == 4 && std::is_empty_v<Value>;
    if ((active->cache == 1 && glyphNode) || (active->cache == 2 && missingNode))
        return true;
    active->markUnknown("ft_hash/unsupported_node_type");
    return false;
}
template<class Node> void field(const char *site, unsigned kind, std::uint64_t bytes) {
    if (knownNode<Node>())
        record(site, kind, bytes);
}
template<class Node> void nodeAction(const char *site, unsigned kind, std::uint64_t bytes) {
    if (knownNode<Node>()) {
        ++active->actions;
        record(site, kind, bytes);
    }
}
template<class Node> void *copy(const char *site, void *dest, const void *src, std::size_t bytes) {
    auto *result = std::memcpy(dest, src, bytes);
    field<Node>(site, 1, bytes);
    return result;
}
template<class Node> void *initialize(const char *site, void *dest, int value, std::size_t bytes) {
    auto *result = std::memset(dest, value, bytes);
    field<Node>(site, 2, bytes);
    return result;
}
} // namespace QcaeFtHash
