"""Prepare the explicitly selected vehicle packages; never write to the game."""
from pathlib import Path, PurePosixPath
import hashlib
import json
import stat
import zipfile
import argparse
import os

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output-root', required=True, type=Path)
parser.add_argument('--downloads', type=Path, default=Path.home() / 'Downloads')
parser.add_argument('--vortex-mods', type=Path, default=Path(os.environ.get('APPDATA', Path.home() / 'AppData/Roaming')) / 'Vortex/cyberpunk2077/mods')
args = parser.parse_args()
ROOT = args.output_root.expanduser().resolve()
DOWNLOADS = args.downloads.expanduser().resolve()
VORTEX = args.vortex_mods.expanduser().resolve()
STAGE = ROOT / 'stage'
MOD_ROOTS = ('archive/pc/mod/', 'r6/scripts/', 'r6/tweaks/', 'r6/input/',
             'red4ext/plugins/',
             'bin/x64/plugins/cyber_engine_tweaks/mods/')
SPECS = [
    ('toyota-yaris', 'Toyota GR Yaris', '3.0', 34357, 'zip', 'Toyota Yaris GR V3 34357 3.0 2026-10-03T11-28Z gMz5MucmU.zip'),
    ('toyota-rocket-bunny', 'Rocket Bunny body kit', '1', 34357, 'zip', 'GR YARIS Rocket Bunny Kit 34357 1 2026-10-03T11-28Z E91D9d7Zx.zip'),
    ('audi-rs6', 'Audi RS6 Performance', '1.4', 33903, 'zip', 'RS6 Performance 33903 1.4 2026-10-05T07-59Z lcXrcZbH4.zip'),
    ('audi-vehicle-services', 'Audi Vehicle Services', '2', 33974, 'zip', 'Audi Vehicle Services 33974 2 2026-10-05T07-56Z 6ZNKZczDf.zip'),
    ('nomad-warlock', 'Nomad Warlock Conversion', '1.0', 33970, 'zip', 'Nomad Warlock 33970 1.0 2026-09-16T02-34Z 3dwHd9wku.zip'),
    ('audioxl', 'AudioXL', '0.5.1', 33442, 'zip', 'AudioXL 33442 0.5.1 2026-09-28T18-36Z cB3xB0WLM.zip'),
]

def sha(data):
    return hashlib.sha256(data).hexdigest().upper()

def safe_relative(raw):
    name = raw.replace('\\', '/')
    p = PurePosixPath(name)
    if p.is_absolute() or any(x in ('', '.', '..') or ':' in x for x in name.split('/')):
        raise ValueError('Unsafe package path: ' + raw)
    return name

packages, seen = [], {}
STAGE.mkdir(parents=True, exist_ok=True)
for key, name, version, mod_id, kind, source_name in SPECS:
    files, skipped = [], []
    source = (DOWNLOADS if kind == 'zip' else VORTEX) / source_name
    package_root = ROOT / key
    package_root.mkdir(exist_ok=True)
    package = dict(id=key, name=name, version=version, enabled=True,
                   source=f'https://www.nexusmods.com/cyberpunk2077/mods/{mod_id}',
                   sourceKind=kind, sourcePath=str(source), files=files, skipped=skipped)
    if kind == 'zip':
        archive_data = source.read_bytes()
        package['archiveSha256'] = sha(archive_data)
        saved = package_root / source.name
        if saved.exists() and sha(saved.read_bytes()) != package['archiveSha256']:
            raise ValueError('Archive backup differs: ' + str(saved))
        if not saved.exists():
            saved.write_bytes(archive_data)
        package['archive'] = str(saved)
        with zipfile.ZipFile(saved) as z:
            bad = z.testzip()
            if bad:
                raise ValueError('CRC failure: ' + bad)
            entries = []
            for entry in z.infolist():
                if entry.is_dir():
                    continue
                if stat.S_ISLNK(entry.external_attr >> 16):
                    raise ValueError('Symlink in archive: ' + entry.filename)
                entries.append((entry.filename, z.read(entry)))
    else:
        entries = []
        for entry in sorted(source.rglob('*')):
            if entry.is_symlink():
                raise ValueError('Symlink in cached package: ' + str(entry))
            if entry.is_file():
                entries.append((entry.relative_to(source).as_posix(), entry.read_bytes()))
    for raw, data in entries:
        rel = safe_relative(raw)
        if key == 'audioxl' and rel in ('r6/storages/RedscriptConfigFramework/AudioXL.card.json',
                                       'r6/storages/RedscriptConfigFramework/AudioXL.docs.txt'):
            skipped.append(dict(path=rel, reason='optional in-game wiki metadata; framework not installed'))
            continue
        if not rel.startswith(MOD_ROOTS):
            if PurePosixPath(rel).name.lower().startswith(('readme', 'audivehicleservices_readme', 'license')):
                skipped.append(dict(path=rel, reason='documentation retained outside the game'))
                (package_root / PurePosixPath(rel).name).write_bytes(data)
                continue
            raise ValueError('Unexpected package root: ' + rel)
        if rel.casefold() in seen:
            raise ValueError('Package collision: ' + rel)
        seen[rel.casefold()] = key
        dest = STAGE / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        if dest.exists() and sha(dest.read_bytes()) != sha(data):
            raise ValueError('Existing staged file differs: ' + rel)
        dest.write_bytes(data)
        files.append(dict(path=rel, sha256=sha(data), size=len(data)))
    packages.append(package)

grant_source = Path(__file__).resolve().parent / 'CyberpunkVRPort_VehicleGrant/init.lua'
grant_path = 'bin/x64/plugins/cyber_engine_tweaks/mods/vr_center_vehicle_grant/init.lua'
data = grant_source.read_bytes()
dest = STAGE / grant_path
dest.parent.mkdir(parents=True, exist_ok=True)
dest.write_bytes(data)
packages.append(dict(id='vr-center-vehicle-grant', name='VR-center three-vehicle grant',
                     version='1', enabled=True, sourceKind='local-source', sourcePath=str(grant_source),
                     files=[dict(path=grant_path, sha256=sha(data), size=len(data))]))
manifest = dict(schema=1, packages=packages)
manifest_path = ROOT / 'approved-content.prepared.json'
manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf-8')
print(json.dumps(dict(manifest=str(manifest_path), stage=str(STAGE), packages=len(packages),
                     files=sum(len(p['files']) for p in packages),
                     bytes=sum(f['size'] for p in packages for f in p['files'])), ensure_ascii=False))
