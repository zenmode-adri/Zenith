#!/usr/bin/env python3
"""Keeps zfront's translations in step with the code (needs GNU gettext: xgettext, msgunfmt).

    cpp/tools/update-locale.py [--es-locale DIR] [--new LANG] [--prune]

1. runs tools/extract-strings.sh -> locale/zfront.pot (every key the code can show: tr(), ntr(), ctr(), N_(), NC_()),
2. reads ES's own catalog of each language (`DIR/<lang>/LC_MESSAGES/emulationstation2.mo`, DIR = --es-locale, $ZF_ES_LOCALE
   or the image's /usr/config/locale): a key ES already translates is inherited and must NOT be repeated in zfront.po,
3. appends to locale/<lang>/LC_MESSAGES/zfront.po the keys still missing there (empty msgstr: fill them in), reports the
   entries that are obsolete (key gone from the code) or shadow an ES translation (`--prune` deletes them),
4. writes locale/es-inherited.txt: the keys ES translates in Spanish, one per line (`ctx|` prefix for a context, `\\n` escaped).
   tests/test_i18n_keys.cpp uses it to prove every key of the code has a Spanish translation without needing ES's catalog.

`--new LANG` starts locale/LANG/LANG/zfront.po (LANG = the folder of the language in ES's catalog: fr, de, pt_BR...).
"""
import argparse, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
LOC = os.path.join(HERE, '..', 'locale')


def unq(s):
    return re.sub(r'\\(n|t|"|\\)', lambda m: {'n': '\n', 't': '\t', '"': '"', '\\': '\\'}[m.group(1)], s)


def q(s):
    s = s.replace('\\', '\\\\').replace('"', '\\"').replace('\t', '\\t')
    parts = s.split('\n')
    if len(parts) == 1:
        return '"%s"' % s
    return '\n'.join(['""'] + ['"%s%s"' % (p, '\\n' if i < len(parts) - 1 else '') for i, p in enumerate(parts)])


def parse_po(text):
    """-> [(ctx, msgid, msgstr, plural_msgstr0)] (header entry excluded), strings unescaped"""
    out = []
    for block in re.split(r'\n\s*\n', text):  # xgettext / msgunfmt / msgfmt-friendly files separate entries by a blank line
        cur, field = {}, None
        for line in block.split('\n'):
            m = re.match(r'(msgctxt|msgid_plural|msgid|msgstr(?:\[\d\])?)\s+"(.*)"$', line)
            if m:
                field = m.group(1); cur[field] = unq(m.group(2)); continue
            m = re.match(r'"(.*)"$', line)
            if m and field:
                cur[field] += unq(m.group(1))
        if cur.get('msgid'):
            out.append((cur.get('msgctxt'), cur['msgid'], cur.get('msgstr', ''), cur.get('msgstr[0]', '')))
    return out


def es_translated(es_locale, lang):
    for cand in (lang, lang.split('_')[0]):
        mo = os.path.join(es_locale, cand, 'LC_MESSAGES', 'emulationstation2.mo')
        if os.path.isfile(mo):
            po = subprocess.run(['msgunfmt', mo], capture_output=True, text=True, check=True).stdout
            return {(c, i) for c, i, s, p in parse_po(po) if s or p}
    return None


def esc_key(ctx, mid):
    return (ctx + '|' if ctx else '') + mid.replace('\\', '\\\\').replace('\n', '\\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--es-locale', default=os.environ.get('ZF_ES_LOCALE', '/usr/config/locale'))
    ap.add_argument('--new', metavar='LANG')
    ap.add_argument('--prune', action='store_true')
    a = ap.parse_args()
    pot_path = os.path.join(LOC, 'zfront.pot')
    subprocess.run(['sh', os.path.join(HERE, 'extract-strings.sh'), pot_path], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    pot = parse_po(open(pot_path, encoding='utf-8').read())
    keys = [(c, i) for c, i, s, p in pot]
    langs = sorted(d for d in os.listdir(LOC) if os.path.isfile(os.path.join(LOC, d, 'LC_MESSAGES', 'zfront.po')))
    if a.new and a.new not in langs:
        os.makedirs(os.path.join(LOC, a.new, 'LC_MESSAGES'), exist_ok=True)
        open(os.path.join(LOC, a.new, 'LC_MESSAGES', 'zfront.po'), 'w', encoding='utf-8').write(
            'msgid ""\nmsgstr ""\n"Project-Id-Version: zfront\\n"\n"Language: %s\\n"\n"MIME-Version: 1.0\\n"\n'
            '"Content-Type: text/plain; charset=UTF-8\\n"\n"Content-Transfer-Encoding: 8bit\\n"\n'
            '"Plural-Forms: nplurals=2; plural=(n != 1);\\n"\n\n' % a.new)
        langs = sorted(langs + [a.new])
    for lang in langs:
        es = es_translated(a.es_locale, lang)
        if es is None:
            print('%s: ES has no catalog for this language in %s: every key is needed' % (lang, a.es_locale))
            es = set()
        path = os.path.join(LOC, lang, 'LC_MESSAGES', 'zfront.po')
        text = open(path, encoding='utf-8').read()
        have = {(c, i): (s, p) for c, i, s, p in parse_po(text)}
        need = [k for k in keys if k not in es]
        add = [k for k in need if k not in have]
        obsolete = [k for k in have if k not in keys]
        shadow = [k for k in have if k in es]
        if add:
            with open(path, 'a', encoding='utf-8', newline='\n') as f:
                for c, i in add:
                    f.write('\n' + ('msgctxt %s\n' % q(c) if c else '') + 'msgid %s\nmsgstr ""\n' % q(i))
        todo = sum(1 for k in need if not (have.get(k, ('', ''))[0] or have.get(k, ('', ''))[1]))
        print('%s: %d keys needed, %d added empty, %d still untranslated, %d obsolete, %d shadow an ES translation' %
              (lang, len(need), len(add), todo, len(obsolete), len(shadow)))
        for k in obsolete + shadow:
            print('   %s %r' % ('obsolete' if k in obsolete else 'shadows ES', k[1][:70]))
        if a.prune and (obsolete or shadow):
            drop, kept = set(obsolete + shadow), []
            for b in re.split(r'\n\s*\n', text):
                e = parse_po(b)
                if not (e and (e[0][0], e[0][1]) in drop):
                    kept.append(b)
            open(path, 'w', encoding='utf-8', newline='\n').write('\n\n'.join(kept).rstrip('\n') + '\n')
        if lang == 'es':
            inh = sorted(esc_key(c, i) for c, i in keys if (c, i) in es)
            open(os.path.join(LOC, 'es-inherited.txt'), 'w', encoding='utf-8', newline='\n').write('\n'.join(inh) + '\n')
            print('es: %d keys inherited from ES -> locale/es-inherited.txt' % len(inh))


if __name__ == '__main__':
    sys.exit(main())
