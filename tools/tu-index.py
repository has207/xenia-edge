#!/usr/bin/env python3

# Copyright 2026 Ben Vanik. All Rights Reserved.

"""Builds assets/title_updates/title_updates.json from the Xbox 360 title
updates archived at https://archive.org/details/xbox_360_title_updates.

  python3 tools/tu-index.py assets/title_updates/title_updates.json [--cache DIR]

The item holds a zip per title, <name>_<title id>.zip, laid out as
<media id>/[TU<n>_]v<version>/<package>, the version being the one in the
package's header. Each zip's "View Contents" page lists its files with their
sizes. When a folder has no TU number (or TU0), or its version is zero as in
early updates, the package is streamed up to its first patch and the
connection dropped. The patch's target version has the TU number in its build
field. Its source version is the release's, or the target's with the build
field zeroed when the source is zero. Requests go out one at a time a second
apart since archive.org bans addresses that hammer it. --cache keeps listings
and patch versions so a rerun only fetches what's missing or changed.

The output maps title id -> media id -> the updates for that release, each
with its TU number, the version of the release it applies to, its size and
download URL, sorted by release version, then TU number.
"""

import argparse
import hashlib
import html
import json
import os
import re
import struct
import sys
import time
import urllib.parse
import urllib.request

ITEM = "xbox_360_title_updates"
USER_AGENT = "xenia-tu-index/1.0"
ROW = re.compile(
    r'<tr><td><a href="(?P<href>[^"]+)">(?P<path>[^<]+)</a><td>[^<]*<td>'
    r'[^<]*<td id="size">(?P<size>\d+)</tr>')
ZIP_NAME = re.compile(r"^.*_(?P<title_id>[0-9A-Fa-f]{8})\.zip$")
MEDIA_ID = re.compile(r"^[0-9A-Fa-f]{8}$")
UPDATE_FOLDER = re.compile(
    r"^(?:TU(?P<tu>\d+)_)?v(?P<version>\d+\.\d+\.\d+\.\d+)$")


class Pacer:
    def __init__(self, interval):
        self.interval = interval
        self.last = 0.0

    def wait(self):
        delay = self.last + self.interval - time.monotonic()
        if delay > 0:
            time.sleep(delay)
        self.last = time.monotonic()


def fetch(url, pacer, read=lambda response: response.read()):
    for attempt in range(3):
        pacer.wait()
        try:
            request = urllib.request.Request(
                url, headers={"User-Agent": USER_AGENT})
            with urllib.request.urlopen(request, timeout=120) as response:
                return read(response)
        except Exception as e:
            error = e
            if attempt == 2:
                break
            retry_after = getattr(e, "headers", None) and e.headers.get(
                "Retry-After")
            time.sleep(int(retry_after)
                       if retry_after and retry_after.isdigit()
                       else 60 * (attempt + 1))
    sys.exit(f"giving up on {url}: {error}")


def listing(zip_name, zip_sha1, pacer, cache):
    path = None
    if cache:
        # The zip's own hash keeps a listing from outliving a change to it.
        key = hashlib.sha1(
            f"{zip_name}\n{zip_sha1}".encode("utf-8")).hexdigest()
        path = os.path.join(cache, key + ".html")
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                return f.read()
    url = "https://archive.org/download/{}/{}/".format(
        ITEM, urllib.parse.quote(zip_name))
    page = fetch(url, pacer).decode("utf-8")
    if '<table class="archext">' not in page:
        sys.exit(f"{zip_name} has no listing")
    if path:
        with open(path + ".tmp", "w", encoding="utf-8") as f:
            f.write(page)
        os.replace(path + ".tmp", path)
    return page


def version_key(version):
    return tuple(int(part) for part in version.split("."))


def version_string(value):
    return "{}.{}.{}.{}".format(value >> 28, (value >> 24) & 0xF,
                                (value >> 8) & 0xFFFF, value & 0xFF)


def patch_header_versions(headers):
    """The target and source versions in the XEX2 headers of a patch."""
    count = (struct.unpack(">I", headers[0x14:0x18])[0]
             if len(headers) >= 0x18 else 0)
    if 0x18 + count * 8 > len(headers):
        return None
    for i in range(count):
        key, offset = struct.unpack(">II", headers[0x18 + i * 8:0x20 + i * 8])
        if key == 0x5FF and offset + 12 <= len(headers):
            _, target, source = struct.unpack(
                ">III", headers[offset:offset + 12])
            return target, source
    return None


def first_patch_versions(response):
    """The versions of the first patch in a package, read as it streams in so
    the rest of the package isn't fetched. Every patch of an update has the
    same ones.
    """
    data = bytearray()
    start = 0
    ended = False
    while not ended:
        chunk = response.read(1 << 16)
        data += chunk
        ended = not chunk
        while True:
            pos = data.find(b"XEX2", start)
            if pos == -1:
                start = max(start, len(data) - 3)
                break
            if not ended and len(data) < pos + 0x10000:
                start = pos
                break
            versions = patch_header_versions(bytes(data[pos:pos + 0x10000]))
            if versions:
                return versions
            start = pos + 4
        if start > 1 << 20:
            del data[:start]
            start = 0
    return None


def patch_versions(url, pacer, cache):
    path = None
    if cache:
        key = hashlib.sha1(url.encode("utf-8")).hexdigest()
        path = os.path.join(cache, "patches", key + ".json")
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                return json.load(f)
    versions = fetch(url, pacer, first_patch_versions)
    # A connection cut short reads as a package without a patch, which is
    # only worth keeping once it's certain.
    if path and versions:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path + ".tmp", "w", encoding="utf-8") as f:
            json.dump(versions, f)
        os.replace(path + ".tmp", path)
    return versions


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("output")
    parser.add_argument("--cache")
    args = parser.parse_args()
    if args.cache:
        os.makedirs(args.cache, exist_ok=True)

    pacer = Pacer(1.0)
    metadata = json.loads(
        fetch(f"https://archive.org/metadata/{ITEM}", pacer))
    zips = sorted((f for f in metadata["files"]
                   if f["name"].lower().endswith(".zip")),
                  key=lambda f: f["name"])

    titles = {}
    skipped = 0
    for i, zip_file in enumerate(zips, 1):
        zip_name = zip_file["name"]
        match = ZIP_NAME.match(zip_name)
        if not match:
            print(f"skipping {zip_name}: no title id", file=sys.stderr)
            continue
        title_id = match["title_id"].upper()
        rows = list(ROW.finditer(listing(zip_name, zip_file.get("sha1", ""),
                                         pacer, args.cache)))
        # A listing laid out differently would match none of its rows.
        if "filecount" in zip_file and len(rows) != int(zip_file["filecount"]):
            sys.exit(f"{zip_name}: listed {len(rows)} files, the item says "
                     f"{zip_file['filecount']}")
        for row in rows:
            parts = html.unescape(row["path"]).split("/")
            folder = UPDATE_FOLDER.match(parts[1]) if len(parts) == 3 else None
            if not folder or not MEDIA_ID.match(parts[0]):
                print(f"skipping {zip_name}: {row['path']}", file=sys.stderr)
                skipped += 1
                continue
            tu = int(folder["tu"] or 0)
            update = {"tu": tu} if tu else {}
            update.update({
                "base_version": folder["version"],
                "size": int(row["size"]),
                "url": urllib.parse.urljoin("https://archive.org/",
                                            html.unescape(row["href"])),
            })
            titles.setdefault(title_id, {}).setdefault(
                parts[0].upper(), []).append(update)
        if i % 100 == 0:
            print(f"{i}/{len(zips)} listed", file=sys.stderr)

    for title_id, medias in titles.items():
        for updates in medias.values():
            for update in updates:
                if "tu" in update and update["base_version"] != "0.0.0.0":
                    continue
                versions = patch_versions(update["url"], pacer, args.cache)
                if not versions:
                    print(f"no patch in {update['url']}", file=sys.stderr)
                    continue
                target, source = versions
                if update["base_version"] == "0.0.0.0":
                    # One with no source version has the release's in its
                    # target, but for the build field.
                    update["base_version"] = version_string(
                        source or target & 0xFF0000FF)
                if "tu" not in update and (target >> 8) & 0xFFFF:
                    update["tu"] = (target >> 8) & 0xFFFF
                print(f"{title_id} TU{update.get('tu', '?')}: base "
                      f"{update['base_version']}", file=sys.stderr)

    for medias in titles.values():
        for updates in medias.values():
            updates.sort(key=lambda u: (version_key(u["base_version"]),
                                        u.get("tu", -1)))
    titles = {title_id: {media_id: titles[title_id][media_id]
                         for media_id in sorted(titles[title_id])}
              for title_id in sorted(titles)}
    with open(args.output, "w", encoding="utf-8", newline="\n") as f:
        json.dump(titles, f, indent=2, ensure_ascii=False)
        f.write("\n")
    count = sum(len(u) for m in titles.values() for u in m.values())
    print(f"{count} updates for {len(titles)} titles from {ITEM}, "
          f"{skipped} skipped")


if __name__ == "__main__":
    main()
