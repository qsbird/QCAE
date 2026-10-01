from pathlib import Path
import sys,json,hashlib,sqlite3
from concurrent.futures import ThreadPoolExecutor
sys.path.insert(0,'/Users/qs/Documents/ChatGPT/QCAE/tests')
from solver_run_ipc_tests import SolverHost
from c3_sync_ipc_tests import IpcClient
from nastran_artifact_ipc_tests import owned_facts
BASE=Path('/private/tmp/qcae-numerical-store-review/live');ROOT=Path('/private/tmp/qcae-numerical-store-review/capacity-v2')
ENGINE='/Users/qs/Documents/ChatGPT/QCAE/build-c3-closure-headless/qcae-engine'
def ensure(v,m):
    if not v:raise AssertionError(m)
def main():
    ROOT.mkdir(exist_ok=True);meta=json.loads((BASE/'metadata.json').read_text());profile=meta['registered_profile'];run=meta['run_id'];prefix='validation-'+hashlib.sha256(run.encode()).hexdigest()[:32]+'-';configuration=BASE/'local-config.json'
    files={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in (BASE/'runs'/run/'.qcae-result').rglob('*') if p.is_file()}
    with (ROOT/'engine.log').open('w+') as log:
        host=SolverHost(ENGINE,ROOT,log,configuration);other=[]
        try:
            host.start();ctx=host.client.call('project.open',{'mode':'normal','path':str(BASE/'original.qcae')},key='open-independent-source')
            before=owned_facts(host.workspace);history=host.client.call('analysis.get_result',{'run_id':run},ctx)['numerical_checks'];ensure(len(history['immutable_history'])==1,'source missing first immutable fact')
            rejected=host.client.call('analysis.validate_result',{'run_id':run},meta['context'],'foreign-old-epoch',extra={'expected_profile':profile},expected=None)
            ensure(rejected.get('status')!='success' and owned_facts(host.workspace)==before,'oldepoch fact installed')
            with sqlite3.connect(host.workspace) as db:
                db.execute("CREATE TRIGGER independent_validation_abort BEFORE INSERT ON store_rows WHEN new.space=7 AND substr(new.identity,1,%d)='%s' BEGIN SELECT RAISE(ABORT,'independent test-only rollback'); END" % (len(prefix),prefix))
            rollback=host.client.call('analysis.validate_result',{'run_id':run},ctx,'atomic-report-rollback',extra={'expected_profile':profile},expected='failed')
            ensure(rollback['error']['code']=='STORAGE_FAILURE' and owned_facts(host.workspace)==before,'ordinary report rollback partial/uncertain')
            with sqlite3.connect(host.workspace) as db:db.execute('DROP TRIGGER independent_validation_abort')
            others=[IpcClient(host.endpoint),IpcClient(host.endpoint)];other=others
            def once(n):return others[n].call('analysis.validate_result',{'run_id':run},ctx,'concurrent-private-'+str(n),extra={'expected_profile':profile})
            with ThreadPoolExecutor(max_workers=2) as pool:received=list(pool.map(once,range(2)))
            ensure({v['summary']['ordinal'] for v in received}=={'2','3'},'concurrent ordinal duplicate')
            for ordinal in range(4,17):
                response=host.client.call('analysis.validate_result',{'run_id':run},ctx,'independent-capacity-'+str(ordinal),extra={'expected_profile':profile});ensure(response['summary']['ordinal']==str(ordinal),'ordinal not continuous')
            full=owned_facts(host.workspace);ensure(all(row in full for row in before if not row[1].endswith('-head')),'source row bytes changed')
            overflow=host.client.call('analysis.validate_result',{'run_id':run},ctx,'independent-capacity-17',extra={'expected_profile':profile},expected='failed')
            ensure(overflow['error']['code']=='RESOURCE_LIMIT' and owned_facts(host.workspace)==full,'capacity17 partial commit')
            retry=host.client.call('analysis.validate_result',{'run_id':run},ctx,'independent-capacity-16',extra={'expected_profile':profile});ensure(retry['replayed'] and owned_facts(host.workspace)==full,'capacity existing retry rejected/changed')
            got=host.client.call('analysis.get_result',{'run_id':run},ctx);checks=got['numerical_checks'];n=len(json.dumps(checks['immutable_history'],separators=(',',':'),ensure_ascii=False).encode());ensure(len(checks['immutable_history'])==16 and not checks['history_overflow'] and n<=32768,'normal immutable history cap changed')
            ensure(host.client.current()['revision']==ctx['revision'],'aux report bumpedmodel')
            host.process.kill();host.process.wait(timeout=5);host.process=None;host.client.instance_id=None
            ensure(host.start()['recovery_available'],'no recovery durable aux facts')
            recovered=host.client.call('project.open',{'mode':'recover'},key='independent-recover')
            restored=host.client.call('analysis.get_result',{'run_id':run},recovered);ensure(restored['numerical_checks']==checks,'recovered checks changed')
            ensure(len(list((BASE/'runs').iterdir()))==1 and all(hashlib.sha256(Path(p).read_bytes()).hexdigest()==digest for p,digest in files.items()),'report or recovery rewrote output/reexecuted child')
            evidence={'actual_engine':True,'real_solver':False,'numerical_acceptance':False,'engine_sha256':hashlib.sha256(Path(ENGINE).read_bytes()).hexdigest(),'old_epoch_rejected_status':rejected['status'],'ordinary_rollback':rollback['error']['code'],'actual_two_socket_ordinals':[v['summary']['ordinal'] for v in received],'normal_history_rows':16,'head_plus_reports':17,'normal_json_bytes':n,'report17_rejected':overflow['error']['code'],'original_source_rows_retained':True,'model_revision_unchanged':True,'recovery_identical_history':True,'child_runs':1,'frozen_output_bytes_unchanged':True}
            (ROOT/'observations.json').write_text(json.dumps(evidence,indent=2)+'\n');print('fresh39 public independent capacity: epoch/rollback/2socket/16+head/17reject/retry/recovery/originalbytes GREEN; synthetic not_run',flush=True)
        finally:
            (ROOT/'transcript.json').write_text(json.dumps({'main':host.client.transcript,'competitors':[x.transcript for x in other]},indent=2)+'\n');host.close()
if __name__=='__main__':main()
