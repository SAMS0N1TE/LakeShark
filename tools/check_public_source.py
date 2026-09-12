"""Validate an unpacked public source tree."""
from pathlib import Path
import re
import sys


def check(root):
    errors = []
    blocked = re.compile(r"(^|/)(?:AGENTS?\.md|CLAUDE[^/]*|AI_USE\.md|LAKESHARK_MARKERS\.txt|\.claude)(/|$)|^(?:migration|LCD_4_3_Docs|dist|c6_firmware)/|^bench/(?:queue|done|parked|failed|patches)/|^bench/(?:agent-loop|run|worktree|claim|new-task)\.ps1$", re.I)
    for path in root.rglob('*'):
        if not path.is_file():
            continue
        rel = path.relative_to(root).as_posix()
        if any(p == '.git' or p.startswith('build') or p == '__pycache__' for p in path.relative_to(root).parts):
            continue
        if blocked.search(rel) or rel.startswith(('managed_components/chmorgan__esp-libhelix-mp3/', 'managed_components/chmorgan__esp-audio-player/')):
            errors.append(rel)
    for name in ('README.md', 'LICENSE', 'CMakeLists.txt', 'main/idf_component.yml', 'boards/t_display_p4.defaults'):
        if not (root / name).is_file():
            errors.append('Missing ' + name)
    for path in [root / 'README.md', *(root / 'docs').glob('*.md')]:
        if not path.exists():
            continue
        for link in re.findall(r'\]\(([^)]+)\)', path.read_text(encoding='utf-8')):
            if '://' in link or link.startswith('#') or link.startswith('mailto:'):
                continue
            target = link.split('#', 1)[0].strip('<>')
            if target and not (path.parent / target).exists():
                errors.append(f'{path.name}: missing {target}')
    return errors


if __name__ == '__main__':
    root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
    errors = check(root)
    print('\n'.join(errors) if errors else 'PUBLIC SOURCE OK')
    sys.exit(bool(errors))
