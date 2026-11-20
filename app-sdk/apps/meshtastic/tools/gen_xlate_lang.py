#!/usr/bin/env python3
"""Build the MESHTASTIC translator's other languages (mt_xlate_lang.c).

Two outputs:

  ../lang/<code>.bin    one file a language (pt, fr, de, it, nl, pl, ru, uk): lexicon,
                        the inflection rules learned from it, the forms those
                        rules cannot make, and the hand tables. They go on the
                        card as /sdcard/meshtastic/lang/<code>.bin (see
                        README.md); the app reads them with its SD API.
  ../mt_xlate_langs.h   built into the app: a small detection profile for
                        every language (so a German line is not read as
                        Spanish when German is off) and the phrasebooks.

Nothing is downloaded here. Fetch into a scratch folder outside the repository:

  kaikki.org-dictionary-<Language>.jsonl.gz
      https://kaikki.org/dictionary/<Language>/  (English Wiktionary's entries
      of that language, extracted by wiktextract; Wiktionary text is
      CC BY-SA 4.0 and GFDL)
  <code>_50k.txt   https://github.com/hermitdave/FrequencyWords content/2018/
      (pt_br, fr, de, it, nl, pl, ru, uk, es, en; word counts from OpenSubtitles 2018,
      CC BY-SA 4.0)

Make a cache of each dump once (it takes a minute), then build:

  python3 -I gen_xlate_lang.py cache --kaikki DIR/kaikki.org-dictionary-French.jsonl.gz --out DIR/French.json
  python3 -I gen_xlate_lang.py lang fr --cache DIR/French.json --freq DIR/fr_50k.txt
  python3 -I -X utf8 gen_xlate_lang.py builtin --freq-dir DIR --cache-dir DIR

Russian and Ukrainian print Cyrillic, so on Windows add -X utf8 (-I ignores
PYTHONIOENCODING). builtin reads the Russian and Ukrainian caches in
--cache-dir (Russian.json, Ukrainian.json) to tell their words apart.

Standard library only (Python 3.10 or newer). The hand tables are
xlate_<code>_en.txt next to this file; the phrasebook's English lines and their
order are xlate_es_en.txt's @book.
"""
import argparse
import collections
import gzip
import json
import os
import re
import struct
import sys
import unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))

LANGS = {
    # code: (Wiktionary language name, FrequencyWords list, regions whose senses are preferred)
    'pt': ('Portuguese', 'pt_br', {'Brazil'}),
    'fr': ('French', 'fr', {'France', 'Europe'}),
    'de': ('German', 'de', {'Germany', 'Germany-German'}),
    'it': ('Italian', 'it', {'Italy'}),
    'nl': ('Dutch', 'nl', {'Netherlands'}),
    'pl': ('Polish', 'pl', {'Poland'}),
    'ru': ('Russian', 'ru', {'Russia'}),
    'uk': ('Ukrainian', 'uk', {'Ukraine'}),
}
# Order of mt_lang_t after MT_LANG_ES (mt_xlate.h); detection profiles also have en and es.
ORDER = ['en', 'es', 'pt', 'fr', 'de', 'it', 'nl', 'pl', 'ru', 'uk']
CYRILLIC = ('ru', 'uk')
SLAVIC_LANGS = ('pl', 'ru', 'uk')

# ---------------------------------------------------------------------------
# Codes shared with mt_xlate_lang.c

POS = {'N': 1, 'A': 2, 'V': 3, 'R': 4, 'I': 5}
TENSES = ['NONE', 'INF', 'PRES', 'PRET', 'IMPF', 'FUT', 'COND', 'SUBJ', 'SUBJI', 'IMP', 'GER', 'PART', 'PLUP']
T = {n: i for i, n in enumerate(TENSES)}
CLASSES = ['PH', 'CL', 'INTJ', 'ADV', 'PREP', 'CONJ', 'Q', 'DET', 'PRON', 'CLIT', 'PRONX', 'NEG', 'NEG2', 'NUM',
           'SKIP', 'EXIST']
CLASS = {n: i + 1 for i, n in enumerate(CLASSES)}
# function-word attribute bits (uint32): class 0..4, person 5..7, then flags
PERSON_SHIFT = 5
FLAGS = {'def': 1 << 8, 'pl': 1 << 9, 'acc': 1 << 10, 'sub': 1 << 11, 'pre': 1 << 12, 'amb': 1 << 13,
         'refl': 1 << 14, 'q': 1 << 15, 'poss': 1 << 16, 'the': 1 << 17, 'neg': 1 << 18, 'A': 1 << 19,
         'S': 1 << 20, 'hyph': 1 << 21, 'obj': 1 << 22, 'never': 1 << 23, 'E': 1 << 24, 'past': 1 << 25,
         'you': 1 << 26, 'int': 1 << 27, 'que': 1 << 28}
ROLES = ['BE', 'HAVE', 'PROG', 'GOING', 'CAN', 'WANT', 'MUST', 'SHOULD', 'WILL', 'MAY', 'PERFBE', 'IT', 'NEED', 'DE',
         'JUST']
ROLE = {n: 1 << i for i, n in enumerate(ROLES)}
LEX_UNCOUNT = 1
MAGIC = 0x314C544D      # "MTL1"
# a preposition and the article in one word, for the phrases that end in that preposition
CONTRACT = {
    'pt': {'de': ['do', 'da', 'dos', 'das'], 'a': ['ao', 'à', 'aos', 'às'], 'em': ['no', 'na', 'nos', 'nas']},
    'fr': {'de': ['du', 'des', "d'"], 'à': ['au', 'aux']},
    'it': {'a': ['al', 'allo', 'alla', "all'", 'ai', 'agli', 'alle'], 'da': ['dal', 'dallo', 'dalla', "dall'", 'dai'],
           'di': ['del', 'dello', 'della', "dell'", 'dei', 'degli', 'delle', "d'"]},
}
SEPARABLE = {
    'de': ['zurück', 'zusammen', 'weiter', 'vorbei', 'heraus', 'herein', 'hinaus', 'nach', 'auf', 'aus', 'ein', 'mit',
           'vor', 'weg', 'los', 'fest', 'her', 'hin', 'ab', 'an', 'zu', 'bei', 'fort', 'heim', 'um', 'durch'],
    'nl': ['terug', 'samen', 'door', 'mee', 'aan', 'af', 'op', 'uit', 'in', 'na', 'toe', 'weg', 'voor', 'over', 'om',
           'bij', 'los', 'vast', 'langs', 'neer', 'tegen'],
}


# ---------------------------------------------------------------------------
# Folding: lower case, accents off; German umlauts as ae/oe/ue; sz, oe, ae ligatures spelled out.
# mt_xlate_lang.c folds the same way for every letter it reads.

# Russian and Ukrainian are read as Latin letters: a word's key is its transliteration (BGN/PCGN
# without marks; ё as e, the soft and hard signs and the Ukrainian apostrophe left out), so a line
# typed in Cyrillic and one typed in chat Latin ("privet, kak dela") meet the same lexicon.
CYR_RU = dict(zip('абвгдеёжзийклмнопрстуфхцчшщъыьэюяєіїґ',
                  ['a', 'b', 'v', 'g', 'd', 'e', 'e', 'zh', 'z', 'i', 'y', 'k', 'l', 'm', 'n', 'o', 'p', 'r', 's',
                   't', 'u', 'f', 'kh', 'ts', 'ch', 'sh', 'shch', '', 'y', '', 'e', 'yu', 'ya', 'ye', 'i', 'yi', 'g']))
CYR_UK = dict(CYR_RU, **{'г': 'h', 'и': 'y'})


def fold(s, lang):
    s = s.lower().replace('’', "'").replace('‘', "'").replace('ʼ', "'")
    if lang in CYRILLIC:
        s = unicodedata.normalize('NFC', s.replace('́', '').replace('̀', ''))   # stress marks off
        table = CYR_UK if lang == 'uk' else CYR_RU
        s = ''.join(table.get(c, c) for c in s).replace("'", '')
        return s if s and all('a' <= c <= 'z' or c in ' -0123456789' for c in s) else None
    if lang == 'de':
        s = s.replace('ä', 'ae').replace('ö', 'oe').replace('ü', 'ue')
    s = s.replace('ß', 'ss').replace('œ', 'oe').replace('æ', 'ae').replace('ł', 'l').replace('đ', 'd')
    s = unicodedata.normalize('NFKD', s)
    s = ''.join(c for c in s if not unicodedata.combining(c))
    if s and all('a' <= c <= 'z' or c in " -'0123456789" for c in s):
        return s
    return None


def is_cyrillic(s):
    return any('Ѐ' <= c <= 'ӿ' for c in s)


def has_acc(s):
    return any(c in 'áàâãäéèêëíìîïóòôõöúùûüçąćęńśźżčřšžěů' for c in s.lower())     # not l with stroke, as C


def to_ascii(s):
    s = unicodedata.normalize('NFKD', s)
    s = ''.join(c for c in s if not unicodedata.combining(c))
    s = s.replace('’', "'").replace('‘', "'").replace('“', '"').replace('”', '"')
    s = s.replace('–', '-').replace('—', '-')
    return s if all(32 <= ord(c) < 127 for c in s) else None


# ---------------------------------------------------------------------------
# Hand tables

def read_hand(path):
    sections = collections.defaultdict(list)
    cur = None
    for n, line in enumerate(open(path, encoding='utf-8'), 1):
        line = line.rstrip('\n')
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        if line.startswith('@'):
            cur = line[1:].strip()
            continue
        if cur is None:
            raise SystemExit('%s:%d: data before a @section' % (path, n))
        sections[cur].append(([f.strip() for f in line.split(' | ')], n))
    return sections


def parse_attr(spec, path, n):
    cls, _, info = spec.partition(':')
    if cls not in CLASS:
        raise SystemExit('%s:%d: unknown class %s' % (path, n, cls))
    a = CLASS[cls]
    for f in filter(None, info.split(',')):
        if f.isdigit():
            a |= int(f) << PERSON_SHIFT
        elif f in FLAGS:
            a |= FLAGS[f]
        else:
            raise SystemExit('%s:%d: unknown flag %s' % (path, n, f))
    return a


# ---------------------------------------------------------------------------
# Wiktionary glosses

BAD_GLOSS = re.compile(r'^(used |expresses |expressing |indicates |indicating |alternative |synonym |plural |'
                       r'feminine |masculine |diminutive |augmentative |superlative |clipping |abbreviation |'
                       r'initialism |acronym |ellipsis |name of |the name |the letter |letter |a type of |any of |'
                       r'someone who |one who |a person who |of or relating |relating to |pertaining to |'
                       r'misspelling |obsolete |archaic |eye dialect |nonstandard |pronunciation |inflection |'
                       r'form of |apocopic |contraction |short for |en dash|compound of |first-person |second-person |'
                       r'third-person |past participle |present participle |gerund |singular |genitive |dative )',
                       re.I)
BAD_TAGS = {'obsolete': 5, 'archaic': 5, 'dated': 3, 'rare': 3, 'historical': 3, 'uncommon': 2, 'literary': 2,
            'figuratively': 1.5, 'derogatory': 1.5, 'vulgar': 1.5, 'offensive': 2, 'euphemistic': 1,
            'humorous': 1, 'poetic': 3, 'nonstandard': 2, 'proscribed': 2, 'Internet': 0.5, 'dialectal': 2.5,
            'regional': 2, 'slang': 1, 'colloquial': 0.3, 'Austria': 1.5, 'Switzerland': 1.5, 'Belgium': 1.5,
            'Portugal': 1.5, 'Quebec': 1.5, 'Flanders': 1.5, 'Southern-Germany': 1, 'Northern-Germany': 0.5,
            'Swiss-Standard-German': 1.5, 'Tuscany': 1, 'Rome': 1, 'Naples': 1.5, 'Sicily': 1.5, 'Northern-Italy': 1,
            'Southern-Italy': 1, 'Canada': 1.5, 'Africa': 1.5, 'Angola': 1.5, 'Mozambique': 1.5}


def clean_gloss(g, pos):
    g = to_ascii(g)
    if g is None:
        return None
    for _ in range(2):
        g = re.sub(r'\([^()]*\)', '', g)
    g = re.sub(r'\[[^\]]*\]', '', g)
    g = g.split(';')[0].split(',')[0].split(' or ')[0] if pos != 'V' else g.split(';')[0].split(',')[0]
    g = re.sub(r'\s+', ' ', g).strip(' .:!"\'')
    if not g or BAD_GLOSS.match(g + ' ') or '"' in g:
        return None
    if pos == 'V':
        if g.startswith('to '):
            g = g[3:]
        g = re.sub(r'\b(oneself|someone|something|somebody|one\'s)\b', '', g)
        g = re.sub(r'\s+', ' ', g).strip()
    if pos == 'N':
        g = re.sub(r'^(a|an|the) ', '', g)
    if not g or len(g) > 24 or len(g.split()) > 3 or re.search(r'[0-9/=<>#@&*|+]', g):
        return None
    if g[0].isupper() and pos not in ('N', 'A') and g not in ('OK',):
        g = g[0].lower() + g[1:]
    return g


def best_gloss(senses, pos, ours, worst=4.0):
    best = None
    for i, (glosses, tags) in enumerate(senses):
        g = glosses[0] if glosses else ''
        if g.endswith(':') and len(glosses) > 1:
            g = glosses[1]
        cg = clean_gloss(g, pos)
        if not cg:
            continue
        c = float(i)
        if any(t in ours for t in tags):
            c -= 2.0
        for t in tags:
            c += BAD_TAGS.get(t, 0)
        if c - i >= worst:
            continue
        if best is None or c < best[0]:
            best = (c, cg)
    return best[1] if best else None


def gender_of(rec):
    for h in rec['h']:
        m = re.match(r'^\S+ (m or f|m or n|f or n|mf|m|f|n)\b', h)
        if m:
            return {'m': 1, 'f': 2, 'n': 3}.get(m.group(1), 0)
    return 0


def pl_person(form, tense):
    """Polish tables often leave the person out; the ending says it."""
    f = form.lower()
    if tense in ('PRET', 'COND'):
        for ends, p in ((('liśmy', 'łyśmy', 'byśmy'), 4), (('liście', 'łyście', 'byście'), 5),
                        (('łem', 'łam', 'łom', 'bym'), 1), (('łeś', 'łaś', 'łoś', 'byś'), 2), (('li', 'ły', 'liby', 'łyby'), 6)):
            if f.endswith(ends):
                return p
        return 3
    for ends, p in ((('my',), 4), (('cie',), 5), (('sz',), 2), (('m', 'ę'), 1), (('ą',), 6)):
        if f.endswith(ends):
            return p
    return 3


def tense_person(tags, lang, form=''):
    """(tense, person, plural) of a form, or None for forms the reader does not use."""
    t = set(tags)
    if t & {'archaic', 'obsolete', 'rare', 'dated', 'auxiliary', 'canonical', 'alternative', 'negative',
            'error-unknown-tag', 'misspelling', 'nonstandard', 'majestic', 'Flanders', 'Portugal', 'romanization',
            'error-unrecognized-form', 'error-unrecognized-tag', 'multiword-construction'}:
        return None
    if lang in ('pl', 'ru', 'uk'):
        if 'adverbial' in t:
            return ('GER', 0, 0)                        # sdelav, robiąc: having done, doing
        if 'participle' in t:
            return ('PART', 0, 1 if 'plural' in t else 0)
        if 'infinitive' in t:
            return ('INF', 0, 0)
        if not t & {'first-person', 'second-person', 'third-person'} and 'imperative' not in t:
            # past and conditional by gender (ru, uk: no person; pl: the ending), present without one (pl)
            for name, tn in (('conditional', 'COND'), ('past', 'PRET'), ('future', 'FUT'), ('present', 'PRES')):
                if name in t:
                    if lang == 'pl':
                        return (tn, pl_person(form, tn), 0)
                    return (tn, 6 if 'plural' in t else 7, 0)
            return None
    if 'diminutive' in t or 'augmentative' in t or 'superlative' in t or 'comparative' in t:
        return None
    if 'gerund' in t and lang == 'nl':
        return None                                     # "het raven": the infinitive
    if 'gerund' in t or ('participle' in t and 'present' in t):
        return ('GER', 0, 0)
    if 'participle' in t:
        return ('PART', 0, 1 if 'plural' in t else 0)
    if 'infinitive' in t:
        return ('INF', 0, 0)
    person = 0
    for n, name in ((1, 'first-person'), (2, 'second-person'), (3, 'third-person')):
        if name in t:
            person = n
    if 'plural' in t:
        person = person + 3 if person else (6 if lang == 'nl' else 0)
    if 'formal' in t or 'second-person-semantically' in t:
        return None
    if 'imperative' in t:
        return ('IMP', person or 2, 0) if person in (0, 2, 5) else None
    if not person:
        return None
    if 'subjunctive' in t or 'subjunctive-i' in t or 'subjunctive-ii' in t:
        if 'future' in t:
            return None
        if 'imperfect' in t or 'subjunctive-ii' in t or 'past' in t:
            return ('SUBJI', person, 0)
        return ('SUBJ', person, 0)
    if 'pluperfect' in t:
        return ('PLUP', person, 0)
    for name, tn in (('conditional', 'COND'), ('future', 'FUT'), ('imperfect', 'IMPF'), ('preterite', 'PRET'),
                     ('historic', 'PRET'), ('past', 'PRET'), ('present', 'PRES')):
        if name in t:
            return (tn, person, 0)
    return None


def noun_form(tags):
    t = set(tags)
    if t & {'archaic', 'obsolete', 'rare', 'dated', 'alternative', 'diminutive', 'augmentative', 'romanization',
            'misspelling', 'nonstandard'}:
        return None
    if 'comparative' in t or 'superlative' in t:
        return None
    return ('NONE', 0, 1 if 'plural' in t and 'singular' not in t else 0)


def load_freq(path):
    freq = {}
    for line in open(path, encoding='utf-8'):
        a = line.split()
        if len(a) == 2 and a[1].isdigit():
            freq[a[0]] = int(a[1])
    return freq


# ---------------------------------------------------------------------------
# The C word reader, mirrored, to find the forms its rules cannot recover

class Reader:
    def __init__(self, lex_keys, rules, min_stem=2):
        self.lex = lex_keys         # (key, pos) -> True
        self.rules = rules          # [(fpre, lpre, fsuf, lsuf, pos, tense, person, pl, weight)] best first
        self.min_stem = min_stem
        self.by_key = collections.defaultdict(list)
        for k, p in lex_keys:
            self.by_key[k].append(p)

    def read(self, w):
        """Readings as mt_xlate_lang.c finds them for a word not in the exception table."""
        pref = ['V', 'A', 'N', 'R', 'I']
        if w in self.by_key:
            return [(w, p, 'NONE' if p != 'V' else 'INF', 0, 0) for p in sorted(self.by_key[w], key=pref.index)]
        out, seen = [], set()
        for (fpre, lpre, fsuf, lsuf, pos, tense, person, pl, wt) in self.rules:
            if len(w) - len(fpre) - len(fsuf) < self.min_stem or not w.endswith(fsuf) or not w.startswith(fpre):
                continue
            stem = w[len(fpre):len(w) - len(fsuf)]
            cand = lpre + stem + lsuf
            if (cand, pos) in self.lex and pos not in seen:
                seen.add(pos)
                out.append((cand, pos, tense, person, pl))
                if len(out) == 3:
                    break
        return out


def lcp(a, b):
    n = 0
    while n < len(a) and n < len(b) and a[n] == b[n]:
        n += 1
    return n


def decompose(f, l, lang):
    """(fpre, lpre, fsuf, lsuf) turning the form f into the lemma l, with the longest shared stem."""
    best = None
    k = lcp(f, l)
    best = (k, '', '', f[k:], l[k:])
    if lang in ('de', 'nl'):
        for p in [''] + SEPARABLE[lang]:
            for infix in ('ge', 'zu', 'te'):
                fp = p + infix
                if not f.startswith(fp) or not l.startswith(p):
                    continue
                k2 = lcp(f[len(fp):], l[len(p):])
                if k2 > best[0]:
                    best = (k2, fp, p, f[len(fp) + k2:], l[len(p) + k2:])
    k, fp, lp, fs, ls = best
    return (fp, lp, fs, ls) if k >= 2 else None


# ---------------------------------------------------------------------------

def cmd_cache(a):
    POS_NAMES = {'noun': 'N', 'adj': 'A', 'verb': 'V', 'adv': 'R', 'intj': 'I'}
    skip_form_tags = {'table-tags', 'inflection-template', 'class', 'combined-form', 'multiword-construction'}
    out = []
    with gzip.open(a.kaikki, 'rt', encoding='utf-8') as f:
        for line in f:
            d = json.loads(line)
            p = POS_NAMES.get(d.get('pos'), 'X')
            senses = []
            for s in d.get('senses', []):
                tags = s.get('tags') or []
                if s.get('form_of') or 'form-of' in tags or s.get('alt_of') or 'alt-of' in tags:
                    continue
                g = s.get('glosses') or []
                if g:
                    senses.append([g, tags])
            if not senses:
                continue
            forms = [[fm.get('form', ''), fm.get('tags') or []] for fm in d.get('forms', [])
                     if not (skip_form_tags & set(fm.get('tags') or []))]
            out.append({'w': d.get('word', ''), 'p': p, 's': senses, 'f': forms,
                        'h': [h.get('expansion', '') for h in d.get('head_templates', [])][:1]})
    json.dump(out, open(a.out, 'w', encoding='utf-8'), ensure_ascii=False)
    print('%s: %d entries' % (a.out, len(out)), file=sys.stderr)


def cmd_lang(a):
    lang = a.code
    wname, _, ours = LANGS[lang]
    data = os.path.join(HERE, 'xlate_%s_en.txt' % lang)
    hand = read_hand(data)
    freq = load_freq(a.freq)
    if lang in SLAVIC_LANGS:
        # Wiktionary marks the stress (говори́ть) and the lists drop the Polish letters often enough:
        # forms are counted by their folded spelling
        ffreq = collections.Counter()
        for w, c in freq.items():
            fw = fold(w, lang)
            if fw:
                ffreq[fw] += c
        said = said_w = lambda s: ffreq.get(fold(s, lang) or "", 0)
    else:
        ffreq = freq
        said = lambda s: freq.get(s.lower(), 0)
        said_w = lambda s: freq.get(s, 0)
    recs = [r for r in json.load(open(a.cache, encoding='utf-8')) if r['p'] in POS]
    F = lambda s: fold(s, lang)

    # ---- hand tables
    func = {}                       # (folded key, acc) -> (english, alt, attr)
    for (f, n) in hand['func']:
        k = F(f[0])
        if k is None or len(f) < 3:
            raise SystemExit('%s:%d: bad line' % (data, n))
        acc = has_acc(f[0])
        attr = parse_attr(f[1], data, n) | (FLAGS['acc'] if acc else 0)
        if (k, acc) in func:
            raise SystemExit('%s:%d: %s twice' % (data, n, f[0]))
        func[(k, acc)] = ('' if f[2] == '_' else f[2], f[3] if len(f) > 3 else '', attr)     # _ says nothing
    if lang == 'de':
        # fur, uber, naturlich: the function words typed without their umlauts too, when free
        for (f, n) in hand['func']:
            if any(ch in f[0] for ch in 'äöüÄÖÜ'):
                plain = F(f[0].replace('ä', 'a').replace('ö', 'o').replace('ü', 'u').replace('Ä', 'A')
                          .replace('Ö', 'O').replace('Ü', 'U'))
                if plain and not any(kk == plain for kk, _ in func):
                    func[(plain, False)] = func[(F(f[0]), has_acc(f[0]))]
    # perto de -> perto do/da/dos/das "near the"; près de -> près du/des; vicino a -> vicino al/alla ...
    contract = {F(p): v for p, v in CONTRACT.get(lang, {}).items()}
    for (k, acc), (en, alt, attr) in list(func.items()):
        words = k.split(' ')
        if (attr & 31) != CLASS['PREP'] or len(words) < 2 or words[-1] not in contract:
            continue
        for c in contract[words[-1]]:
            key = ' '.join(words[:-1] + [F(c)])
            the = '' if c.endswith("'") and words[-1] in ('de', 'di') and c in ("d'",) else ' the'
            if (key, acc) not in func and (key, not acc) not in func:
                func[(key, acc)] = (en + the, alt, attr | (FLAGS['the'] if the else 0))
    func_keys = {k for k, _ in func}
    abbr = {}
    for (f, n) in hand['abbr']:
        k, v = F(f[0]), f[1]
        if k is None or k in abbr:
            raise SystemExit('%s:%d: bad or repeated chat spelling %s' % (data, n, f[0]))
        abbr[k] = v
    hand_lex = {}
    for (f, n) in hand['lex']:
        k = F(f[0])
        pos, _, info = f[1].partition(':')
        if k is None or pos not in POS:
            raise SystemExit('%s:%d: bad lex line' % (data, n))
        g, flags = 0, 0
        for x in filter(None, info.split(',')):
            if x in ('m', 'f', 'n'):
                g = {'m': 1, 'f': 2, 'n': 3}[x]
            elif x == 'u':
                flags |= LEX_UNCOUNT
            else:
                raise SystemExit('%s:%d: unknown flag %s' % (data, n, x))
        hand_lex[(k, pos)] = (f[2], g, flags)
    drop = {(F(f[0]), f[1]) for (f, n) in hand['drop']}
    roles = {}
    for (f, n) in hand['verbs']:
        k = F(f[0])
        r = 0
        for x in f[1].split(','):
            if x not in ROLE:
                raise SystemExit('%s:%d: unknown role %s' % (data, n, x))
            r |= ROLE[x]
        roles[k] = roles.get(k, 0) | r

    # ---- lemmas ranked by how often their forms are said
    headwords = collections.defaultdict(set)
    for r in recs:
        headwords[r['w']].add(r['p'])
    weight = lambda r: said_w(r['w']) + 1
    claims = collections.Counter()
    for r in recs:
        for fm in {fm for fm, _ in r['f']} | {r['w']}:
            claims[fm] += weight(r)
    score = collections.Counter()
    best = {}
    for r in recs:
        k = F(r['w'])
        if ' ' in r['w'] or len(r['w']) < 2 or not k or (lang == 'de' and r['p'] == 'N' and not r['w'][0].isupper()):
            continue
        if lang != 'de' and r['w'][0].isupper() and r['p'] != 'I':
            continue                                    # names, demonyms
        forms = {r['w']} | {fm for fm, _ in r['f'] if ' ' not in fm}
        sc = said(r['w']) / max(1, len(headwords[r['w']]))
        for fm in forms - {r['w']}:
            if fm in headwords or F(fm) in func_keys:
                continue
            sc += said(fm) * weight(r) / max(1, claims[fm])
        key = (k, r['p'])
        if sc > score[key]:
            score[key] = sc
            best[key] = r
    lex = {}                                            # (key, pos) -> (gloss, gender, flags, score)
    kept = 0
    verb_forms = set()          # forms of verbs: interjections and adverbs spelled so are dropped (estou, vamos)
    for r in recs:
        if r['p'] == 'V':
            verb_forms.update(F(fm) for fm, tags in r['f'] if ' ' not in fm and fm != r['w'] and tense_person(tags, lang))
    for (k, p), sc in score.most_common():
        if kept >= a.lemmas:
            break
        if (k in func_keys and (k, p) not in hand_lex) or (k, p) in drop:
            continue
        if p in ('I', 'R') and k in verb_forms and (k, p) not in hand_lex:
            continue
        g = best_gloss(best[(k, p)]['s'], p, ours)
        if not g or (p == 'R' and g.startswith('the ')):
            continue
        lex[(k, p)] = (g, gender_of(best[(k, p)]) if p == 'N' else 0, 0, sc)
        kept += 1
    for (k, p), (g, gen, flags) in hand_lex.items():
        old = lex.get((k, p))
        lex[(k, p)] = (g, gen, flags, old[3] if old else 1e9)
    for k in list(roles):
        if (k, 'V') not in lex:
            print('%s: role verb %s is not in the lexicon, left out' % (os.path.basename(data), k), file=sys.stderr)
            del roles[k]
    # the same gloss under two parts of speech: keep the first by preference
    pref = ['V', 'A', 'N', 'R', 'I']
    bykey = collections.defaultdict(list)
    for (k, p) in lex:
        bykey[k].append(p)
    for k, ps in bykey.items():
        ps.sort(key=pref.index)
        seen = set()
        for p in ps:
            g = lex[(k, p)][0]
            if g in seen and (k, p) not in hand_lex:
                del lex[(k, p)]
            seen.add(g)

    # ---- every reading of every form of the kept lemmas
    readings = collections.defaultdict(set)             # folded form -> {(lemma, pos, tense, person, pl)}
    for (k, p) in list(lex):
        r = best.get((k, p))
        readings[k].add((k, p, 'NONE' if p != 'V' else 'INF', 0, 0))
        if r is None:
            continue
        per_form = collections.defaultdict(set)
        for fm, tags in r['f']:
            ff = F(fm) if fm and ' ' not in fm and fm != '-' else None
            if not ff or ff == k or ("'" in ff and "'" not in k) or ('-' in ff and '-' not in k):
                continue                                # m'entends, dis-moi: the reader splits those
            tp = tense_person(tags, lang, fm) if p == 'V' else noun_form(tags)
            if tp:
                per_form[ff].add(tp)
        if lang == 'it' and p == 'V':
            # Wiktionary lists the participle once (arrivato): its other genders and numbers too
            for ff, tps in list(per_form.items()):
                if ff.endswith('o') and any(t == 'PART' for t, _, _ in tps):
                    for end, pl in (('a', 0), ('i', 1), ('e', 1)):
                        per_form[ff[:-1] + end].add(('PART', 0, pl))
        for ff, tps in per_form.items():
            # first and third singular alike (IMPF, SUBJ, ...): person 7
            for (t, pp, pl) in list(tps):
                if pp == 1 and (t, 3, pl) in tps:
                    tps.discard((t, 1, pl))
                    tps.discard((t, 3, pl))
                    tps.add((t, 7, pl))
            for (t, pp, pl) in tps:
                readings[ff].add((k, p, t, pp, pl))

    # ---- rules: (fpre, lpre, fsuf, lsuf, pos, tense, person, pl), by how many lemmas use them
    support = collections.Counter()
    mass = collections.Counter()
    for ff, rd in readings.items():
        for (k, p, t, pp, pl) in rd:
            if ff == k:
                continue
            d = decompose(ff, k, lang)
            if not d:
                continue
            key = d + (p, t, pp, pl)
            support[key] += 1
            mass[key] += ffreq.get(ff, 0)
    tense_pref = ['PRES', 'INF', 'NONE', 'PART', 'GER', 'PRET', 'IMPF', 'IMP', 'FUT', 'COND', 'SUBJ', 'SUBJI', 'PLUP']
    # one rule a change and part of speech: of its readings, the most useful tense among the well supported
    groups = collections.defaultdict(list)
    for key in support:
        if support[key] >= a.min_support:
            groups[key[:5]].append(key)
    rules = []
    for g, keys in groups.items():
        top = max(support[k] for k in keys)
        keys = [k for k in keys if support[k] * 2 >= top]
        keys.sort(key=lambda k: (tense_pref.index(k[5]), k[6] == 0, -support[k]))
        k = keys[0]
        rules.append(k + (sum(support[x] for x in groups[g]),))
    rules.sort(key=lambda r: (-r[8], -mass[r[:8]], tense_pref.index(r[5]), r))
    rules = rules[:a.max_rules]
    reader = Reader(set(lex), rules)

    # ---- forms the rules get wrong: the exceptions
    lscore = {kp: v[3] for kp, v in lex.items()}
    core = {k for (k, p), v in sorted(lex.items(), key=lambda kv: -kv[1][3])[:150]}
    core_verbs = {k for (k, p), v in sorted(((kp, v) for kp, v in lex.items() if kp[1] == 'V'),
                                            key=lambda kv: -kv[1][3])[:200]}

    def rank(x):
        # a form of a common verb reads as that verb (preciso: I need, not "precise")
        k, p, t, pp, pl = x
        return (not (p == 'V' and k in core_verbs), -lscore.get((k, p), 0), pref.index(p),
                tense_pref.index(t), pp)

    exc = {}
    checked = 0
    for ff, rd in readings.items():
        said_n = ffreq.get(ff, 0)
        if not said_n and not any(x[0] in core for x in rd):
            continue
        checked += 1
        want = sorted(rd, key=rank)
        top = want[0]
        got = reader.read(ff)
        ok = got and got[0][:2] == top[:2] and any(
            x[:2] == got[0][:2] and x[2] == got[0][2] and (x[3] == got[0][3] or (x[3] == 7 and got[0][3] in (1, 3)) or
                                                          (got[0][3] == 7 and x[3] in (1, 3))) for x in rd)
        if ok:
            continue
        lst = [top]
        for x in want[1:]:
            if x[1] != top[1] and lscore.get(x[:2], 0) >= 0.05 * lscore.get(top[:2], 1):
                lst.append(x)
                break
        exc[ff] = lst
    # German without umlauts (fur, schon for schön): the plain-vowel spelling of a common word, when free
    if lang == 'de':
        top_words = sorted(((freq.get(w, 0), w) for w in freq), reverse=True)[:8000]
        for c, w in top_words:
            fw = F(w)
            if not fw or not re.search('ae|oe|ue', fw) or not any(ch in w for ch in 'äöü'):
                continue
            plain = F(w.replace('ä', 'a').replace('ö', 'o').replace('ü', 'u'))
            if plain in readings or plain in exc or plain in func_keys or (plain, 'N') in lex or freq.get(plain, 0) > c:
                continue
            got = exc.get(fw) or reader.read(fw)
            if got:
                exc[plain] = got[:2]

    for w in a.why or []:
        fw = F(w)
        print('why %s: readings %s; rules read %s; exception %s' % (
            fw, sorted(readings.get(fw, []), key=rank)[:6], reader.read(fw), exc.get(fw)), file=sys.stderr)

    # ---------------------------------------------------------------------------
    # Emit the file
    pool = bytearray()

    def put(*fields):
        off = len(pool)
        for f in fields:
            if isinstance(f, str):
                f = f.encode('utf-8')
            assert b'\0' not in f
            pool.extend(f)
            pool.append(0)
        return off

    lex_items = sorted(lex.items(), key=lambda kv: (kv[0][0].encode(), pref.index(kv[0][1])))
    lex_index = {}
    lex_words = []
    for i, ((k, p), (g, gen, flags, sc)) in enumerate(lex_items):
        off = put(k, g)
        assert off < (1 << 22)
        lex_words.append(off | (POS[p] << 22) | (gen << 26) | (flags << 28))
        lex_index[(k, p)] = i
    assert len(lex_words) < 65535
    exc_offs, exc_lex = [], []
    for ff in sorted(exc, key=lambda s: s.encode()):
        off = put(ff)
        for (k, p, t, pp, pl) in exc[ff][:2]:
            exc_offs.append(off | (T[t] << 22) | (pp << 26) | (pl << 29))
            exc_lex.append(lex_index[(k, p)])
    rule_offs, rule_info = [], []
    for (fpre, lpre, fsuf, lsuf, p, t, pp, pl, wt) in rules:
        rule_offs.append(put(fpre, lpre, fsuf, lsuf))
        rule_info.append(POS[p] | (T[t] << 4) | (pp << 8) | (pl << 11) | (min(wt, 65535) << 16))
    func_items = sorted(func.items(), key=lambda kv: (kv[0][0].encode(), kv[0][1]))
    func_offs, func_attr = [], []
    for (k, acc), (en, alt, attr) in func_items:
        func_offs.append(put(k, en, alt))
        func_attr.append(attr)
    abbr_offs = [put(k, abbr[k]) for k in sorted(abbr, key=lambda s: s.encode())]
    verb_words = sorted((lex_index[(k, 'V')] << 16) | r for k, r in roles.items())
    credit = ('%s lexicon: English glosses adapted from Wiktionary (https://en.wiktionary.org), CC BY-SA 4.0, '
              'as extracted by wiktextract / kaikki.org; lemmas chosen by FrequencyWords counts (Hermit Dave, '
              'CC BY-SA 4.0). Hand tables written for LakeShark, GPL-3.0-or-later.' % wname)
    credit_off = put(credit)
    while len(pool) % 4:
        pool.append(0)

    sections = [('lex', lex_words, 'I'), ('exc', exc_offs, 'I'), ('excl', exc_lex, 'H'), ('rule', rule_offs, 'I'),
                ('rinfo', rule_info, 'I'), ('func', func_offs, 'I'), ('funca', func_attr, 'I'),
                ('abbr', abbr_offs, 'I'), ('verb', verb_words, 'I')]
    head_words = 4 + 2 * len(sections) + 1
    body = bytearray()
    table = []
    at = head_words * 4 + len(pool)
    for name, vals, typ in sections:
        blob = struct.pack('<%d%s' % (len(vals), typ), *vals)
        while len(blob) % 4:
            blob += b'\0'
        table += [at + len(body), len(vals)]
        body += blob
    header = struct.pack('<4I', MAGIC, struct.unpack('<I', lang.encode().ljust(4, b'\0'))[0],
                         int(a.source_date.replace('-', '')), len(pool))
    header += struct.pack('<%dI' % (2 * len(sections)), *table) + struct.pack('<I', credit_off)
    assert len(header) == head_words * 4
    blob = header + bytes(pool) + bytes(body)
    out = a.out or os.path.join(HERE, '..', 'lang', '%s.bin' % lang)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    open(out, 'wb').write(blob)
    pc = collections.Counter(p for (k, p) in lex)
    print('%s: %d lemmas %s, %d rules, %d exceptions of %d forms checked, %d function words, %d chat spellings, '
          '%d bytes' % (os.path.relpath(out), len(lex_words), dict(pc), len(rules), len(exc), checked, len(func),
                        len(abbr), len(blob)), file=sys.stderr)
    if a.report:
        for r in rules[:40]:
            print('  rule', r, file=sys.stderr)
        for ff in sorted(exc)[::max(1, len(exc) // 40)]:
            print('  exc %-14s %s' % (ff, exc[ff]), file=sys.stderr)
        line = ''
        for (k, p), (g, gen, flags, sc) in sorted(lex.items(), key=lambda kv: -kv[1][3])[:400]:
            t = '%s.%s=%s' % (k, p, g)
            if len(line) + len(t) > 150:
                print(line, file=sys.stderr)
                line = ''
            line += t + ' | '
        print(line, file=sys.stderr)


# ---------------------------------------------------------------------------
# Built-in header: detection profiles and phrasebooks

def cmd_builtin(a):
    freqs = {}
    for code in ORDER:
        name = LANGS[code][1] if code in LANGS else code
        freqs[code] = load_freq(os.path.join(a.freq_dir, '%s_50k.txt' % name))
    totals = {c: sum(f.values()) for c, f in freqs.items()}
    hands = {c: read_hand(os.path.join(HERE, 'xlate_%s_en.txt' % c)) for c in LANGS}
    # words of each language: the top of its list, folded as that language folds,
    # weighed by how often they are said there (log scale, 1..15)
    import math
    prof = collections.defaultdict(dict)                # folded word -> {lang: weight}

    def weight(share):
        return max(1, min(15, int(round((math.log10(share) + 5.0) * 3.4))))

    # Russian and Ukrainian: the subtitle lists quote each other (the Ukrainian one has что at rank 7),
    # so a Cyrillic word counts for a language only when that language's Wiktionary has it as a word or
    # a form of one; a word both have counts for both, each by how often it is said there
    cyr_words = {}
    if a.cache_dir:
        for c in CYRILLIC:
            ws = set()
            for r in json.load(open(os.path.join(a.cache_dir, '%s.json' % LANGS[c][0]), encoding='utf-8')):
                for fm in [r['w']] + [f[0] for f in r['f']]:
                    if ' ' not in fm and is_cyrillic(fm):
                        ws.add(unicodedata.normalize('NFC', unicodedata.normalize('NFD', fm.lower())
                                                     .replace('́', '').replace('̀', '')))
            for sec in ('func', 'abbr', 'lex', 'verbs'):
                for f, n in hands[c][sec]:
                    if is_cyrillic(f[0]) and ' ' not in f[0]:
                        ws.add(f[0].lower())
            cyr_words[c] = ws

    def cyr_ok(w, c):
        return c not in cyr_words or w in cyr_words[c]

    for c in ORDER:
        n = 0
        other = {'ru': 'uk', 'uk': 'ru'}.get(c)
        for w, cnt in sorted(freqs[c].items(), key=lambda x: -x[1]):
            if n >= a.words:
                break
            if (c in CYRILLIC) != is_cyrillic(w):
                continue                                # English in the Russian list, Cyrillic in the others
            share = cnt / totals[c]
            if other and (not cyr_ok(w, c) if cyr_words else freqs[other].get(w, 0) / totals[other] >= 2 * share):
                continue                                # the Ukrainian list quotes Russian (что, ты) and back
            # a Cyrillic word is looked for by its transliteration, marked ~ so typed Latin never meets it
            fw = '~' + (fold(w, c) or '') if c in CYRILLIC else fold(w, 'de')  # umlauts as German folds them
            # elisions count as their own words (c', l', dell'), as the lists and the C reader split them
            if not fw or fw == '~' or ("'" in fw[:-1]) or ' ' in fw or \
                    (len(fw) < 2 and fw not in ('a', 'e', 'o', 'y', 'u', 'i')):
                continue
            q = weight(share)
            if prof[fw].get(c, 0) < q:
                prof[fw][c] = q
            n += 1
            # the same word in the other language, when it is one there too
            if other and cyr_words and cyr_ok(w, other) and freqs[other].get(w, 0):
                fo = '~' + (fold(w, other) or '')
                qo = weight(freqs[other][w] / totals[other])
                if fo != '~' and prof[fo].get(other, 0) < qo:
                    prof[fo][other] = qo
    # then deeper down each list: the words only one of the two has (годину, чекаю; утро, жду)
    for c in (CYRILLIC if cyr_words else ()):
        other = 'uk' if c == 'ru' else 'ru'
        n = 0
        for w, cnt in sorted(freqs[c].items(), key=lambda x: -x[1]):
            if n >= a.cyr_words:
                break
            share = cnt / totals[c]
            if share < 4e-6:
                break
            # the other's Wiktionary may have it as an old or rare word (година, чекать), or miss a word
            # it has: then it must be said there far less (the Ukrainian list quotes Russian at about 0.4)
            o_share = freqs[other].get(w, 0) / totals[other]
            if not is_cyrillic(w) or not cyr_ok(w, c) or (w in cyr_words[other] and o_share * 8 >= share) or \
                    o_share >= share * (0.6 if c == 'ru' else 0.25):
                continue
            fw = '~' + (fold(w, c) or '')
            if fw == '~' or len(fw) < 3 or prof[fw].get(c, 0):
                continue
            prof[fw][c] = max(7, weight(share))         # a word the other has not says much, however rare
            n += 1
    for c in LANGS:
        for (f, n) in hands[c]['detect']:
            fw = '~' + fold(f[0], c) if c in CYRILLIC and is_cyrillic(f[0]) else fold(f[0], 'de')
            prof[fw][c] = max(prof[fw].get(c, 0), int(f[1]))
    words = sorted((w for w in prof if not w.startswith('~')), key=lambda s: s.encode())
    pool = bytearray()
    offs = []
    for w in words:
        offs.append(len(pool))
        pool += w.encode() + b'\0'
    packed = []
    for w in words:
        v = 0
        for i, c in enumerate(ORDER):
            v |= prof[w].get(c, 0) << (4 * i)
        packed.append(v)
    # words written in Cyrillic: their own table, keyed by transliteration, a byte of weights (ru, uk << 4)
    cwords = sorted((w[1:] for w in prof if w.startswith('~')), key=lambda s: s.encode())
    cpool = bytearray()
    coffs = []
    for w in cwords:
        coffs.append(len(cpool))
        cpool += w.encode() + b'\0'
    cpacked = [prof['~' + w].get('ru', 0) | prof['~' + w].get('uk', 0) << 4 for w in cwords]
    # phrasebook: the English lines and order of xlate_es_en.txt @book
    es = read_hand(os.path.join(HERE, 'xlate_es_en.txt'))
    english = [f[1] for f, n in es['book']]
    books = {}
    for c in LANGS:
        got = {}
        for (f, n) in hands[c]['book']:
            if f[0] not in english:
                raise SystemExit('xlate_%s_en.txt:%d: "%s" is not an English line of the phrasebook' % (c, n, f[0]))
            got[f[0]] = f[1]
        missing = [e for e in english if e not in got]
        if missing:
            raise SystemExit('xlate_%s_en.txt: no phrase for %s' % (c, missing))
        books[c] = [got[e] for e in english]
    bpool = bytearray()
    boffs = []
    for c in LANGS:
        for s in books[c]:
            boffs.append(len(bpool))
            bpool += s.encode('utf-8') + b'\0'

    def cstr(b):
        out, line = [], ''
        for i, ch in enumerate(b):
            if ch == 0:
                s = '\\0'
                if i + 1 < len(b) and chr(b[i + 1]) in '01234567':
                    s = '\\000'
            elif ch in (34, 92):
                s = '\\' + chr(ch)
            elif ch == 63:
                s = '\\?'
            elif 32 <= ch < 127:
                s = chr(ch)
            else:
                s = '\\%03o' % ch
            line += s
            if ch == 0 and len(line) > 90:
                out.append('"%s"' % line)
                line = ''
        if line:
            out.append('"%s"' % line)
        return '\n'.join(out)

    def arr(name, typ, vals, per=10):
        lines = ['static const %s %s[%d] = {' % (typ, name, len(vals))]
        for i in range(0, len(vals), per):
            lines.append('    ' + ','.join(str(v) for v in vals[i:i + per]) + ',')
        lines.append('};')
        return '\n'.join(lines)

    text = ['''/* Generated by app-sdk/apps/meshtastic/tools/gen_xlate_lang.py builtin. Do not
   edit: change the sources and run the generator again.

   Detection profiles: the %d most said words of each language (%s), weighed by
   how often they are said, from FrequencyWords by Hermit Dave
   (https://github.com/hermitdave/FrequencyWords, content CC BY-SA 4.0, counted
   from OpenSubtitles 2018), plus chat spellings from xlate_<code>_en.txt. Each
   word has a 4-bit weight a language, in the order of XLD_LANGS. Words written
   in Cyrillic are in XLC_*, keyed by transliteration, with Russian's weight in
   the low four bits and Ukrainian's in the high; besides the most said words,
   the %d most said words of each that are not words of the other (by
   Wiktionary's forms, from kaikki.org).
   Phrasebooks: written for this app (xlate_<code>_en.txt @book), in the order
   of xlate_es_en.txt's @book. %d words, %d phrases. */
#pragma once
#include <stdint.h>
''' % (a.words, ', '.join(ORDER), a.cyr_words, len(words) + len(cwords), len(boffs))]
    text.append('#define XLD_NLANG %d' % len(ORDER))
    text.append('#define XLB_NLANG %d     /* the card languages, in mt_lang_t order after Spanish */' % len(LANGS))
    text.append('#define XLB_NBOOK %d' % len(english))
    text.append('static const char XLD_POOL[%d] =\n%s;' % (len(pool) + 1, cstr(bytes(pool))))
    text.append(arr('XLD_WORD', 'uint16_t', offs, 14))
    text.append(arr('XLD_W', 'uint64_t', ['%dULL' % v for v in packed], 8))
    text.append('static const char XLC_POOL[%d] =\n%s;' % (len(cpool) + 1, cstr(bytes(cpool))))
    text.append(arr('XLC_WORD', 'uint16_t', coffs, 14))
    text.append(arr('XLC_W', 'uint8_t', cpacked, 20))
    text.append('static const char XLB_POOL[%d] =\n%s;' % (len(bpool) + 1, cstr(bytes(bpool))))
    text.append(arr('XLB_BOOK', 'uint16_t', boffs, 14))
    assert len(pool) < 65536 and len(bpool) < 65536 and len(cpool) < 65536
    out = os.path.join(HERE, '..', 'mt_xlate_langs.h')
    open(out, 'w', encoding='ascii', newline='\n').write('\n'.join(text) + '\n')
    print('%s: %d + %d Cyrillic detection words, %d phrases, %d + %d + %d bytes of strings' % (
        os.path.relpath(out), len(words), len(cwords), len(boffs), len(pool), len(cpool), len(bpool)),
        file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('cache', help='compact a kaikki dump into a JSON cache')
    c.add_argument('--kaikki', required=True)
    c.add_argument('--out', required=True)
    l = sub.add_parser('lang', help='build lang/<code>.bin')
    l.add_argument('code', choices=sorted(LANGS))
    l.add_argument('--cache', required=True)
    l.add_argument('--freq', required=True)
    l.add_argument('--out')
    l.add_argument('--lemmas', type=int, default=6000)
    l.add_argument('--min-support', type=int, default=3)
    l.add_argument('--max-rules', type=int, default=1500)
    l.add_argument('--source-date', default='2026-11-19', help='date of the kaikki extraction')
    l.add_argument('--report', action='store_true')
    l.add_argument('--why', nargs='*', help='print how these words are read')
    b = sub.add_parser('builtin', help='build mt_xlate_langs.h')
    b.add_argument('--freq-dir', required=True)
    b.add_argument('--words', type=int, default=220)
    b.add_argument('--cache-dir', help='the cache files (<Language>.json): tells Russian words from Ukrainian ones')
    b.add_argument('--cyr-words', type=int, default=600, help='words only Russian or only Ukrainian has, each')
    a = ap.parse_args()
    {'cache': cmd_cache, 'lang': cmd_lang, 'builtin': cmd_builtin}[a.cmd](a)


if __name__ == '__main__':
    main()
