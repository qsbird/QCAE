#include "qcae/static_validation.hpp"
#include "qcae/nastran_codec.hpp"
#include "qcae/records.hpp"
#include "qcae/records_model_bridge.hpp"
#include "decimal_reference.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>

using namespace qcae;
namespace a=features::analysis;
namespace r=features::results;
void demand(bool value,const char* text) {if(!value)throw std::runtime_error(text);}
template<class T>T good(Result<T> value){demand(value.ok(),value.error?value.error->message.c_str():"no result");return std::move(*value.value);}
a::FrozenAnalysisInput load_original(){
 NastranCodec codec; ImportRequest request;
 request.root_resource="cantilever.bdf"; request.source_model_id="independent-reference-original";
 request.source_profile=codec.definition().reference;request.unit_system="mm-N-MPa";
 for(const auto name:{"cantilever.bdf","nodes.bdf","properties.bdf","beams.bdf"}){
  std::ifstream stream(std::string("/Users/qs/Documents/ChatGPT/QCAE/tests/fixtures/nastran-real-benchmark-v3/")+name);
  demand(stream.good(),"source missing");request.resources.push_back({name,{std::istreambuf_iterator<char>(stream),{}}});
 }
 auto decoded=codec.decode(request);demand(decoded.candidate&&decoded.report.complete&&decoded.report.issues.empty(),"original codec rejected fixture");
 const auto& model=*decoded.candidate;demand(model.analyses.size()==1,"one original analysis");
 auto out=codec.encode(model,model.analyses.front().id,request.source_profile);
 demand(out.artifact&&out.report.complete&&!std::any_of(out.report.issues.begin(),out.report.issues.end(),[](const auto& issue){return issue.blocking;}),"cannot export original");
 auto view=records_from_model(model,make_record_registry(),{{DocumentId("independent-original"),DocumentEpoch("original-epoch")},71});
 return good(a::freeze_analysis_input(view,model.analyses.front().id,request.source_profile,out.artifact->identities));
}

#include "solver_validation_store.hpp"
#include "qcae/operation_registry.hpp"
#include "solver_result_store.hpp"
#include "qcae/solver_run_record.hpp"
#include "qcae/artifacts_local.hpp"
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <sstream>
#include "summary_source_fragment.hpp"
using namespace qcae::ipc;
namespace d=qcae::ipc::detail;
StoredSolverResult make_stored(unsigned escaped) {
 auto input=load_original(); SolverOwnedRun origin;origin.principal="independent-format-review";origin.idempotency_key="test-start";origin.test_only=true;origin.startup_intent_persisted=true;origin.source_input=input;
 auto& run=origin.run;run.request.task_id=escaped ? std::string(escaped,'\x01') : "independent-review-task";run.request.run_id="run-"+run.request.task_id;run.request.run_directory="/private/tmp/not-an-executed-solver/"+run.request.run_id;
 run.request.input={input.version.document.id,input.version.document.epoch,input.version.revision,input.analysis,input.target.profile,"independent-input-artifact",artifact_sha256("synthetic manifest"),artifact_sha256(input.input_signature),solver_export_identity_digest(input),"independent-export","qcae.nastran.static-f06.v1",a::encode_frozen_analysis_input(input)};
 run.request.configuration={"independent-format-review","/not-executed/test-only",{},"test-only","test-only-process","test-only-1","no OS execution observation",artifact_sha256("synthetic config"),{"result.f06"}};
 origin.signature=solver_run_signature(input.analysis,run.request.input.artifact_id,run.request.configuration.id,run.request.configuration.configuration_digest);run.sequence=3;run.execution=SolverExecutionState::exited;run.process=SolverProcessIdentity{1,"explicit-not-executed"};run.exit_code=0;run.output_state=SolverOutputState::collected;origin.result_reader=SolverResultReaderConfiguration{};origin.result_reader->resource="result.f06";
 std::ostringstream text;text.precision(17);text<<"1 INDEPENDENT SYNTHETIC REVIEW PAGE 1\nSUBCASE 1\nD I S P L A C E M E N T V E C T O R\nPOINT ID. TYPE T1 T2 T3 R1 R2 R3\n";
 const auto rows=record_wire::read_strings(input.input_signature);std::uint64_t support{};
 for(std::size_t i=8;i<rows.size();++i){auto row=record_wire::read_strings(rows[i]);if(row[0]!="GRID")continue;auto x=record_wire::read_vector3(row[2])[0];auto index=static_cast<std::size_t>(x/50);auto found=std::find_if(input.identities.begin(),input.identities.end(),[&](const auto& id){return id.name_space=="GRID"&&id.entity.value==row[1];});demand(found!=input.identities.end()&&index<21,"map");text<<found->number<<" G";for(auto value:reference_rows[index].value)text<<' '<<value;text<<'\n';if(index==0)support=found->number;}
 text<<"F O R C E S O F S I N G L E - P O I N T C O N S T R A I N T\nPOINT ID. TYPE T1 T2 T3 R1 R2 R3\n"<<support<<" G";for(auto value:reaction_values)text<<' '<<value;text<<'\n';
 std::vector<TextResource> resources{{"result.f06",text.str()},{"runner.stdout","independent synthetic review\n"},{"runner.stderr",""}};for(const auto& file:resources)run.outputs.push_back({file.path,file.text.size(),artifact_sha256(file.text)});
 auto parsed=good(d::prepare_solver_result(origin,*origin.result_reader,resources));auto artifact=d::solver_result_artifact(parsed,std::filesystem::path(run.request.run_directory)/".qcae-result");return {std::move(parsed),std::move(artifact),true};
}
QJsonValue json_value(const operations::Value& value){return std::visit([](const auto& item)->QJsonValue{using T=std::decay_t<decltype(item)>;if constexpr(std::is_same_v<T,std::string>)return QString::fromUtf8(item);else if constexpr(std::is_same_v<T,std::int64_t>)return QString::number(item);else if constexpr(std::is_same_v<T,operations::Value::Object>){QJsonObject out;for(const auto& [key,v]:item)out.insert(QString::fromUtf8(key),json_value(v));return out;}else if constexpr(std::is_same_v<T,operations::Value::Array>){QJsonArray out;for(const auto& v:item)out.append(json_value(v));return out;}else return item;},value.data);}
#include "project_mutator.hpp"
int main(int argc,char** argv){try{
 if(argc==3){rewrite_project(argv[1],argv[2]);return 0;}
 const auto profile=NastranCodec{}.definition().reference;unsigned linked{};
 for(const unsigned escaped:{0U,110U,121U}){auto stored=make_stored(escaped);qcae::ipc::ValidationHistory history;for(unsigned n=1;n<=16;++n){auto fact=good(d::prepare_solver_validation(stored,profile));fact.idempotency_key="key-"+std::to_string(n);fact.ordinal=n;fact.signature=record_wire::strings(std::array<std::string,8>{"independent-request-document","independent-request-epoch","72",fact.run_id,fact.principal,profile.profile_id,profile.profile_version,profile.definition_digest});auto image=d::solver_validation_row(fact);auto decoded=d::decode_solver_validation_row(*image);d::validate_solver_validation_link(decoded,stored,profile);++linked;history.reports.emplace_back(std::move(decoded),image);}
  auto result=qcae::ipc::numerical_checks(history,true,true);const auto object=json_value(result).toObject();const auto immutable=object.value("immutable_history").toArray();auto json=QJsonDocument(immutable).toJson(QJsonDocument::Compact);operations::Value::Array summaries;for(const auto& entry:history.reports)summaries.push_back(qcae::ipc::validation_summary(entry.first,entry.second->key.identity));auto canonical=good(operations::canonical_value(operations::Value(std::move(summaries))));
  std::ofstream("/private/tmp/qcae-numerical-store-review/summary-"+std::to_string(escaped)+".json", std::ios::binary).write(json.constData(), json.size());
  std::cout<<"escaped="<<escaped<<" linked="<<history.reports.size()<<" canonical_bytes="<<canonical.size()<<" json_bytes="<<json.size()<<" reported_overflow="<<object.value("history_overflow").toBool()<<" has_applicable="<<object.value("has_applicable_validation").toBool()<<" source_kind="<<object.value("latest_applicable_summary").toObject().value("source_kind").toString().toStdString()<<'\n';
 }
 std::cout<<"private_format_summary_probe=true linked_canonical_facts="<<linked<<" common_release=false actual_engine=false real_solver=false numerical_acceptance=false\n";return 0;}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
