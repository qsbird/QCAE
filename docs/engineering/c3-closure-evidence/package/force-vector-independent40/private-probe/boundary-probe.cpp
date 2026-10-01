#define main original_analysis_checks_main
#include "/private/tmp/qcae-force-vector-independent40/analysis-check-fixture.cpp"
#undef main
int main(){try{
 Fixture fixture;const auto data=create_case(fixture);unsigned count{};
 const auto probe=[&](bool pass,const char*message){check(pass,message);++count;};
 const ForceSetVectorInput kn{data.force,{-0.0,"N"},{-2,"kN"},{0,"N"}};
 const ForceSetVectorInput n{data.force,{0,"N"},{-2000,"N"},{0,"N"}};
 const auto first=good(analysis_features::prepare_set_force_vector(kn));
 const auto equivalent=good(analysis_features::prepare_set_force_vector(n));
 probe(first.signature==equivalent.signature,"Equivalent force units and signed zero changed normalized signature");
 const auto context=fixture.context("same-normalized-input");
 const auto receipt=good(fixture.invoke(kn,context));const auto committed=good(fixture.app.current_document());
 const auto replay=good(fixture.invoke(n,context));
 const auto& original=std::get<Value::Object>(receipt.data);const auto& repeated=std::get<Value::Object>(replay.data);
 probe(std::get<bool>(repeated.at("replayed").data)&&original.at("transaction_id")==repeated.at("transaction_id"),"Unit-equivalent retry did not return original transaction");
 probe(good(fixture.app.current_document()).revision==committed.revision,"Unit-equivalent retry made another revision");
 good(fixture.app.undo(caller,at(committed),"undo-equivalent"));const auto undone=good(fixture.app.current_document());
 const auto undone_fields=fixture.view().find<records::NodalForce>(data.force)->get<records::NodalForce>();
 good(fixture.invoke(n,context));
 probe(good(fixture.app.current_document()).revision==undone.revision&&fixture.view().find<records::NodalForce>(data.force)->get<records::NodalForce>().force_n==undone_fields.force_n,"Equivalent retry after undo reapplied edit");
 const auto preview_context=fixture.context("");
 const ForcePreviewVectorInput preview{data.force,{1,"kN"},{-1,"N"},{2,"N"}};
 const auto handle=good(fixture.invoke(preview,preview_context));
 const auto& fields=std::get<Value::Object>(handle.data);const PreviewId id(std::get<std::string>(fields.at("preview_id").data));
 probe(!std::get<bool>(fields.at("creates_entity").data)&&std::get<std::string>(fields.at("affected_entity_id").data)==data.force.value,"Preview lost existing force identity");
 const auto before=good(fixture.app.current_document());const auto history=good(fixture.app.history(before.document)).items.size();const auto history_cursor=good(fixture.app.history(before.document)).cursor;
 const auto alien=fixture.app.commit(Caller{"another-actor"},at(before),id,"alien-commit");
 probe(!alien.ok()&&alien.error->code==ErrorCode::preview_expired,"Another actor committed force preview");
 probe(good(fixture.app.current_document()).revision==before.revision&&good(fixture.app.history(before.document)).items.size()==history,"Rejected actor preview changed shared state");
 auto missing=preview;missing.y.unit="";const auto missing_result=fixture.invoke(missing,preview_context);
 probe(!missing_result.ok()&&missing_result.error->code==ErrorCode::missing_input&&missing_result.error->field=="input.y.unit","Preview missing unit diagnostic differs from force contract");
 auto wrong=preview;wrong.force_id=EntityId("material");const auto wrong_result=fixture.invoke(wrong,preview_context);
 probe(!wrong_result.ok()&&wrong_result.error->code==ErrorCode::entity_not_found,"Preview accepted wrong-kind force ID");
 good(fixture.app.commit(caller,at(before),id,"own-preview-commit"));
 const auto force=fixture.view().find<records::NodalForce>(data.force)->get<records::NodalForce>();
 probe(force.node==undone_fields.node&&force.id==data.force&&force.force_n==std::array<double,3>{1000,-1,2},"Preview commit lost normalization, identity or node reference");
 const auto after=good(fixture.app.current_document());
 const auto old=fixture.invoke(preview,preview_context);
 probe(!old.ok()&&old.error->code==ErrorCode::revision_conflict&&good(fixture.app.current_document()).revision==after.revision,"New stale preview was accepted");
 probe(good(fixture.app.history(after.document)).cursor==history_cursor+1 && after.revision==before.revision+1,"Preview failed to share exactly one history entry");
 std::cout<<"PASS "<<count<<" independent force normalization/replay/preview-authority checks; no GUI/solver\n";return 0;
 }catch(const std::exception&error){std::cerr<<error.what()<<'\n';return 1;}}
