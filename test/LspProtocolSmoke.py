import json
import subprocess
import sys
from pathlib import Path

server = sys.argv[1]
compile_commands = sys.argv[2]
fixture = Path(sys.argv[3]).resolve()
uri = fixture.as_uri()
broken_uri = uri + ".broken.cpp"
messages = [
    {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
        "initializationOptions": {"enableSemantic": True, "compileCommands": compile_commands}}},
    {"jsonrpc": "2.0", "method": "initialized", "params": {}},
    {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
        "uri": uri, "languageId": "cpp", "version": 1, "text": "void f(){\nint value = NULL;\nint unused;\n}\n"}}},
    {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {
        "uri": broken_uri, "languageId": "cpp", "version": 1,
        "text": "int broken()\n{\n    int value = 1\n    return value;\n}\n"}}},
    {"jsonrpc": "2.0", "id": 2, "method": "textDocument/formatting", "params": {
        "textDocument": {"uri": uri}, "options": {"tabSize": 4, "insertSpaces": True}}},
    {"jsonrpc": "2.0", "id": 3, "method": "textDocument/codeAction", "params": {
        "textDocument": {"uri": uri},
        "range": {"start": {"line": 0, "character": 0}, "end": {"line": 2, "character": 0}},
        "context": {"diagnostics": []}}},
    {"jsonrpc": "2.0", "id": 4, "method": "shutdown", "params": {}},
    {"jsonrpc": "2.0", "method": "exit"},
]

wire = bytearray()
for message in messages:
    body = json.dumps(message, separators=(",", ":")).encode("utf-8")
    wire.extend(f"Content-Length: {len(body)}\r\n\r\n".encode("ascii"))
    wire.extend(body)

process = subprocess.run([server], input=wire, capture_output=True, timeout=10, check=False)
if process.returncode != 0:
    raise SystemExit(f"LSP server exited with {process.returncode}: {process.stderr.decode(errors='replace')}")

responses = []
stream = process.stdout
while stream:
    header, stream = stream.split(b"\r\n\r\n", 1)
    length = int(header.decode("ascii").split(":", 1)[1].strip())
    body, stream = stream[:length], stream[length:]
    responses.append(json.loads(body))

initialize = next(item for item in responses if item.get("id") == 1)
assert initialize["result"]["capabilities"]["documentFormattingProvider"] is True
diagnostics = next(item for item in responses if item.get("method") == "textDocument/publishDiagnostics")
assert diagnostics["params"]["diagnostics"][0]["code"] == "CPPLSP001"
formatted = next(item for item in responses if item.get("id") == 2)["result"]
assert formatted[0]["newText"] == "void f(){\n    int value = NULL;\n    int unused;\n}\n"
codes = {item["code"] for item in diagnostics["params"]["diagnostics"]}
assert "CPPLSP101" in codes
broken = next(item for item in responses
              if item.get("method") == "textDocument/publishDiagnostics"
              and item["params"]["uri"] == broken_uri)
assert any(item["code"] == "CPPLSP900" and item["severity"] == 1
           for item in broken["params"]["diagnostics"]), broken
actions = next(item for item in responses if item.get("id") == 3)["result"]
assert actions[0]["edit"]["changes"][uri][0]["newText"] == "nullptr"
