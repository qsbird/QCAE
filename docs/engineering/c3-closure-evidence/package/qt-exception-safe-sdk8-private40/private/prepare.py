from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import difflib,hashlib,json,re,shutil,subprocess
ROOT=Path('/Users/qs/Documents/ChatGPT/QCAE')
OLD=ROOT/'build-c3-qt-observed/tranche7'
NEW=ROOT/'build-c3-qt-observed/tranche8b-private'
BUILD=ROOT/'build-c3-qt-observed/tranche2/build'
CORE='src/corelib/global/qglobal.cpp'
FT='src/gui/text/freetype/qfontengine_ft.cpp'
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def run(argv,log):
 p=subprocess.run(argv,cwd=BUILD,text=True,capture_output=True)
 log.write_text('command: '+json.dumps(argv)+'\n'+p.stdout+p.stderr+'\nexit_code: '+str(p.returncode)+'\n')
 if p.returncode:raise RuntimeError(str(log))
 return p
if (NEW/'qt-prefix').exists():raise RuntimeError('Existing SDK8 must not be overwritten')
watch=[OLD/'qt-prefix/lib/QtCore.framework/Versions/A/QtCore',OLD/'qt-prefix/lib/QtGui.framework/Versions/A/QtGui',OLD/'qcae-ft-hash-observer.hpp',OLD/'qt-observer-manifest.json',OLD/'object-reuse.json',OLD/'qt-ft-hash-sources'/CORE,ROOT/'docs/engineering/c3-closure-evidence/package/qt-freetype-tranche7-independent-review/private-probe/exception.log']
before={str(p):sha(p) for p in watch}
header_old=ROOT/'build-c3-qt-observed/tranche2/qtbase-everywhere-src-6.11.1/qcae-sdk-observer.hpp'
h = '#pragma once\n#include "'+str(header_old)+'"\n' + r'''
#if !defined(QT_BOOTSTRAPPED)
extern "C" {
Q_CORE_EXPORT std::uint64_t qcae_qt_sdk_observer_status() noexcept;
Q_CORE_EXPORT unsigned qcae_qt_sdk_observer_depth() noexcept;
Q_CORE_EXPORT void qcae_qt_sdk_observer_collector_loss() noexcept;
}
#else
inline std::uint64_t qcae_qt_sdk_observer_status() noexcept { return 0; }
inline unsigned qcae_qt_sdk_observer_depth() noexcept { return 0; }
inline void qcae_qt_sdk_observer_collector_loss() noexcept {}
#endif
// The original C declarations and legacy scope layout are unchanged.
// SDK8's implementations fence every callback; these entry/exit wrappers are
// explicitly noexcept without conflicting with reused SDK7 declarations.
struct QcaeQtSdkNoexceptScope {
    explicit QcaeQtSdkNoexceptScope(const char *site) noexcept { qcae_qt_sdk_observer_enter(site); }
    ~QcaeQtSdkNoexceptScope() noexcept { qcae_qt_sdk_observer_leave(); }
    QcaeQtSdkNoexceptScope(const QcaeQtSdkNoexceptScope &) = delete;
    QcaeQtSdkNoexceptScope &operator=(const QcaeQtSdkNoexceptScope &) = delete;
};
'''
header=NEW/'qcae-sdk-observer.hpp';header.write_text(h)
body=r'''#if !defined(QT_BOOTSTRAPPED)
#include <atomic>
namespace {
std::atomic<QcaeQtSdkObserver> qcae_qt_sdk_callback{};
std::atomic<void *> qcae_qt_sdk_context{};
// Sticky process status: callback exception=1, admission-capacity loss=2,
// collector allocation/record loss=4, malformed callback metadata=8,
// unbalanced leave=16. No strings or allocation are required to preserve loss.
std::atomic<std::uint64_t> qcae_qt_sdk_loss{};
thread_local unsigned qcae_qt_sdk_depth{};
thread_local bool qcae_qt_sdk_dispatching{};
thread_local bool qcae_qt_sdk_admitted[1024]{};
thread_local unsigned qcae_qt_sdk_frames{};
thread_local std::uint64_t qcae_qt_sdk_overflow_frames{};
struct QcaeDispatchGuard {
    QcaeDispatchGuard() noexcept { qcae_qt_sdk_dispatching = true; }
    ~QcaeDispatchGuard() noexcept { qcae_qt_sdk_dispatching = false; }
};
bool qcae_dispatch(const char *site, unsigned kind, std::uint64_t bytes) noexcept {
    if (!site || kind > 3) {
        qcae_qt_sdk_loss.fetch_or(8, std::memory_order_relaxed);
        return false;
    }
    if (qcae_qt_sdk_dispatching)
        return true; // Observer-induced nested work stays unobserved.
    if (auto callback = qcae_qt_sdk_callback.load(std::memory_order_acquire)) {
        QcaeDispatchGuard guard;
        try {
            callback(qcae_qt_sdk_context.load(std::memory_order_acquire), site, kind, bytes);
        } catch (...) {
            qcae_qt_sdk_loss.fetch_or(1, std::memory_order_relaxed);
            return false;
        }
    }
    return true;
}
}
extern "C" Q_CORE_EXPORT void qcae_qt_sdk_observer_install(QcaeQtSdkObserver callback, void *context) {
    // Installation and callback/context destruction remain outside active calls.
    // Reinstallation intentionally does not erase sticky loss.
    qcae_qt_sdk_callback.store(nullptr, std::memory_order_release);
    qcae_qt_sdk_context.store(context, std::memory_order_release);
    qcae_qt_sdk_callback.store(callback, std::memory_order_release);
}
extern "C" Q_CORE_EXPORT const char *qcae_qt_sdk_observer_manifest() {
    return QCAE_QT_SDK_MANIFEST;
}
extern "C" Q_CORE_EXPORT std::uint64_t qcae_qt_sdk_observer_status() noexcept {
    return qcae_qt_sdk_loss.load(std::memory_order_relaxed);
}
extern "C" Q_CORE_EXPORT unsigned qcae_qt_sdk_observer_depth() noexcept {
    return qcae_qt_sdk_depth;
}
extern "C" Q_CORE_EXPORT void qcae_qt_sdk_observer_collector_loss() noexcept {
    qcae_qt_sdk_loss.fetch_or(4, std::memory_order_relaxed);
}
extern "C" Q_CORE_EXPORT void qcae_qt_sdk_observer_emit(const char *site, unsigned kind, std::uint64_t bytes) {
    if (qcae_qt_sdk_depth)
        (void)qcae_dispatch(site, kind, bytes);
}
extern "C" Q_CORE_EXPORT void qcae_qt_sdk_observer_enter(const char *site) {
    if (qcae_qt_sdk_frames == 1024 || qcae_qt_sdk_overflow_frames) {
        ++qcae_qt_sdk_overflow_frames;
        qcae_qt_sdk_loss.fetch_or(2, std::memory_order_relaxed);
        return;
    }
    const auto frame = qcae_qt_sdk_frames++;
    qcae_qt_sdk_admitted[frame] = false;
    ++qcae_qt_sdk_depth;
    if (qcae_dispatch(site, 0, 0))
        qcae_qt_sdk_admitted[frame] = true;
    else
        --qcae_qt_sdk_depth; // Failed admission still has a balanced leave frame.
}
extern "C" Q_CORE_EXPORT void qcae_qt_sdk_observer_leave() {
    if (qcae_qt_sdk_overflow_frames) {
        --qcae_qt_sdk_overflow_frames;
        return;
    }
    if (!qcae_qt_sdk_frames) {
        qcae_qt_sdk_loss.fetch_or(16, std::memory_order_relaxed);
        return;
    }
    if (qcae_qt_sdk_admitted[--qcae_qt_sdk_frames])
        --qcae_qt_sdk_depth;
}
#endif
'''
source=NEW/'qt-ft-hash-sources';shutil.copytree(OLD/'qt-ft-hash-sources',source)
shadow=NEW/'qt-headers';shutil.copytree(OLD/'qt-headers',shadow)
helper_old=OLD/'qcae-ft-hash-observer.hpp';helper=NEW/'qcae-ft-hash-observer.hpp'
helper_text=helper_old.read_text().replace(str(header_old),str(header)).replace('QcaeQtSdkScope sdk;', 'QcaeQtSdkNoexceptScope sdk;')
helper_text=helper_text.replace('    ~Scope() {','    ~Scope() noexcept {\n        struct Restore { Scope *previous; ~Restore() noexcept { active = previous; } } restore{previous};\n        if (qcae_qt_sdk_observer_status()) unknown = true;')
helper_text=helper_text.replace('        active = previous;\n','')
helper_text=helper_text.replace('    void markUnknown(const char *site) {','    void markUnknown(const char *site) noexcept {')
helper_text=helper_text.replace('    void finish() {','    void finish() noexcept {')
helper.write_text(helper_text)
for p in (shadow/'QtCore/qhash.h',source/FT):
 p.write_text(p.read_text().replace(str(helper_old),str(helper)))
manifest=json.loads((OLD/'qt-observer-manifest.json').read_text())
manifest.update(tranche=8,sdk_prefix=str(NEW/'qt-prefix'),previous_prefix_modified=False,coverage_complete=False)
manifest['bridge_header_sha256']=sha(header)
manifest['bridge_implementation_sha256']=hashlib.sha256(body.encode()).hexdigest()
manifest['observer_failure_fence']={'version':1,'callback_abi_unchanged':True,'failure_is_unknown':True,'sticky_status_symbol':'qcae_qt_sdk_observer_status','noexcept_callback_boundary':True,'balanced_admission_stack_capacity':1024,'original_C_declarations_and_scope_layout_unchanged':True,'concurrent_install_or_context_destruction_during_callbacks':'unsupported; installation/destruction must remain outside callbacks','counter_unbounded_stream':'not validated; all finite probes retain overflow unknown'}
core_old=(OLD/'qt-ft-hash-sources'/CORE).read_text()
start=core_old.index('#if !defined(QT_BOOTSTRAPPED)',core_old.index('#define QCAE_QT_SDK_MANIFEST'))
core=core_old[:start]+body
core=core.replace(str(header_old),str(header))
a=core.index('#define QCAE_QT_SDK_MANIFEST ');b=core.index('\n',a)
compact=json.dumps(manifest,sort_keys=True,separators=(',',':'))
core=core[:a]+'#define QCAE_QT_SDK_MANIFEST '+json.dumps(compact)+core[b:]
(source/CORE).write_text(core)
(NEW/'qt-observer-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
(NEW/'observer-header.diff').write_text(''.join(difflib.unified_diff(header_old.read_text().splitlines(True),h.splitlines(True),fromfile='SDK7 shared observer header (read only)',tofile='SDK8 private observer header')))
(NEW/'observer-body.diff').write_text(''.join(difflib.unified_diff(core_old[start:].splitlines(True),body.splitlines(True),fromfile='SDK7 bridge implementation',tofile='SDK8 fenced bridge implementation')))
(NEW/'hash-helper.diff').write_text(''.join(difflib.unified_diff(helper_old.read_text().splitlines(True),helper_text.splitlines(True),fromfile='SDK7 hash helper',tofile='SDK8 private hash helper')))
prior=json.loads((OLD/'object-reuse.json').read_text())
objects=NEW/'qt-objects';objects.mkdir()
commands={};replacements={}
for rel in (CORE,FT):
 args=list(prior['actual_compile_commands'][rel]);old_obj=args[args.index('-o')+1]
 obj=objects/(Path(rel).name+'.o')
 args[args.index('-o')+1]=str(obj)
 args[args.index('-c')+1]=str(source/rel)
 args=[x.replace(str(OLD/'qt-headers'),str(shadow)) for x in args]
 if '-MF' in args:args[args.index('-MF')+1]=str(obj)+'.d'
 commands[rel]=args;replacements[old_obj]=str(obj)
with ThreadPoolExecutor(max_workers=2) as pool:
 futures=[pool.submit(run,args,NEW/('compile-'+Path(rel).name+'.log')) for rel,args in commands.items()]
 for f in futures:f.result()
prefix=NEW/'qt-prefix';shutil.copytree(OLD/'qt-prefix',prefix,symlinks=True)
for suffix in ('.cmake','.pc','.prl','.pri'):
 for p in prefix.rglob('*'+suffix):
  s=p.read_text();t=s.replace(str(OLD/'qt-prefix'),str(prefix))
  if t!=s:p.write_text(t)
links={};reuse={}
for module,args0 in prior['actual_link_commands'].items():
 args=list(args0);image=prefix/f'lib/Qt{module}.framework/Versions/A/Qt{module}'
 args[args.index('-o')+1]=str(image)
 for i,x in enumerate(args):
  if x in replacements:args[i]=replacements[x]
  elif x.endswith(('.o','.a')):
   p=Path(x);p=(p if p.is_absolute() else BUILD/p).resolve();reuse[str(p)]=sha(p)
  elif x.endswith('libharfbuzz.dylib'):args[i]=str(prefix/'lib/libharfbuzz.dylib')
 run(args,NEW/('link-'+module+'.log'));links[module]=args
 details=subprocess.check_output(['otool','-l',str(image)],text=True)
 rpaths=re.findall(r'cmd LC_RPATH\n.*?\n\s*path (.*?) \(offset',details)
 for r in rpaths:
  if r.startswith(str(ROOT/'build-c3-qt-observed')) or r=='/opt/homebrew/lib':
   run(['install_name_tool','-delete_rpath',r,str(image)],NEW/('bind-'+module+'-'+hashlib.sha256(r.encode()).hexdigest()[:8]+'.log'))
 if '@loader_path/../../..' not in rpaths:run(['install_name_tool','-add_rpath','@loader_path/../../..',str(image)],NEW/('bind-'+module+'-local.log'))
after={str(p):sha(p) for p in watch}
assert before==after,'Old frozen SDK/evidence changed'
report={'schema':'qcae.sdk8-private-build/1','old_frozen_before':before,'old_frozen_after':after,'old_frozen_unchanged':True,'compile_commands':commands,'link_commands':links,'reused_objects':reuse,'reused_object_count':len(reuse),'source_header_sha256':sha(header),'source_implementation_sha256':sha(source/CORE),'manifest_sha256':sha(NEW/'qt-observer-manifest.json'),'new_images':{m:{'path':str(prefix/f'lib/Qt{m}.framework/Versions/A/Qt{m}'),'sha256':sha(prefix/f'lib/Qt{m}.framework/Versions/A/Qt{m}')} for m in ('Core','Gui')},'coverage_complete':False,'whole_pipeline_owned_copy_coverage':'unknown','native_gui_run':False,'shared_product_source_changed':False}
(NEW/'build-report.json').write_text(json.dumps(report,indent=2)+'\n')
print('SDK8 actual Core+FT compile/link GREEN; reused',len(reuse),'objects; old SDK7 frozen hashes unchanged')
