"""The app API as data, read from the SDK's own headers and README.

    python app-sdk/apidoc.py            prints a summary
    import apidoc; apidoc.entries()     [{name, kind, signature, doc, header}, ...]

Used by lsdk (the VS Code snippets) and LakeShark Studio (autocompletion
and the API docs pane), so both say what the headers say and nothing else.
"""
import json
import re
from pathlib import Path

SDK = Path(__file__).resolve().parent
HEADERS = ["lakeshark_app.h", "lakeshark_ui.h", "lap_canvas.h", "lap3d.h"]
PREFIX = re.compile(r"\b(lap_|l3_|lc_|l3v\b)")
DECL = re.compile(r"^\s*(?:static\s+inline\s+)?(?:const\s+)?[A-Za-z_][\w\s\*]*?\b((?:lap|l3|lc)[a-z0-9_]*|l3v)\s*\(")


def _clean(comment):
    text = re.sub(r"^\s*/\*+|\*+/\s*$", "", comment.strip())
    lines = [re.sub(r"^\s*\*\s?", "", l) for l in text.splitlines()]
    return re.sub(r"[ \t]+", " ", " ".join(l.strip() for l in lines)).strip()


def _strip_comments(s):
    return re.sub(r"/\*.*?\*/", "", s, flags=re.S)


def parse(path):
    src = Path(path).read_text(encoding="utf-8")
    lines = src.splitlines()
    out, seen = [], set()
    comment, comment_end = "", -2
    i = 0
    enum_doc = None
    in_enum = False
    struct_doc = ""

    def add(name, kind, signature, doc):
        if name in seen or name.startswith("lap_main") or name == "lap_icon_px":
            return
        seen.add(name)
        out.append({"name": name, "kind": kind, "signature": signature.strip(), "doc": doc.strip(),
                    "header": Path(path).name})

    while i < len(lines):
        line = lines[i]
        s = line.strip()
        if s.startswith("/*"):                      # a block comment: the doc of what follows
            block = [line]
            while "*/" not in lines[i] and i + 1 < len(lines):
                i += 1
                block.append(lines[i])
            comment, comment_end = _clean("\n".join(block)), i
            i += 1
            continue
        trailing = ""
        m = re.search(r"/\*(.*?)\*/\s*$", line)
        if m:
            trailing = m.group(1).strip()
        doc_above = comment if comment_end >= i - 4 else ""
        if not s:
            i += 1
            continue
        if re.match(r"(typedef\s+)?enum\b", s):
            in_enum = "}" not in s
            enum_doc = comment if comment_end >= i - 1 else ""
            body = _strip_comments(s)
            for name in re.findall(r"\b((?:LAP|L3|LC)_[A-Z0-9_]+)\b", body.split("{", 1)[-1]):
                add(name, "constant", name, trailing or enum_doc)
            i += 1
            continue
        if in_enum:
            if "}" in s:
                in_enum = False
            for name in re.findall(r"\b((?:LAP|L3|LC)_[A-Z0-9_]+)\s*(?:=|,|$)", _strip_comments(s)):
                add(name, "constant", name, trailing or enum_doc or "")
            i += 1
            continue
        m = re.match(r"#define\s+((?:LAP|L3|LC)_[A-Z0-9_]+)(\([^)]*\))?\s*(.*)$", s)
        if m:
            name, args, value = m.group(1), m.group(2) or "", _strip_comments(m.group(3)).strip()
            if not name.endswith("_H") and name not in ("LAP_API_21", "LAP_API_22"):
                add(name, "macro", name + args + ("  " + value if value and len(value) < 60 and not args else ""),
                    trailing or doc_above)
            i += 1
            continue
        if re.match(r"typedef\s+struct\b[^;]*\{", s):
            struct_doc = doc_above
        m = re.match(r"\}\s*((?:lap|l3)_[a-z0-9_]+_t|l3_[a-z]+)\s*;", s)
        if m:
            add(m.group(1), "type", m.group(1), struct_doc or doc_above)
            struct_doc = ""
            i += 1
            continue
        m = re.match(r"typedef\s+struct\s+(\w+)\s+((?:lap|l3)_\w+)\s*;", s)
        if m:
            add(m.group(2), "type", m.group(2), doc_above)
            i += 1
            continue
        m = DECL.match(line)
        if m and not s.startswith(("return", "if", "for", "while", "#")):
            stmt, j = line, i
            while not re.search(r"[;{]", _strip_comments(stmt)) and j + 1 < len(lines) and j - i < 6:
                j += 1
                stmt += " " + lines[j].strip()
            sig = re.split(r"[;{]", _strip_comments(stmt), maxsplit=1)[0]
            sig = re.sub(r"\s+", " ", sig.replace("static inline ", "")).strip()
            m2 = re.search(r"/\*(.*?)\*/", stmt)
            add(m.group(1), "function", sig, (m2.group(1).strip() if m2 else "") or doc_above)
            i = j + 1
            continue
        i += 1
    return out


def entries():
    out = []
    for h in HEADERS:
        p = SDK / "include" / h
        if p.exists():
            out += parse(p)
    return out


def readme_sections():
    """app-sdk/README.md split at its ## headings: [{title, markdown}]."""
    text = (SDK / "README.md").read_text(encoding="utf-8")
    parts = re.split(r"(?m)^## ", text)
    out = [{"title": "LakeShark apps", "markdown": parts[0].split("\n", 1)[1] if "\n" in parts[0] else ""}]
    for p in parts[1:]:
        title, _, body = p.partition("\n")
        out.append({"title": title.strip(), "markdown": body})
    return out


def placeholder_args(sig):
    """'void lap_box(int x, int y, ...)' -> 'lap_box(${1:x}, ${2:y}, ...)' for a snippet."""
    m = re.match(r".*?\b(\w+)\s*\((.*)\)\s*$", sig)
    if not m:
        return None
    name, args = m.group(1), m.group(2).strip()
    if args in ("", "void"):
        return name + "()"
    parts = []
    for k, a in enumerate([a.strip() for a in args.split(",")], 1):
        word = re.findall(r"(\w+)\s*(?:\[[^\]]*\])?$", a)
        parts.append("${%d:%s}" % (k, word[0] if word else "arg"))
    return "%s(%s)" % (name, ", ".join(parts))


def vscode_snippets():
    snips = {}
    for e in entries():
        if e["kind"] != "function":
            continue
        body = placeholder_args(e["signature"])
        if body:
            snips[e["name"]] = {"prefix": e["name"], "body": [body], "description": (e["signature"] + "  " + e["doc"]).strip(),
                                "scope": "c"}
    snips["LAP_APP"] = {"prefix": "LAP_APP", "scope": "c", "description": "Declare the app: manifest and callbacks",
                        "body": ['LAP_APP(.id = "${1:myapp}", .name = "${2:MY APP}", .sub = "${3:what it does}", .version = "0.1",',
                                 '        .author = "${4:you}", .color = LAP_${5:CYAN},',
                                 '        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);']}
    snips["lapdraw"] = {"prefix": "lapdraw", "scope": "c", "description": "A draw() with the board's keys along the bottom",
                        "body": ["static void draw(void)", "{", "    const int w = lap_cols(), h = lap_rows();",
                                 "    const int kh = lap_ui_keys_rows(${1:2}), ky = h - kh;", "    lap_clear(LAP_UI_DIM);",
                                 "    $0", "    lap_ui_keys(0, ky, w, kh, keys, ${1:2}, -1);", "}"]}
    return snips


if __name__ == "__main__":
    es = entries()
    kinds = {}
    for e in es:
        kinds[e["kind"]] = kinds.get(e["kind"], 0) + 1
    print(json.dumps(kinds), len(readme_sections()), "README sections")
    for e in es[:8]:
        print(e["kind"], e["signature"], "--", e["doc"][:70])
