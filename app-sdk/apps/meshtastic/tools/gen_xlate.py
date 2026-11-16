#!/usr/bin/env python3
"""Build ../mt_xlate_es_en.h, the data of the MESHTASTIC app's offline
Spanish -> English translator (mt_xlate.c).

Nothing is downloaded here. Fetch the sources into a scratch folder outside
the repository and pass their paths:

  kaikki.org-dictionary-Spanish.jsonl.gz
      https://kaikki.org/dictionary/Spanish/  (English Wiktionary's Spanish
      entries, extracted by wiktextract; Wiktionary text is CC BY-SA 4.0 and GFDL)
  es_50k.txt, en_50k.txt
      https://github.com/hermitdave/FrequencyWords  content/2018/{es,en}/
      (word counts from OpenSubtitles 2018; content CC BY-SA 4.0)

and run, with Python 3.12, standard library only:

  python3 -I gen_xlate.py --kaikki DIR/kaikki.org-dictionary-Spanish.jsonl.gz \\
      --freq DIR/es_50k.txt --freq-en DIR/en_50k.txt

The hand-written tables (function words, phrases, chat spellings, overrides,
English irregulars, phrasebook) are in xlate_es_en.txt next to this file.

The header holds one string pool and tables of 32-bit offsets into it, each
sorted for binary search: a lemma lexicon (no full-form tables; mt_xlate.c
inflects by rule), the verb forms its rules cannot recover, and the hand
tables. --report prints sizes and the lemmas it kept.
"""
import argparse
import collections
import gzip
import json
import os
import re
import sys
import unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))

# ---------------------------------------------------------------------------
# Codes shared with mt_xlate.c (written into the header as #defines)

POS = {'N': 1, 'A': 2, 'V': 3, 'R': 4, 'I': 5}
POS_NAMES = {'noun': 'N', 'adj': 'A', 'verb': 'V', 'adv': 'R', 'intj': 'I'}
# lexicon entry: offset in bits 0..21, then pos (4), gender (2), flags (4)
LEX_SUBJ_AFTER = 1      # verb whose subject usually follows it (llega el bus)
LEX_UNCOUNT = 2         # English noun without a plural
LEX_RANK_SHIFT = 2      # 2 bits: 0 very common .. 3 rare (verb lemma choice)

CLASSES = ['PH', 'CL', 'INTJ', 'ADV', 'PREP', 'CONJ', 'Q', 'DET', 'PRON', 'CLIT', 'PRONX', 'NEG', 'NUM']
CLASS = {n: i + 1 for i, n in enumerate(CLASSES)}
# phrase/function attribute bits (uint16): class in bits 0..3, placement of
# phrases 4..6, ACC (the key is written with an accent) 7, then per class
FLAGS = {
    'A': 1 << 4, 'S': 1 << 5, 'E': 1 << 6, 'acc': 1 << 7,
    'p': 1 << 8, 's': 0, 'def': 1 << 9, 'clit': 1 << 10, 'q': 1 << 11, 'neg': 1 << 12,   # DET, CONJ, PRONX
    'u': 1 << 11, 'io': 1 << 12, 'refl': 1 << 13,                                        # PRON, CLIT
    'del': 1 << 8, 'al': 1 << 9,                                                         # PREP
    'why': 1 << 9, 'que': 1 << 13, 'si': 1 << 14, 'como': 1 << 15,                       # CONJ, INTJ si
    'int': 1 << 8, 'ya': 1 << 9, 'mas': 1 << 10, 'solo': 1 << 13, 'past': 1 << 14,       # ADV
    'adj': 1 << 15,                                                                      # INTJ as adjective
}
PERSON_SHIFT = 8        # PRON/CLIT person 1..6 in bits 8..10

TENSES = ['INF', 'PRES', 'PRET', 'IMPF', 'FUT', 'COND', 'SUBJ', 'SUBJI', 'IMP', 'GER', 'PART']
T = {n: i for i, n in enumerate(TENSES)}
CLS_BITS = {'a': 1, 'e': 2, 'i': 4, 'r': 8}

# Regular verb endings, folded (no accents). acc = the written form carries an
# accent on its last vowel. cls: a/e/i = -ar/-er/-ir stem, r = the rest is the
# whole infinitive (future, conditional). person 7 = first or third singular.
# Order breaks ties between equally good readings: earlier wins.
ENDINGS = [
    ('ando', 0, 'a', 'GER', 0, 0), ('iendo', 0, 'ei', 'GER', 0, 2), ('yendo', 0, 'ei', 'GER', 0, 0),
    ('ado', 0, 'a', 'PART', 0, 0), ('ada', 0, 'a', 'PART', 0, 0), ('ados', 0, 'a', 'PART', 0, 0),
    ('adas', 0, 'a', 'PART', 0, 0), ('ido', 0, 'ei', 'PART', 0, 0), ('ida', 0, 'ei', 'PART', 0, 0),
    ('idos', 0, 'ei', 'PART', 0, 0), ('idas', 0, 'ei', 'PART', 0, 0), ('e', 1, 'r', 'FUT', 1, 0),
    ('as', 1, 'r', 'FUT', 2, 0), ('a', 1, 'r', 'FUT', 3, 0), ('emos', 0, 'r', 'FUT', 4, 0),
    ('eis', 1, 'r', 'FUT', 5, 0), ('an', 1, 'r', 'FUT', 6, 0), ('ia', 0, 'r', 'COND', 7, 0),
    ('ias', 0, 'r', 'COND', 2, 0), ('iamos', 0, 'r', 'COND', 4, 0), ('iais', 0, 'r', 'COND', 5, 0),
    ('ian', 0, 'r', 'COND', 6, 0), ('o', 0, 'aei', 'PRES', 1, 1), ('as', 0, 'a', 'PRES', 2, 1),
    ('es', 0, 'ei', 'PRES', 2, 1), ('as', 1, 'a', 'PRES', 2, 0), ('es', 1, 'e', 'PRES', 2, 0),
    ('is', 1, 'i', 'PRES', 2, 0), ('is', 0, 'i', 'PRES', 2, 0), ('a', 0, 'a', 'PRES', 3, 1),
    ('e', 0, 'ei', 'PRES', 3, 1), ('amos', 0, 'a', 'PRES', 4, 0), ('emos', 0, 'e', 'PRES', 4, 0),
    ('imos', 0, 'i', 'PRES', 4, 0), ('ais', 1, 'a', 'PRES', 5, 0), ('eis', 1, 'e', 'PRES', 5, 0),
    ('an', 0, 'a', 'PRES', 6, 1), ('en', 0, 'ei', 'PRES', 6, 1), ('e', 1, 'a', 'PRET', 1, 0),
    ('i', 1, 'ei', 'PRET', 1, 0), ('aste', 0, 'a', 'PRET', 2, 0), ('iste', 0, 'ei', 'PRET', 2, 0),
    ('astes', 0, 'a', 'PRET', 2, 0), ('istes', 0, 'ei', 'PRET', 2, 0), ('o', 1, 'a', 'PRET', 3, 0),
    ('io', 1, 'ei', 'PRET', 3, 2), ('yo', 1, 'ei', 'PRET', 3, 0), ('amos', 0, 'a', 'PRET', 4, 0),
    ('imos', 0, 'e', 'PRET', 4, 0), ('asteis', 0, 'a', 'PRET', 5, 0), ('isteis', 0, 'ei', 'PRET', 5, 0),
    ('aron', 0, 'a', 'PRET', 6, 0), ('ieron', 0, 'ei', 'PRET', 6, 2), ('yeron', 0, 'ei', 'PRET', 6, 0),
    ('aba', 0, 'a', 'IMPF', 7, 0), ('abas', 0, 'a', 'IMPF', 2, 0), ('abamos', 0, 'a', 'IMPF', 4, 0),
    ('abais', 0, 'a', 'IMPF', 5, 0), ('aban', 0, 'a', 'IMPF', 6, 0), ('ia', 0, 'ei', 'IMPF', 7, 0),
    ('ias', 0, 'ei', 'IMPF', 2, 0), ('iamos', 0, 'ei', 'IMPF', 4, 0), ('iais', 0, 'ei', 'IMPF', 5, 0),
    ('ian', 0, 'ei', 'IMPF', 6, 0), ('e', 0, 'a', 'SUBJ', 7, 1), ('es', 0, 'a', 'SUBJ', 2, 1),
    ('es', 1, 'a', 'SUBJ', 2, 2), ('emos', 0, 'a', 'SUBJ', 4, 2), ('eis', 1, 'a', 'SUBJ', 5, 2),
    ('en', 0, 'a', 'SUBJ', 6, 1), ('a', 0, 'ei', 'SUBJ', 7, 1), ('as', 0, 'ei', 'SUBJ', 2, 1),
    ('as', 1, 'ei', 'SUBJ', 2, 2), ('amos', 0, 'ei', 'SUBJ', 4, 2), ('ais', 1, 'ei', 'SUBJ', 5, 2),
    ('an', 0, 'ei', 'SUBJ', 6, 1), ('ara', 0, 'a', 'SUBJI', 7, 2), ('aras', 0, 'a', 'SUBJI', 2, 2),
    ('aramos', 0, 'a', 'SUBJI', 4, 2), ('aran', 0, 'a', 'SUBJI', 6, 2), ('iera', 0, 'ei', 'SUBJI', 7, 2),
    ('ieras', 0, 'ei', 'SUBJI', 2, 2), ('ieramos', 0, 'ei', 'SUBJI', 4, 2), ('ieran', 0, 'ei', 'SUBJI', 6, 2),
    ('ase', 0, 'a', 'SUBJI', 7, 2), ('ases', 0, 'a', 'SUBJI', 2, 2), ('asemos', 0, 'a', 'SUBJI', 4, 2),
    ('asen', 0, 'a', 'SUBJI', 6, 2), ('iese', 0, 'ei', 'SUBJI', 7, 2), ('ieses', 0, 'ei', 'SUBJI', 2, 2),
    ('iesemos', 0, 'ei', 'SUBJI', 4, 2), ('iesen', 0, 'ei', 'SUBJI', 6, 2), ('a', 1, 'a', 'IMP', 2, 0),
    ('e', 1, 'e', 'IMP', 2, 0), ('i', 1, 'i', 'IMP', 2, 0), ('i', 0, 'i', 'IMP', 2, 0), ('ad', 0, 'a', 'IMP', 5, 0),
    ('ed', 0, 'e', 'IMP', 5, 0), ('id', 0, 'i', 'IMP', 5, 0),
]
# Stem changes undone when looking for the infinitive (last occurrence), then
# spelling changes at the end of the stem. mt_xlate.c applies the same lists.
# The last field of an ending says which stem changes it allows: 1 = present
# (pienso, puedo, juego, pido), 2 = -ir verbs elsewhere (pidió, durmiendo).
STEM_CHANGES = [('ie', 'e', 1), ('ue', 'o', 1), ('ue', 'u', 1), ('i', 'e', 3), ('u', 'o', 2)]
CLITICS = [c.encode() for c in ('melo', 'mela', 'telo', 'tela', 'selo', 'sela', 'selos', 'selas', 'noslo',
                                 'me', 'te', 'se', 'nos', 'los', 'las', 'les', 'lo', 'la', 'le')]
SPELLING = [('qu', 'c'), ('gu', 'g'), ('zc', 'c'), ('c', 'z'), ('j', 'g'), ('g', 'gu'), ('z', 'c'), ('y', '')]

# ---------------------------------------------------------------------------
# Folding: lower case, accents off, n-tilde kept as the single byte 0xF1.

ACC = {'á': 'a', 'é': 'e', 'í': 'i', 'ó': 'o', 'ú': 'u', 'ü': 'u', 'à': 'a', 'è': 'e', 'ì': 'i', 'ò': 'o', 'ù': 'u',
       'ï': 'i'}
VOWELS = set('aeiouáéíóúü')


def fold(s):
    """Folded key as bytes, or None if it has letters outside Spanish."""
    out = bytearray()
    for ch in s.lower():
        ch = ACC.get(ch, ch)
        if ch == 'ñ':
            out.append(0xF1)
        elif 'a' <= ch <= 'z' or ch in " -'0123456789":
            out.append(ord(ch))
        else:
            return None
    return bytes(out)


def acc_last(s):
    """The last vowel of the word carries an accent."""
    for ch in reversed(s.lower()):
        if ch in VOWELS:
            return ch in 'áéíóú'
    return False


def acc_any(s):
    return any(ch in 'áéíóú' for ch in s.lower())


def to_ascii(s):
    s = unicodedata.normalize('NFKD', s)
    s = ''.join(c for c in s if not unicodedata.combining(c))
    s = s.replace('’', "'").replace('‘', "'").replace('“', '"').replace('”', '"').replace('–', '-').replace('—', '-')
    return s if all(32 <= ord(c) < 127 for c in s) else None


# ---------------------------------------------------------------------------
# Hand data

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


def parse_flags(spec, path, n):
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
# Wiktionary (kaikki) extraction

BAD_GLOSS = re.compile(r'^(used |expresses |expressing |indicates |indicating |alternative |synonym |plural |'
                       r'feminine |masculine |diminutive |augmentative |superlative |clipping |abbreviation |'
                       r'initialism |acronym |ellipsis |name of |the name |the letter |letter |a type of |any of |'
                       r'someone who |one who |a person who |of or relating |relating to |pertaining to |'
                       r'misspelling |obsolete |archaic |eye dialect |nonstandard |pronunciation |inflection |'
                       r'form of |apocopic |contraction |short for |en dash)', re.I)
BAD_TAGS = {'obsolete': 5, 'archaic': 5, 'dated': 3, 'rare': 3, 'historical': 3, 'uncommon': 2, 'literary': 2,
            'figuratively': 1.5, 'derogatory': 1.5, 'vulgar': 1.5, 'offensive': 2, 'euphemistic': 1,
            'humorous': 1, 'poetic': 3, 'nonstandard': 2, 'proscribed': 2, 'Internet': 0.5}
OUR_REGIONS = {'Argentina', 'Rioplatense', 'Uruguay', 'Paraguay', 'Latin-America', 'South-America', 'Americas'}
REGIONS = {'Spain', 'Mexico', 'Chile', 'Peru', 'Colombia', 'Venezuela', 'Cuba', 'Caribbean', 'Central-America',
           'Philippines', 'Equatorial-Guinea', 'Bolivia', 'Ecuador', 'Puerto-Rico', 'Dominican-Republic',
           'Costa-Rica', 'Honduras', 'Guatemala', 'Nicaragua', 'Panama', 'El-Salvador', 'Andalusia', 'Canary-Islands',
           'Valencia', 'Navarre', 'Aragon', 'US', 'New-Mexico', 'Louisiana', 'Andes'} | OUR_REGIONS


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


def sense_cost(index, tags):
    c = float(index)
    regions = [t for t in tags if t in REGIONS]
    if regions:
        c += -2.5 if any(t in OUR_REGIONS for t in regions) else 3.0
    for t in tags:
        c += BAD_TAGS.get(t, 0)
    return c


def best_gloss(senses, pos, worst=4.0):
    best = None
    for i, (glosses, tags) in enumerate(senses):
        g = glosses[0] if glosses else ''
        if g.endswith(':') and len(glosses) > 1:
            g = glosses[1]
        cg = clean_gloss(g, pos)
        if not cg:
            continue
        c = sense_cost(i, tags)
        if c - i >= worst:
            continue
        if best is None or c < best[0]:
            best = (c, cg)
    return best[1] if best else None


def extract(kaikki_path):
    """Compact records: word, pos, senses [(glosses, tags)], forms [(form, tags)], heads."""
    skip_form_tags = {'table-tags', 'inflection-template', 'class', 'combined-form'}
    out = []
    with gzip.open(kaikki_path, 'rt', encoding='utf-8') as f:
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
                        'h': [h.get('expansion', '') for h in d.get('head_templates', [])]})
    return out


def gender_of(rec):
    for h in rec['h']:
        m = re.match(r'^\S+ (m or f|mf|m|f)\b', h)
        if m:
            return {'m': 1, 'f': 2}.get(m.group(1), 3)
    tags = set(rec['s'][0][1])
    return (1 if 'masculine' in tags else 0) | (2 if 'feminine' in tags else 0)


def tense_person(tags):
    t = set(tags)
    if 'vos-form' in t:
        t.add('second-person')
        t.add('singular')
    if 'gerund' in t:
        return 'GER', 0
    if 'participle' in t:
        return 'PART', 0
    if 'infinitive' in t:
        return None
    person = 0
    for n, name in ((1, 'first-person'), (2, 'second-person'), (3, 'third-person')):
        if name in t:
            person = n
    if 'plural' in t:
        person += 3
    if not person or person == 5:
        return None
    if 'imperative' in t:
        if t & {'negative', 'formal', 'second-person-semantically'} or person != 2:
            return None
        return 'IMP', 2
    if 'subjunctive' in t:
        if 'future' in t:
            return None
        if 'imperfect' in t:
            return 'SUBJI', person
        if 'present' in t:
            return 'SUBJ', person
        return None
    for name, tn in (('preterite', 'PRET'), ('conditional', 'COND'), ('future', 'FUT'), ('imperfect', 'IMPF'),
                     ('present', 'PRES')):
        if name in t:
            return tn, person
    return None


# ---------------------------------------------------------------------------
# The C verb reader, mirrored, to find the forms it cannot recover

class Reader:
    def __init__(self, verbs):
        self.verbs = verbs          # folded infinitive -> rank (0 best)

    def candidates(self, stem, sets):
        out = [stem]
        for a, b, m in STEM_CHANGES:
            k = stem.rfind(a.encode())
            if k >= 0 and (m & sets):
                out.append(stem[:k] + b.encode() + stem[k + len(a):])
        more = []
        for s in out:
            for a, b in SPELLING:
                if s.endswith(a.encode()):
                    more.append(s[:-len(a)] + b.encode())
        seen, res = set(), []
        for s in out + more:
            if s and s not in seen:
                seen.add(s)
                res.append(s)
        return res

    def read(self, form, wacc, wany):
        """Best (infinitive, tense, person) as mt_xlate.c picks it, or None."""
        best = None
        for order, (end, eacc, cls, tense, person, sets) in enumerate(ENDINGS):
            e = end.encode()
            if not form.endswith(e) or len(form) <= len(e):
                continue
            if wacc:
                score = 2 if eacc else -2
            else:
                score = -1 if (eacc and wany) else 0
            stem = form[:-len(e)]
            infs = []
            if cls == 'r':
                if stem[-2:] in (b'ar', b'er', b'ir'):
                    infs.append(stem)
            else:
                for c in cls:
                    for s in self.candidates(stem, sets):
                        infs.append(s + {'a': b'ar', 'e': b'er', 'i': b'ir'}[c])
            for inf in infs:
                r = self.verbs.get(inf)
                if r is None:
                    continue
                key = (score, -r, -order)
                if best is None or key > best[0]:
                    best = (key, inf, tense, person)
        return None if best is None else best[1:]


# ---------------------------------------------------------------------------

def load_freq(path):
    freq = {}
    if path:
        for line in open(path, encoding='utf-8'):
            a = line.split()
            if len(a) == 2 and a[1].isdigit():
                freq[a[0]] = int(a[1])
    return freq


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--kaikki', required=True)
    ap.add_argument('--freq', required=True)
    ap.add_argument('--freq-en', required=True)
    ap.add_argument('--data', default=os.path.join(HERE, 'xlate_es_en.txt'))
    ap.add_argument('--out', default=os.path.join(HERE, '..', 'mt_xlate_es_en.h'))
    ap.add_argument('--cache', help='JSON cache of the extracted dump (made if missing)')
    ap.add_argument('--lemmas', type=int, default=6500, help='Wiktionary lemmas to keep (by frequency)')
    ap.add_argument('--en-words', type=int, default=1200, help='English words for language detection')
    ap.add_argument('--source-date', default='2026-10-30', help='date of the kaikki extraction')
    ap.add_argument('--report', action='store_true')
    a = ap.parse_args()

    hand = read_hand(a.data)
    freq = load_freq(a.freq)
    freq_en = load_freq(a.freq_en)

    if a.cache and os.path.exists(a.cache):
        recs = json.load(open(a.cache, encoding='utf-8'))
    else:
        recs = extract(a.kaikki)
        if a.cache:
            json.dump(recs, open(a.cache, 'w', encoding='utf-8'), ensure_ascii=False)

    # ---- hand tables
    func_keys = set()
    phr = {}                                    # folded key -> (english, alt, attr)
    for (f, n) in hand['func']:
        k = fold(f[0])
        attr = parse_flags(f[1], a.data, n) | (FLAGS['acc'] if acc_any(f[0]) else 0)
        if (k, acc_any(f[0])) in phr:
            raise SystemExit('%s:%d: %s twice' % (a.data, n, f[0]))
        phr[(k, acc_any(f[0]))] = (f[2], f[3] if len(f) > 3 else '', attr)
        func_keys.add(k)
    for (f, n) in hand['phrase']:
        k = fold(f[0])
        if (k, acc_any(f[0])) in phr:
            raise SystemExit('%s:%d: %s twice' % (a.data, n, f[0]))
        phr[(k, acc_any(f[0]))] = (f[1], '', parse_flags(f[2], a.data, n) | (FLAGS['acc'] if acc_any(f[0]) else 0))
    # cerca de -> cerca del "near the", junto a -> junto al "next to the"
    for (k, acc), (en, alt, attr) in list(phr.items()):
        if (attr & 15) != CLASS['PREP']:
            continue
        for tail, new_tail in ((b' de', b' del'), (b' a', b' al')):
            if k.endswith(tail) and (k[:-len(tail)] + new_tail, acc) not in phr:
                phr[(k[:-len(tail)] + new_tail, acc)] = (en + ' the', alt, attr)
    abbr = {}
    for (f, n) in hand['abbr']:
        k, v = fold(f[0]), fold(f[1])
        if k is None or v is None or k == v or k in abbr:
            raise SystemExit('%s:%d: bad or repeated abbreviation %s' % (a.data, n, f[0]))
        abbr[k] = v
    hand_lex = {}
    for (f, n) in hand['lex'] + hand['lex2']:
        k = fold(f[0])
        pos, _, info = f[1].partition(':')
        g, flags = 0, 0
        for x in filter(None, info.split(',')):
            if x in ('m', 'f'):
                g |= 1 if x == 'm' else 2
            elif x == 'i':
                flags |= LEX_SUBJ_AFTER
            elif x == 'u':
                flags |= LEX_UNCOUNT
            else:
                raise SystemExit('%s:%d: unknown flag %s' % (a.data, n, x))
        hand_lex[(k, pos)] = (f[2], g, flags)     # @lex2 may refine @lex
    drop = set()
    for (f, n) in hand['drop']:
        drop.add((fold(f[0]), f[1]))

    # ---- Wiktionary lemmas, ranked by how often their forms are said
    headwords = collections.defaultdict(set)
    for r in recs:
        headwords[r['w']].add(r['p'])
    recs = [r for r in recs if r['p'] != 'X']
    verb_forms = set()          # forms of verbs, to drop interjection/adverb homographs
    for r in recs:
        if r['p'] == 'V':
            verb_forms.update(fold(fm) for fm, tags in r['f'] if ' ' not in fm and fm != r['w'])
    # A form two lemmas share (cosas: cosa, coser) is shared by how often each
    # lemma's own headword is said.
    weight = lambda r: freq.get(r['w'], 0) + 1
    claims = collections.Counter()
    for r in recs:
        for fm in {fm for fm, _ in r['f']} | {r['w']}:
            claims[fm] += weight(r)
    score = collections.Counter()
    best = {}
    for r in recs:
        if ' ' in r['w'] or len(r['w']) < 2 or not fold(r['w']):
            continue
        forms = {r['w']} | {fm for fm, _ in r['f'] if ' ' not in fm}
        sc = freq.get(r['w'], 0) / max(1, len(headwords[r['w']]))
        for fm in forms - {r['w']}:
            if fm in headwords or fold(fm) in func_keys:    # another headword: its count is that word's
                continue
            sc += freq.get(fm, 0) * weight(r) / max(1, claims[fm])
        key = (fold(r['w']), r['p'])
        if sc > score[key]:
            score[key] = sc
            best[key] = r
    lex = {}                                    # (key, pos) -> (gloss, gender, flags, score)
    kept = 0
    for (k, p), sc in score.most_common():
        if kept >= a.lemmas:
            break
        if k in func_keys and (k, p) not in hand_lex:
            continue
        if (k, p) in drop or (p in ('I', 'R') and k in verb_forms and (k, p) not in hand_lex):
            continue
        if p == 'N' and (k, 'V') in score and k.endswith((b'ar', b'er', b'ir')) and (k, p) not in hand_lex:
            continue                            # el decir, el hacer: the verb reads better
        r = best[(k, p)]
        g = best_gloss(r['s'], p)
        if not g or (p == 'R' and g.startswith('the ')):    # "the hell"
            continue
        lex[(k, p)] = (g, gender_of(r) if p == 'N' else 0, 0, sc)
        kept += 1
    # reflexive verbs whose meaning differs (levantar -> levantarse "get up")
    nrefl = 0
    for (k, p), (g, _, _, sc) in list(lex.items()):
        if p != 'V' or not k.endswith((b'ar', b'er', b'ir')):
            continue
        r = best[(k, p)]
        rs = [(gl, tags) for gl, tags in r['s'] if 'reflexive' in tags and 'impersonal' not in tags]
        rg = best_gloss(rs, 'V') if rs else None
        if rg and rg != g and not rg.startswith('be ') and (k + b'se', 'V') not in lex and (k + b'se', 'V') not in drop:
            lex[(k + b'se', 'V')] = (rg, 0, 0, sc * 0.3)
            nrefl += 1
    for (k, p), (g, gen, flags) in hand_lex.items():
        old = lex.get((k, p))
        lex[(k, p)] = (g, gen, flags, old[3] if old else 1e9)
    # the same gloss under two parts of speech: keep the first by preference
    # (blanco "white": the adjective; it also serves as a noun after an article)
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

    # verb ranks (2 bits) for choosing between two infinitives
    vs = sorted(((v[3], k) for (k, p), v in lex.items() if p == 'V'), reverse=True)
    vrank = {}
    for i, (_, k) in enumerate(vs):
        vrank[k] = 0 if i < 300 else 1 if i < 1000 else 2 if i < 2500 else 3

    # ---- verb forms the rules cannot recover
    reader = Reader(vrank)
    core = set(k for _, k in vs[:120])
    # every reading of every written form of the kept verbs, by folded form
    readings = collections.defaultdict(set)     # folded form -> {(infinitive, tense, person)}
    written = collections.defaultdict(set)      # folded form -> written forms
    wreadings = collections.defaultdict(set)    # written form -> tenses
    for (k, p) in [kp for kp in lex if kp[1] == 'V']:
        r = best.get((k, p))
        if r is None or not k.endswith((b'ar', b'er', b'ir')) or vrank.get(k, 3) == 3 and k not in core:
            continue
        for fm, tags in r['f']:
            ff = fold(fm) if fm and ' ' not in fm else None
            tp = tense_person(tags) if ff else None
            if not tp:
                continue
            readings[ff].add((k,) + tp)         # every reading counts as right ...
            wreadings[fm].add(tp[0])
            if freq.get(fm) or k in core:
                written[ff].add(fm)             # ... but only forms people say are checked
    order = ['PRES', 'PRET', 'IMPF', 'FUT', 'COND', 'IMP', 'SUBJ', 'SUBJI', 'GER', 'PART']
    irregular = {}                              # (folded form, acc) -> [(infinitive, tense, person, count)]
    nforms = 0
    for ff, rd in readings.items():
        for fm in sorted(written[ff]):
            nforms += 1
            got = reader.read(ff, acc_last(fm), acc_any(fm))
            ok = lambda g: any(g[0] == k and g[1] == t and (g[2] == pp or (g[2] == 7 and pp in (1, 3)))
                               for k, t, pp in rd)
            pref_rd = [x for x in rd if x[1] != 'IMP'] or list(rd)     # a command only when nothing else fits
            top = sorted({x[0] for x in pref_rd}, key=lambda k: (vrank.get(k, 3), -lex[(k, 'V')][3]))[0]
            if got and ok(got) and not (got[1] == 'IMP' and wreadings[fm] - {'IMP'}) and \
                    (got[0] == top or vrank.get(got[0], 3) < vrank.get(top, 3)):
                continue                        # (está is "is", not the command; creo is creer)
            if any(t in ('GER', 'IMP') for _, t, _ in rd):     # dándome, volvete: C takes the clitic off first
                lemmas = {k for k, _, _ in rd}
                if any(ff.endswith(cl) and len(ff) > len(cl) + 2 and
                       (reader.read(ff[:-len(cl)], False, False) or ('',))[0] in lemmas for cl in CLITICS):
                    continue
            # the most common lemma, then the most useful tense
            k, t, pp = sorted(rd, key=lambda x: (x[1] == 'IMP', vrank.get(x[0], 3), -lex[(x[0], 'V')][3],
                                                 order.index(x[1]), x[2]))[0]
            if got and got[1] == 'IMP' and t == 'IMP':
                continue
            if t in ('IMPF', 'COND', 'SUBJ', 'SUBJI') and pp in (1, 3) and (k, t, 4 - pp) in rd:
                pp = 7
            if t == 'PART' and ((ff, 'N') in lex or (ff, 'A') in lex) and k not in core:
                continue                        # pagos: the noun reads better
            lst = irregular.setdefault((ff, acc_last(fm)), [])
            if not any(x[:3] == (k, t, pp) for x in lst):
                lst.append((k, t, pp, freq.get(fm, 0)))

    # ---- English detection words (common English that is not also Spanish)
    # Spanish subtitles quote English (you, the, what): a word counts as Spanish
    # when it is said at least a fifth as often, per word of text, in Spanish.
    n_es, n_en = sum(freq.values()) or 1, sum(freq_en.values()) or 1
    es_common = set()
    for w, c in sorted(freq.items(), key=lambda x: -x[1])[:20000]:
        fw = fold(w)
        if fw and c / n_es >= 0.2 * freq_en.get(w, 0) / n_en:
            es_common.add(fw)
    es_common |= set(k for k, _ in phr) | set(abbr)
    en_words = []
    for w, c in sorted(freq_en.items(), key=lambda x: -x[1]):
        if len(en_words) >= a.en_words:
            break
        if not re.fullmatch(r"[a-z']+", w) or (len(w) < 2 and w not in ('i',)):
            continue
        fw = w.encode()
        if fw in es_common:
            continue
        en_words.append(fw)

    # ---------------------------------------------------------------------------
    # Emit
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

    lex_items = sorted(lex.items(), key=lambda kv: (kv[0][0], pref.index(kv[0][1])))
    lex_index = {}
    lex_words = []
    for i, ((k, p), (g, gen, flags, sc)) in enumerate(lex_items):
        if p == 'V':
            flags |= vrank.get(k, 3) << LEX_RANK_SHIFT
        off = put(k, g)
        assert off < (1 << 22)
        lex_words.append(off | (POS[p] << 22) | (gen << 26) | (flags << 28))
        lex_index.setdefault((k, p), i)

    phr_items = sorted(phr.items())
    phr_offs, phr_attr = [], []
    for (k, _), (en, alt, attr) in phr_items:
        phr_offs.append(put(k, en, alt))
        phr_attr.append(attr)

    abbr_offs = [put(k, v) for k, v in sorted(abbr.items())]

    irr_items = sorted(irregular.items())
    irr_offs, irr_lex = [], []
    for (ff, acc), lst in irr_items:
        lst.sort(key=lambda x: (-x[3], vrank.get(x[0], 3), x[1] == 'IMP'))
        for (k, t, pp, _) in lst[:2]:
            off = put(ff)
            irr_offs.append(off | (T[t] << 22) | (pp << 26) | ((1 if acc else 0) << 29))
            irr_lex.append(lex_index[(k, 'V')])

    env = sorted((f[0], f[1], f[2]) for f, _ in hand['enirr'])
    env_offs = [put(*x) for x in env]
    enpl = sorted((f[0], f[1]) for f, _ in hand['enpl'])
    enpl_offs = [put(*x) for x in enpl]
    tener = sorted((fold(f[0]), f[1]) for f, _ in hand['tener'])
    tener_offs = [put(*x) for x in tener]
    hacer = sorted((fold(f[0]), f[1]) for f, _ in hand['hacer'])
    hacer_offs = [put(*x) for x in hacer]
    top_en = set(en_words[:150])                # the commonest: function words, weigh more
    enw_offs = [put(w) | ((1 if w in top_en else 0) << 22) for w in sorted(set(en_words))]
    cats = []
    book_offs = []
    for (f, n) in hand['book']:
        if f[0] not in cats:
            cats.append(f[0])
        book_offs.append(put(f[1], f[2]) | (cats.index(f[0]) << 22))
    cat_offs = [put(c) for c in cats]

    def arr(name, typ, vals, per=12):
        lines = ['static const %s %s[%d] = {' % (typ, name, len(vals))]
        for i in range(0, len(vals), per):
            lines.append('    ' + ','.join(str(v) for v in vals[i:i + per]) + ',')
        lines.append('};')
        return '\n'.join(lines)

    def cstr(b):
        out, line = [], ''
        for i, c in enumerate(b):
            if c == 0:
                ch = '\\0'
                if i + 1 < len(b) and chr(b[i + 1]) in '01234567':
                    ch = '\\000'
            elif c in (34, 92):
                ch = '\\' + chr(c)
            elif c == 63:
                ch = '\\?'
            elif 32 <= c < 127:
                ch = chr(c)
            else:
                ch = '\\%03o' % c
            line += ch
            if c == 0 and len(line) > 90:
                out.append('"%s"' % line)
                line = ''
        if line:
            out.append('"%s"' % line)
        return '\n'.join(out)

    defs = []
    for n, v in POS.items():
        defs.append('#define XP_%s %d' % (n, v))
    for n, v in CLASS.items():
        defs.append('#define XC_%s %d' % (n, v))
    for n, v in FLAGS.items():
        if v:
            defs.append('#define XF_%s 0x%04x' % (n.upper(), v))
    defs.append('#define XF_PERSON_SHIFT %d' % PERSON_SHIFT)
    for n, v in T.items():
        defs.append('#define XT_%s %d' % (n, v))
    defs.append('#define XLEX_SUBJ_AFTER %d' % LEX_SUBJ_AFTER)
    defs.append('#define XLEX_UNCOUNT %d' % LEX_UNCOUNT)
    defs.append('#define XLEX_RANK_SHIFT %d' % LEX_RANK_SHIFT)
    ends = []
    for (e, eacc, cls, tense, person, sets) in ENDINGS:
        bits = 0
        for c in cls:
            bits |= CLS_BITS[c]
        ends.append('    {"%s", %d, %d, XT_%s, %d, %d},' % (e, bits, eacc, tense, person, sets))
    stem_c = ', '.join('{"%s", "%s", %d}' % x for x in STEM_CHANGES)
    spell_c = ', '.join('{"%s", "%s", 0}' % x for x in SPELLING)

    text = []
    text.append('''/* Generated by app-sdk/apps/meshtastic/tools/gen_xlate.py from xlate_es_en.txt (hand-written
   for this app) and English Wiktionary's Spanish entries. Do not edit: change
   the sources and run the generator again.

   Spanish lexicon: English glosses adapted from Wiktionary, the free
   dictionary (https://en.wiktionary.org), by its contributors, as extracted
   by wiktextract / kaikki.org (dump of %s). Wiktionary text is available
   under CC BY-SA 4.0 (https://creativecommons.org/licenses/by-sa/4.0/); this
   adaptation (short glosses, chosen and shortened) is shared under GPL-3.0-or-later,
   which CC BY-SA 4.0 allows (Creative Commons' compatible-licence list).
   Which lemmas are kept follows word counts from FrequencyWords by Hermit
   Dave (https://github.com/hermitdave/FrequencyWords, content CC BY-SA 4.0,
   counted from OpenSubtitles 2018); no counts are stored here. The English
   detection words are the most common words of the same project's English list.

   %d lemmas, %d verb forms the rules cannot make, %d phrases and function
   words, %d chat spellings, %d phrasebook lines. Pool %d bytes. */
#pragma once
#include <stdint.h>
''' % (a.source_date, len(lex_words), len(irr_offs), len(phr_offs),
       len(abbr_offs), len(book_offs), len(pool)))
    text.append('\n'.join(defs))
    text.append('''
typedef struct { char end[8]; uint8_t cls, acc, tense, person, sets; } xl_ending_t;
typedef struct { char from[3], to[3]; uint8_t sets; } xl_change_t;
static const xl_ending_t XL_ENDINGS[%d] = {
%s
};
static const xl_change_t XL_STEM_CHANGES[%d] = {%s};
static const xl_change_t XL_SPELLING[%d] = {%s};
''' % (len(ENDINGS), '\n'.join(ends), len(STEM_CHANGES), stem_c, len(SPELLING), spell_c))
    text.append('static const char XL_POOL[%d] =\n%s;' % (len(pool) + 1, cstr(bytes(pool))))
    text.append(arr('XL_LEX', 'uint32_t', lex_words, 10))
    text.append(arr('XL_PHR', 'uint32_t', phr_offs, 10))
    text.append(arr('XL_PHR_A', 'uint16_t', phr_attr, 14))
    text.append(arr('XL_ABBR', 'uint32_t', abbr_offs, 10))
    text.append(arr('XL_IRR', 'uint32_t', irr_offs, 10))
    text.append(arr('XL_IRR_LEX', 'uint16_t', irr_lex, 14))
    text.append(arr('XL_ENV', 'uint32_t', env_offs, 10))
    text.append(arr('XL_ENPL', 'uint32_t', enpl_offs, 10))
    text.append(arr('XL_TENER', 'uint32_t', tener_offs, 10))
    text.append(arr('XL_HACER', 'uint32_t', hacer_offs, 10))
    text.append(arr('XL_ENW', 'uint32_t', enw_offs, 10))
    text.append(arr('XL_BOOK', 'uint32_t', book_offs, 10))
    text.append(arr('XL_BOOKCAT', 'uint32_t', cat_offs, 10))
    open(a.out, 'w', encoding='ascii', newline='\n').write('\n'.join(text) + '\n')

    sizes = {'pool': len(pool), 'lex': 4 * len(lex_words), 'phr': 6 * len(phr_offs), 'irr': 6 * len(irr_offs),
             'enw': 4 * len(enw_offs)}
    print('%s: %d lemmas (%d reflexive), %d irregular forms of %d checked, %d phrases, %d English words, '
          'data %d bytes' % (os.path.relpath(a.out), len(lex_words), nrefl, len(irr_offs), nforms, len(phr_offs),
                             len(enw_offs), sum(sizes.values())), file=sys.stderr)
    if a.report:
        print(sizes, file=sys.stderr)
        for (ff, acc), lst in irr_items[::12]:
            print('  irr %-12s %s' % (ff.decode('latin-1'), [(k.decode('latin-1'), t, pp) for k, t, pp, _ in lst]),
                  file=sys.stderr)
        pc = collections.Counter(p for (k, p) in lex)
        print(pc, file=sys.stderr)
        line = ''
        for (k, p), (g, gen, flags, sc) in sorted(lex.items(), key=lambda kv: -kv[1][3]):
            if sc >= 1e9 or p == 'V':
                continue
            t = '%s.%s=%s' % (k.decode('latin-1'), p, g)
            if len(line) + len(t) > 150:
                print(line, file=sys.stderr)
                line = ''
            line += t + ' | '
        print(line, file=sys.stderr)


if __name__ == '__main__':
    main()
