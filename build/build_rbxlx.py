#!/usr/bin/env python3
"""
build_rbxlx.py
--------------
Generates a Roblox place file (DonationCentral.rbxlx) from the Luau source tree,
mirroring default.project.json. No external dependencies, no network, no Roblox
tooling required -- it just emits the documented XML place format, which Roblox
Studio opens exactly like a binary .rbxl (File -> Open from File).

Run from the repo root:   python3 build/build_rbxlx.py
"""

import html
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "DonationCentral.rbxlx")

_ref = 0


def ref():
    global _ref
    _ref += 1
    return "RBX%08X" % _ref


def read(rel):
    with open(os.path.join(ROOT, rel), "r", encoding="utf-8") as f:
        return f.read()


def cdata(source):
    # CDATA only breaks on the literal "]]>"; split those safely. Lua block
    # comments end in "]]" which is harmless unless immediately followed by ">".
    safe = source.replace("]]>", "]]]]><![CDATA[>")
    return "<![CDATA[" + safe + "]]>"


def script(class_name, name, path, run_context=None):
    src = read(path)
    props = [f"<string name=\"Name\">{html.escape(name)}</string>",
             f"<ProtectedString name=\"Source\">{cdata(src)}</ProtectedString>"]
    if run_context is not None:
        props.append(f"<token name=\"RunContext\">{run_context}</token>")
    return {"class": class_name, "props": props, "children": []}


def folder(name):
    return {
        "class": "Folder",
        "props": [f"<string name=\"Name\">{html.escape(name)}</string>"],
        "children": [],
    }


def service(class_name):
    # Services carry no properties; Studio matches them by class on open.
    return {"class": class_name, "props": [], "children": []}


def render(node, depth=1):
    indent = "\t" * depth
    out = [f"{indent}<Item class=\"{node['class']}\" referent=\"{ref()}\">"]
    out.append(f"{indent}\t<Properties>")
    for p in node["props"]:
        out.append(f"{indent}\t\t{p}")
    out.append(f"{indent}\t</Properties>")
    for child in node["children"]:
        out.append(render(child, depth + 1))
    out.append(f"{indent}</Item>")
    return "\n".join(out)


def build_tree():
    # ReplicatedStorage > DonationShared > {Config, Net}
    rs = service("ReplicatedStorage")
    shared = folder("DonationShared")
    shared["children"].append(script("ModuleScript", "Config", "src/shared/Config.luau"))
    shared["children"].append(script("ModuleScript", "Net", "src/shared/Net.luau"))
    rs["children"].append(shared)

    # ServerScriptService > DonationCentral (Script) + sibling ModuleScripts
    sss = service("ServerScriptService")
    server = script("Script", "DonationCentral", "src/server/init.server.luau", run_context=0)
    for mod in ["Data", "Donations", "Leaderboards", "Stands", "Events", "Admin"]:
        server["children"].append(
            script("ModuleScript", mod, f"src/server/{mod}.luau")
        )
    sss["children"].append(server)

    # StarterPlayer > StarterPlayerScripts > DonationClient (LocalScript)
    sp = service("StarterPlayer")
    sps = service("StarterPlayerScripts")
    sps["children"].append(
        script("LocalScript", "DonationClient", "src/client/init.client.luau")
    )
    sp["children"].append(sps)

    return [rs, sss, sp]


def main():
    header = (
        '<roblox xmlns:xmime="http://www.w3.org/2005/05/xmlmime" '
        'xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" '
        'xsi:noNamespaceSchemaLocation="http://www.roblox.com/roblox.xsd" '
        'version="4">'
    )
    parts = [
        '<?xml version="1.0" encoding="utf-8"?>',
        header,
        '\t<Meta name="ExplicitAutoJoints">true</Meta>',
    ]
    for node in build_tree():
        parts.append(render(node, 1))
    parts.append("</roblox>")

    xml = "\n".join(parts) + "\n"
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(xml)

    size = os.path.getsize(OUT)
    print(f"Wrote {OUT} ({size} bytes)")
    # Sanity: make sure every source file made it in.
    for rel in [
        "src/shared/Config.luau", "src/shared/Net.luau",
        "src/server/init.server.luau", "src/server/Data.luau",
        "src/server/Donations.luau", "src/server/Leaderboards.luau",
        "src/server/Stands.luau", "src/server/Events.luau",
        "src/server/Admin.luau", "src/client/init.client.luau",
    ]:
        if not os.path.exists(os.path.join(ROOT, rel)):
            print(f"WARNING: missing source {rel}", file=sys.stderr)


if __name__ == "__main__":
    main()
