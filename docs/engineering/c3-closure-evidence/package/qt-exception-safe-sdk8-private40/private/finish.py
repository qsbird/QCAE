from pathlib import Path
import difflib,hashlib,json,re,subprocess
ROOT=Path('/Users/qs/Documents/ChatGPT/QCAE');OLD=ROOT/'build-c3-qt-observed/tranche7';NEW=ROOT/'build-c3-qt-observed/tranche8b-private';BUILD=ROOT/'build-c3-qt-observed/tranche2/build'
CORE='src/corelib/global/qglobal.cpp';FT='src/gui/text/freetype/qfontengine_ft.cpp'
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def run(args,name):
 p=subprocess.run(args,cwd=BUILD,capture_output=True,text=True);(NEW/name).write_text('command: '+json.dumps(args)+'\n'+p.stdout+p.stderr+'\nexit_code: '+str(p.returncode)+'\n')
 if p.returncode:raise RuntimeError(str(NEW/name))
core=NEW/'qt-ft-hash-sources'/CORE;s=core.read_text();before=s
s=s.replace('    if (qcae_qt_sdk_depth)\n        (void)qcae_dispatch', '    if (qcae_qt_sdk_depth && !qcae_qt_sdk_overflow_frames && qcae_qt_sdk_frames\n        && qcae_qt_sdk_admitted[qcae_qt_sdk_frames - 1])\n        (void)qcae_dispatch')
manifest=json.loads((NEW/'qt-observer-manifest.json').read_text());start=s.index('#if !defined(QT_BOOTSTRAPPED)',s.index('#define QCAE_QT_SDK_MANIFEST'));manifest['bridge_implementation_sha256']=hashlib.sha256(s[start:].encode()).hexdigest();a=s.index('#define QCAE_QT_SDK_MANIFEST ');b=s.index('\n',a);s=s[:a]+'#define QCAE_QT_SDK_MANIFEST '+json.dumps(json.dumps(manifest,sort_keys=True,separators=(',',':')))+s[b:];core.write_text(s);(NEW/'qt-observer-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
(NEW/'admission-frame-gate.diff').write_text(''.join(difflib.unified_diff(before[start:].splitlines(True),s[s.index('#if !defined(QT_BOOTSTRAPPED)',s.index('#define QCAE_QT_SDK_MANIFEST')):].splitlines(True),fromfile='SDK8b initial bridge',tofile='SDK8b final admitted-frame gate')))
prior=json.loads((OLD/'object-reuse.json').read_text());commands={};replace={}
for rel in (CORE,FT):
 old=prior['actual_compile_commands'][rel];old_obj=old[old.index('-o')+1]
 if rel==CORE:
  args=json.loads((NEW/'compile-qglobal.cpp.log').read_text().splitlines()[0][9:]);args[args.index('-o')+1]=str(NEW/'qt-objects/qglobal-final.cpp.o');args[args.index('-MF')+1]=str(NEW/'qt-objects/qglobal-final.cpp.o.d');run(args,'compile-qglobal-final.cpp.log')
 else:args=json.loads((NEW/'compile-qfontengine_ft.cpp.log').read_text().splitlines()[0][9:])
 commands[rel]=args;replace[old_obj]=args[args.index('-o')+1]
prefix=NEW/'qt-prefix';links={};reuse={}
for module,args0 in prior['actual_link_commands'].items():
 args=list(args0);image=prefix/f'lib/Qt{module}.framework/Versions/A/Qt{module}';args[args.index('-o')+1]=str(image)
 for i,x in enumerate(args):
  if x in replace:args[i]=replace[x]
  elif x.endswith('QtCore.framework/Versions/A/QtCore') and not x.startswith('@rpath'):
   args[i]=str(prefix/'lib/QtCore.framework/Versions/A/QtCore')
  elif x.endswith(('.o','.a')):
   p=Path(x);p=(p if p.is_absolute() else BUILD/p).resolve();reuse[str(p)]=sha(p)
  elif x.endswith('libharfbuzz.dylib'):args[i]=str(prefix/'lib/libharfbuzz.dylib')
 run(args,'link-'+module+'-final.log');links[module]=args
 details=subprocess.check_output(['otool','-l',str(image)],text=True);rpaths=re.findall(r'cmd LC_RPATH\n.*?\n\s*path (.*?) \(offset',details)
 for r in rpaths:
  if r.startswith(str(ROOT/'build-c3-qt-observed')) or r=='/opt/homebrew/lib':run(['install_name_tool','-delete_rpath',r,str(image)],'bind-'+module+'-final-'+hashlib.sha256(r.encode()).hexdigest()[:8]+'.log')
 if '@loader_path/../../..' not in rpaths:run(['install_name_tool','-add_rpath','@loader_path/../../..',str(image)],'bind-'+module+'-final-local.log')
watch=[OLD/'qt-prefix/lib/QtCore.framework/Versions/A/QtCore',OLD/'qt-prefix/lib/QtGui.framework/Versions/A/QtGui',OLD/'qcae-ft-hash-observer.hpp',OLD/'qt-observer-manifest.json',OLD/'object-reuse.json',OLD/'qt-ft-hash-sources'/CORE,ROOT/'docs/engineering/c3-closure-evidence/package/qt-freetype-tranche7-independent-review/private-probe/exception.log']
bound={str(p):sha(p) for p in watch}
report={'schema':'qcae.sdk8-private-build/1','old_frozen_current_sha256':bound,'compile_commands':commands,'link_commands':links,'reused_objects':reuse,'reused_object_count':len(reuse),'source_header_sha256':sha(NEW/'qcae-sdk-observer.hpp'),'source_implementation_sha256':sha(core),'manifest_sha256':sha(NEW/'qt-observer-manifest.json'),'new_images':{m:{'path':str(prefix/f'lib/Qt{m}.framework/Versions/A/Qt{m}'),'sha256':sha(prefix/f'lib/Qt{m}.framework/Versions/A/Qt{m}')} for m in ('Core','Gui')},'coverage_complete':False,'whole_pipeline_owned_copy_coverage':'unknown','native_gui_run':False,'shared_product_source_changed':False,'first_compile_and_link_failures_retained':True,'original_C_ABI_and_flags_retained':True}
(NEW/'build-report.json').write_text(json.dumps(report,indent=2)+'\n');print('SDK8b final actual compile/link GREEN; new local Core dependency; reused',len(reuse),'objects')
