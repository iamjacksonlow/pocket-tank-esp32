#!/usr/bin/env python3
"""make_installer.py - assemble the browser installer (ESP Web Tools).

Gathers the firmware build's bootloader / partition table / app, the
shipped model_q4.bin, the installer page and the vendored ESP Web Tools
bundle into ONE static folder that any HTTPS host can serve as-is:

    installer/dist/
        index.html          the page (version stamped in)
        manifest.json       what to flash where (ESP Web Tools format); the
                            Install button: NEVER erases, so a tank already
                            on the board (its save lives in NVS at 0x9000,
                            which no part touches) carries on after an update
        manifest-erase.json the same parts behind the "start over" button:
                            ESP Web Tools' erase question first
        firmware/*.bin      bootloader, partition table, app, model
        vendor/esp-web-tools-<tag>/*.js   the flasher (Apache-2.0, vendored so the
                                    page has no third-party runtime deps;
                                    the install dialog gets the one-line
                                    never_erase patch below at assembly)

Offsets come from the build's flasher_args.json (bootloader / partition
table / app) and from firmware/partitions.csv (the model partition), so a
layout change can't silently ship a stale offset.

    tools/make_installer.py                      # default build dir
    tools/make_installer.py --build-dir path     # another idf.py -B dir
    tools/make_installer.py --version 1.2.0      # instead of git describe

Web Serial needs a secure context: serve the folder over HTTPS (or from
http://localhost for a local check: `python3 -m http.server -d installer/dist`)."""
import argparse, datetime, hashlib, json, os, re, shutil, struct, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BUILD = os.path.expanduser("~/.cache/pocket-tank/fw-build")

# ESP Web Tools (10.4.0) has no manifest option for "install without erasing"
# on a device that does not speak Improv: with new_install_prompt_erase the
# dialog asks (checkbox off by default), without it the dialog ERASES first,
# unconditionally. The tank's save would go with it. So the vendored dialog
# gets one edit at assembly: a manifest with "never_erase": true skips the
# question and starts a plain install (bootloader / partition table / app /
# model written at their offsets, NVS untouched). The edit is a literal
# replace of the two click handlers; a vendor upgrade that changes the text
# fails the build here instead of quietly shipping an erasing page.
DIALOG_ERASE_CLICK = 'this._manifest.new_install_prompt_erase?this._state="ASK_ERASE":this._startInstall(!0)'
DIALOG_NEVER_ERASE = 'this._manifest.new_install_prompt_erase?this._state="ASK_ERASE":this._startInstall(!this._manifest.never_erase)'


def patch_dialog(vendor_dir):
    """apply the never_erase edit to the copied install dialog bundle"""
    names = [n for n in os.listdir(vendor_dir) if n.startswith("install-dialog") and n.endswith(".js")]
    if len(names) != 1:
        sys.exit(f"{vendor_dir}: expected one install-dialog*.js, found {names}")
    path = os.path.join(vendor_dir, names[0])
    js = open(path).read()
    n = js.count(DIALOG_ERASE_CLICK)
    if n != 2:
        sys.exit(f"{path}: the erase click handler occurs {n} times, expected 2 - ESP Web Tools "
                 "changed; re-check the never_erase patch before shipping the installer")
    js = js.replace(DIALOG_ERASE_CLICK, DIALOG_NEVER_ERASE)
    open(path, "w").write(js)
    return names[0], hashlib.sha1(js.encode()).hexdigest()[:8]


def model_offset():
    """the 'model' row of firmware/partitions.csv -> int offset"""
    with open(os.path.join(ROOT, "firmware", "partitions.csv")) as f:
        for line in f:
            cols = [c.strip() for c in line.split(",")]
            if len(cols) >= 4 and cols[0] == "model":
                return int(cols[3], 0)
    sys.exit("partitions.csv: no 'model' partition")


# The tank's save lives in NVS (firmware/partitions.csv: nvs at 0x9000, 0x6000
# long), and "updating never erases" holds only while it stays there: the
# page writes the partition table too, so a moved or resized nvs row would
# boot the update on blank NVS (the tank gone), and a part that grew into
# 0x9000..0xF000 would overwrite it. So the build fails on either, reading the
# partition table the page actually ships.
NVS_OFFSET, NVS_SIZE = 0x9000, 0x6000


def check_nvs_untouched(parts, ptable):
    rows = {}
    raw = open(ptable, "rb").read()
    for i in range(0, len(raw) - 31, 32):
        magic, ptype, sub, off, size = struct.unpack_from("<HBBII", raw, i)
        if magic != 0x50AA:                  # 0xEBEB = the md5 row, 0xFFFF = the end
            break
        rows[raw[i + 12:i + 28].split(b"\0")[0].decode()] = (ptype, sub, off, size)
    nvs = [(label, r) for label, r in rows.items() if r[:2] == (1, 2)]   # data, nvs
    if len(nvs) != 1 or nvs[0][1][2:] != (NVS_OFFSET, NVS_SIZE):
        sys.exit(f"{ptable}: the nvs row must stay at 0x{NVS_OFFSET:x}, 0x{NVS_SIZE:x} long - found "
                 + (", ".join(f"{l} 0x{r[2]:x} 0x{r[3]:x}" for l, r in nvs) or "none")
                 + "; every keeper's tank lives there and an update would leave it behind")
    for off, src, pub in parts:
        end = off + os.path.getsize(src)
        if off < NVS_OFFSET + NVS_SIZE and end > NVS_OFFSET:
            sys.exit(f"{pub}: 0x{off:x}..0x{end:x} overlaps nvs 0x{NVS_OFFSET:x}..0x{NVS_OFFSET + NVS_SIZE:x} "
                     "- installing it would overwrite the keeper's tank")
    if "model" not in rows or rows["model"][2] != model_offset():
        sys.exit(f"{ptable}: its model row disagrees with firmware/partitions.csv - rebuild the firmware")


def release_version():
    """common/version.h's release (2026-09-29): "v0.2.0 alpha" - the number
    the tank's settings page shows - with the git build id beside it."""
    h = open(os.path.join(ROOT, "common", "version.h")).read()
    rel = re.search(r'#define PT_RELEASE\s+"([^"]+)"', h).group(1)
    stage = re.search(r'#define PT_RELEASE_STAGE\s+"([^"]*)"', h).group(1)
    return f"v{rel} {stage}".strip() + f" (build {git_version()})"


def git_version():
    try:
        out = subprocess.run(["git", "-C", ROOT, "describe", "--always", "--dirty", "--exclude=*"],   # the hash, never a tag
                             capture_output=True, text=True, check=True).stdout.strip()
        return out
    except Exception:
        return "dev"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default=DEFAULT_BUILD if os.path.isdir(DEFAULT_BUILD)
                    else os.path.join(ROOT, "firmware", "build"))
    ap.add_argument("--model", default=os.path.join(ROOT, "model", "out", "model_q4.bin"))
    ap.add_argument("--out", default=os.path.join(ROOT, "installer", "dist"))
    ap.add_argument("--version", default=None)
    ap.add_argument("--manifest-url", default="manifest.json",
                    help="what the page's install button points at: the relative default for a "
                         "self-contained folder, or an absolute URL (e.g. the GitHub Pages copy) "
                         "for a page hosted somewhere else")
    a = ap.parse_args()

    fa_path = os.path.join(a.build_dir, "flasher_args.json")
    if not os.path.isfile(fa_path):
        sys.exit(f"{fa_path}: not a firmware build dir (run idf.py build first)")
    fa = json.load(open(fa_path))
    parts = []   # (offset, source path, published name)
    for key, pub in (("bootloader", "bootloader.bin"), ("partition-table", "partition-table.bin"),
                     ("app", "pocket_tank.bin")):
        ent = fa[key]
        parts.append((int(ent["offset"], 0), os.path.join(a.build_dir, ent["file"]), pub))
    parts.append((model_offset(), a.model, "model_q4.bin"))
    parts.sort()
    for off, src, pub in parts:
        if not os.path.isfile(src):
            sys.exit(f"missing: {src}")
    check_nvs_untouched(parts, os.path.join(a.build_dir, fa["partition-table"]["file"]))

    version = a.version or release_version()
    date = datetime.date.today().isoformat()
    out = a.out
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(os.path.join(out, "firmware"))
    total = 0
    for off, src, pub in parts:
        shutil.copyfile(src, os.path.join(out, "firmware", pub))
        total += os.path.getsize(src)
    shutil.copytree(os.path.join(ROOT, "installer", "vendor"), os.path.join(out, "vendor"))
    dialog, tag = patch_dialog(os.path.join(out, "vendor", "esp-web-tools"))
    # The bundle's file names are content hashes of the PRISTINE vendor, and
    # hosts serve .js with a year's max-age: a patched dialog under the old
    # path stays the old, erasing one in every CDN and browser cache (it
    # happened, 2026-09-18). So the folder carries the patched dialog's hash -
    # the imports inside are relative, a new folder is a new URL for all of it.
    vendor = f"vendor/esp-web-tools-{tag}"
    os.rename(os.path.join(out, "vendor", "esp-web-tools"), os.path.join(out, vendor))

    build = {"chipFamily": "ESP32-S3",
             "parts": [{"path": f"firmware/{pub}", "offset": off} for off, _, pub in parts]}
    manifest = {
        "name": "Pocket Tank",
        "version": version,
        "built": date,                       # read by the page (ESP Web Tools ignores extra keys)
        "new_install_prompt_erase": False,
        "never_erase": True,                 # the patched dialog: no erase question, no erase - the
                                             # tank on the board (NVS) survives; a blank board boots fresh
        "builds": [build],
    }
    json.dump(manifest, open(os.path.join(out, "manifest.json"), "w"), indent=2)
    erase = dict(manifest, name="Pocket Tank (fresh)", new_install_prompt_erase=True)
    del erase["never_erase"]                 # the "start over" button: the dialog asks, checkbox off by default
    json.dump(erase, open(os.path.join(out, "manifest-erase.json"), "w"), indent=2)
    # Apache / LiteSpeed hosts sometimes refuse .bin or serve .json as text;
    # harmless elsewhere. The page itself must never come out of a host's
    # page cache: it names the vendor folder, and a stale page is a stale
    # (once: erasing) dialog.
    open(os.path.join(out, ".htaccess"), "w").write(
        "AddType application/octet-stream .bin\nAddType application/json .json\n"
        "AddType text/javascript .js\n"
        "<IfModule mod_headers>\n<FilesMatch \"\\.html$\">\n"
        "Header set Cache-Control \"no-cache, must-revalidate\"\n</FilesMatch>\n</IfModule>\n")

    page = open(os.path.join(ROOT, "installer", "index.html")).read()
    page = page.replace("{{VERSION}}", version).replace("{{DATE}}", date)
    page = page.replace("{{TOTAL_MB}}", f"{total / 1e6:.1f}")
    page = page.replace("{{VENDOR}}", vendor)
    page = page.replace("{{MANIFEST}}", a.manifest_url)
    page = page.replace("{{MANIFEST_ERASE}}", a.manifest_url.replace("manifest.json", "manifest-erase.json"))
    open(os.path.join(out, "index.html"), "w").write(page)
    open(os.path.join(out, ".nojekyll"), "w").close()   # GitHub Pages: serve the folder as-is

    print(f"installer -> {out}  (version {version}, {date}; manifest {a.manifest_url}; "
          f"never_erase patch in {vendor}/{dialog})")
    for off, src, pub in parts:
        print(f"  0x{off:06x}  {os.path.getsize(src):>9,} B  {pub}")
    print(f"  {total:,} B to flash")


if __name__ == "__main__":
    main()
