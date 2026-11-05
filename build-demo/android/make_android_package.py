#!/usr/bin/env python3
"""Builds the Android package-source folder (manifest + Java) for ONE Qt version.

Why: AndroidManifest.xml of every Qt release has its own mandatory entries (they changed between Qt 6.4, 6.5 and 6.8+), so a manifest
written for one Qt breaks the app on another. This script takes the manifest template that ships with the Qt kit itself and only ADDS what
RavAva needs from android/AndroidManifest.xml: the <uses-permission> lines, the foreground <service> and its meta-data. The Java sources
(android/src) are copied as they are.

usage: make_android_package.py <Qt Android kit dir> <repo android dir> <out dir>
"""
import re
import shutil
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    qt, ours, out = (Path(a) for a in sys.argv[1:])

    template = qt / "src" / "android" / "templates" / "AndroidManifest.xml"
    if not template.is_file():
        print(f"[error] Qt's manifest template not found: {template}")
        return 1
    ours_manifest = (ours / "AndroidManifest.xml").read_text(encoding="utf-8")
    ours_manifest = re.sub(r"<!--.*?-->", "", ours_manifest, flags=re.S)         # comments out: only real elements are copied

    permissions = re.findall(r"<uses-permission\b[^>]*/>", ours_manifest)
    services = re.findall(r"<service\b[^>]*/>|<service\b[^>]*>.*?</service>", ours_manifest, flags=re.S)
    app_meta = [m for m in re.findall(r"<meta-data\b[^>]*/>", ours_manifest) if 'android.app.background_running' in m]
    if not permissions or not services:
        print("[error] no <uses-permission> / <service> found in android/AndroidManifest.xml")
        return 1

    text = template.read_text(encoding="utf-8")
    have = set(re.findall(r'<uses-permission\b[^>]*android:name="([^"]+)"', text))
    new_perm = [p for p in permissions if re.search(r'android:name="([^"]+)"', p).group(1) not in have]

    if "</application>" not in text or "</manifest>" not in text:
        print("[error] unexpected Qt manifest template")
        return 1
    # the service and its meta-data go at the end of <application>, the permissions at the end of <manifest>
    text = text.replace("</application>", "\n        " + "\n        ".join(app_meta + services) + "\n    </application>", 1)
    text = text.replace("</manifest>", "\n    " + "\n    ".join(new_perm) + "\n</manifest>", 1)

    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    (out / "AndroidManifest.xml").write_text(text, encoding="utf-8")
    if (ours / "src").is_dir():
        shutil.copytree(ours / "src", out / "src")
    if (ours / "res").is_dir():
        shutil.copytree(ours / "res", out / "res")
    print(f"[ok] {out}: manifest from {template} + {len(new_perm)} permission(s), {len(services)} service(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
