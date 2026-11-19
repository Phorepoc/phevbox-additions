#!/usr/bin/env python3
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: GPL-3.0-only
r"""pack -- package phevboxsvc as a SIS file for Phorepoc (Python 3; in the pdk's toolchain container, or on a host with the pdk checked out).

    python3 recipes/pack.py --service phevboxsvc.exe --version 0.2.0 --out phevbox-additions-x32.sis

The package carries:
  * `!:\sys\bin\phevboxsvc.exe`       the service (CommDD: it opens the kernel's PheVmmDev channel)
  * `!:\resource\phe\services\phevbox.txt`  the image starts the service at boot from this list (pheshellsel)
  * `!:\private\10003a3f\import\apps\phevboxsvc_reg.rsc` + `!:\resource\apps\phevboxsvc_loc.rsc` + the icon: the menu entry
    "Guest Additions" (it starts the service too, for a device that has not rebooted since the install)
  * the licences, NOTICE and the written source offer, in `!:\resource\phevbox\licences\`
Nothing is signed (Phorepoc installs unsigned). Needs the pdk's tools/emitter (sis.py, e32image.py, mmp.py; env PHOREPOC_PDK,
default ../pdk) and, for the registration and the icon, the tools of the pdk's toolchain image (rcomp, mifconv).
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PMS = os.path.dirname(HERE)
PDK = os.environ.get("PHOREPOC_PDK", os.path.join(os.path.dirname(PMS), "pdk"))
sys.path.insert(0, os.path.join(PDK, "tools", "emitter"))
import e32image as E32   # noqa: E402
import mmp as MMP        # noqa: E402
import sis as SIS        # noqa: E402

APP_UID = 0xE5A0F201     # phevboxsvc.exe (its UID3, the application)
PKG_UID = 0xE5A0F200     # the package
CAPS = ("CommDD",)       # phevboxsvc.mmp
BS = chr(92)             # a backslash: file names below are built with it, never written as an escape


def target(*parts):
    """An install path: !:\\part\\part ..."""
    return "!:" + "".join(BS + p for p in parts)


def caps_bits(names):
    return sum(1 << MMP.Mmp.CAPS.index(n.lower()) for n in names)


def with_caps(img, caps):
    b = bytearray(img)
    struct.pack_into("<II", b, 0x88, caps & 0xFFFFFFFF, 0)
    E32.set_header_crc(b)
    return bytes(b)


def compile_rss(text, name, out_rsc):
    d = tempfile.mkdtemp()
    rss = os.path.join(d, name + ".rss")
    with open(rss, "w") as f:
        f.write(text)
    r = subprocess.run([sys.executable, os.path.join(PDK, "tools", "rsc.py"), rss, out_rsc], cwd=PDK, capture_output=True, text=True)
    if r.returncode or not os.path.isfile(out_rsc):
        sys.exit("pack: rcomp failed on %s:\n%s%s" % (name, r.stdout[-800:], r.stderr[-800:]))


def make_mif(svg, out_name):
    out_mif = os.path.join(tempfile.mkdtemp(), out_name)
    r = subprocess.run([sys.executable, os.path.join(PDK, "tools", "app_icon_mif.py"), svg, out_mif], cwd=PDK, capture_output=True, text=True)
    if r.returncode or not os.path.isfile(out_mif):
        sys.exit("pack: app_icon_mif failed: " + (r.stdout + r.stderr)[-400:])
    with open(out_mif, "rb") as f:
        return f.read()


def licences():
    out = []
    names = ["NOTICE", "SOURCE-OFFER.txt", "LICENSE"] + ["LICENSES/" + x for x in sorted(os.listdir(os.path.join(PMS, "LICENSES")))]
    for f in names:
        with open(os.path.join(PMS, f), "rb") as fh:
            out.append((fh.read(), target("resource", "phevbox", "licences", os.path.basename(f)), None))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--service", required=True, help="phevboxsvc.exe built with sis=True")
    ap.add_argument("--icon", default=os.path.join(PMS, "os", "src", "phevboxsvc", "phevbox_icon.svg"))
    ap.add_argument("--version", default="0.1.0")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    version = tuple(int(x) for x in a.version.split("."))
    td = tempfile.mkdtemp()
    with open(a.service, "rb") as f:
        exe = with_caps(f.read(), caps_bits(CAPS))
    files = [(exe, target("sys", "bin", "phevboxsvc.exe"), caps_bits(CAPS))]
    mif = "phevbox_%08x.mif" % APP_UID
    with open(os.path.join(PMS, "os", "data", "phevbox-service.txt"), "rb") as f:
        files.append((f.read(), target("resource", "phe", "services", "phevbox.txt"), None))   # started at boot by the image
    files.append((make_mif(a.icon, mif), target("resource", "apps", mif), None))
    # (in a resource file a backslash is written twice)
    bb = BS + BS
    reg = ("#include <appinfo.rh>\nUID2 KUidAppRegistrationResourceFile\nUID3 0x%08X\n"
           "RESOURCE APP_REGISTRATION_INFO\n    {\n    app_file = \"phevboxsvc\";\n"
           "    localisable_resource_file = \"%sresource%sapps%sphevboxsvc_loc\";\n    }\n") % (APP_UID, bb, bb, bb)
    loc = ("#include <appinfo.rh>\nRESOURCE LOCALISABLE_APP_INFO\n    {\n    short_caption = \"Guest Add.\";\n"
           "    caption_and_icon = CAPTION_AND_ICON_INFO\n        {\n        caption = \"Guest Additions\";\n        number_of_icons = 1;\n"
           "        icon_file = \"%sresource%sapps%s%s\";\n        };\n    }\n") % (bb, bb, bb, mif)
    compile_rss(reg, "phevboxsvc_reg", os.path.join(td, "phevboxsvc_reg.rsc"))
    compile_rss(loc, "phevboxsvc_loc", os.path.join(td, "phevboxsvc_loc.rsc"))
    with open(os.path.join(td, "phevboxsvc_reg.rsc"), "rb") as f:
        files.append((f.read(), target("private", "10003a3f", "import", "apps", "phevboxsvc_reg.rsc"), None))
    with open(os.path.join(td, "phevboxsvc_loc.rsc"), "rb") as f:
        files.append((f.read(), target("resource", "apps", "phevboxsvc_loc.rsc"), None))
    files += licences()
    for _, t, _ in files:
        assert t.startswith("!:" + BS) and all(32 <= ord(c) < 127 for c in t), "bad install path %r" % t
    pkg = SIS.build_package(files, app_uid=PKG_UID, vendor="Phorepoc", app_name="Guest Additions", signer=None, version=version)
    with open(a.out, "wb") as f:
        f.write(pkg)
    print("%s: %d bytes, package uid %#x, %d files" % (a.out, len(pkg), PKG_UID, len(files)))
    for _, t, _ in files:
        print("    " + t)


if __name__ == "__main__":
    main()
