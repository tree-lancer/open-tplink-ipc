"""Read-only MCP protocol and live camera smoke check. Uses saved config or IPC_* environment."""

import json
import os
import pathlib
import subprocess
import sys


binary = pathlib.Path(__file__).resolve().parents[1] / "build" / "tplink-ipc-mcp"
def request(ident, method, params=None):
    value = {"jsonrpc": "2.0", "id": ident, "method": method}
    if params is not None:
        value["params"] = params
    return value


messages = [
    request(1, "initialize", {"protocolVersion": "2025-11-25", "capabilities": {}, "clientInfo": {"name": "smoke", "version": "1"}}),
    {"jsonrpc": "2.0", "method": "notifications/initialized"},
    request(2, "tools/list"),
]
for ident, name, args in (
    (3, "camera_info", {}),
    (4, "camera_status", {}),
    (5, "camera_ptz_status", {}),
    (6, "camera_presets", {}),
    (7, "camera_events", {"wait_seconds": 1}),
    (8, "camera_snapshot", {}),
):
    messages.append(request(ident, "tools/call", {"name": name, "arguments": args}))

result = subprocess.run(
    [str(binary)], input="".join(json.dumps(message) + "\n" for message in messages),
    text=True, capture_output=True, timeout=35, check=True,
)
responses = {message["id"]: message for message in map(json.loads, result.stdout.splitlines())}
assert len(responses) == 8
assert responses[1]["result"]["protocolVersion"] == "2025-11-25"
assert len(responses[2]["result"]["tools"]) >= 10
for ident in range(3, 9):
    assert not responses[ident]["result"].get("isError"), responses[ident]
info = json.loads(responses[3]["result"]["content"][0]["text"])
status = json.loads(responses[4]["result"]["content"][0]["text"])
position = json.loads(responses[5]["result"]["content"][0]["text"])
assert info["model"].startswith("TL-IPC")
assert "person" in status and "storage" in status
assert -1 <= position["pan"] <= 1 and -1 <= position["tilt"] <= 1
assert responses[8]["result"]["content"][0]["type"] == "image"
assert len(responses[8]["result"]["content"][0]["data"]) > 1000
if os.environ.get("IPC_PASSWORD"):
    assert os.environ["IPC_PASSWORD"] not in result.stdout
print("MCP live smoke: passed", file=sys.stderr)
