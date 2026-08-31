#!/usr/bin/env python3
"""One-time Spotify authorisation (PKCE) -- run this on your own machine.

Uses Authorization Code with PKCE, so there is no client secret anywhere: not
in this script, not in your firmware, not on the board. A device that only ever
reads your playback state has no business carrying a secret that could be
pulled out of its flash.

Prints a refresh token to paste into src/secrets.h. Nothing is transmitted
anywhere except to Spotify, and nothing is written to disk.

Setup, first:
  1. https://developer.spotify.com/dashboard -> your app -> Settings
  2. Redirect URI must include exactly:  http://127.0.0.1:8888/callback
  3. Copy the Client ID (public -- the secret is not needed and not used)

Then:  python tools/spotify_auth.py
"""
import base64
import hashlib
import http.server
import json
import os
import secrets
import socketserver
import sys
import threading
import urllib.parse
import urllib.request
import webbrowser

PORT = 8888
REDIRECT = f"http://127.0.0.1:{PORT}/callback"
SCOPES = "user-read-currently-playing user-read-playback-state"

result = {}
state = secrets.token_urlsafe(16)


def b64url(raw: bytes) -> str:
    return base64.urlsafe_b64encode(raw).decode().rstrip("=")


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
        if "code" in q and q.get("state", [None])[0] == state:
            result["code"] = q["code"][0]
            body = b"<h2>Authorised.</h2><p>You can close this tab.</p>"
        else:
            result["error"] = q.get("error", ["state mismatch"])[0]
            body = b"<h2>Failed.</h2><p>Check the terminal.</p>"
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


def main():
    print(__doc__)
    client_id = input("Client ID: ").strip()
    if not client_id:
        sys.exit("client id is required")

    # PKCE: a high-entropy verifier stays local; only its SHA-256 goes to
    # Spotify up front. The verifier proves, at token exchange, that whoever
    # redeems the code is whoever requested it -- which is the job a client
    # secret would otherwise do.
    verifier = b64url(os.urandom(64))
    challenge = b64url(hashlib.sha256(verifier.encode("ascii")).digest())

    auth_url = "https://accounts.spotify.com/authorize?" + urllib.parse.urlencode({
        "client_id": client_id,
        "response_type": "code",
        "redirect_uri": REDIRECT,
        "scope": SCOPES,
        "state": state,
        "code_challenge_method": "S256",
        "code_challenge": challenge,
    })

    socketserver.TCPServer.allow_reuse_address = True
    try:
        srv = socketserver.TCPServer(("127.0.0.1", PORT), Handler)
    except OSError as e:
        sys.exit(f"could not bind 127.0.0.1:{PORT} ({e}). Close whatever is using it.")
    threading.Thread(target=srv.handle_request, daemon=True).start()

    print(f"\nOpening your browser. If it does not open, visit:\n{auth_url}\n")
    webbrowser.open(auth_url)

    for _ in range(120):
        if result:
            break
        threading.Event().wait(1)
    srv.server_close()

    if "code" not in result:
        sys.exit(f"authorisation failed: {result.get('error', 'timed out')}")

    req = urllib.request.Request(
        "https://accounts.spotify.com/api/token",
        data=urllib.parse.urlencode({
            "grant_type": "authorization_code",
            "code": result["code"],
            "redirect_uri": REDIRECT,
            "client_id": client_id,
            "code_verifier": verifier,
        }).encode(),
        headers={"Content-Type": "application/x-www-form-urlencoded"},
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            tok = json.load(r)
    except urllib.error.HTTPError as e:
        sys.exit(f"token exchange failed: {e.code} {e.read().decode(errors='replace')}")

    if "refresh_token" not in tok:
        sys.exit(f"no refresh token in response: {tok}")

    print("\n" + "=" * 68)
    print("Paste these into src/secrets.h (gitignored). No client secret needed.\n")
    print(f'#define SPOTIFY_CLIENT_ID     "{client_id}"')
    print(f'#define SPOTIFY_REFRESH_TOKEN "{tok["refresh_token"]}"')
    print("=" * 68)
    print("\nNote: with PKCE, Spotify rotates the refresh token on every refresh.")
    print("The firmware saves each new one to NVS, so this value is only the seed")
    print("for first boot -- you do not need to rerun this when it rotates.")


if __name__ == "__main__":
    main()
