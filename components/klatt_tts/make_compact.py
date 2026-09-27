"""Regenerate g2p/src/g2p_compact_data.h from the upstream dictionary.

The upstream table (micro/g2p/src/g2p_dict_data.h, derived from CMUdict) holds
the 10,855 common words whose CMUdict pronunciation disagrees with the
letter-to-sound rules. This keeps only the ones LakeShark says. Any word not in
the result is still spoken, through the rules.

    python make_compact.py [path/to/g2p_dict_data.h]

With no path, the pinned upstream revision is downloaded.
"""
import re
import sys
import urllib.request
from pathlib import Path

REV = "234f60faa0eb388b01cdf7e60aca232af37aefda"
URL = f"https://raw.githubusercontent.com/moonshine-ai/moonshine/{REV}/micro/g2p/src/g2p_dict_data.h"

WORDS = """
lake shark receiver ready scanning scan frequency signal lost found battery low
warning aircraft altitude feet heading north south east west traffic alert
channel digital analog radio connected disconnected recording started stopped
enabled disabled map card missing error storage full volume squelch search hold
resume weather emergency one two three four five six seven eight nine zero ten
hundred thousand point minus decimal alpha bravo charlie delta echo foxtrot golf
hotel india juliet kilo lima mike november oscar papa quebec romeo sierra tango
uniform victor whiskey xray yankee zulu the a an is are on off at to from miles
knots detected distance system device power satellite fix acquired position
unknown please check antenna no active available audio test this hello world
eight nineteen seventy seven seventeen eleven twelve thirteen fourteen fifteen
sixteen eighteen twenty thirty forty fifty sixty eighty ninety
welcome new contact questionable military commercial general aviation and more
position fix alfa juliett niner test this is the a d s b voice check
""".split()


def main():
    here = Path(__file__).parent
    if len(sys.argv) > 1:
        s = Path(sys.argv[1]).read_text()
    else:
        s = urllib.request.urlopen(URL, timeout=60).read().decode()

    body = [int(x) for x in re.search(r"kG2pBody\[\]\s*=\s*\{(.*?)\};", s, re.S).group(1).split(",") if x.strip()]
    count = int(re.search(r"kG2pNumEntries = (\d+)", s).group(1))
    entries = {}
    off = 0
    prev = ""
    for _ in range(count):
        shared, length = body[off:off + 2]
        off += 2
        key = prev[:shared] + bytes(body[off:off + length]).decode("ascii")
        off += length
        n = body[off]
        off += 1
        entries[key] = body[off:off + n]
        off += n
        prev = key

    selected = sorted(set(WORDS) & entries.keys())
    out = []
    offsets = []
    prev = ""
    for i, key in enumerate(selected):
        shared = 0
        if i % 16 == 0:
            offsets.append(len(out))
        else:
            while shared < min(len(prev), len(key)) and prev[shared] == key[shared]:
                shared += 1
        suffix = key[shared:].encode()
        phones = entries[key]
        out.extend([shared, len(suffix), *suffix, len(phones), *phones])
        prev = key

    phones_line = re.search(r"inline const char\* const kG2pPhones\[\].*?;", s).group(0)
    result = "// Derived from upstream CMUdict data by make_compact.py. Retain licenses/CMUdict.txt.\n"
    result += "#include <cstdint>\nnamespace g2p {\n" + phones_line + "\n"
    result += f"inline constexpr int kG2pNumPhones=39, kG2pBlockSize=16, kG2pNumEntries={len(selected)}, kG2pNumBlocks={len(offsets)};\n"
    result += "inline constexpr uint32_t kG2pBlockOffsets[]={" + ",".join(map(str, offsets)) + "};\n"
    result += "inline constexpr uint8_t kG2pBody[]={" + ",".join(map(str, out)) + "};\n}\n"
    (here / "g2p" / "src" / "g2p_compact_data.h").write_text(result, newline="\n")
    print(f"{len(selected)} words, {len(out)} bytes: {' '.join(selected)}")


if __name__ == "__main__":
    main()
