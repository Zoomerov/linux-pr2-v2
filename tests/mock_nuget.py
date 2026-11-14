"""Exercise NuGet V3 registration parsing against a local HTTP feed."""

import json
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
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


class Handler(BaseHTTPRequestHandler):
    """Serve a small registration index with an external page."""

    def do_GET(self):
        """Return a NuGet V3 JSON document for the requested path."""
        base = f"http://127.0.0.1:{self.server.server_port}"
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
        if self.path not in documents:
            self.send_error(404)
            return
        body = json.dumps(documents[self.path]).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, _format, *_args):
        """Keep test output quiet."""


def main():
    """Run the CLI and check version selection and graph closure."""
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "graph.png"
            result = subprocess.run(
                [
                    sys.argv[1], "--package", "A", "--repository",
                    f"http://127.0.0.1:{server.server_port}/index.json",
                    "--output", str(output), "--order",
                ],
                capture_output=True, text=True, check=True,
            )
            d2 = output.with_suffix(".d2").read_text()
            assert '"a" -> "b"' in d2
            assert '"a" -> "c"' in d2
            assert '"d"' not in d2 and '"e"' not in d2
            assert output.read_bytes().startswith(b"\x89PNG\r\n\x1a\n")
            assert "Direct dependencies:\nb\nc\n" in result.stdout
            assert "Loading order:\nb\nc\na\n" in result.stdout
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
