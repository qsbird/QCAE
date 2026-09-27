#pragma once

#include "qcae/read_view.hpp"
#include "qcae/document_view.hpp"
#include "qcae/render_packet.hpp"
#include "qcae/view_session.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace qcae {

enum class QueryOp {
    all,
    kind,
    ids,
    name_contains,
    source_number_range,
    member_of,
    material,
    section,
    world_box,
    and_,
    or_,
    not_
};
enum class MembershipKind { part, assembly, set, include };
enum class BoxRelation { contained, intersects };
enum class VisibilityMode { through, visible_only, picker_candidates };
enum class SetOperation { intersection, union_, difference };

struct WorldBox {
    Vec3 minimum;
    Vec3 maximum;
    BoxRelation relation{BoxRelation::contained};
};

// Only the field associated with op is read. Logical nodes use children.
struct QueryPredicate {
    QueryOp op{QueryOp::all};
    std::string text;
    std::vector<EntityId> ids;
    EntityId related_entity{};
    MembershipKind membership_kind{MembershipKind::part};
    std::uint64_t first_number{};
    std::uint64_t last_number{};
    WorldBox box;
    std::vector<QueryPredicate> children;
};

struct SelectionScope {
    // nullopt means every model entity; an empty vector is an empty universe.
    std::optional<std::vector<EntityId>> candidate_ids;
    bool include_hidden{};
    bool invert{};
    VisibilityMode visibility{VisibilityMode::through};
};
struct QuerySpec {
    QueryPredicate predicate;
    SelectionScope scope;
};
struct QueryResult {
    DocumentRef document;
    Revision model_revision{};
    std::string view_session_id;
    std::uint64_t view_revision{};
    std::vector<EntityId> ids;
};
struct SelectionHandle {
    std::string id;
    DocumentRef document;
    Revision model_revision{};
    std::string view_session_id;
    std::uint64_t view_revision{};
    std::size_t count{};
};
struct SelectionPage {
    SelectionHandle handle;
    std::size_t offset{};
    std::vector<EntityId> ids;
};
struct SelectionLimits {
    std::size_t max_views{128};
    std::size_t max_handles{128};
    std::size_t max_packet_entities{200000};
    std::size_t max_page_size{10000};
};

struct EntityPage {
    RecordVersion version;
    std::size_t offset{};
    std::size_t total{};
    std::vector<EntitySummary> entities;
};
struct ReferencePage {
    RecordVersion version;
    std::size_t offset{};
    std::size_t total{};
    std::vector<Reference> references;
};
// Organization views include intermediate organization rows and exclude only the owner.
// This differs intentionally from the selection predicate's member-only scope.
struct EntityFilter {
    std::string kind;
    std::string name_contains;
    std::optional<std::vector<EntityId>> ids;
    std::string view{"all"};
    std::optional<EntityId> owner;
};
bool query_kind_registered(const DocumentView&, std::string_view);
std::size_t query_entity_count(const DocumentView&, std::string_view kind = {});
Result<EntityPage> query_entities(const DocumentView&,
                                  const EntityFilter&,
                                  std::size_t offset,
                                  std::size_t limit,
                                  RecordStats* = nullptr);
Result<EntityPage> query_entities(const DocumentView&,
                                  std::size_t offset,
                                  std::size_t limit,
                                  std::string_view kind = {},
                                  RecordStats* = nullptr);
Result<RecordInput> query_fields(const DocumentView&, const EntityId&, RecordStats* = nullptr);
Result<ReferencePage> query_references(const DocumentView&,
                                       const EntityId&,
                                       bool incoming,
                                       std::size_t offset,
                                       std::size_t limit,
                                       RecordStats* = nullptr);
std::vector<EntityId> query_affected_analyses(const DocumentView&, const EntityId&);

// Pure record query. The supplied session must match this document/epoch/revision.
Result<QueryResult> execute_query(const DocumentView&, const ViewSession&, const QuerySpec&);
Result<RenderPacket>
produce_render_packet(const DocumentView&, const ViewSession&, std::size_t max_entities = 200000);

// Compatibility overloads materialize records from the legacy snapshot and are instrumented.
Result<QueryResult> execute_query(const ModelSnapshot&, const ViewSession&, const QuerySpec&);
Result<RenderPacket>
produce_render_packet(const ModelSnapshot&, const ViewSession&, std::size_t max_entities = 200000);

// Session state is disposable. The caller supplies an immutable, current snapshot
// for each operation; model/view changes invalidate existing selection handles.
class SelectionService {
  public:
    explicit SelectionService(SelectionLimits limits = {});
    Result<ViewSession> create_view(const DocumentView&,
                                    const Caller&,
                                    std::vector<EntityId> hidden_ids = {},
                                    std::string camera_fingerprint = {});
    Result<ViewSession>
    get_view(const DocumentView&, const Caller&, const std::string& view_id) const;
    // Allows a host to check expected_view_revision before rebasing after a model edit.
    Result<ViewSession>
    inspect_view(const DocumentView&, const Caller&, const std::string& view_id) const;
    Result<ViewSession> update_view(const DocumentView&,
                                    const Caller&,
                                    const std::string& view_id,
                                    std::vector<EntityId> hidden_ids,
                                    std::string camera_fingerprint);
    Result<SelectionHandle>
    select(const DocumentView&, const Caller&, const std::string& view_id, const QuerySpec&);
    Result<SelectionHandle> combine(const DocumentView&,
                                    const Caller&,
                                    const std::string& view_id,
                                    const std::string& left_handle,
                                    const std::string& right_handle,
                                    SetOperation);
    Result<SelectionPage> page(const DocumentView&,
                               const Caller&,
                               const std::string& handle_id,
                               std::size_t offset,
                               std::size_t limit) const;
    Result<RenderPacket>
    render_packet(const DocumentView&, const Caller&, const std::string& view_id) const;

    // Legacy snapshot compatibility, excluded from production locality measurements.
    Result<ViewSession> create_view(const ModelSnapshot&,
                                    const Caller&,
                                    std::vector<EntityId> hidden_ids = {},
                                    std::string camera_fingerprint = {});
    Result<ViewSession>
    get_view(const ModelSnapshot&, const Caller&, const std::string& view_id) const;
    // Allows a host to check expected_view_revision before rebasing after a model edit.
    Result<ViewSession>
    inspect_view(const ModelSnapshot&, const Caller&, const std::string& view_id) const;
    Result<ViewSession> update_view(const ModelSnapshot&,
                                    const Caller&,
                                    const std::string& view_id,
                                    std::vector<EntityId> hidden_ids,
                                    std::string camera_fingerprint);
    Result<SelectionHandle>
    select(const ModelSnapshot&, const Caller&, const std::string& view_id, const QuerySpec&);
    Result<SelectionHandle> combine(const ModelSnapshot&,
                                    const Caller&,
                                    const std::string& view_id,
                                    const std::string& left_handle,
                                    const std::string& right_handle,
                                    SetOperation);
    Result<SelectionPage> page(const ModelSnapshot&,
                               const Caller&,
                               const std::string& handle_id,
                               std::size_t offset,
                               std::size_t limit) const;
    Result<RenderPacket>
    render_packet(const ModelSnapshot&, const Caller&, const std::string& view_id) const;

  private:
    struct OwnedView {
        ViewSession view;
        std::string owner;
    };
    struct StoredSelection {
        SelectionHandle handle;
        std::string owner;
        std::vector<EntityId> ids;
        std::uint64_t order{};
    };
    SelectionLimits limits_;
    std::string nonce_;
    std::uint64_t next_id_{1};
    std::map<std::string, OwnedView> views_;
    std::map<std::string, StoredSelection> selections_;
    std::set<std::string> expired_views_;
    std::deque<std::string> expired_view_order_;
    std::set<std::string> expired_selections_;
    std::deque<std::string> expired_selection_order_;

    Result<ViewSession>
    checked_view(const DocumentView&, const Caller&, const std::string& view_id) const;
    Result<SelectionHandle>
    store_selection(const ViewSession&, const Caller&, std::vector<EntityId>);
    void remember_expired_view(const std::string&);
    void remember_expired_selection(const std::string&);
};

} // namespace qcae
