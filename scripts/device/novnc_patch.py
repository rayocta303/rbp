#!/usr/bin/env python3
"""novnc_patch.py -- the edits this unit makes to the installed noVNC pages.

WHY A PATCHER RATHER THAN A VENDORED PAGE. The tree's precedent for third-party sources is
fetch-upstream-and-apply-the-diff (tools/build-directfb/README.md), not shipping a copy: a
vendored vnc_lite.html would look like ours, rot silently against an apt upgrade, and lose the
upstream fixes that came with it. These edits are anchored on text that changes only if noVNC
changes, each one is applied at most once, each file is backed up once before the first edit, and
a set of anchors that does not match is LOGGED AND SURVIVED -- the client must never be taken down
by a page tweak. The stamp each file gains at the end is what makes a re-run cheap and quiet, and
what makes an upgraded (replaced) file get patched again rather than skipped.

The edits:
  1. CROSSHAIR '+' CURSOR: Replace the 3x3 dot in RFB.cursors.dot with a high-contrast 9x9 '+'
     crosshair cursor with black contrast outline and white center (matching XDJ-AZ).
  2. DEFAULT CROSSHAIR CSS: Force canvas and container to show cursor: crosshair !important.
  3. CURSOR.JS DEFAULT: When clearing cursor image, fallback to 'crosshair' instead of 'none'.
  4. SHOW_DOT TRUE BY DEFAULT: In app/ui.js, defaults.json and mandatory.json.
  5. AUTOCONNECT & SCALE: Connects immediately upon loading page.
  6. REMOVE Ctrl-Alt-Del & HTTPS Warning.

Usage: novnc_patch.py [root]      (default /usr/share/novnc)
Exit is always 0: a failure here is a page that is unpatched, never a client that is down.
"""
import json
import os
import shutil
import sys

STAMP = "patched by novnc_patch.py v2"

LITE = "vnc_lite.html"
UI = "app/ui.js"
RFB = "core/rfb.js"
CURSOR = "core/util/cursor.js"
BASE_CSS = "app/styles/base.css"

CROSSHAIR_PIXELS = """        /* eslint-disable indent */
        rgbaPixels: new Uint8Array([
              0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0, 255, 255, 255, 255, 255,   0,   0,   0, 255,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
              0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0, 255, 255, 255, 255, 255,   0,   0,   0, 255,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
              0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0, 255, 255, 255, 255, 255,   0,   0,   0, 255,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
              0,   0,   0, 255,   0,   0,   0, 255,   0,   0,   0, 255,   0,   0,   0, 255, 255, 255, 255, 255,   0,   0,   0, 255,   0,   0,   0, 255,   0,   0,   0, 255,   0,   0,   0, 255,
            255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
              0,   0,   0, 255,   0,   0,   0, 255,   0,   0,   0, 255,   0,   0,   0, 255, 255, 255, 255, 255,   0,   0,   0, 255,   0,   0,   0, 255,   0,   0,   0, 255,   0,   0,   0, 255,
              0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0, 255, 255, 255, 255, 255,   0,   0,   0, 255,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
              0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0, 255, 255, 255, 255, 255,   0,   0,   0, 255,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
              0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0, 255, 255, 255, 255, 255,   0,   0,   0, 255,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        ]),
        /* eslint-enable indent */
        w: 9, h: 9,
        hotx: 4, hoty: 4,"""

OLD_DOT_PIXELS = """        /* eslint-disable indent */
        rgbaPixels: new Uint8Array([
            255, 255, 255, 255,   0,   0,   0, 255, 255, 255, 255, 255,
              0,   0,   0, 255,   0,   0,   0,   0,   0,   0,  0,  255,
            255, 255, 255, 255,   0,   0,   0, 255, 255, 255, 255, 255,
        ]),
        /* eslint-enable indent */
        w: 3, h: 3,
        hotx: 1, hoty: 1,"""

EDITS = [
    (LITE, "the dot cursor is always on",
     "                      { credentials: { password: password } });",
     "                      { credentials: { password: password },\n"
     "                        showDotCursor: true });"),

    (LITE, "the Send CtrlAltDel button's CSS",
     """        #sendCtrlAltDelButton {
            position: fixed;
            top: 0px;
            right: 0px;
            border: 1px outset;
            padding: 5px 5px 4px 5px;
            cursor: pointer;
        }

""", ""),

    (LITE, "the sendCtrlAltDel() function",
     """        // Since most operating systems will catch Ctrl+Alt+Del
        // before they get a chance to be intercepted by the browser,
        // we provide a way to emulate this key sequence.
        function sendCtrlAltDel() {
            rfb.sendCtrlAltDel();
            return false;
        }

""", ""),

    (LITE, "the button's click handler",
     """        document.getElementById('sendCtrlAltDelButton')
            .onclick = sendCtrlAltDel;

""", ""),

    (LITE, "the button itself",
     """        <div id="sendCtrlAltDelButton">Send CtrlAltDel</div>
""", ""),

    (UI, "the \"Running without HTTPS\" warning",
     """        // We rely on modern APIs which might not be available in an
        // insecure context
        if (!window.isSecureContext) {
            // FIXME: This gets hidden when connecting
            UI.showStatus(_("Running without HTTPS is not recommended, crashes or other issues are likely."), 'error');
        }

""", ""),

    (UI, "autoconnect and scale by default",
     """        UI.initSetting('autoconnect', false);
        UI.initSetting('view_clip', false);
        UI.initSetting('resize', 'off');""",
     """        UI.initSetting('autoconnect', true);
        UI.initSetting('view_clip', false);
        UI.initSetting('resize', 'scale');"""),

    (UI, "show_dot true default",
     "        UI.initSetting('show_dot', false);",
     "        UI.initSetting('show_dot', true);"),

    (RFB, "replace dot with crosshair '+' pointer",
     OLD_DOT_PIXELS,
     CROSSHAIR_PIXELS),

    (CURSOR, "default crosshair cursor on clear",
     "        this._target.style.cursor = 'none';",
     "        this._target.style.cursor = 'crosshair';"),

    (BASE_CSS, "crosshair cursor on canvas",
     "/*\n * noVNC base CSS",
     "#noVNC_container canvas, #noVNC_canvas, canvas {\n    cursor: crosshair !important;\n}\n\n/*\n * noVNC base CSS"),
]

STAMPS = {
    LITE: ("</body>", "<!-- %s; see scripts/device/novnc_patch.py -->\n</body>"),
    UI: ("", "\n// %s; see scripts/device/novnc_patch.py\n"),
    RFB: ("", "\n// %s; see scripts/device/novnc_patch.py\n"),
    CURSOR: ("", "\n// %s; see scripts/device/novnc_patch.py\n"),
    BASE_CSS: ("", "\n/* %s; see scripts/device/novnc_patch.py */\n"),
}


def patch_json_configs(root, report):
    for fname in ("defaults.json", "mandatory.json"):
        fpath = os.path.join(root, fname)
        try:
            data = {}
            if os.path.exists(fpath):
                with open(fpath, "r", encoding="utf-8") as f:
                    try:
                        data = json.load(f)
                    except Exception:
                        data = {}
            changed = False
            if fname == "defaults.json":
                for k, v in [("autoconnect", True), ("resize", "scale"), ("show_dot", True)]:
                    if data.get(k) != v:
                        data[k] = v
                        changed = True
            elif fname == "mandatory.json":
                if data.get("show_dot") != True:
                    data["show_dot"] = True
                    changed = True
            if changed or not os.path.exists(fpath):
                with open(fpath, "w", encoding="utf-8") as f:
                    json.dump(data, f, indent=2)
                report.append("%s: updated with show_dot: true" % fname)
        except Exception as e:
            report.append("%s: could not update (%s)" % (fname, e))


def patch(root, report):
    patch_json_configs(root, report)

    by_file = {}
    for rel, what, old, new in EDITS:
        by_file.setdefault(rel, []).append((what, old, new))

    for rel in by_file.keys():
        path = os.path.join(root, rel)
        try:
            with open(path, "r", encoding="utf-8", errors="surrogateescape") as f:
                text = f.read()
        except OSError as e:
            report.append("%s: not readable (%s) -- skipped" % (rel, e.strerror))
            continue

        if STAMP in text:
            report.append("%s: already patched" % rel)
            continue

        pending = list(by_file.get(rel, []))
        applied, refused = [], []
        for what, old, new in pending:
            if new in text:
                continue
            n = text.count(old)
            if n == 0:
                refused.append("%s: NOT FOUND" % what)
                continue
            if n > 1:
                refused.append("%s: appears %d times, refusing ambiguous edit" % (what, n))
                continue
            text = text.replace(old, new)
            applied.append(what)

        if not applied and not refused:
            continue

        if refused:
            report.append("%s: LEFT ALONE -- %s" % (rel, "; ".join(refused)))
            continue

        anchor, stamp = STAMPS.get(rel, ("", "\n// %s\n"))
        if anchor:
            if text.count(anchor) != 1:
                report.append("%s: no single %s to stamp before -- LEFT ALONE" % (rel, anchor))
                continue
            text = text.replace(anchor, stamp % STAMP)
        else:
            text += stamp % STAMP

        try:
            backup = path + ".rbpi4b-orig"
            if not os.path.exists(backup):
                shutil.copy2(path, backup)
            with open(path, "w", encoding="utf-8", errors="surrogateescape") as f:
                f.write(text)
        except OSError as e:
            report.append("%s: could not be written (%s)" % (rel, e.strerror))
            continue

        for what in applied:
            report.append("%s: %s" % (rel, what))
        report.append("%s: backed up once to %s" % (rel, os.path.basename(path) + ".rbpi4b-orig"))


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/novnc"
    report = []
    try:
        patch(root, report)
    except Exception as e:
        report.append("unexpected failure: %s" % e)
    for line in report:
        print("novnc_patch: %s" % line, file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
