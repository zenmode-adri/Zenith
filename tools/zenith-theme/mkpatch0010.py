#!/usr/bin/env python3
"""Genera 0010-es-brand-name.patch sobre el arbol de ES ya parcheado (0001-0009)."""
import glob, os, shutil, subprocess, sys

SRC = glob.glob('/home/epsil/r36s-fork/build.ROCKNIX-RK3326.aarch64/build/emulationstation-cada*/')[0].rstrip('/')
WORK = '/home/epsil/es0010'
shutil.rmtree(WORK, ignore_errors=True)
files = ['es-app/src/ApiSystem.cpp', 'es-app/src/guis/GuiMenu.cpp']
for d in ('a', 'b'):
    for f in files:
        os.makedirs(os.path.dirname(f'{WORK}/{d}/{f}'), exist_ok=True)
        shutil.copy(f'{SRC}/{f}', f'{WORK}/{d}/{f}')


def edit(path, old, new):
    p = f'{WORK}/b/{path}'
    s = open(p, encoding='utf-8', newline='').read()
    if old not in s:
        sys.exit(f'NO ENCONTRADO en {path}: {old[:70]!r}')
    open(p, 'w', encoding='utf-8', newline='').write(s.replace(old, new, 1))


NEW_NAME = r'''std::string ApiSystem::getApplicationName()
{
	// distribution name from /etc/os-release (OS_NAME); ROCKNIX only as a fallback
	static std::string name;
	if (name.empty())
	{
		std::ifstream f("/etc/os-release");
		std::string line;
		while (std::getline(f, line))
		{
			if (line.compare(0, 8, "OS_NAME=") != 0)
				continue;

			name = Utils::String::trim(line.substr(8));
			if (name.size() >= 2 && (name.front() == '"' || name.front() == '\'') && name.back() == name.front())
				name = name.substr(1, name.size() - 2);
			break;
		}

		if (name.empty())
			name = "ROCKNIX";
	}

	return name;
}'''

edit('es-app/src/ApiSystem.cpp',
     'std::string ApiSystem::getApplicationName()\n{\n\treturn "ROCKNIX";\n}', NEW_NAME)
edit('es-app/src/ApiSystem.cpp', '#include "ApiSystem.h"\n', '#include "ApiSystem.h"\n#include <fstream>\n')

# pie del menu: de "if (!getVersion().empty())" hasta el cierre del bloque (el else lleva espacios finales)
gm = f'{WORK}/b/es-app/src/guis/GuiMenu.cpp'
txt = open(gm, encoding='utf-8', newline='').read()
start_marker = '\tif (!ApiSystem::getInstance()->getVersion().empty())\n\t{\n\t\tif (ApiSystem::getInstance()->getApplicationName() == "ROCKNIX")'
end_marker = 'label = aboutInfo + buildDate;\n'
i = txt.find(start_marker)
if i < 0:
    sys.exit('NO ENCONTRADO el inicio del bloque del pie del menu en GuiMenu.cpp')
j = txt.index(end_marker, i) + len(end_marker)
k = txt.index('\n\t}\n', j) + len('\n\t}\n')
new_block = ('\tif (!ApiSystem::getInstance()->getVersion().empty())\n'
             '\t\tlabel = ApiSystem::getInstance()->getApplicationName() + " " + ApiSystem::getInstance()->getVersion();\n')
open(gm, 'w', encoding='utf-8', newline='').write(txt[:i] + new_block + txt[k:])

r = subprocess.run(['diff', '-ruN', 'a', 'b'], cwd=WORK, capture_output=True, text=True)
patch = r.stdout.replace('\r\n', '\n')
header = ('Zenith 0010: show the distribution name from os-release (OS_NAME) in the main menu footer and update dialog\n'
          'instead of the hard-coded "ROCKNIX"; the footer reads "<name> <version>" (the internal build tag is not shown).\n\n')
open('/mnt/c/tmp/zt/es/0010-es-brand-name.patch', 'w', encoding='utf-8', newline='\n').write(header + patch)
print('patch bytes', len(patch))
