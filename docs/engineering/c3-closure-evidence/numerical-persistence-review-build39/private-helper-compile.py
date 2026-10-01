from pathlib import Path
import shlex,shutil,subprocess,hashlib,json
root=Path('/Users/qs/Documents/ChatGPT/QCAE');build=root/'build-c3-closure-headless';out=Path('/private/tmp/qcae-numerical-store-review')
commands=subprocess.check_output(['/opt/homebrew/bin/ninja','-C',str(build),'-t','commands','qcae_solver_numerical_validation_tests'],text=True).splitlines()
base=next(c for c in commands if ' -c ' in c and c.endswith('/solver_numerical_validation_tests.cpp'))
sources={'probe.cpp':out/'probe.cpp','solver_validation_store.cpp':root/'adapters/engine_api/src/solver_validation_store.cpp','solver_result_store.cpp':root/'adapters/engine_api/src/solver_result_store.cpp','solver_run_record.cpp':root/'adapters/engine_api/src/solver_run_record.cpp','solver_version_probe.cpp':root/'adapters/engine_api/src/solver_version_probe.cpp','static_validation.cpp':root/'features/results/src/static_validation.cpp','nastran_static_result.cpp':root/'profiles/nastran/src/nastran_static_result.cpp'}
for name,src in sources.items():
 args=shlex.split(base);args[-1]=str(src);args[args.index('-o')+1]=str(out/(name+'.o'))
 if '-MF' in args:args[args.index('-MF')+1]=str(out/(name+'.d'))
 print('Private fresh strict TU '+name,flush=True);subprocess.run(args,cwd=build,check=True)
args=shlex.split(commands[-1])[2:-2];args[args.index('-o')+1]=str(out/'probe')
for i,v in enumerate(args):
 if v.endswith('solver_numerical_validation_tests.cpp.o'):args[i]=str(out/'probe.cpp.o')
 elif v.endswith('.a'):
  dst=out/Path(v).name;shutil.copyfile(build/v,dst);args[i]=str(dst)
  members=[]
  if v.endswith('/libqcae_engine_api.a'):members=['solver_validation_store.cpp','solver_result_store.cpp','solver_run_record.cpp','solver_version_probe.cpp']
  elif v.endswith('/libqcae_result_features.a'):members=['static_validation.cpp']
  elif v.endswith('/libqcae_nastran.a'):members=['nastran_static_result.cpp']
  if members:subprocess.run(['/usr/bin/ar','r',str(dst),*[str(out/(m+'.o')) for m in members]],check=True);subprocess.run(['/usr/bin/ranlib',str(dst)],check=True)
(out/'source-snapshot.json').write_text(json.dumps({str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in [*sources.values(),root/'adapters/engine_api/src/solver_contribution.cpp']},indent=2)+'\n')
print('Private fragment/helper link; not a shared Release build',flush=True);subprocess.run(args,cwd=build,check=True);subprocess.run([str(out/'probe')],check=True)
