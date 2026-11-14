"""Exercise NuGet V3 registration parsing against a local HTTP feed."""

import json
import subprocess
import sys
import tempfile
import threading
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


def leaf(version, dependencies):
    """Build a registration leaf with an embedded catalog entry."""
    return {
        "catalogEntry": {
            "version": version,
            "dependencyGroups": [
                {
                    "targetFramework": "net8.0",
                    "dependencies": [{"id": name} for name in dependencies],
                }
            ],
        }
    }


def write_document(directory, name, data):
    """Write one JSON response to the temporary feed directory."""
    path = directory / name.lstrip("/")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data), encoding="utf-8")


def create_feed(directory, base):
    """Create a registration feed with external and embedded pages."""
    documents = {
        "/index.json": {
            "resources": [
                {
                    "@type": "RegistrationsBaseUrl/3.6.0",
                    "@id": base + "/registrations/",
                }
            ]
        },
        "/registrations/a/index.json": {
            "items": [{"@id": base + "/registrations/a/page.json"}]
        },
        "/registrations/a/page.json": {
            "items": [
                leaf("1.0.0", ["D"]),
                leaf("1.2.0", ["B", "C"]),
                leaf("2.0.0-preview", ["E"]),
            ]
        },
        "/registrations/b/index.json": {
            "items": [{"items": [leaf("1.0.0", [])]}]
        },
        "/registrations/c/index.json": {
            "items": [{"items": [leaf("1.0.0", [])]}]
        },
    }
    for name, data in documents.items():
        write_document(directory, name, data)


def verify_graph(binary, base, directory):
    """Check selected versions, direct dependencies, D2, and PNG."""
    output = directory / "graph.png"
    result = subprocess.run(
        [
            binary, "--package", "A", "--repository",
            base + "/index.json", "--output", str(output), "--order",
        ],
        capture_output=True, text=True, check=True,
    )
    d2 = output.with_suffix(".d2").read_text(encoding="utf-8")
    assert '"a" -> "b"' in d2
    assert '"a" -> "c"' in d2
    assert '"d"' not in d2 and '"e"' not in d2
    assert output.read_bytes().startswith(b"\x89PNG\r\n\x1a\n")
    assert "Direct dependencies:\nb\nc\n" in result.stdout
    assert "Loading order:\nb\nc\na\n" in result.stdout


def main():
    """Run a temporary static NuGet feed and check the CLI result."""
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        handler = partial(SimpleHTTPRequestHandler, directory=temporary)
        server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        base = f"http://127.0.0.1:{server.server_port}"
        create_feed(directory, base)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            verify_graph(sys.argv[1], base, directory)
        finally:
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    main()
