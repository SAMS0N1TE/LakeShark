"""Keep working material off the wrong remote.

Three tiers, because "sensitive" is not one thing here:

  NEVER      Logs, coredumps, credentials, and the working log itself. These
             are wrong on any remote including a private one.

  PRIVATE    Agent rules, coprocessor images, vendor dumps, captures, the
             research scripts, release binaries. Useful to carry
             between machines, wrong in public.

  fine       Everything else.

The remote decides what PRIVATE means. A repository is treated as public
unless it is named private below, because guessing wrong in that direction is
the expensive mistake.

Already-tracked files are reported and never fatal when the remote is private:
664 of them predate this check, and a gate that fails forever on history is a
gate people learn to skip. Removing them is a history rewrite on pushed
branches, which is the operator's decision alone.
"""
from pathlib import Path
import re
import subprocess
import sys

# Repositories known to be private. Anything else counts as public.
PRIVATE_REMOTES = [
    r'LakeShark-temp-snapshot',
    r'LS_Test1',
]

# Wrong on every remote, private ones included.
NEVER = [
    (r'^LAKESHARK_MARKERS\.txt$', 'the working log'),
    (r'\.log$', 'a log'),
    (r'^coredump', 'a coredump'),
    (r'\.pem$', 'a key'),
    (r'\.key$', 'a key'),
    (r'(^|/)id_rsa', 'a key'),
    (r'(^|/)\.env($|\.)', 'an environment file'),
    # Name-based credential matching, but never against source files: libsodium
    # ships crypto_secretbox.c, and a checker that cries wolf on vendored crypto
    # is a checker people stop reading.
    (r'(^|/)[^/]*(secret|token|credential|password)[^/]*'
     r'(?<!\.c)(?<!\.h)(?<!\.cpp)(?<!\.hpp)(?<!\.py)(?<!\.md)(?<!\.rs)(?<!\.go)$',
     'credential-shaped'),
]

# Vendored trees are not ours and their names are not evidence of anything.
SKIP = (r'^managed_components/', r'^vendor/', r'^third_party/',
        r'^components/lakeshark/apps/p25/(mbelib|dsd)')

# Fine on a private remote, never in public.
PRIVATE = [
    (r'^AGENTS\.md$', 'agent working rules'),
    (r'^c6_firmware/', 'coprocessor images'),
    (r'^LCD_4_3_Docs/', 'a vendor document dump'),
    (r'^URH_Files/', 'signal captures'),
    (r'^tools/rec_research/', 'research scripts'),
    (r'^dist/', 'release binaries'),
]


def classify(path, remote_is_private):
    if any(re.search(p, path, re.IGNORECASE) for p in SKIP):
        return None, False
    for pattern, why in NEVER:
        if re.search(pattern, path, re.IGNORECASE):
            return why, True
    if not remote_is_private:
        for pattern, why in PRIVATE:
            if re.search(pattern, path, re.IGNORECASE):
                return why + ', and this remote is public', True
    for pattern, why in PRIVATE:
        if re.search(pattern, path, re.IGNORECASE):
            return why, False
    return None, False


def git(*args):
    out = subprocess.run(('git',) + args, capture_output=True, text=True)
    return out.stdout.splitlines() if out.returncode == 0 else []


def upstream_remote():
    """The remote this branch pushes to, and whether it is private."""
    branch = (git('rev-parse', '--abbrev-ref', 'HEAD') or ['HEAD'])[0]
    up = git('rev-parse', '--abbrev-ref', f'{branch}@{{upstream}}')
    if not up:
        return branch, None, None, False
    name = up[0].split('/')[0]
    url = (git('remote', 'get-url', name) or [''])[0]
    private = any(re.search(p, url, re.IGNORECASE) for p in PRIVATE_REMOTES)
    return branch, up[0], url, private


def main():
    branch, up, url, private = upstream_remote()

    if up is None:
        print(f'ERROR: {branch} has no upstream, so the target remote is '
              f'unknown and nothing can be checked. Set one before pushing.')
        return 1

    print(f'remote: {url}  ({"private" if private else "PUBLIC"})')

    added = git('diff', '--name-only', '--diff-filter=A', f'{up}..HEAD')
    fatal, noted = [], []
    for f in added:
        why, is_fatal = classify(f, private)
        if why:
            (fatal if is_fatal else noted).append((f, why))

    tracked_bad = [(f, classify(f, private)[0]) for f in git('ls-files')
                   if classify(f, private)[1]]
    if tracked_bad:
        print(f'note: {len(tracked_bad)} already-tracked file(s) would not be '
              f'allowed here, e.g.')
        for f, why in tracked_bad[:3]:
            print(f'        {f}  ({why})')
        print('      not failing on history; removing them is a rewrite.')

    for f, why in noted:
        print(f'ok:   {f}  ({why}, allowed on a private remote)')

    if fatal:
        print('ERROR: these are about to be pushed and must not be:')
        for f, why in fatal:
            print(f'         {f}   ({why})')
        return 1

    print(f'OK: {len(added)} file(s) added since {up}, none disallowed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
