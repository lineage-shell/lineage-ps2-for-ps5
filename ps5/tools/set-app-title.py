#!/usr/bin/env python3
"""Set the app's name in a package folder's sce_sys/param.json (titleName, every language).

usage: set-app-title.py <app folder> "<name>"
The in-app name follows the build variable PS5SX2_APP_TITLE (coreorbis/Makefile.vk); the default for both is "PlayStation 2".
"""
import json, sys
folder, name = sys.argv[1], sys.argv[2]
path = folder.rstrip('/') + '/sce_sys/param.json'
with open(path) as f:
    p = json.load(f)
for lang, v in p.get('localizedParameters', {}).items():
    if isinstance(v, dict) and 'titleName' in v:
        v['titleName'] = name
with open(path, 'w') as f:
    json.dump(p, f, indent=2)
    f.write('\n')
print(f'{path}: titleName = {name}')
