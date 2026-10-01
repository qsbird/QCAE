import hashlib,json,re,shutil,subprocess
from collections import Counter,defaultdict
from pathlib import Path

root=Path('/private/tmp/qcae-ext02-evidence41')
checkout=Path('/private/tmp/qcae-c4-ext02-41')
def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

index=json.loads((root/'context-index-final.json').read_text())
for row in index['records']:
    path=root/row['output_file']
    data=path.read_bytes()
    expected=row['actual_delivered_utf8_bytes']
    if len(data)<expected:
        raise RuntimeError(f'incomplete retained returned text: {path}')
    path.write_bytes(data[:expected])
    row['retained_output_sha256']=sha(path)
    row['retained_output_utf8_bytes']=len(path.read_bytes())
    row['source_path_candidates']=sorted(set(re.findall(r"(?:AGENTS\.md|(?:docs|schemas|features|modules|adapters|tests|tools)/[A-Za-z0-9_./*+-]+\.(?:md|json|cpp|hpp|py|txt)|(?:CMakeLists\.txt))",row['command'])))
    row['source_path_scope']='literal paths in command; wildcard and partial searches are not complete file delivery'
(root/'context-index-final.json').write_text(json.dumps(index,ensure_ascii=False,indent=2)+'\n')
shutil.copyfile('/private/tmp/qcae-ext02-context.jsonl',root/'context-producer-claims.jsonl')
claims=[json.loads(line) for line in (root/'context-producer-claims.jsonl').read_text().splitlines()]
accesses=defaultdict(list)
for row in claims:
    accesses[row['path']].append(row)
producer_ledger={
    'scope':'Producer reader file accesses and full-output claims. The reader logs before tool clipping; these are not actual model deliveries. Actual returned bytes and clipping evidence are retained in context-index-final.json.',
    'paths':[{'path':path,'reader_access_count':len(rows),'phases':dict(Counter(r['phase'] for r in rows)),'requested_content_utf8_bytes_sum':sum(r['utf8_bytes'] for r in rows),'producer_stdout_utf8_bytes_sum':sum(r['delivered_utf8_bytes'] for r in rows),'source_sha256s':sorted(set(r['sha256'] for r in rows)),'actual_delivery_scope':'per-command returned text; full-file delivery unavailable where nested or outer output clipped'} for path,rows in sorted(accesses.items())]
}
(root/'context-source-accesses.json').write_text(json.dumps(producer_ledger,ensure_ascii=False,indent=2)+'\n')
commands=[json.loads(line) for line in (root/'commands.jsonl').read_text().splitlines()]
reconstructed=[]
for row in commands:
    if row['label'] in ('core-configure','core-build-repair1'):
        raw=Path(row['raw_log']).read_bytes()
        candidate=json.dumps(row)+'\n'+raw[-10000:].decode('utf-8',errors='replace')
        reconstructed.append({'label':row['label'],'text':candidate,'candidate_utf8_bytes':len(candidate.encode()),'original_actual_returned_utf8_bytes':None,'tokens':None,'scope':'deterministic harness output reconstructed from receipt and raw log; excluded from measured original delivery'})
(root/'reconstructed-early-command-output.json').write_text(json.dumps(reconstructed,ensure_ascii=False,indent=2)+'\n')

receipt=json.loads((root/'extension-receipt.json').read_text())
frozen=json.loads((root/'frozen-integrity.json').read_text())
baseline=json.loads((checkout/'docs/engineering/c3-closure-evidence/package/c4-common40-baseline-manifest.json').read_text())
measured=[r for r in index['records'] if not r.get('reconstruction')]
packet=[r for r in measured if r['phase'] in ('initial','initial-redelivery')]
followup=[r for r in measured if r['phase'] not in ('initial','initial-redelivery')]
context_summary={
    'cutoff':'delivery-diff-receipt; subsequent evidence packaging stdout is tracked separately if delivered',
    'initial_file_count':8,
    'initial_content_utf8_bytes':42078,
    'initial_full_packet_stdout_utf8_bytes':42601,
    'first_nested_return_complete':True,
    'first_nested_return_utf8_bytes':42601,
    'first_outer_display_complete':False,
    'first_outer_display_utf8_bytes':None,
    'first_outer_display_reason':'functions default output clipping reported 651 tokens truncated; exact outer displayed bytes unavailable',
    'redelivery_before_product_edit_complete':True,
    'redelivery_nested_return_utf8_bytes':42601,
    'redelivery_outer_output_budget_tokens':16000,
    'duplicate_initial_packet_count':2,
    'initial_location_duplicate_utf8_bytes':6843,
    'initial_location_scope':'exact text transparently reconstructed from known original path/status prefix and unchanged task bytes; retained as record 000',
    'measured_nested_return_records':len(measured),
    'measured_nested_return_utf8_bytes':sum(r['actual_delivered_utf8_bytes'] for r in measured),
    'followup_measured_nested_return_records':len(followup),
    'followup_measured_nested_return_utf8_bytes':sum(r['actual_delivered_utf8_bytes'] for r in followup),
    'extra_messages_count':len(index['extra_messages']),
    'extra_messages_utf8_bytes':sum(r['utf8_bytes'] for r in index['extra_messages']),
    'unknown_early_observations':index['unknown_early_observations'],
    'token_usage':None,
    'actual_total_model_context_utf8_bytes':None,
    'total_context_cost_gate':'not evaluated: outer clipping and early observation gaps make actual total unknown',
    'producer_claims_are_actual_delivery':False,
    'raw_logs_are_actual_delivery':False,
    'inherited_system_user_developer_instructions_scope':'outside frozen source-package byte measurement; real complete model-context/token cost unavailable'
}
(root/'context-summary.json').write_text(json.dumps(context_summary,ensure_ascii=False,indent=2)+'\n')
command_evidence=[]
for row in commands:
    log=Path(row['raw_log'])
    command_evidence.append({**row,'raw_log_sha256':sha(log),'raw_log_size_verified':log.stat().st_size==row['raw_log_utf8_bytes'],'environment':{'QT_QPA_PLATFORM':'cocoa'} if row['label']=='native-desktop-tests' else {},'diagnostic_delivery_scope':'harness print range before nested tool clipping; exact actual returned strings in context index'})
artifact_files=[]
for path in sorted(root.rglob('*')):
    if path.is_file() and path.name not in ('delivery-manifest.json','delivery-manifest.sha256'):
        artifact_files.append({'path':str(path.relative_to(root)),'bytes':path.stat().st_size,'sha256':sha(path)})
manifest={
    'task_id':'EXT-02',
    'executor':'Codex native subagent /root/c4_ext02_frozen41',
    'workdir':str(checkout),
    'branch':'codex/c4-ext02-baseline41',
    **receipt,
    'handwritten_product_limit':6,
    'protected_file_count':len(frozen['protected_files']),
    'protected_files_unchanged':frozen['protected_files_unchanged'],
    'generator_file_count':len(frozen['generators']),
    'generators_unchanged':frozen['generators_unchanged'],
    'tool_images_unchanged':frozen['tool_images_unchanged'],
    'no_dependencies_added':True,
    'generated_contracts_check':'passed byte-identical generator --check, no generated hand edits',
    'core_release_ctest':{'passed':39,'failed':0,'command_label':'core-final-tests'},
    'local_sqlite_ipc_regression':{'passed':13,'failed':0,'command_label':'local-regressions'},
    'new_batch_translation_ipc':{'modes':['cli','script'],'nodes':11,'line2':10,'rejections_each_mode':10,'one_transaction':True,'undo_redo':True,'retry_after_undo':True,'preview_cancel':True,'preview_commit_and_undo':True,'legacy_node_move':True,'command_label':'ext02-preview-apply-ctest','evidence':'ipc-ctest-final'},
    'public_consumers':{'compiled':48,'new_consumer_run_exit':0,'command_label':'consumers-build','new_contract':'consumer_batch_translation'},
    'native_gui':{'passed':5,'failed':0,'exclusive_slot':True,'released':True,'environment':{'QT_QPA_PLATFORM':'cocoa'},'tests':['desktop','desktop_selection','modeling_tools','desktop_smoke','c3_tool_lifecycle'],'command_label':'native-desktop-tests'},
    'independent_readability_review':{'gate':'QG-02','reviewer':'/root','result':'pass','product_repairs_requested':0,'message_retained':'context-index-final.json extra_messages'},
    'failures':{'product_test_compile_failures':1,'product_test_compile_repairs':1,'validation_timeout_failures':2,'validation_tool_or_threshold_changes':0,'unchanged_successful_freeze_retries':2,'resolved_setup_failures':['clang-format default command unavailable; used frozen absolute tool','git staging index sandbox denied; authorized isolated-worktree escalation','context reader missing required CLI arguments; rerun correct argv'],'frozen_environment_config_selections_corrected_before_build':['Python 3.14.7 -> explicit frozen Python 3.14.0','SDK SQLite 3.51.0 -> frozen Homebrew SQLite 3.53.4'],'raw_failed_command_labels':['core-build','core-tests','core-freeze-retry']},
    'context':context_summary,
    'command_evidence':command_evidence,
    'artifacts':artifact_files,
    'final_common_release_integration':False,
    'complete_c4_or_p0_acceptance_claimed':False
}
(root/'delivery-manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n')
(root/'delivery-manifest.sha256').write_text(sha(root/'delivery-manifest.json')+'  delivery-manifest.json\n')
print(json.dumps({'extension_commit':receipt['extension_commit'],'diff_sha256':receipt['diff_sha256'],'manifest_sha256':sha(root/'delivery-manifest.json'),'context_measured_nested_bytes':context_summary['measured_nested_return_utf8_bytes'],'context_followup_measured_nested_bytes':context_summary['followup_measured_nested_return_utf8_bytes'],'actual_total_model_context_bytes':None,'tokens':None,'artifacts':len(artifact_files),'records':len(index['records'])},indent=2))
