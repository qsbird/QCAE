#include <QtCore/QHash>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <thread>
#include <dlfcn.h>
struct Key { std::uint32_t glyph; struct { std::uint32_t x, y; } subPixelPosition;
friend bool operator==(const Key& a,const Key& b) noexcept{return a.glyph==b.glyph && a.subPixelPosition.x==b.subPixelPosition.x && a.subPixelPosition.y==b.subPixelPosition.y;} };
std::size_t qHash(const Key& k,std::size_t seed) noexcept {return qHashMulti(seed,k.glyph,k.subPixelPosition.x,k.subPixelPosition.y);}
std::atomic<unsigned> total{},entries{},complete{},unknown{},nodes{};
bool throwing{},reenter{},entered{};
QHash<Key,void*> nested;
void collect(void*,const char* site,unsigned kind,std::uint64_t) {
 ++total;
 if(throwing) throw std::bad_alloc{};
 if(std::strcmp(site,"ft_hash/glyph_operation")==0)++entries;
 if(std::strcmp(site,"ft_hash/glyph_operation_complete")==0){if(kind==0)++complete;else ++unknown;}
 if(std::strcmp(site,"ft_hash/node_create_move")==0){
  ++nodes;
  if(reenter && !entered){entered=true;QcaeFtHash::Scope child(1);nested.insert(Key{7,{0,0}},reinterpret_cast<void*>(2));child.finish();}
 }
}
int main(int argc,char**argv){
 if(argc!=2)return 2;
 Dl_info image{};if(!dladdr(reinterpret_cast<void*>(&qcae_qt_sdk_observer_emit),&image))return 2;std::printf("SDK_image=%s\n",image.dli_fname);
 qcae_qt_sdk_observer_install(collect,nullptr);
 if(std::strcmp(argv[1],"exception")==0){
  throwing=true;bool caught=false;
  try{QcaeQtSdkScope scope("first_callback");}catch(const std::bad_alloc&){caught=true;}
  const auto first=total.load();throwing=false;
  qcae_qt_sdk_observer_emit("after_caught_exception",0,0);
  qcae_qt_sdk_observer_install(collect,nullptr);
  qcae_qt_sdk_observer_emit("after_reinstall",0,0);
  std::printf("caught=%d callbacks_before=%u callbacks_after=%u reinstall_did_not_recover=%d\n",caught,first,total.load(),total.load()==first);
  qcae_qt_sdk_observer_install(nullptr,nullptr);
  return caught && total.load()==3 ? 0:1;
 }
 if(std::strcmp(argv[1],"reentry")==0){
  reenter=true;QHash<Key,void*> original;
  {QcaeFtHash::Scope outer(1);original.insert(Key{1,{0,0}},reinterpret_cast<void*>(1));outer.finish();}
  const bool good=original.size()==1 && nested.size()==1 && QcaeFtHash::active==nullptr && entries==1 && complete==1 && unknown==0 && nodes==1;
  std::printf("original=%lld nested=%lld active_restored=%d entries=%u complete=%u unknown=%u node_callbacks=%u\n",static_cast<long long>(original.size()),static_cast<long long>(nested.size()),QcaeFtHash::active==nullptr,entries.load(),complete.load(),unknown.load(),nodes.load());
  qcae_qt_sdk_observer_install(nullptr,nullptr);return good?0:1;
 }
 if(std::strcmp(argv[1],"threads")==0){
  std::atomic<bool> preserved{true};
  const auto work=[&]{QHash<Key,void*> original;for(unsigned i=0;i<100;++i){QcaeFtHash::Scope scope(1);original.insert(Key{i,{0,0}},reinterpret_cast<void*>(std::uintptr_t(i)+1));scope.finish();}if(original.size()!=100 || QcaeFtHash::active!=nullptr)preserved=false;};
  std::thread a(work),b(work);a.join();b.join();
  std::printf("preserved=%d entries=%u complete=%u unknown=%u node_callbacks=%u\n",preserved.load(),entries.load(),complete.load(),unknown.load(),nodes.load());
  qcae_qt_sdk_observer_install(nullptr,nullptr);return preserved && entries==200 && complete==200 && unknown==0 && nodes==200?0:1;
 }
 return 2;
}
