from pathlib import Path
from types import SimpleNamespace
import importlib.util,json,hashlib,tempfile
source=Path('/Users/qs/Documents/ChatGPT/QCAE/tools/stage_macos_package.py')
spec=importlib.util.spec_from_file_location('stage_scope_probe',source);stage=importlib.util.module_from_spec(spec);spec.loader.exec_module(stage)
base=Path(tempfile.mkdtemp(prefix='qcae-stage-scope-',dir='/private/tmp'));workspace=base/'fixture-workspace';out=workspace/'out';out.mkdir(parents=True);build=base/'fixture-build';build.mkdir();(build/'CMakeCache.txt').write_text('CMAKE_BUILD_TYPE:STRING=Release\nQCAE_C3_SQLITE_OBSERVED_OBJECT:FILEPATH=\n')
stage.ROOT=workspace
cases=[]
for name,bundle in [('dotdot',out/'..'/'escaped.app'),('symlink',out/'linked'/'escaped.app')]:
 if name=='symlink':
  external=base/'fixture-other-parent';external.mkdir();(out/'linked').symlink_to(external,target_is_directory=True)
 args=SimpleNamespace(build_dir=build,bundle=bundle)
 try:stage.stage(args);failure='unexpected_completed'
 except Exception as e:failure=type(e).__name__+': '+str(e)
 actual=bundle.resolve();admitted=(actual/'Contents'/'MacOS').is_dir();cases.append({'case':name,'argument':str(bundle),'resolved_target':str(actual),'within_ignored_out_after_resolve':actual.is_relative_to(out.resolve()),'actual_stage_preflight_admitted_and_created_directory':admitted,'copy_stop':failure,'external_commands_executed':False})
result={'source':str(source),'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'scope':'actual production stage() early path/cache checks+mkdir only, module ROOT fixture under private tmp; expected missing source binary stops before command/macdeployqt/install_name_tool/signature','cases':cases,'red':all(x['actual_stage_preflight_admitted_and_created_directory'] and not x['within_ignored_out_after_resolve'] for x in cases)}
print(json.dumps(result,indent=2));Path('/private/tmp/qcae-common40-readability-review/staging-path-red.json').write_text(json.dumps(result,indent=2)+'\n')
raise SystemExit(1 if result['red'] else 0)
