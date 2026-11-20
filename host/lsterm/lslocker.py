#!/usr/bin/env python3
"""lslocker: the LakeShark locker from a desktop (hub.terminalbay.com). Standard library only.

The board shows a 6-digit code (LOCKER app, or `locker pair` on its console). Then:

    lslocker.py pair 123456        join that board's locker (saved in ~/.lakeshark/locker.json)
    lslocker.py ls                 files in the locker (name, size, age, who put it there)
    lslocker.py push a.gpx b.bin   upload files (<= 25 MB each, 200 MB per locker, kept 14 days)
    lslocker.py pull [name ...]    download (no names: everything the board put there) into -d DIR
    lslocker.py rm name            delete a file
    lslocker.py info               usage
    lslocker.py unpair             forget this desktop (the board keeps its locker)

Other programs can import it: Locker().push(path), .files(), .pull(name, dest), .pair(code).
Exit status is 0 on success, 1 on any failure; errors go to stderr.
"""
import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

HUB = os.environ.get("LS_HUB", "https://hub.terminalbay.com")
CONF = os.path.join(os.path.expanduser("~"), ".lakeshark", "locker.json")
MAX_FILE = 25 * 1024 * 1024


class HubError(Exception):
    pass


def _req(method, path, token=None, body=None, headers=None, timeout=120):
    h = {"User-Agent": "lslocker/1"}
    h.update(headers or {})
    if token:
        h["Authorization"] = "Bearer " + token
    if isinstance(body, (dict, list)):
        body = json.dumps(body).encode()
        h["Content-Type"] = "application/json"
    r = urllib.request.Request(HUB + path, data=body, method=method, headers=h)
    try:
        return urllib.request.urlopen(r, timeout=timeout)
    except urllib.error.HTTPError as e:
        try:
            msg = json.load(e).get("error", "")
        except Exception:
            msg = ""
        raise HubError("HTTP %d %s" % (e.code, msg))
    except urllib.error.URLError as e:
        raise HubError("cannot reach %s: %s" % (HUB, e.reason))


class Locker:
    def __init__(self, conf=CONF):
        self.conf = conf
        self.token = None
        try:
            self.token = json.load(open(conf)).get("secret")
        except (OSError, ValueError):
            pass

    def need(self):
        if not self.token:
            raise HubError("not paired: run  lslocker.py pair <code from the board>")

    def pair(self, code):
        code = "".join(c for c in code if c.isdigit())
        j = json.load(_req("POST", "/v1/locker/claim", body={"code": code}))
        os.makedirs(os.path.dirname(self.conf), exist_ok=True)
        with open(self.conf, "w") as f:
            json.dump({"secret": j["secret"]}, f)
        try:
            os.chmod(self.conf, 0o600)
        except OSError:
            pass
        self.token = j["secret"]
        return j["lid"]

    def files(self):
        self.need()
        return json.load(_req("GET", "/v1/locker/files", self.token))["files"]

    def info(self):
        self.need()
        return json.load(_req("GET", "/v1/locker/info", self.token))

    def push(self, path, name=None):
        self.need()
        name = name or os.path.basename(path)
        size = os.path.getsize(path)
        if size > MAX_FILE:
            raise HubError("%s is %d MB, the limit is 25 MB" % (name, size >> 20))
        with open(path, "rb") as f:
            data = f.read()
        _req("PUT", "/v1/locker/files/" + urllib.parse.quote(name), self.token, data,
             {"Content-Type": "application/octet-stream"}, 600)
        return size

    def pull(self, name, dest):
        self.need()
        r = _req("GET", "/v1/locker/files/" + urllib.parse.quote(name), self.token, timeout=600)
        part = dest + ".part"
        with open(part, "wb") as f:
            while True:
                b = r.read(1 << 16)
                if not b:
                    break
                f.write(b)
        os.replace(part, dest)

    def remove(self, name):
        self.need()
        _req("DELETE", "/v1/locker/files/" + urllib.parse.quote(name), self.token)

    def unpair(self):
        if self.token:
            try:
                _req("POST", "/v1/locker/unpair?self=1", self.token, b"")
            except HubError:
                pass
        try:
            os.remove(self.conf)
        except OSError:
            pass
        self.token = None


def _age(s):
    return "%dd" % (s // 86400) if s >= 86400 else "%dh" % (s // 3600) if s >= 3600 else "%dm" % (s // 60)


def main(argv=None):
    ap = argparse.ArgumentParser(description="LakeShark locker")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("pair").add_argument("code")
    sub.add_parser("ls")
    sub.add_parser("info")
    sub.add_parser("unpair")
    sub.add_parser("push").add_argument("files", nargs="+")
    p = sub.add_parser("pull")
    p.add_argument("names", nargs="*")
    p.add_argument("-d", "--dir", default=".")
    p.add_argument("--all", action="store_true", help="also files this desktop put there")
    sub.add_parser("rm").add_argument("name")
    a = ap.parse_args(argv)
    L = Locker()
    try:
        if a.cmd == "pair":
            print("paired, locker", L.pair(a.code))
        elif a.cmd == "ls":
            for f in L.files():
                print("%-40s %9d  %4s  %s" % (f["name"], f["size"], _age(int(time.time()) - f["ts"]),
                                              "board" if f["src"] == "dev" else "web/desktop"))
        elif a.cmd == "info":
            i = L.info()
            print("%d files, %.1f of %d MB used, files kept %d days" % (i["files"], i["used"] / 1048576, i["quota"] >> 20, i["ttl_s"] // 86400))
        elif a.cmd == "push":
            for f in a.files:
                print("push %s: %d bytes" % (f, L.push(f)))
        elif a.cmd == "pull":
            names = a.names or [f["name"] for f in L.files() if a.all or f["src"] == "dev"]
            os.makedirs(a.dir, exist_ok=True)
            for n in names:
                L.pull(n, os.path.join(a.dir, os.path.basename(n)))
                print("pull", n)
            if not names:
                print("nothing to pull")
        elif a.cmd == "rm":
            L.remove(a.name)
            print("removed", a.name)
        elif a.cmd == "unpair":
            L.unpair()
            print("unpaired")
    except HubError as e:
        print("lslocker:", e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
