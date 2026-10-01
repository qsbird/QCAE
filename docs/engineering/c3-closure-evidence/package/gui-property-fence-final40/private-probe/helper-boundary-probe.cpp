#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
bool succeeded(const QJsonObject& response) { return response.value("status")=="success"; }
bool propertyReplyMatches(const QJsonObject& response,
                          const QString& document,
                          const QString& epoch,
                          const QString& revision) {
    const auto matches_revision = [&](const QJsonValue& value) {
        if (value.isString())
            return value.toStringView() == revision;
        // Older engine metadata used JSON numbers. Only exactly representable
        // nonnegative integers can establish a version; never truncate a value.
        if (!value.isDouble())
            return false;
        const auto number = value.toDouble();
        constexpr auto max_exact = 9007199254740991ULL;
        bool valid = false;
        const auto expected = revision.toULongLong(&valid);
        return valid && expected <= max_exact && std::isfinite(number) && number >= 0 &&
               std::trunc(number) == number && number <= static_cast<double>(max_exact) &&
               static_cast<std::uint64_t>(number) == expected;
    };
    if (!succeeded(response) || !response.value("data").isObject())
        return false;
    const auto data = response.value("data").toObject();
    // Both entity.query and preview contracts carry data.revision. An optional
    // envelope version or identity must agree rather than override that source.
    if (!matches_revision(data.value("revision")))
        return false;
    for (const auto& object : {response, data}) {
        if ((object.contains("revision") && !matches_revision(object.value("revision"))) ||
            (object.contains("document_id") && object.value("document_id") != document) ||
            (object.contains("document_epoch") && object.value("document_epoch") != epoch))
            return false;
    }
    return true;
}
struct State {
 QString document_id_{"document"},epoch_{"epoch"},revision_{"7"};
 bool scene_invalidated_{},context_refresh_required_{},render_pending_{true},fields_nonempty{true};
 std::uint64_t event_generation_{},render_generation_{},selection_generation_{},property_preview_generation_{37};
 struct Resources { void clear() {} } resources_;
 struct Client { bool ready() const { return true; } } client_;
 struct Modeling { void setSelectedGeometry(QString,QString) {} } model; Modeling* modeling_{&model};
 void invalidateSelectionRequests(bool) { ++selection_generation_; }
 void clearPropertyPreview() { ++property_preview_generation_; }
 void clearPropertyFields() { fields_nonempty=false; }
void invalidateModelContext() {
        scene_invalidated_ = true;
        context_refresh_required_ = true;
        ++event_generation_;
        ++render_generation_;
        resources_.clear();
        render_pending_ = false;
        invalidateSelectionRequests(true);
        clearPropertyPreview();
        clearPropertyFields();
        modeling_->setSelectedGeometry({}, {});
    }
bool propertyRepliesBlocked() const {
        return !client_.ready() || document_id_.isEmpty() || context_refresh_required_ ||
               scene_invalidated_;
    }
};
int main() {
 int checks=0;
 auto check=[&](bool ok,const char* text){ ++checks;if(!ok)throw std::runtime_error(text); };
 auto response=[](QJsonValue revision){ return QJsonObject{{"status","success"},{"data",QJsonObject{{"revision",revision}}}}; };
 auto match=[&](const QJsonObject& r,const QString& revision="7"){ return propertyReplyMatches(r,"document","epoch",revision); };
 try {
 check(match(response("7")),"canonical string");
 check(match(response(7)),"legacy number");
 check(!match(response("07")),"noncanonical metadata");
 check(!match(response("7.0")),"decimal string");
 check(!match(response(7.5)),"fractional");
 check(!match(response(-1)),"negative");
 check(!match(response(true)),"bool");
 check(!match(response(QJsonValue())),"null");
 check(!match(response(QJsonObject{})),"object");
 check(!match(response(QJsonArray{})),"array");
 check(!match(QJsonObject{{"status","success"},{"data",QJsonObject{}}}),"required data revision");
 check(!match(QJsonObject{{"status","success"},{"revision","7"}}),"required object data");
 auto r=response("7");r.insert("status","failed");check(!match(r),"failed status");
 r=response("7");r.insert("revision","8");check(!match(r),"envelope revision conflict");
 r=response("7");r.insert("revision",7);check(match(r),"exact envelope legacy");
 r=response("7");r.insert("document_id","other");check(!match(r),"envelope document conflict");
 r=response("7");r.insert("document_epoch","other");check(!match(r),"envelope epoch conflict");
 r=response("7");auto d=r.value("data").toObject();d.insert("document_id","other");r.insert("data",d);check(!match(r),"data document conflict");
 r=response("7");d=r.value("data").toObject();d.insert("document_epoch",false);r.insert("data",d);check(!match(r),"wrong identity type");
 r=response("7");d=r.value("data").toObject();d.insert("document_id","document");d.insert("document_epoch","epoch");r.insert("data",d);check(match(r),"matching optional identities");
 check(match(response(9007199254740991.0),"9007199254740991"),"max exact legacy revision");
 check(!match(response(9007199254740992.0),"9007199254740992"),"2^53 numeric rejected");
 check(!match(response(static_cast<qint64>(9007199254740993LL)),"9007199254740993"),"precision lost integer rejected");
 check(match(response("18446744073709551615"),"18446744073709551615"),"uint64 canonical text preserved");
 check(!match(response(static_cast<qint64>(9223372036854775807LL)),"9223372036854775807"),"int64 unsafe numeric rejected");
 check(match(response(-0.0),"0"),"legacy zero");
 State s;check(!s.propertyRepliesBlocked(),"confirmed property context open");
 s.invalidateModelContext();check(s.propertyRepliesBlocked(),"immediate resync/gap property blocking");
 check(s.property_preview_generation_==38&&!s.fields_nonempty&&s.selection_generation_==1,"immediate generation+cache invalidation");
 check(s.context_refresh_required_&&s.scene_invalidated_&&!s.render_pending_&&s.event_generation_==1,"existing model fence retained");
 std::cout << "source_extracted_helpers=true actual_native_gui=false checks="<<checks<<" PASS\nmetadata_numeric_max=9007199254740991 missing_conflicting_revision_rejected=true old_property_generation_invalidated=true\n";return 0;
 } catch(const std::exception& e){std::cerr<<"check "<<checks<<" failed: "<<e.what()<<"\n";return 1;}
}
