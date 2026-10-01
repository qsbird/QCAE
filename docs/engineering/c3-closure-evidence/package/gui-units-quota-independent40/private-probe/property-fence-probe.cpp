#include <QJsonObject>
#include <QStringList>
#include <cstdint>
#include <iostream>
namespace json_ledger { QString string(const QJsonValue& v) { return v.toString(); } }
struct State {
 QString document_id_{"document"},epoch_{"epoch"},revision_{"7"};
 QStringList selected_ids_{"force-id"};
 bool scene_invalidated_{}, context_refresh_required_{}, render_pending_{true};
 std::uint64_t event_generation_{},render_generation_{},selection_generation_{},property_preview_generation_{37};
 struct Resources { void clear() {} } resources_;
 void invalidateSelectionRequests(bool) { ++selection_generation_; }
 bool succeeded(const QJsonObject& r) const { return r.value("status") == "success"; }
 QJsonObject context(bool) const { return {{"document_id",document_id_},{"document_epoch",epoch_},{"expected_revision",revision_}}; }
 void change() {
 const QString kind="DocumentChanged";
 const QJsonObject event{{"document_id","document"},{"document_epoch","epoch"},{"revision","8"}};
 const QJsonObject event_data{{"resync_required",true}};
            if (kind == "DocumentChanged" &&
                (json_ledger::string(event.value("document_id")) != document_id_ ||
                 json_ledger::string(event.value("document_epoch")) != epoch_ ||
                 json_ledger::string(event.value("revision")) != revision_ ||
                 event_data.value("active") == QJsonValue(false) ||
                 event_data.value("resync_required") == QJsonValue(true))) {
                // Fence pending replies immediately; the authoritative query may still be queued.
                scene_invalidated_ = true;
                context_refresh_required_ = true;
                ++event_generation_;
                ++render_generation_;
                resources_.clear();
                render_pending_ = false;
                invalidateSelectionRequests(true);
            }
 }
 bool rowAdmitted(const QJsonObject& response) {
 const QString doc="document",epoch="epoch",rev="7",selected_id="force-id";
 if (doc != document_id_ || epoch != epoch_ || rev != revision_ ||
                     selected_ids_.size() != 1 || selected_ids_.front() != selected_id ||
                     !succeeded(response)) return false;
 return true;
 }
 bool previewAdmitted() {
 const std::uint64_t generation=37;
 const QJsonObject expected{{"document_id","document"},{"document_epoch","epoch"},{"expected_revision","7"}};
 if (generation != property_preview_generation_ || expected != context(true)) return false;
 return true;
 }
 bool automaticPreviewAdmitted() {
 const std::uint64_t generation=37; const bool property_request=true;
 const QJsonObject expected{{"document_id","document"},{"document_epoch","epoch"},{"expected_revision","7"}};
 if (expected != context(true) ||
                    (property_request && generation != property_preview_generation_)) return false;
 return true;
 }
};
int main() {
 State state; state.change();
 const QJsonObject oldReply{{"status","success"},{"revision","7"}};
 const QJsonObject newerReply{{"status","success"},{"revision","8"}};
 const bool oldRow=state.rowAdmitted(oldReply),newerRow=state.rowAdmitted(newerReply),manual=state.previewAdmitted(),automatic=state.automaticPreviewAdmitted();
 std::cout << "source_extracted_guards=true actual_native_gui=false\ncontext_refresh_required=" << state.context_refresh_required_ << " scene_invalidated=" << state.scene_invalidated_ << " selection_generation=" << state.selection_generation_ << " local_revision=" << state.revision_.toStdString() << "\n" << "old_row_admitted=" << oldRow << " newer_row_admitted=" << newerRow << " old_manual_preview_admitted=" << manual << " old_automatic_preview_admitted=" << automatic << "\n";
 return (oldRow||newerRow||manual||automatic) ? 1 : 0;
}
