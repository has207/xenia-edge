#!/usr/bin/env python3

# Copyright 2026 Ben Vanik. All Rights Reserved.

"""Builds assets/xdb/games.json from the original Xbox title database at
https://github.com/xemu-project/xdb.

  python3 tools/xdb-index.py assets/xdb/games.json [--ref main]

The output uses x360db's games.json schema so the app reads both the same way:
id, alternative_id (the ids of the game's other releases), title and boxart.
xdb keeps each game under titles/<publisher>/<number>/, the two halves of its
title id with the number in decimal. The box art URLs point at the commit the
index was made from so they keep working when xdb moves files.
"""

import argparse
import concurrent.futures
import json
import sys
import urllib.request

REPO = "xemu-project/xdb"
API = f"https://api.github.com/repos/{REPO}"
RAW = f"https://raw.githubusercontent.com/{REPO}"

# Thumbnails first since they're about the size of x360db's covers.
COVER_FILES = [
    "cover_front_thumbnail.jpg",
    "cover_front_thumbnail.png",
    "cover_front.jpg",
    "cover_front.png",
]


def fetch(url, accept=None):
    request = urllib.request.Request(url)
    if accept:
        request.add_header("Accept", accept)
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read()


def folder_title_id(publisher, number):
    if len(publisher) != 2 or not number.isdigit():
        return None
    code = publisher.encode("ascii", "replace")
    return "{:02X}{:02X}{:04X}".format(code[0], code[1], int(number))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("output")
    parser.add_argument("--ref", default="main")
    args = parser.parse_args()

    commit = fetch(f"{API}/commits/{args.ref}",
                   "application/vnd.github.sha").decode().strip()
    tree = json.loads(fetch(f"{API}/git/trees/{commit}?recursive=1"))
    if tree.get("truncated"):
        sys.exit("xdb tree listing is truncated")
    paths = {e["path"] for e in tree["tree"] if e["type"] == "blob"}
    folders = sorted({p.rsplit("/", 1)[0] for p in paths
                      if p.startswith("titles/") and p.endswith("/info.json")
                      and p.count("/") == 3})

    def load(folder):
        return folder, json.loads(fetch(f"{RAW}/{commit}/{folder}/info.json"))

    with concurrent.futures.ThreadPoolExecutor(16) as pool:
        infos = list(pool.map(load, folders))

    games = []
    for folder, info in infos:
        _, publisher, number = folder.split("/")
        title_id = folder_title_id(publisher, number)
        info_id = (info.get("title_id") or "").upper()
        if title_id is None or info_id != title_id:
            print(f"skipping {folder}: title id {info.get('title_id')} isn't "
                  "the folder's", file=sys.stderr)
            continue
        cover = next((f for f in COVER_FILES if f"{folder}/{f}" in paths),
                     None)
        alternative_ids = []
        for release in info.get("releases") or []:
            release_id = (release.get("title_id") or "").upper()
            if release_id and release_id != title_id and \
                    release_id not in alternative_ids:
                alternative_ids.append(release_id)
        games.append({
            "id": title_id,
            "alternative_id": alternative_ids,
            "title": info.get("name", ""),
            "boxart": f"{RAW}/{commit}/{folder}/{cover}" if cover else "",
        })

    games.sort(key=lambda g: g["id"])
    with open(args.output, "w", encoding="utf-8", newline="\n") as f:
        json.dump(games, f, indent=2, ensure_ascii=False)
        f.write("\n")
    print(f"{len(games)} games from {REPO}@{commit}")


if __name__ == "__main__":
    main()
