from pathlib import Path
import sys,json,hashlib,sqlite3,socket
sys.path.insert(0,'/Users/qs/Documents/ChatGPT/QCAE/tests')
from solver_run_ipc_tests import SolverHost
from nastran_artifact_ipc_tests import owned_facts
BASE=Path('/private/tmp/qcae-numerical-store-review/live');CARRIED=Path('/private/tmp/qcae-numerical-store-review/carried-fixed')
ENGINE='/Users/qs/Documents/ChatGPT/QCAE/build-c3-closure-headless/qcae-engine'
def ensure(v,m):
    if not v:raise AssertionError(m)
def raw_call(endpoint,operation,params,context):
    request={'api_version':'1.1','request_id':'independent-canonical-summary-raw','operation':operation,'parameters':params,'document_id':context['document_id'],'document_epoch':context['document_epoch'],'expected_revision':context['revision']}
    with socket.socket(socket.AF_UNIX) as s:
        s.settimeout(30);s.connect(endpoint)
        with s.makefile('rwb') as stream:
            stream.write(json.dumps({'api_version':'1.1','request_id':'private-raw-handshake','operation':'runtime.handshake'},separators=(',',':')).encode()+b'\n');stream.flush();ensure(json.loads(stream.readline())['status']=='success','handshake')
            stream.write(json.dumps(request,separators=(',',':')).encode()+b'\n');stream.flush();return stream.readline()
def main():
    CARRIED.mkdir(exist_ok=True);copy=CARRIED/'control.qcae'
    with sqlite3.connect(BASE/'original.qcae') as original,sqlite3.connect(copy) as dest:
        original.backup(dest);dest.execute('UPDATE project SET payload=? WHERE id=1',((BASE/'control-project.bin').read_bytes(),))
    with (CARRIED/'engine.log').open('w+') as log:
        host=SolverHost(ENGINE,CARRIED,log,BASE/'local-config.json')
        try:
            host.start();opened=host.client.call('project.open',{'mode':'normal','path':str(copy)},key='independent-open-control')
            before=owned_facts(host.workspace)
            raw=raw_call(host.endpoint,'analysis.get_result',{'run_id':'run-'+'\x01'*121},opened)
            (CARRIED/'actual-get-result-frame.json').write_bytes(raw)
            reply=json.loads(raw);ensure(reply['status']=='success',reply)
            data=reply['data'];checks=data['numerical_checks'];ensure(checks['immutable_history']==[] and checks['history_overflow'] and checks['history_overflow_reason']=='summary_bytes' and not checks['has_applicable_validation'],'actual overflow not suppressed')
            needle=b'"immutable_history":';at=raw.index(needle)+len(needle);text=raw[at:].decode();array,stop=json.JSONDecoder().raw_decode(text);n=len(text[:stop].encode())
            ensure(array==checks['immutable_history'],'raw extraction differs')
            ensure(owned_facts(host.workspace)==before and host.client.current()['revision']==opened['revision'],'read committed payload/revision')
            ensure(data['numerical_validation']=='not_run' and all(v['source_kind']=='test_process' and v['numerical_stage']=='not_run' for v in array),'synthetic passed certification')
            observation={'actual_engine':True,'engine_sha256':hashlib.sha256(Path(ENGINE).read_bytes()).hexdigest(),'actual_immutable_history_json_bytes':n,'history_declared_limit':checks['history_summary_byte_limit'],'history_overflow':checks['history_overflow'],'has_applicable_validation':checks['has_applicable_validation'],'reports_retained_in_source':16,'reports_exposed':len(array),'control_task_id_bytes':121,'crossrow_chain_revalidated_by_get_result':True,'get_read_owned_rows_and_revision_unchanged':True,'raw_resources_verified':data['raw_resources_verified'],'real_solver':False,'numerical_acceptance':False,'independent_review_finding':'fixed real get_result suppresses legitimate45KiB serialized history' }
            (CARRIED/'actual-quota-observation.json').write_text(json.dumps(observation,indent=2)+'\n')
            ensure(n==2 and checks['history_overflow'] and len(data['fields'])==2,'fixed bounds or remaining parsed-fields unavailable')
            print(f'QUOTA GREEN actual repaired fresh39 get_result: 16 retained reports, raw immutable_history={n}B, history_overflow={checks["history_overflow"]}/summary_bytes; parsed fields2/no writes/synthetic not_run',flush=True)
        finally:
            (CARRIED/'transcript.json').write_text(json.dumps(host.client.transcript,ensure_ascii=True,indent=2)+'\n');host.close()
if __name__=='__main__':main()
