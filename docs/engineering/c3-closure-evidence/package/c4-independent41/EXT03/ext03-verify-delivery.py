import hashlib
import json
from pathlib import Path
import shutil
import subprocess

root = Path('/private/tmp/qcae-c4-ext03-41')
e = Path('/private/tmp/qcae-ext03-41-evidence')

def digest(data):
    return hashlib.sha256(data).hexdigest()

summary = json.loads((e/'delivery-summary.json').read_text())
commit = subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip()
assert commit == summary['extension_commit']
diff = subprocess.check_output(['git','diff',summary['baseline_commit'],commit,'--binary'],cwd=root)
assert digest(diff) == summary['diff_sha256']
assert diff == (e/'extension-baseline.diff').read_bytes()
status = subprocess.check_output(['git','status','--short'],cwd=root,text=True)
assert status.strip() == '?? docs/engineering/c3-closure-evidence/package/c4-common40-baseline-manifest.json'
subprocess.run(['git','diff','--check'],cwd=root,check=True)
pick = json.loads((e/'pick-matrix-receipt.json').read_text())
assert len(pick['cases']) == 12
for case in pick['cases']:
    assert case['actual'] == case['expected'] and case['missed'] == case['extra'] == 0
    assert digest(Path(case['png']).read_bytes()) == case['png_sha256']
for name in ['ext03-run.py','ext03-audit.py','ext03-finalize.py','ext03-verify-delivery.py']:
    shutil.copyfile(Path('/private/tmp')/name,e/name)

output = 'PASS: final commit/diff, 12 native images/ID sets, and preserved-only baseline manifest verified\n'
(e/'final-delivery-verify.log').write_text(output)
with (e/'context-commands.jsonl').open('a') as log:
    log.write(json.dumps({'name':'final-delivery-verify',
                         'command':'/Library/Frameworks/Python.framework/Versions/3.14/bin/python3.14 /private/tmp/ext03-verify-delivery.py',
                         'utf8_bytes':len(output.encode()),'raw_log_bytes':len(output.encode()),
                         'complete':True,'exit_code':0,'sha256':digest(output.encode()),
                         'token_count':None})+'\n')
subprocess.run(['/Library/Frameworks/Python.framework/Versions/3.14/bin/python3.14',
                '/private/tmp/ext03-audit.py'],stdout=subprocess.DEVNULL,check=True)
artifacts=[]
for path in sorted(e.rglob('*')):
    if path.is_file() and path.name != 'artifacts-sha256.json':
        data=path.read_bytes()
        artifacts.append({'path':str(path.relative_to(e)),'bytes':len(data),'sha256':digest(data)})
(e/'artifacts-sha256.json').write_text(json.dumps({'executor':'/root/c4_ext03_frozen41',
                                                'extension_commit':commit,
                                                'artifacts':artifacts},indent=2)+'\n')
print(output,end='')
