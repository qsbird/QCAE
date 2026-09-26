#!/usr/bin/env python3
"""Freeze/check the bounded Archer reference inventory without copying source.

Traversal deliberately ignores .gitignore (template contributions are ignored there).
Unknown source roots, SDK families and template branches fail instead of becoming AP-20.
The manifest is L1 evidence only; no reference build or QCAE L2 is inferred.
"""

from __future__ import annotations

import argparse
from collections import Counter
import fnmatch
import hashlib
import json
import os
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_REFERENCE = Path('/Users/qs/Documents/ArcherprePublic')
CONTRACT_FIELDS = {
    'reference_evidence', 'owner_module', 'authoritative_data', 'public_contracts',
    'allowed_dependencies', 'extension_points', 'state_failure_version_rules',
    'acceptance_case_ids',
}
OWNERS = {
    'AP-01': 'application', 'AP-02': 'document/schema', 'AP-03': 'geometry',
    'AP-04': 'mesh/association', 'AP-05': 'meshing_adapter', 'AP-06': 'mesh/document',
    'AP-07': 'features/mesh_editing', 'AP-08': 'organization',
    'AP-09': 'physics/parameters', 'AP-10': 'extensions/capability_package',
    'AP-11': 'analysis', 'AP-12': 'operations/history', 'AP-13': 'interaction',
    'AP-14': 'validation', 'AP-15': 'project/exchange', 'AP-16': 'visualization',
    'AP-17': 'ui', 'AP-18': 'clients/extensions', 'AP-19': 'results',
    'AP-20': 'runtime/platform_support',
}
SOURCE_ROOTS = (
    'ArcherBuild', 'ArcherMeshLib', 'ArcherModelcore', 'ArcherPre',
    'ArcherPreConsole', 'ArcherPythonInterpreter', 'ArcherSimUI', 'ArcherVCAD',
    'Linux_share', 'Mingw_share', 'NCLicense', 'cloud', 'cmakes',
)
TEMPLATES = (
    'ArcherAPI', 'ArcherAnsys', 'ArcherOptistruct', 'ArcherPreLsdyna',
    'ArcherPreNastran', 'ArcherPrePS', 'ArcherPrePamCrash',
    'ArcherPrePostProcessing', 'Printing3D', 'Radioss', 'abaqus',
    'archershipmesh', 'geodyna',
)
SDK_DOMAINS = {
    'archer': 'AP-01', 'geom': 'AP-03', 'interact': 'AP-13',
    'interactmodel': 'AP-13', 'meshlib': 'AP-06', 'modelcore': 'AP-02',
    'morph': 'AP-07', 'para': 'AP-05', 'pyinterpreter': 'AP-18',
    'render': 'AP-16', 'script': 'AP-18', 'tetra': 'AP-05',
    'vcad': 'AP-03', 'vtopo': 'AP-04',
}
# Every exclusion is frozen in the manifest. Vendor implementation is never evidence.
EXCLUSIONS = (
    ('metadata', '.git', 'Nested submodule metadata is not reference source.'),
    ('metadata', '.vscode', 'Local editor state is not an architecture contract.'),
    ('vendor', 'ArcherPre3rd', 'Bundled third-party implementations and SDK runtimes.'),
    ('vendor', 'ArcherPre/3rd', 'Third-party implementations are outside the owned-source denominator.'),
    ('vendor', 'ArcherSimUI/src/base', 'Qtitan vendor implementation, Developer Machines copyright.'),
    ('vendor', 'ArcherSimUI/src/chart', 'Qtitan vendor implementation.'),
    ('vendor', 'ArcherSimUI/src/docking', 'Qtitan vendor implementation.'),
    ('vendor', 'ArcherSimUI/src/fastinfoset', 'Qtitan vendor implementation.'),
    ('vendor', 'ArcherSimUI/src/grid', 'Qtitan vendor implementation.'),
    ('vendor', 'ArcherSimUI/src/navigation', 'Qtitan vendor implementation.'),
    ('vendor', 'ArcherSimUI/src/ribbon', 'Qtitan vendor implementation.'),
    ('vendor', 'ArcherSimUI/src/styles', 'Qtitan vendor implementation.'),
    ('vendor', 'ArcherSimUI/src/CMakeLists.txt', 'Qtitan vendor build script; owned parent integration CMake remains evidence.'),
    ('vendor', 'ArcherSimUI/src/translations', 'Qtitan vendor translations.'),
    ('vendor', 'ArcherSimUI/src/res', 'Qtitan vendor resources.'),
    ('vendor', 'ArcherSimUI/src/*.rc', 'Qtitan vendor resource build declarations.'),
    ('vendor', 'ArcherSimUI/src/DevMachines', 'Vendor component metadata.'),
    ('vendor', 'ArcherPre/gui/qtpropertybrowser', 'Bundled Qt Solutions property-browser implementation.'),
    ('vendor', 'ArcherVCAD/inc/boost_ext', 'Bundled Boost extension implementation.'),
    ('vendor', 'ArcherVCAD/inc/rtree', 'Bundled RTree spatial-index implementation, not a QCAE domain.'),
    ('vendor', 'ArcherMeshLib/inc/tiny_obj_loader.h', 'MIT tinyobjloader implementation by Syoyo Fujita.'),
    ('vendor', 'ArcherPre/sdk/include/pythonqt', 'PythonQt vendor API, not Archer-owned SDK.'),
    ('vendor', 'ArcherPre/sdk/include/licensecc', 'Licensecc vendor API; optional policy adapter recorded via NCLicense.'),
    ('vendor', 'ArcherPre/sdk/include/zip', 'Third-party archive API; owned exchange boundary remains inventoried.'),
    ('vendor', 'ArcherPre/doc/python-docx', 'Bundled documentation library.'),
    ('generated', 'ArcherPre/doc/Doxgen', 'Generated API documentation, original headers inventoried.'),
    ('generated', 'ArcherPre/sdk/sdkdoc', 'Generated SDK documentation, original/public headers inventoried.'),
    ('generated', 'ArcherPre/gui/nx_res/html', 'Generated/vendor web help, not owned application source.'),
    ('build', 'build*', 'Probe/build outputs are excluded independently of source suffix.'),
    ('build', '*/CMakeFiles', 'Generated CMake compiler checks and build outputs.'),
    ('build', '*/__pycache__', 'Generated Python bytecode cache.'),
    ('build', '*/.pytest_cache', 'Generated pytest cache.'),
    ('sdk_runtime', 'ArcherPre/sdk/win64', 'SDK runtime/binaries; public include boundaries listed separately.'),
    ('sdk_runtime', 'ArcherPre/sdk/mingw64', 'SDK runtime/binaries.'),
    ('sdk_runtime', 'ArcherPre/sdk/centos79gcc75_x86', 'SDK runtime/binaries.'),
    ('asset', 'pictures', 'Illustration assets, no executable or contract source.'),
)
SOURCE_SUFFIXES = {
    '.h', '.hpp', '.hxx', '.c', '.cpp', '.cxx', '.cc', '.py', '.cmake',
    '.pro', '.pri', '.proto', '.ui', '.qrc', '.rc', '.json', '.xml', '.yaml',
    '.yml', '.ini', '.in', '.iss', '.sh', '.bat', '.md', '.txt', '.template',
    '.ts', '.manifest',
}
NON_SOURCE_SUFFIXES = {
    '.png', '.jpg', '.jpeg', '.svg', '.ico', '.bmp', '.psd', '.ttf', '.qm',
    '.dll', '.lib', '.pyd', '.so', '.a', '.exe', '.o', '.obj', '.bin', '.out',
    '.zip', '.rar', '.7z', '.pptx', '.doc', '.docx', '.pdf', '.xlsx', '.xls',
    '.html', '.js', '.css', '.qdoc', '.index', '.qhp', '.dcf', '.user', '.orig',
    '.bak', '.blob', '.armsh', '.armshb', '.stp', '.step', '.brep', '.iges',
    '.inp', '.art', '.gdn', '.bdf', '.op2', '.arp', '.h5', '.dat', '.ggb',
    '.xmind', '.gan', '.gmind', '.design', '.property', '.natvis', '.lxx', '.dmp', '.csv',
}
BUILD_NAMES = {'CMakeLists.txt', '.gitlab-ci.yml', '.clang-format', '.gitignore', '.gitmodules'}
# Filename/explicit-directory responsibilities precede the bounded root defaults.
SEMANTIC_RULES = (
    ('association', r'(meshdata2|brep|cad4msh|cad_model_data|geom.*replace|geom.*bind|vtopo)', 'AP-04', 'Geometry support/replacement and mesh association boundary.'),
    ('validation', r'(check|quality|issue|diagnostic)', 'AP-14', 'Checking/quality evidence and issue responsibility.'),
    ('results', r'(post_|postmanager|op2|hdf5|result|frame.?manager|variable.?manager)', 'AP-19', 'Results, frames, fields or result reader responsibility.'),
    ('history', r'(undo|redo|uraction|command|cmdimp)', 'AP-12', 'Operation dispatch or reversible history responsibility.'),
    ('application', r'(mmodule|mappimp|mapp\.|ar_service|mainmodelmanager)', 'AP-01', 'Host/application registration and document service composition.'),
    ('package', r'(template.*model|templatemodel|templategui)', 'AP-10', 'Template contribution registration and package composition.'),
    ('analysis', r'(jobmanager|loadcase|loadstep|loadbc|constraint|loadmanager|solver)', 'AP-11', 'Analysis/load configuration; a JobManager name does not establish durable task scheduling.'),
    ('parameters', r'(material|property|section|coordinate|field|table|units?)', 'AP-09', 'Physics/parameter data, coordinate basis and evaluation.'),
    ('organization', r'(assembly|assemblies|part\.|mpart|setsmanager|setmanager|group|include.?file|modeltreeinfo|partition)', 'AP-08', 'Organization/sets/INCLUDE projection over stable identities.'),
    ('exchange', r'(archive|neutral|import|export|mproject|mmodel\.|callbackforsave|meshio|interop|txtreader|ar_zip)', 'AP-15', 'Project persistence or format exchange responsibility.'),
    ('mesh_edit', r'(morph|translate|transform|rotate|mirror|scale|nodeeditor|elemedit|meshedit|renumber|smooth|deform|refine|subdiv|mesh.*cut|mesh.*extrud|mesh.*spin)', 'AP-07', 'Mesh edit/deformation feature and replacement scope.'),
    ('meshing', r'(mesher|meshengine|meshctrl|mesh.?size|generate.?mesh|batchmesh|surfmesh|solidmesh|electromesh|gmsh)', 'AP-05', 'Meshing provider input/quality/generation boundary.'),
    ('geometry', r'(geom|ivcad|occ|cad|curve|surface|topo|rhino|acis|parasolid)', 'AP-03', 'Geometry kernel/topology and geometric operations.'),
    ('interaction', r'(pick|select|interactor|nodecreator|preview|ribbonpanelmanager|toolpanelbase|creator)', 'AP-13', 'Tool lifecycle, selection and preview responsibility.'),
    ('render', r'(render|viewdata|colorbar|graphics|viewlock)', 'AP-16', 'Read-only rendering/view projection responsibility.'),
    ('client', r'(wrapper|interpreter|archerpy|python|server_rendering|jsonapi)', 'AP-18', 'SDK/binding/client protocol entry responsibility.'),
    ('runtime', r'(config|memory|thread|task|progress|license|log|resource|utils|util\.)', 'AP-20', 'Runtime/configuration/resource/optional policy support; not model authority.'),
)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def excluded(path: str) -> tuple[str, str, str] | None:
    for rule in EXCLUSIONS:
        if path == rule[1] or fnmatch.fnmatchcase(path, rule[1]):
            return rule
    return None


def classify(path: str) -> tuple[list[str], str, str]:
    """Return AP domains, auditable rule ID, rationale for an included file."""
    parts = Path(path).parts
    name = parts[-1].lower()
    if path == 'ArcherVCAD/include/ar_ivcad.h':
        return ['AP-03', 'AP-08'], 'geometry-assembly-facade', 'Geometry facade also declares AssemblyData occurrence/placement boundary.'
    if path == 'ArcherModelcore/public/mentitymanager.h':
        return ['AP-02', 'AP-15'], 'entity-exchange-facade', 'Entity catalog public header also exposes archive/neutral exchange responsibilities.'
    if path.startswith('ArcherPre/sdk/include/'):
        family = parts[3]
        if family not in SDK_DOMAINS:
            raise ValueError(f'Unmapped SDK family: {path}')
        return [SDK_DOMAINS[family], 'AP-18'], 'sdk-' + family, 'Public SDK boundary only; implementation presence and behavior are not inferred.'
    if path.startswith('NCLicense/Lib/'):
        return ['AP-20'], 'vendor-license-boundary', 'Vendor license SDK declaration only; optional policy implementation and binaries are excluded.'
    if path.startswith('ArcherSimUI/src/Qtitan'):
        return ['AP-17'], 'vendor-public-ui-boundary', 'Vendor public UI wrapper dependency only; vendor implementation is excluded.'
    is_template = path.startswith('ArcherPre/template/')
    if is_template and parts[2] not in TEMPLATES:
        raise ValueError(f'New template branch requires explicit audit: {path}')
    if path.startswith('cloud/'):
        return ['AP-18', 'AP-16'], 'cloud-schema-boundary', 'Remote client rendering protocol schema; remote server implementation is outside QCAE P0.'
    if path in {'TemplateDevelop.md', 'setting_ar.cmake', 'ArcherPre/modules.cmake'}:
        return ['AP-10', 'AP-20'], 'template-registration-document', 'Template package discovery/build registration and documented extension contribution contract.'
    if Path(path).name in BUILD_NAMES or Path(path).suffix.lower() in {'.cmake', '.pro', '.pri', '.sh', '.bat', '.iss', '.template'}:
        domains = ['AP-10', 'AP-20'] if is_template else ['AP-20']
        return domains, 'build-delivery', 'Owned build/registration/deployment configuration; template registration adds AP-10.'
    if path.startswith('ArcherPre/template/ArcherPrePostProcessing/'):
        base = 'AP-19'
        if '/gui/' in path:
            domains = ['AP-19', 'AP-17', 'AP-10']
        elif '/modelrender/' in path:
            domains = ['AP-19', 'AP-16', 'AP-10']
        else:
            domains = [base, 'AP-10']
        return domains, 'postprocessing-template', 'Result-specific template data/readers/UI/render contributions; full product remains reference only.'
    if '/modelrender/' in path:
        domains = ['AP-16', 'AP-10'] if is_template else ['AP-16']
        return domains, 'render-contribution', 'Read-only render projection contribution; template renderer belongs to its capability package.'
    directory_domains = {'/meshedit/': 'AP-07', '/jobmanager/': 'AP-11', '/modeltree/': 'AP-08'}
    for directory_marker, ap in directory_domains.items():
        if directory_marker in path:
            domains = [ap, 'AP-17'] + (['AP-10'] if is_template else [])
            return sorted(set(domains)), 'directory-' + directory_marker.strip('/'), 'Explicit feature directory fixes responsibility; GUI projection also belongs to UI.'
    for rule_id, pattern, ap, rationale in SEMANTIC_RULES:
        if re.search(pattern, path.lower() if '/tests/' in path else name):
            domains = [ap]
            if is_template:
                domains.append('AP-10')
            if '/gui/' in path:
                domains.append('AP-17')
            if '/modelrender/' in path:
                domains.append('AP-16')
            return sorted(set(domains)), 'semantic-' + rule_id, rationale
    defaults = (
        ('ArcherVCAD/', 'AP-03', 'Geometry facade and kernel adapter source under the explicit VCAD root.'),
        ('ArcherMeshLib/mesher/', 'AP-05', 'Meshing provider implementation and provider tests.'),
        ('ArcherMeshLib/', 'AP-06', 'Mesh storage/topology/helper implementation under explicit MeshLib root.'),
        ('ArcherModelcore/', 'AP-02', 'Entity/model metadata and generic document managers under explicit Modelcore root.'),
        ('ArcherPre/template/', 'AP-10', 'Owned template contribution under one of the thirteen explicitly audited package branches.'),
        ('ArcherPre/archer-py/modelrender/', 'AP-16', 'Rendering projection contribution.'),
        ('ArcherPre/archer-py/model/', 'AP-02', 'Generic model manager/entity feature contribution.'),
        ('ArcherPre/archer-py/', 'AP-18', 'Python-facing binding support.'),
        ('ArcherPre/gui/jobmanager/', 'AP-11', 'GUI analysis configuration; runtime state-machine parity is not implied.'),
        ('ArcherPre/gui/mygraphics/', 'AP-16', 'GUI scene/view projection.'),
        ('ArcherPre/gui/', 'AP-17', 'Owned shell/tree/panel/editor and preference UI contribution.'),
        ('ArcherPre/ArcherPreScript/', 'AP-18', 'Script runtime entry boundary.'),
        ('ArcherPre/client/', 'AP-18', 'Transport client boundary.'),
        ('ArcherPre/server/', 'AP-18', 'Transport server boundary; remote implementation remains out of QCAE P0.'),
        ('ArcherPre/tests/py/ArcherPreScripts/', 'AP-18', 'Script/API regression consumer; specialized domains additionally classified by explicit path rules.'),
        ('ArcherPre/tests/py/ArcherPreGuiScripts/', 'AP-17', 'GUI regression consumer.'),
        ('ArcherPre/tests/archerautotest/apps/', 'AP-18', 'Application/API regression consumer; specialized domains classified by explicit path rules.'),
        ('ArcherPre/tests/', 'AP-20', 'Owned test harness or unspecialized regression; no claim it was run.'),
        ('ArcherPre/doc/', 'AP-20', 'Original developer/build documentation evidence, not generated API pages.'),
        ('ArcherPre/install/', 'AP-20', 'Owned installation/resource delivery support.'),
        ('ArcherPreConsole/', 'AP-18', 'Console entry through shared model services.'),
        ('ArcherPythonInterpreter/', 'AP-18', 'Script interpreter boundary.'),
        ('ArcherBuild/', 'AP-20', 'Owned build automation.'),
        ('ArcherSimUI/', 'AP-17', 'UI integration build contract; Qtitan implementation excluded.'),
        ('NCLicense/', 'AP-20', 'Optional commercial policy adapter, no policy replication requirement.'),
        ('cloud/', 'AP-18', 'Remote transport schema boundary; no QCAE remote server obligation.'),
        ('cmakes/', 'AP-20', 'Owned product build configuration variants.'),
        ('Linux_share/', 'AP-20', 'Owned platform delivery scripts.'),
        ('Mingw_share/', 'AP-20', 'Platform build documentation.'),
    )
    for prefix, ap, rationale in defaults:
        if path.startswith(prefix):
            return [ap], 'root-' + prefix.rstrip('/').replace('/', '-'), rationale
    if len(parts) == 1 or (len(parts) == 2 and parts[0] == 'ArcherPre'):
        return ['AP-20'], 'root-build-documentation', 'Top-level owned build/development/packaging support file.'
    raise ValueError(f'Unmapped reference source: {path}')


def source_kind(path: str) -> str:
    if path.startswith('ArcherPre/sdk/include/'):
        return 'sdk_public_boundary'
    if path.startswith('ArcherSimUI/src/Qtitan') or path.startswith('NCLicense/Lib/'):
        return 'vendor_public_boundary'
    if '/template/' in path:
        return 'template_contribution'
    if Path(path).name in BUILD_NAMES or Path(path).suffix.lower() in {'.cmake', '.pro', '.pri', '.bat', '.sh', '.iss', '.template'}:
        return 'owned_build_delivery'
    if Path(path).suffix.lower() in {'.md', '.txt'}:
        return 'owned_developer_document'
    if '/tests/' in path:
        return 'owned_test_source'
    if '/public/' in path or '/include/' in path:
        return 'owned_public_header'
    return 'owned_source'


def missing_components(reference: Path) -> list[dict]:
    components = (
        ('ArcherGeom', 'AP-03', 'ArcherPre/sdk/include/geom', 'SDK geometry headers; owned source root absent.'),
        ('ArcherGeomAdv', 'AP-03', None, 'Configured advanced geometry source root absent; no implementation evidence.'),
        ('ArcherVTopo', 'AP-04', 'ArcherPre/sdk/include/vtopo', 'SDK topology boundary; owned source root absent.'),
        ('ArcherRender', 'AP-16', 'ArcherPre/sdk/include/render', 'SDK rendering boundary; owned source root absent.'),
        ('ArcherInteract', 'AP-13', 'ArcherPre/sdk/include/interact', 'SDK interaction boundary; owned source root absent.'),
        ('ArcherInteractModel', 'AP-13', 'ArcherPre/sdk/include/interactmodel', 'SDK interaction-model boundary; owned source root absent.'),
        ('ArcherMorph', 'AP-07', 'ArcherPre/sdk/include/morph', 'SDK morph boundary; owned source root absent.'),
        ('ArcherMeshCore', 'AP-05', None, 'Configured meshing core source root absent.'),
        ('ArcherParaMesh', 'AP-05', 'ArcherPre/sdk/include/para', 'SDK parallel-meshing boundary; owned source root absent.'),
        ('ArcherTet', 'AP-05', 'ArcherPre/sdk/include/tetra', 'SDK tetra mesher boundary; owned source root absent.'),
        ('ArcherHex', 'AP-05', None, 'Configured hex mesher source root absent.'),
        ('QuadMesh', 'AP-05', None, 'Configured quad mesher source root absent.'),
        ('ArcherTetRemesh', 'AP-07', None, 'Configured remesher source root absent.'),
        ('ArcherIO', 'AP-15', None, 'Optional exchange component source root absent.'),
    )
    return [
        {'component': component, 'ap_domains': [ap], 'qcae_owner': OWNERS[ap],
         'source_present': (reference / component).is_dir(), 'sdk_boundary_root': sdk,
         'sdk_boundary_present': bool(sdk and (reference / sdk).is_dir()),
         'rationale': rationale, 'status': 'reference_only_not_implemented_in_qcae'}
        for component, ap, sdk, rationale in components
    ]


def collect(reference: Path) -> dict:
    if not reference.is_dir():
        raise ValueError(f'Reference root missing: {reference}')
    items, boundaries, errors = [], [], []
    excluded_counts = Counter()
    for directory, dirs, files in os.walk(reference, followlinks=False):
        relative_dir = Path(directory).relative_to(reference)
        kept = []
        for name in sorted(dirs):
            rel = (relative_dir / name).as_posix()
            match = excluded(rel)
            if match:
                excluded_counts[match[1]] += 1
            elif (Path(directory) / name).is_symlink():
                errors.append('Unreviewed directory symlink: ' + rel)
            else:
                kept.append(name)
        dirs[:] = kept
        for name in sorted(files):
            path = Path(directory) / name
            rel = path.relative_to(reference).as_posix()
            match = excluded(rel)
            if match:
                excluded_counts[match[1]] += 1
                continue
            if name == '.DS_Store' or name.startswith('.git') and name not in BUILD_NAMES:
                excluded_counts['metadata-files'] += 1
                continue
            suffix = path.suffix.lower()
            if suffix in NON_SOURCE_SUFFIXES:
                excluded_counts['non-source-suffix'] += 1
                continue
            if suffix not in SOURCE_SUFFIXES and name not in BUILD_NAMES:
                # Known resource/license files with no suffix are explicitly non-source.
                if not suffix:
                    excluded_counts['extensionless-resource'] += 1
                    continue
                errors.append('Unreviewed file kind: ' + rel)
                continue
            if len(Path(rel).parts) > 1 and Path(rel).parts[0] not in SOURCE_ROOTS:
                errors.append('Unreviewed source root: ' + rel)
                continue
            if path.is_symlink():
                errors.append('Unreviewed file symlink: ' + rel)
                continue
            try:
                domains, rule, rationale = classify(rel)
            except ValueError as error:
                errors.append(str(error))
                continue
            domains = sorted(set(domains))
            kind = source_kind(rel)
            item = {
                'inventory_id': 'REF-' + hashlib.sha256(rel.encode('utf-8')).hexdigest()[:16],
                'path': rel, 'sha256': digest(path), 'source_kind': kind,
                'ap_domains': domains, 'qcae_owner': OWNERS[domains[0]],
                'classification_rule': rule, 'rationale': rationale,
            }
            (boundaries if kind.endswith('boundary') else items).append(item)
    if errors:
        raise ValueError('\n'.join(sorted(errors)))
    all_items = items + boundaries
    by_ap = {ap: sum(ap in item['ap_domains'] for item in all_items) for ap in OWNERS}
    return {
        'schema_version': '1.0', 'status': 'L1_draft_reference_inventory',
        'generator_path': 'tools/inventory_reference.py', 'generator_sha256': digest(Path(__file__)),
        'reference_root': str(reference), 'reference_git_metadata': 'unavailable_local_copy',
        'scope': 'All recognized owned source/build/template/public-contract files in explicit roots; SDK/public vendor boundaries separated. No source copied; no reference build/run.',
        'source_roots': list(SOURCE_ROOTS), 'template_branches': list(TEMPLATES),
        'included_suffixes': sorted(SOURCE_SUFFIXES),
        'exclusion_rules': [dict(category=c, pattern=p, rationale=r) for c, p, r in EXCLUSIONS],
        'non_source_suffixes': sorted(NON_SOURCE_SUFFIXES),
        'extensionless_exclusion': 'Non-source resource/license marker files; any executable script must carry a recognized source suffix.',
        'exclusion_hits': dict(sorted(excluded_counts.items())),
        'exclusion_hit_unit': 'pruned directory or skipped file; pruned tree contents are not individually counted',
        'classification_rule_order': [rule[0] for rule in SEMANTIC_RULES],
        'missing_or_reference_only_components': missing_components(reference),
        'exceptions': [
            {'id': 'EX-01', 'status': 'bounded_reference_only', 'detail': '13 template packages are mapped as contributions; QCAE implements only the Nastran controlled subset.'},
            {'id': 'EX-02', 'status': 'boundary_only', 'detail': 'Absent geometry/render/interaction/advanced-mesher implementations are recorded separately; headers cannot prove runtime behavior.'},
            {'id': 'EX-03', 'status': 'optional_policy', 'detail': 'NCLicense/build switches map to platform policy adapters; commercial policy replication is excluded from product scope.'},
            {'id': 'EX-04', 'status': 'vendor_boundary_only', 'detail': 'Qtitan public wrapper headers/build integration are dependency evidence; vendor implementation is excluded.'},
            {'id': 'EX-05', 'status': 'not_executed', 'detail': 'Owned test scripts are inventory evidence; Archer compilation/test execution was not performed.'},
        ],
        'build_target_evidence': build_targets(reference, items),
        'items': sorted(items, key=lambda item: item['path']),
        'sdk_public_boundaries': sorted(boundaries, key=lambda item: item['path']),
        'summary': {'included_items': len(all_items), 'owned_items': len(items),
                    'boundary_items': len(boundaries), 'mapped_items': len(all_items),
                    'mapping_ratio': 1.0 if all_items else 0.0, 'unclassified_count': 0,
                    'ap_item_counts': by_ap, 'qcae_l2_count': 0},
    }


def build_targets(reference: Path, items: list[dict]) -> list[dict]:
    """Lexical build evidence only; conditions/variables are not a resolved build graph."""
    targets = []
    for item in items:
        if Path(item['path']).name != 'CMakeLists.txt' and not item['path'].endswith('.cmake'):
            continue
        content = (reference / item['path']).read_text(encoding='utf-8', errors='replace')
        for line_number, line in enumerate(content.splitlines(), 1):
            # Commented declarations cannot establish an active target.
            match = re.match(r'\s*(add_library|add_executable)\s*\(\s*([^\s)]+)', line, re.I)
            if match:
                expression = match.group(2)
                resolved = expression
                for variable in re.findall(r'\$\{([^}]+)\}', expression):
                    assignment = re.search(r'(?:set|project)\s*\(\s*' + re.escape(variable) + r'\s+([^\s)]+)', content[:content.index(line)], re.I)
                    if assignment:
                        resolved = resolved.replace('${' + variable + '}', assignment.group(1).strip('"'))
                targets.append({'declaration': match.group(1), 'target_expression': expression,
                                'local_name_evidence': resolved, 'path': item['path'],
                                'line': line_number, 'inventory_id': item['inventory_id'],
                                'status': 'lexical_declaration_not_configured_or_built'})
    return sorted(targets, key=lambda target: (target['path'], target['line']))


def write_compact_manifest(path: Path, manifest: dict) -> None:
    """One item per line preserves reviewable hashes without megabytes of whitespace."""
    lines = ['{']
    pairs = list(manifest.items())
    for index, (key, value) in enumerate(pairs):
        comma = ',' if index != len(pairs) - 1 else ''
        if key in {'items', 'sdk_public_boundaries'}:
            lines.append('  ' + json.dumps(key) + ': [')
            lines.extend('    ' + json.dumps(item, ensure_ascii=False, separators=(',', ':')) + (',' if pos != len(value) - 1 else '') for pos, item in enumerate(value))
            lines.append('  ]' + comma)
        else:
            encoded = json.dumps(value, ensure_ascii=False, indent=2)
            lines.append('  ' + json.dumps(key) + ': ' + encoded.replace('\n', '\n  ') + comma)
    lines.append('}')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('\n'.join(lines) + '\n', encoding='utf-8')


def validate_contracts(directory: Path, manifest: dict) -> None:
    indexed = {item['inventory_id']: item for item in manifest['items'] + manifest['sdk_public_boundaries']}
    expected = {ap + '.json' for ap in OWNERS}
    actual = {path.name for path in directory.glob('*.json')}
    if actual != expected:
        raise ValueError(f'Contract set mismatch: missing={sorted(expected-actual)}, extra={sorted(actual-expected)}')
    for ap in OWNERS:
        data = json.loads((directory / (ap + '.json')).read_text(encoding='utf-8'))
        if set(data) != CONTRACT_FIELDS:
            raise ValueError(f'{ap}: exactly the eight required contract fields are required')
        for field, value in data.items():
            if not value or re.search(r'\b(TODO|TBD|placeholder)\b', json.dumps(value), flags=re.I):
                raise ValueError(f'{ap}: empty/placeholder {field}')
        evidence = data['reference_evidence']
        if evidence['status'] != 'L1_draft' or evidence['ap_id'] != ap:
            raise ValueError(f'{ap}: status must be L1_draft with matching AP ID')
        if data['owner_module'] != OWNERS[ap]:
            raise ValueError(f'{ap}: owner differs from inventory owner map')
        if not evidence['items']:
            raise ValueError(f'{ap}: no linked reference evidence')
        for entry in evidence['items']:
            item = indexed.get(entry['inventory_id'])
            if not item or ap not in item['ap_domains'] or entry['path'] != item['path']:
                raise ValueError(f'{ap}: evidence not mapped to this domain: {entry}')
        if f'BP-{ap[-2:]}' not in data['acceptance_case_ids']:
            raise ValueError(f'{ap}: representative BP case missing')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference-root', type=Path, default=DEFAULT_REFERENCE)
    parser.add_argument('--inventory', type=Path, default=ROOT / 'docs/engineering/reference-inventory.json')
    parser.add_argument('--contracts-dir', type=Path, default=ROOT / 'docs/engineering/module-contracts')
    parser.add_argument('--write', action='store_true', help='Regenerate inventory after explicitly reviewing changed scope/rules; never writes reference source')
    args = parser.parse_args()
    try:
        manifest = collect(args.reference_root)
        if args.write:
            if args.inventory.resolve().is_relative_to(args.reference_root.resolve()):
                raise ValueError('Reference directory is read-only; inventory output cannot be inside it')
            write_compact_manifest(args.inventory, manifest)
        else:
            frozen = json.loads(args.inventory.read_text(encoding='utf-8'))
            if frozen != manifest:
                frozen_items = {item['path']: item for item in frozen['items'] + frozen['sdk_public_boundaries']}
                live_items = {item['path']: item for item in manifest['items'] + manifest['sdk_public_boundaries']}
                differences = [path for path in sorted(frozen_items.keys() | live_items.keys()) if frozen_items.get(path) != live_items.get(path)]
                raise ValueError('Frozen inventory differs from live source/rules/hashes: ' + ', '.join(differences[:12]))
        all_items = manifest['items'] + manifest['sdk_public_boundaries']
        if not all_items or len({item['inventory_id'] for item in all_items}) != len(all_items):
            raise ValueError('Empty inventory or duplicate inventory IDs')
        if len({item['path'] for item in all_items}) != len(all_items):
            raise ValueError('Duplicate inventory paths')
        if any(not item['ap_domains'] or not item['qcae_owner'] or not item['rationale'] or not re.fullmatch('[0-9a-f]{64}', item['sha256']) for item in all_items):
            raise ValueError('Incomplete mapping/rationale/hash')
        if any(count == 0 for count in manifest['summary']['ap_item_counts'].values()):
            raise ValueError('AP domain lacks reference evidence')
        if not args.write:
            validate_contracts(args.contracts_dir, manifest)
        print(json.dumps({'status': 'pass', 'inventory': str(args.inventory), **manifest['summary'], 'contracts_checked': 0 if args.write else 20, 'claim': 'L1 draft evidence validation only; SK-01/L2 not passed'}, ensure_ascii=False))
        return 0
    except (ValueError, OSError, KeyError, json.JSONDecodeError) as error:
        print('reference inventory validation failed: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
