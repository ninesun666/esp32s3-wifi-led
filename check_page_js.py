"""Syntax-check the dashboard's JavaScript before it ships in firmware.

The page is one C raw string in `web_index.h`, so a broken expression does not
fail the build -- it parses out of the `<script>` tag at load time and takes the
*whole* script with it (functions never defined, every card stuck on its "-"
placeholder). That happened: a `res.ok&&(a?b:c):d` ternary kept the dashboard
blank while every API endpoint answered 200 in seconds, so no HTTP probe could
have noticed. `node --check` is the only cheap way to catch it.

  python check_page_js.py
"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HEADER = Path(__file__).parent / "wifi_led/components/monitor_server/web_index.h"


def script_of(header_text):
    body = header_text.split('R"HTML(', 1)[1].split(')HTML"', 1)[0]
    return re.search(r"<script>(.*?)</script>", body, re.S).group(1)


def main():
    js = script_of(HEADER.read_text(encoding="utf-8"))
    with tempfile.NamedTemporaryFile("w", suffix=".js", delete=False, encoding="utf-8") as f:
        f.write(js)
        path = f.name
    proc = subprocess.run(["node", "--check", path], capture_output=True, text=True)
    out = (proc.stderr or proc.stdout).replace(path, "web_index.h <script>")
    if proc.returncode:
        print(out)
        sys.exit("the dashboard script does not parse -- the page will load but never update")
    print("OK: %d chars of dashboard JS parse cleanly" % len(js))


if __name__ == "__main__":
    main()
