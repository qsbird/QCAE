import subprocess, pathlib, json, sys, hashlib

root=pathlib.Path('/private/tmp/qcae-c4-ext03-41')
evidence=pathlib.Path('/private/tmp/qcae-ext03-41-evidence')
name=sys.argv[1]
command=sys.argv[2]
limit=int(sys.argv[3]) if len(sys.argv)>3 else 32000
r=subprocess.run(command,shell=True,cwd=root,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
raw=r.stdout.decode('utf-8',errors='replace')
(evidence/(name+'.log')).write_text(raw)
delivered=raw.encode('utf-8')[:limit].decode('utf-8',errors='ignore')
if len(delivered.encode('utf-8'))<len(raw.encode('utf-8')):
    delivered+='\n[bounded delivery; full raw log retained]\n'
with (evidence/'context-commands.jsonl').open('a') as f:
    f.write(json.dumps({'command':command,'name':name,'utf8_bytes':len(delivered.encode('utf-8')),'raw_log_bytes':len(raw.encode('utf-8')),'complete':delivered==raw,'exit_code':r.returncode,'sha256':hashlib.sha256(delivered.encode('utf-8')).hexdigest(),'token_count':None})+'\n')
sys.stdout.write(delivered)
sys.exit(r.returncode)
