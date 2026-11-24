#!/usr/bin/env python3
"""Static dashboard server.

Serves `web/` and adds `Cache-Control: no-store` so changes to
`src/*.js`/`styles.css` show up on reload (the demo changes often).
"""

from __future__ import annotations

import os
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer


class NoCacheHandler(SimpleHTTPRequestHandler):
    def end_headers(self) -> None:
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


def main() -> None:
    root = os.path.dirname(os.path.abspath(__file__))
    os.chdir(root)
    port = int(os.getenv("PORT", "3000"))
    server = ThreadingHTTPServer(("0.0.0.0", port), NoCacheHandler)
    server.serve_forever()


if __name__ == "__main__":
    main()
