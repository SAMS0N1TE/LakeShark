"""Studio's AI panel: the same tools an AI gets over MCP (mcp_tools.json),
run inside Studio, and a pluggable model provider.

Providers:
  commands  no model: type /build, /sim_run, /sdk_docs lap_ui_keys ... and
            the tool runs. Always there.
  openai    any OpenAI-compatible chat endpoint (a hosted one, or a local
            one such as Ollama or LM Studio on this computer): URL, model and
            the name of an environment variable holding the key are settings.

The key is read from that environment variable, or from the per-user
settings file (~/.lakeshark/studio.json, never in a repository). It is never
sent back to the page. Tools that change the board ask the owner first.
"""
import base64
import json
import os
import re
import time
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
CONTRACT = json.loads((HERE / "mcp_tools.json").read_text(encoding="utf-8"))
BOARD_TOOLS = {t["name"] for t in CONTRACT["tools"] if t.get("board")}


def _schema(tool):
    props = {}
    for k, v in tool["args"].items():
        t = "integer" if v.startswith("int") else "boolean" if v.startswith("bool") else "string"
        props[k] = {"type": t, "description": v}
    return {"type": "object", "properties": props}


class Assistant:
    def __init__(self, studio):
        self.studio = studio

    # ---- settings

    def settings(self):
        import server
        try:
            return json.loads(server.settings_path().read_text(encoding="utf-8")).get("ai", {})
        except (OSError, ValueError):
            return {}

    def save_settings(self, ai):
        import server
        p = server.settings_path()
        try:
            whole = json.loads(p.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            whole = {}
        whole["ai"] = ai
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(json.dumps(whole, indent=1), encoding="utf-8")
        try:
            os.chmod(p, 0o600)
        except OSError:
            pass

    def key(self, s):
        env = s.get("key_env") or "LSDK_AI_KEY"
        return os.environ.get(env) or s.get("key") or ""

    def public(self):
        s = self.settings()
        return {"provider": s.get("provider", "commands"), "url": s.get("url", ""), "model": s.get("model", ""),
                "key_env": s.get("key_env", "LSDK_AI_KEY"), "key_set": bool(self.key(s)),
                "key_from": "environment" if os.environ.get(s.get("key_env") or "LSDK_AI_KEY") else
                            ("settings file" if s.get("key") else "none"),
                "settings_file": str(__import__("server").settings_path()),
                "providers": [{"id": "commands", "text": "No model: type /tool commands"},
                              {"id": "openai", "text": "An OpenAI-compatible chat endpoint (hosted, or local like Ollama)"}],
                "tools": CONTRACT["tools"]}

    # ---- the tools

    def tool(self, name, args, project=None):
        st, lsdk = self.studio, __import__("lsdk")
        proj = st.known(args.get("project") or project) if name in ("build", "sim_run", "install") else None
        if name == "sdk_docs":
            q = (args.get("query") or "").lower()
            api = st.api or st.call(None, "GET", "api", {})
            hits = [e for e in api["entries"] if q in e["name"].lower()] or \
                   [e for e in api["entries"] if q and q in e["doc"].lower()]
            secs = [s["title"] for s in api["sections"] if q and q in s["markdown"].lower()]
            return {"api": [{"name": e["name"], "signature": e["signature"], "doc": e["doc"][:300]} for e in hits[:8]],
                    "guide_sections": secs[:6]}
        if name == "new_app":
            folder = lsdk.new_app(args["name"], args.get("template", "blank"), st.workspace)
            st.touch_recent(folder)
            return {"ok": True, "project": str(folder)}
        if name == "build":
            r = lsdk.build(proj)
            return {"ok": r.ok, "size": r.size, "problems": st.rel(proj, {"problems": r.problems})["problems"]}
        if name == "sim_run":
            r = lsdk.sim_build(proj, say=lambda s: None)
            if not r.ok:
                return {"ok": False, "problems": st.rel(proj, {"problems": r.problems})["problems"]}
            text = []
            out = lsdk.sim_frames(proj, int(args.get("frames") or 40), bool(args.get("landscape")), args.get("script"),
                                  bool(args.get("no_radio")), text)
            png = out / "last.png"
            img = ("data:image/png;base64," + base64.b64encode(png.read_bytes()).decode()) if png.exists() else None
            screen = "\n".join(l for l in text[0].splitlines() if re.match(r"^\s*\d+ \|", l))
            return {"ok": True, "screen_text": screen[-6000:], "png": img}
        if name == "sim_input":
            if not st.sim.running():
                return {"ok": False, "message": "nothing is running in the run panel: press Run (F5) first"}
            st.sim.say("step " + args["step"])
            return {"ok": True}
        if name == "install":
            if not st.board.install(proj):
                return {"ok": False, "message": "an install is already running"}
            end = time.time() + 180
            while st.board.job.get("state") == "running" and time.time() < end:
                time.sleep(0.3)
            return {k: v for k, v in st.board.job.items() if k != "problems"}
        if name == "board_console":
            return {"lines": st.board.link().rpc([args["line"][:200]], r".", 3.0)}
        if name == "board_logs":
            n = int(args.get("lines") or 50)
            _, lines = st.log.since(0)
            rows = [l["text"] for l in lines if l["src"] == "board"]
            if args.get("app"):
                rows = [r for r in rows if "[app %s]" % args["app"] in r]
            if not st.board.tailing:
                st.board.set_tail(True)
            return {"lines": rows[-n:], "note": "" if rows else "connected to the board's console now; ask again in a moment"}
        if name in ("board_screen", "board_input"):
            return {"ok": False, "message": "%s comes with the LakeShark MCP server (app-sdk/mcp); in Studio, watch the "
                                            "board in LakeShark Desktop's window" % name}
        raise ValueError("no tool " + name)

    # ---- requests from the page

    def call(self, what, method, data):
        if what == "config":
            if method == "GET":
                return self.public()
            s = self.settings()
            for k in ("provider", "url", "model", "key_env"):
                if k in data:
                    s[k] = str(data[k])[:300]
            if data.get("key"):                         # kept in the per-user file only
                s["key"] = str(data["key"])[:400]
            if data.get("forget_key"):
                s.pop("key", None)
            self.save_settings(s)
            return self.public()
        if what == "run":                                # one tool, the page having asked when it must
            name = data["tool"]
            if name in BOARD_TOOLS and not data.get("confirmed"):
                return {"confirm": {"tool": name, "args": data.get("args", {})}}
            return {"result": self.tool(name, data.get("args", {}), data.get("project"))}
        if what == "chat":
            return self.chat(data)
        raise ValueError("no such AI call: " + what)

    def chat(self, data):
        s = self.settings()
        msgs = data.get("messages", [])
        project = data.get("project")
        if s.get("provider", "commands") != "openai":
            return self.commands(msgs[-1]["content"] if msgs else "", project)
        if not s.get("url") or not s.get("model"):
            return {"reply": "Set the endpoint URL and model under Connect an AI first.", "messages": msgs}
        tools = [{"type": "function", "function": {"name": t["name"], "description": t["description"],
                                                   "parameters": _schema(t)}} for t in CONTRACT["tools"]]
        system = {"role": "system", "content": "You help write LakeShark board apps (C, the LAP API). The open project is %s. "
                  "Use the tools: build to check code, sim_run to see the screen, sdk_docs for the API." % project}
        convo = [system] + [m for m in msgs if m.get("role") != "system"]
        for _ in range(6):
            body = json.dumps({"model": s["model"], "messages": convo, "tools": tools}).encode()
            req = urllib.request.Request(s["url"].rstrip("/") + "/chat/completions", data=body,
                                         headers={"Content-Type": "application/json"})
            if self.key(s):
                req.add_header("Authorization", "Bearer " + self.key(s))
            try:
                with urllib.request.urlopen(req, timeout=120) as r:
                    j = json.loads(r.read().decode())
            except (urllib.error.URLError, OSError, ValueError) as e:
                return {"reply": "The model did not answer: %s" % e, "messages": msgs}
            m = j["choices"][0]["message"]
            convo.append({k: v for k, v in m.items() if k in ("role", "content", "tool_calls")})
            calls = m.get("tool_calls") or []
            if not calls:
                return {"reply": m.get("content") or "", "messages": convo[1:]}
            for c in calls:
                name = c["function"]["name"]
                try:
                    args = json.loads(c["function"].get("arguments") or "{}")
                except ValueError:
                    args = {}
                if name in BOARD_TOOLS and not data.get("confirmed"):
                    return {"confirm": {"tool": name, "args": args}, "messages": convo[1:-1],
                            "reply": "The model wants to run %s on the board." % name}
                try:
                    res = self.tool(name, args, project)
                except Exception as e:
                    res = {"error": str(e)}
                res.pop("png", None) if isinstance(res, dict) else None
                convo.append({"role": "tool", "tool_call_id": c.get("id", name), "content": json.dumps(res)[:12000]})
        return {"reply": "(stopped after six rounds of tools)", "messages": convo[1:]}

    def commands(self, text, project):
        """No model: /tool key=value ... runs a tool; anything else gets help."""
        text = text.strip()
        m = re.match(r"^/(\w+)\s*(.*)$", text)
        names = [t["name"] for t in CONTRACT["tools"]]
        if not m or m.group(1) not in names:
            return {"reply": "No model is connected (Connect an AI sets one up). Tools you can run now:\n" +
                    "\n".join("  /%s %s - %s" % (t["name"], " ".join("%s=" % a for a in t["args"]), t["description"])
                              for t in CONTRACT["tools"])}
        name, rest = m.group(1), m.group(2)
        args = dict(re.findall(r"(\w+)=(\"[^\"]*\"|\S+)", rest))
        args = {k: v.strip('"') for k, v in args.items()}
        if not args and rest and name in ("sdk_docs", "sim_input", "board_console", "new_app"):
            args = {{"sdk_docs": "query", "sim_input": "step", "board_console": "line", "new_app": "name"}[name]: rest}
        if name in BOARD_TOOLS:
            return {"confirm": {"tool": name, "args": args}, "reply": "This changes the board: run %s?" % name}
        return {"tool": name, "result": self.tool(name, args, project)}
