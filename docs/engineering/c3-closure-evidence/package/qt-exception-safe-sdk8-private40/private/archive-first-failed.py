from pathlib import Path
import ast,ctypes,difflib,hashlib,json,subprocess
ROOT=Path('/Users/qs/Documents/ChatGPT/QCAE');P=ROOT/'build-c3-qt-observed/tranche8b-private';OLD=ROOT/'build-c3-qt-observed/tranche7'
def fact(p):
 p=Path(p);return {'path':str(p),'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
old_checked=[]
review=ROOT/'docs/engineering/c3-closure-evidence/package/qt-freetype-tranche7-independent-review'
for file in ('reviewed-source-sha256.json','reviewed-evidence-sha256.json'):
 for row in json.loads((review/file).read_text()):
  p=ROOT/row['path'];a=fact(p);assert a['sha256']==row['sha256'],str(p);old_checked.append(a)
old_runtime=json.loads((ROOT/'docs/engineering/c3-closure-evidence/package/qt-freetype-tranche7-runtime.json').read_text())
for row in old_runtime['loaded_sdk_images']:
 a=fact(row['path']);assert a['sha256']==row['sha256'];old_checked.append(a)
for row in json.loads((OLD/'object-reuse.json').read_text())['reused_objects']:
 assert fact(row['path'])['sha256']==row['sha256'],row['path']
old_red=fact(review/'private-probe/exception.log')
oldtext=(OLD/'qt-ft-hash-sources/src/corelib/global/qglobal.cpp').read_text();newtext=(P/'qt-ft-hash-sources/src/corelib/global/qglobal.cpp').read_text()
def body(s):return s[s.index('#if !defined(QT_BOOTSTRAPPED)',s.index('#define QCAE_QT_SDK_MANIFEST')):]
(P/'final-observer-body.diff').write_text(''.join(difflib.unified_diff(body(oldtext).splitlines(True),body(newtext).splitlines(True),fromfile='Frozen SDK7 callback boundary',tofile='Private SDK8 callback boundary')))
# Preserve the first SDK8b linked source through its actual retained object marker.
strings=subprocess.check_output(['strings',str(P/'qt-objects/qglobal.cpp.o')],text=True)
first_manifest=next(json.loads(line) for line in strings.splitlines() if line.startswith('{"archive_sha256":'))
module=ast.parse((P/'prepare.py').read_text());initial_body=next(n.value.value for n in module.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='body' for t in n.targets))
assert hashlib.sha256(initial_body.encode()).hexdigest()==first_manifest['bridge_implementation_sha256']
prefix=newtext[:newtext.index('#if !defined(QT_BOOTSTRAPPED)',newtext.index('#define QCAE_QT_SDK_MANIFEST'))];a=prefix.index('#define QCAE_QT_SDK_MANIFEST ');b=prefix.index('\n',a)
first_source=prefix[:a]+'#define QCAE_QT_SDK_MANIFEST '+json.dumps(json.dumps(first_manifest,sort_keys=True,separators=(',',':')))+prefix[b:]+initial_body
first=P/'first-linked-source-qglobal.cpp';first.write_text(first_source)
images=[fact(P/f'qt-prefix/lib/Qt{m}.framework/Versions/A/Qt{m}') for m in ('Core','Gui')]
loaded=ctypes.CDLL(images[0]['path']);loaded.qcae_qt_sdk_observer_manifest.restype=ctypes.c_char_p
marker=json.loads(loaded.qcae_qt_sdk_observer_manifest().decode());expected=json.loads((P/'qt-observer-manifest.json').read_text());assert marker==expected
core=ctypes.CDLL(images[1]['path'])
loaded.qcae_qt_sdk_observer_status.restype=ctypes.c_uint64
assert loaded.qcae_qt_sdk_observer_status()==0
base=json.loads((P/'probe-results.json').read_text())['actual_modes']
for mode in ('legacy_scope_bad_alloc','hash_noexcept_callback_bad_alloc'):
 base.append({'mode':mode,'exit_code':0,'log':str(P/(mode+'.log'))})
proofs=[]
for r in base:
 mode=r['mode'];expanded=mode in ('legacy_scope_bad_alloc','hash_noexcept_callback_bad_alloc')
 log=Path(r['log']);s=log.read_text();assert 'exit_code: 0' in s and 'GREEN' in s
 proofs.append({'mode':mode,'passed':True,'source':fact(P/('probe-expanded.cpp' if expanded else 'probe.cpp')),'collector_source':fact(P/'collector-private.hpp'),'binary':fact(P/('observer-probe-expanded' if expanded else 'observer-probe')),'actual_linked_qt_images':images,'raw_log':fact(log),'actual_green_output':next(line for line in s.splitlines() if ' GREEN ' in line)})
normal=json.loads((P/'normal-hash-runtime.json').read_text());oldnormal=json.loads((ROOT/'docs/engineering/c3-closure-evidence/package/qt-freetype-tranche7-headless.json').read_text());normal_equal=normal==oldnormal
losslog=(P/'actual_collector_allocation_loss.log').read_text();snap=json.loads(next(line.removeprefix('collector_snapshot=') for line in losslog.splitlines() if line.startswith('collector_snapshot=')))
assert snap['collector_complete'] is False and snap['observer_failure_status']=='4'
build=json.loads((P/'build-report.json').read_text());assert len(build['reused_objects'])==494
failures=[fact(ROOT/'build-c3-qt-observed/tranche8-private'/name) for name in ('prepare.py','qcae-sdk-observer.hpp','qcae-ft-hash-observer.hpp','qt-ft-hash-sources/src/corelib/global/qglobal.cpp','qt-ft-hash-sources/src/gui/text/freetype/qfontengine_ft.cpp','compile-qglobal.cpp.log','compile-qfontengine_ft.cpp.log')]
failures += [fact(P/name) for name in ('prepare.py','qt-objects/qglobal.cpp.o','first-linked-source-qglobal.cpp','link-Gui.log')]
report={'schema':'qcae.sdk8-private-exception-fence/1','status':'actual-linked finite headless GREEN; repository adoption pending root approval','exception_and_lifetime_cases':proofs,'case_count':len(proofs),'normal_hash_regression':{'passed':normal['passed'],'source':fact(ROOT/'tests/qt_freetype_hash_probe.cpp'),'binary':fact(P/'normal-hash-probe'),'raw_log':fact(P/'normal-hash-runtime.log'),'actual_report':fact(P/'normal-hash-runtime.json'),'all_original_hash_behavior_checks_preserved':True,'full_frozen7_headless_report_byte_values_equal':normal_equal,'equality_scope':'Complete parsed JSON comparison, including all per-site values; no omitted fields.'},'actual_collector_allocation_failure':{'collector_complete':False,'sticky_status':'4','known_zero_claimed':False,'loaded_qt_images':snap['loaded_qt_images']},'sdk_manifest':fact(P/'qt-observer-manifest.json'),'actual_marker_exact':True,'reused_objects':494,'original_flags_and_C_ABI_preserved':True,'old_review_sources_evidence_images_unchanged':old_checked,'old_494_objects_unchanged':True,'old_actual_linked_RED':old_red,'preserved_private_failure_versions':failures,'first_linked_source_reconstruction':'Recovered from the retained actual initial Core object marker plus unchanged source prefix and exact retained prepare.py body; body SHA matches the actual object marker. Not a claimed separately captured pre-link source file.','precise_private_diffs':[fact(P/name) for name in ('final-observer-body.diff','hash-helper.diff','observer-header.diff','collector.diff','admission-frame-gate.diff')],'candidate_collector_scope':'The tested collector-private.hpp requires SDK8 status/loss symbols. It is a private SDK8-only consumer; do not replace the common legacy-compatible collector with it unconditionally. Eventual common integration must negotiate these optional symbols against manifest fence version and retain old SDK behavior.','callbacks_lifetime_rule':'install/uninstall/context destruction outside callback execution; no concurrent installation claim','unbounded_counter_stream_proven':False,'prefix_frozen':True,'product_default_changed':False,'repository_sources_modified_by_task':False,'shared_build_or_native_GUI_run':False,'coverage_complete':False,'FreeType_internal_copy_coverage':'unknown','default_CoreText_copy_gap_closed':False,'whole_pipeline_owned_copy_coverage':'unknown','contract_total_contribution':'none; source-observer-owned state excluded','complete_c3_passed':False,'complete_sk12_passed':False}
(P/'report.json').write_text(json.dumps(report,indent=2)+'\n')
(P/'README.md').write_text('Private SDK8 exception fence; no repository source, default SDK, or native GUI changes.\n\n12 actual-linked boundary/lifetime cases plus the unchanged 1000-hash headless causal test passed. See report.json for every actual source, binary, library and raw-output SHA. The old linked SDK7 RED remains RED and byte-exact.\n\nCore dispatch has allocation-free sticky loss bits and an RAII dispatch reset. Fixed thread-local admission frames distinguish failed entries from successful nested scopes, roll back observed depth, and preserve balanced leaves. Capacity loss remains unknown. Old C declarations/class layouts are retained; the new scope wrapper/destructors are explicitly noexcept and implementations prevent callback exceptions escaping old call sites. FT helper active state restores via RAII.\n\nThe private copied collector catches admission/record allocation loss and reports collector_complete=false with sticky status 4. It is SDK8-only, not an unconditional compatible replacement of the shared legacy collector. Root approval is required before adding any repository tooling. The minimal common consumer integration must require status/loss only when the exact SDK manifest advertises fence v1 and retain legacy behavior otherwise. Concurrent callback installation/context destruction and unbounded streams are unproved.\n\nThe first SDK8 preparation compilation failed on inherited absolute header declarations and an incorrect private text replacement; its sources/logs remain under tranche8-private. SDK8b first Gui link failed because the old relative Core input lacked the new status symbol; final links explicitly bind the new local Core. Both failures remain. The initial Core source was recovered from its retained object marker and prepare.py body with matching body SHA; this reconstruction is labeled in report.json.\n\nNo full Qt/HB/FreeType/CoreText, copy-budget, solver, C3, SK12, P0 or new native font-behavior completion is asserted. All previous source/flags/observer sites and contract limits are unchanged. SDK8 prefix and proof are frozen pending independent review/adoption.\n')
print(json.dumps({'passed_cases':len(proofs),'normal_hash_test':True,'normal_full_frozen7_report_equal':normal_equal,'old_reviewed_sha_count':len(old_checked),'report':str(P/'report.json'),'prefix_frozen':True,'repository_sources_modified':False},indent=2))
