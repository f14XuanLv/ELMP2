"""Client for the existing research MCPs; no additional server or Python dependency.

  python tools/mcp_client.py cue4parse list
  python tools/mcp_client.py cue4parse schema get_package_summary
  python tools/mcp_client.py cue4parse call search_assets --args '{"query":"RestingPoint"}'
  python tools/mcp_client.py ida call idb_list

Use --args-file for shell-independent JSON, --out for complete evidence. CUE providers
are mounted once per invocation; batch calls share that provider. IDA uses its running
headless HTTP service and requires an explicit database session argument.
"""

import argparse
from collections import deque
import ipaddress
import json
import math
import queue
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from urllib.parse import urlsplit
from pathlib import Path

import devconfig


class McpError(RuntimeError):
    pass


class Client:
    def __init__(self, config: dict, timeout: float = 60):
        self.config = config
        self.timeout = timeout
        self.sequence = 0
        self.process = None
        self.session = None
        self.version = "2024-11-05"
        self.messages = queue.Queue()
        self.errors = deque(maxlen=30)
        self.readers = []
        self.http_opener = None
        if "command" not in config:
            host = (urlsplit(config["url"]).hostname or "").lower()
            try:
                local = ipaddress.ip_address(host).is_loopback
            except ValueError:
                local = host == "localhost"
            # Local research services must not pass through HTTP_PROXY, including
            # when NO_PROXY is unset. Preserve normal proxy discovery elsewhere.
            handlers = [urllib.request.ProxyHandler({})] if local else []
            self.http_opener = urllib.request.build_opener(*handlers)

    def __enter__(self):
        try:
            if "command" in self.config:
                self.process = subprocess.Popen(
                    [self.config["command"], *self.config.get("args", [])],
                    cwd=self.config.get("cwd"), stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                    text=True, encoding="utf-8", errors="replace",
                    creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
                self.readers = [threading.Thread(target=self._read, args=(self.process.stdout,), daemon=True),
                                threading.Thread(target=self._read_errors, args=(self.process.stderr,), daemon=True)]
                for reader in self.readers:
                    reader.start()
            self.info = self.request("initialize", {
                "protocolVersion": self.version, "capabilities": {},
                "clientInfo": {"name": "ELMP-toolchain", "version": "1.0"}})
            self.version = self.info.get("protocolVersion", self.version)
            self.request("notifications/initialized", notification=True)
            return self
        except BaseException:
            self.close()
            raise

    def __exit__(self, *_):
        self.close()

    def close(self):
        if self.process:
            try:
                self.process.stdin.close()
            except OSError:
                pass  # A server may already have exited with unread buffered input.
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.terminate()
                try:
                    self.process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=3)
            for reader in self.readers:
                reader.join(timeout=1)
            self.process.stdout.close()
            self.process.stderr.close()
            self.process = None
            self.readers.clear()
        # HTTP service and its database deliberately outlive this client.

    def _read(self, stream):
        try:
            for line in stream:
                try:
                    self.messages.put(json.loads(line))
                except json.JSONDecodeError:
                    self.errors.append(line.rstrip())
        finally:
            self.messages.put(None)

    def _read_errors(self, stream):
        for line in stream:
            self.errors.append(line.rstrip())

    def request(self, method: str, params=None, notification=False):
        self.sequence += 1
        body = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            body["params"] = params
        if not notification:
            body["id"] = self.sequence
        if self.process:
            self.process.stdin.write(json.dumps(body, ensure_ascii=False) + "\n")
            self.process.stdin.flush()
            if notification:
                return None
            end = time.monotonic() + self.timeout
            while True:
                remaining = end - time.monotonic()
                if remaining <= 0:
                    raise McpError(f"{method} timed out after {self.timeout}s")
                try:
                    response = self.messages.get(timeout=remaining)
                except queue.Empty:
                    raise McpError(f"{method} timed out after {self.timeout}s") from None
                if response is None:
                    raise McpError("MCP process exited: " + "\n".join(self.errors))
                if not isinstance(response, dict):
                    raise McpError(f"{method}: expected a JSON-RPC object")
                if response.get("id") == body["id"]:
                    break
                if "method" in response and "id" in response:
                    # These local servers do not require elicitation or sampling.
                    self.process.stdin.write(json.dumps({"jsonrpc": "2.0", "id": response["id"],
                        "error": {"code": -32601, "message": "Client method not supported"}}) + "\n")
                    self.process.stdin.flush()
        else:
            headers = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream",
                       "MCP-Protocol-Version": self.version}
            if self.session:
                headers["Mcp-Session-Id"] = self.session
            req = urllib.request.Request(self.config["url"], json.dumps(body).encode("utf-8"), headers)
            try:
                with self.http_opener.open(req, timeout=self.timeout) as reply:
                    self.session = reply.headers.get("Mcp-Session-Id", self.session)
                    if notification:
                        reply.read()
                        return None
                    if "text/event-stream" in reply.headers.get("Content-Type", ""):
                        response = None
                        end = time.monotonic() + self.timeout
                        for line in reply:
                            if time.monotonic() >= end:
                                raise McpError(f"{method} timed out after {self.timeout}s")
                            if line.startswith(b"data:"):
                                item = json.loads(line[5:])
                                if item.get("id") == body["id"]:
                                    response = item
                                    break
                        if response is None:
                            raise McpError(f"{method}: SSE stream ended without a response")
                    else:
                        response = json.load(reply)
            except urllib.error.HTTPError as exc:
                raise McpError(f"HTTP {exc.code}: {exc.read().decode('utf-8', errors='replace')[:2000]}") from exc
            except urllib.error.URLError as exc:
                raise McpError(f"MCP unavailable at {self.config['url']}: {exc.reason}") from exc
        if not isinstance(response, dict):
            raise McpError(f"{method}: expected a JSON-RPC object")
        if "error" in response:
            raise McpError(json.dumps(response["error"], ensure_ascii=False))
        return response.get("result")

    def tools(self):
        found, cursor = [], None
        while True:
            result = self.request("tools/list", {"cursor": cursor} if cursor else {})
            found.extend(result.get("tools", []))
            cursor = result.get("nextCursor")
            if not cursor:
                return found

    def call(self, name, arguments=None):
        result = self.request("tools/call", {"name": name, "arguments": arguments or {}})
        if not isinstance(result, dict):
            raise McpError(f"{name}: expected a tool result object")
        value = text_result(result)
        if result.get("isError") or (isinstance(value, dict) and value.get("ok") is False):
            raise McpError(json.dumps(result, ensure_ascii=False))
        return result


def text_result(result):
    """Unwrap structured/text JSON while retaining non-JSON output for diagnostics."""
    if "structuredContent" in result:
        return result["structuredContent"]
    texts = [x["text"] for x in result.get("content", []) if x.get("type") == "text"]
    if len(texts) == 1:
        try:
            return json.loads(texts[0])
        except json.JSONDecodeError:
            return texts[0]
    return texts


def prepare(client, name, cfg):
    if name == "cue4parse":
        return client.call("init_provider", {
            "root": cfg["paks"], "gameVersion": "GAME_UE4_26",
            "mappingsFile": cfg["usmap"], "readScriptData": True, "caseInsensitive": True})
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("server", choices=("cue4parse", "ida"))
    parser.add_argument("action", choices=("list", "schema", "call", "batch"))
    parser.add_argument("tool", nargs="?")
    args_group = parser.add_mutually_exclusive_group()
    args_group.add_argument("--args", help="Tool arguments as a JSON object")
    args_group.add_argument("--args-file", type=Path, help="JSON object, or array of {name, arguments} for batch")
    parser.add_argument("--out", type=Path)
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--no-init", action="store_true", help="Do not auto-mount the CUE provider")
    args = parser.parse_args()
    if args.action in ("schema", "call") and not args.tool:
        parser.error("schema/call requires a tool name")
    if args.action in ("list", "batch") and args.tool:
        parser.error("list/batch does not accept a tool name")
    if args.action in ("list", "schema") and (args.args is not None or args.args_file is not None):
        parser.error("list/schema does not accept tool arguments")
    if args.action == "batch" and not args.args_file:
        parser.error("batch requires --args-file")
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be a finite positive number")
    try:
        payload = json.loads(args.args_file.read_text(encoding="utf-8-sig") if args.args_file else (args.args or "{}"))
        if args.action == "call" and not isinstance(payload, dict):
            parser.error("call arguments must be a JSON object")
        if args.action == "batch" and (not isinstance(payload, list) or not payload or any(
                not isinstance(x, dict) or not isinstance(x.get("name"), str) or not x["name"].strip() or
                set(x) - {"name", "arguments"} or not isinstance(x.get("arguments", {}), dict) for x in payload)):
            parser.error("batch must be an array of {name, arguments} objects")
        cfg = devconfig.load()
        with Client(cfg["toolchain"]["mcp"][args.server], args.timeout) as client:
            if args.action == "list":
                output = {"server": client.info, "tools": [{"name": t["name"], "description": t.get("description", "")}
                                                           for t in client.tools()]}
            elif args.action == "schema":
                output = next((t for t in client.tools() if t["name"] == args.tool), None)
                if output is None:
                    raise McpError(f"Unknown tool: {args.tool}")
            else:
                if not args.no_init:
                    prepare(client, args.server, cfg)
                if args.action == "batch":
                    output = [{"name": item["name"], "result": client.call(item["name"], item.get("arguments"))}
                              for item in payload]
                else:
                    output = client.call(args.tool, payload)
        rendered = json.dumps(output, ensure_ascii=False, indent=2)
        if args.out:
            args.out.parent.mkdir(parents=True, exist_ok=True)
            args.out.write_text(rendered + "\n", encoding="utf-8")
            print(args.out.resolve())
        else:
            print(rendered)
        return 0
    except (McpError, OSError, ValueError, KeyError) as exc:
        print(f"MCP error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
