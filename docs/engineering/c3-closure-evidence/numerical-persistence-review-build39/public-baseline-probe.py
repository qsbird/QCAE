from pathlib import Path
import sys,json,hashlib,sqlite3,time
sys.path.insert(0,'/Users/qs/Documents/ChatGPT/QCAE/tests')
from solver_run_ipc_tests import SolverHost,configuration,prepare,admitted,wait_started
from nastran_artifact_ipc_tests import wait_task,owned_facts
BASE=Path('/private/tmp/qcae-numerical-store-review/live')
ENGINE='/Users/qs/Documents/ChatGPT/QCAE/build-c3-closure-headless/qcae-engine'
def ensure(v,message):
    if not v: raise AssertionError(message)
def write(p,v): p.write_text(json.dumps(v,ensure_ascii=True,indent=2)+'\n')
def baseline():
    BASE.mkdir(exist_ok=True)
    cfg=configuration(BASE,'numeric-match',with_reader=True)
    with (BASE/'engine.log').open('w+') as log:
        host=SolverHost(ENGINE,BASE,log,cfg)
        try:
            cap=host.start();ctx,profile,params=prepare(host,cap,BASE,benchmark=True)
            task,run=admitted(host,params,ctx,profile,'independent-start')
            wait_started(BASE,run)
            (BASE/'runs'/run/'allow-output').write_text('explicit synthetic independent review')
            finish=wait_task(host.client,task,ctx)
            ensure(finish['state']=='succeeded',finish)
            original=owned_facts(host.workspace)
            before=host.client.call('analysis.get_result',{'run_id':run},ctx)
            ensure(before['numerical_validation']=='not_run' and len(before['fields'])==2 and not before['numerical_checks']['immutable_history'], 'original pending numerical state')
            wrong=dict(profile,definition_digest='0'*64)
            host.client.call('analysis.validate_result',{'run_id':run},ctx,'wrong-profile',extra={'expected_profile':wrong},expected='failed')
            ensure(owned_facts(host.workspace)==original,'profile fence writes')
            checked=host.client.call('analysis.validate_result',{'run_id':run},ctx,'independent-check',extra={'expected_profile':profile})
            ensure(len(checked['components'])==132 and checked['summary']['numerical_stage']=='not_run' and checked['summary']['source_kind']=='test_process','synthetic became acceptance')
            after=owned_facts(host.workspace)
            ensure(len(after)==len(original)+2 and all(v in after for v in original),'original source/run/result row mutated')
            replay=host.client.call('analysis.validate_result',{'run_id':run},ctx,'independent-check',extra={'expected_profile':profile})
            ensure(replay['replayed'] and replay['components']==checked['components'] and owned_facts(host.workspace)==after,'replay changed original')
            raw=BASE/'runs'/run/'.qcae-result'/'result.f06';data=raw.read_bytes();raw.write_bytes(b'x'*len(data))
            damaged=host.client.call('analysis.get_result',{'run_id':run},ctx)
            ensure(not damaged['raw_resources_verified'] and not damaged['numerical_checks']['current_files_verified'] and damaged['numerical_validation']=='not_run','broken files current verification spoof')
            host.client.call('analysis.validate_result',{'run_id':run},ctx,'broken-new',extra={'expected_profile':profile},expected='failed')
            ensure(owned_facts(host.workspace)==after,'broken-files fresh validation committed')
            raw.write_bytes(data)
            material=host.client.call('entity.query',{'kind':'material'},ctx)['entities'][0]['entity_id']
            host.client.call('material.set_young_modulus',{'entity_id':material,'young_modulus':{'value':200,'unit':'GPa'}},ctx,'independent-material-edit')
            stale_ctx=host.client.current()
            stale=host.client.call('analysis.get_result',{'run_id':run},stale_ctx)
            ensure(stale['numerical_checks']['original_source_state']=='stale' and not stale['numerical_checks']['applies_to_current_input'],'stale fields became current')
            ensure(stale['numerical_checks']['latest_applicable_summary']==checked['summary'],'stale source rewrote report')
            # Restore physical input by the normal transaction API, not a DB edit.
            host.client.call('history.undo',{},stale_ctx,'independent-undo')
            ctx=host.client.current()
            saved=BASE/'original.qcae'
            host.client.call('project.save',{'path':str(saved)},ctx,'independent-save')
            with sqlite3.connect(saved) as db: blob=db.execute('SELECT payload FROM project WHERE id=1').fetchone()[0]
            (BASE/'original-project.bin').write_bytes(blob)
            write(BASE/'metadata.json',{'run_id':run,'context':ctx,'registered_profile':profile,'engine_sha256':hashlib.sha256(Path(ENGINE).read_bytes()).hexdigest(),'synthetic':True,'real_solver':False})
            write(BASE/'public-baseline-observations.json',{'comparison':checked,'broken_files':damaged,'stale':stale,'run_id':run,'original_owned_rows_unchanged':True,'actual_engine':True,'numerical_acceptance':False})
            print('fresh39 actual private IPC baseline:132components/profile-reject/originalbytes/replay/damage/stale GREEN',flush=True)
        finally:
            write(BASE/'transcript.json',host.client.transcript);host.close()
if __name__=='__main__': baseline()
