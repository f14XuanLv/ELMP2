"""Keep the installed development UEBridge poll on its game-thread Lua executor."""

import hashlib
from pathlib import Path

import devconfig

MARKER = "-- ELMP development bridge: serialize polling with game-thread Lua callbacks."
ORIGINAL = "LoopAsync(POLL_MS, function()\n    if myGeneration ~= UEB_GENERATION then return true end"
SERIAL_V1 = MARKER + "\nlocal elmpPollHandle\nelmpPollHandle = LoopInGameThreadWithDelay(POLL_MS, function()\n    if myGeneration ~= UEB_GENERATION then CancelDelayedAction(elmpPollHandle); return end"
SERIAL = MARKER + "\nlocal elmpPollHandle\nelmpPollHandle = LoopInGameThreadAfterFrames(math.max(1, math.floor(POLL_MS / 16.667 + 0.5)), function()\n    if myGeneration ~= UEB_GENERATION then CancelDelayedAction(elmpPollHandle); return end"
HANDLE = "local function handle(req)"
INLINE = "local function elmpBridgeInline(fn) fn() end\n\n" + HANDLE
QUEUED_CALL = "ExecuteInGameThread(function()"
INLINE_CALL = "elmpBridgeInline(function()"


def prepare(inst: dict, name: str) -> None:
    path: Path = inst["mods"] / "UEBridge" / "Scripts" / "main.lua"
    if not path.exists():
        return
    if not path.resolve().is_relative_to(inst["mods"].resolve()):
        raise SystemExit("UEBridge script resolves outside the configured Mods directory")
    original = path.read_bytes()
    text = original.decode("utf-8").replace("\r\n", "\n")
    if SERIAL in text and INLINE in text and text.count(INLINE_CALL) == 2 and QUEUED_CALL not in text:
        print("UEBridge: EngineTick polling and inline requests already configured")
        return
    prior = ORIGINAL if text.count(ORIGINAL) == 1 and MARKER not in text else SERIAL_V1
    if ('local VERSION = "1.2.1"' not in text or text.count(prior) != 1
            or text.count(HANDLE) != 1 or text.count(QUEUED_CALL) != 2 or INLINE in text):
        raise SystemExit("unknown UEBridge polling implementation; refusing to rewrite it")
    digest = hashlib.sha256(original).hexdigest()
    backup = devconfig.REPO / "build" / "bridge-runtime" / name / ("main-" + digest + ".lua")
    backup.parent.mkdir(parents=True, exist_ok=True)
    if backup.exists() and backup.read_bytes() != original:
        raise SystemExit("UEBridge backup identity mismatch")
    backup.write_bytes(original)
    patched = text.replace(prior, SERIAL).replace(HANDLE, INLINE).replace(QUEUED_CALL, INLINE_CALL)
    if b"\r\n" in original:
        patched = patched.replace("\n", "\r\n")
    path.write_bytes(patched.encode("utf-8"))
    print(f"UEBridge: EngineTick polling and inline requests configured; original saved to {backup}")
