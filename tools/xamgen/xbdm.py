#!/usr/bin/env python3
"""The XeCLI rgh subcommands run.py uses, over XBDM, for hosts XeCLI doesn't
run on. XBDM_HOST names the console. Point RGH at this file.

    xbdm.py title
    xbdm.py fs put --path CONSOLE --in LOCAL
    xbdm.py fs get --path CONSOLE --out LOCAL
    xbdm.py fs rm --path CONSOLE
    xbdm.py launch --xex CONSOLE
    xbdm.py mem peek --addr ADDR --type u32
    xbdm.py mem poke --addr ADDR --type u32 --value VALUE
    xbdm.py debug watch --duration SECONDS --raw --out JSONL

fs goes through Aurora's FTP server, Hdd: being /Hdd1, logging in as
XBDM_FTP_USER and XBDM_FTP_PASS (xboxftp by default). XBDM's sendfile hung the
console. debug watch opens a notification channel and writes each notification
as a JSON line with its first word as Type and the whole line as Raw.

Two more, which run.py doesn't use, look at a console by hand:

    xbdm.py drives
    xbdm.py dir --path CONSOLE

drives lists the drives XBDM names, a running title's game and cache drives
among them. dir lists a folder, each entry with its size and its creation and
change times.
"""

import argparse
import datetime
import ftplib
import json
import ntpath
import os
import re
import select
import socket
import struct
import sys
import time

PORT = 730


class XbdmError(Exception):
    pass


class Connection:
    def __init__(self, timeout=10):
        host = os.environ.get('XBDM_HOST')
        if not host:
            raise XbdmError('XBDM_HOST is not set')
        self.sock = socket.create_connection((host, PORT), timeout=timeout)
        self.file = self.sock.makefile('rwb', buffering=0)
        banner = self.line()
        if not banner.startswith('201'):
            raise XbdmError(f'unexpected banner {banner!r}')

    def line(self):
        data = self.file.readline()
        if not data:
            raise XbdmError('connection closed')
        return data.decode('latin-1').rstrip('\r\n')

    def send(self, command):
        self.file.write(command.encode('latin-1') + b'\r\n')
        return self.line()

    def command(self, command, ok=('200',)):
        response = self.send(command)
        if not response.startswith(ok):
            raise XbdmError(f'{command.split()[0]}: {response}')
        return response

    def multiline(self, command):
        self.command(command, ok=('202',))
        lines = []
        while (line := self.line()) != '.':
            lines.append(line)
        return lines

    def close(self):
        try:
            self.file.write(b'bye\r\n')
        except OSError:
            pass
        self.sock.close()


def title(c, args):
    for line in c.multiline('xbeinfo running'):
        if line.startswith('name='):
            print(line[len('name='):].strip('"'))


def ftp():
    f = ftplib.FTP()
    f.connect(os.environ['XBDM_HOST'], 21, timeout=30)
    f.login(os.environ.get('XBDM_FTP_USER', 'xboxftp'),
            os.environ.get('XBDM_FTP_PASS', 'xboxftp'))
    return f


def ftp_path(path):
    drive, _, rest = path.partition(':')
    if drive.lower() != 'hdd':
        raise XbdmError(f'{path}: only Hdd: is mapped to FTP')
    return '/Hdd1/' + rest.strip('\\').replace('\\', '/')


def fs_put(args):
    with ftp() as f, open(args.input, 'rb') as data:
        f.storbinary(f'STOR {ftp_path(args.path)}', data)


def fs_get(args):
    with ftp() as f, open(args.out, 'wb') as out:
        f.retrbinary(f'RETR {ftp_path(args.path)}', out.write)


def fs_rm(args):
    with ftp() as f:
        f.delete(ftp_path(args.path))


def launch(c, args):
    c.command(f'magicboot title="{args.xex}" '
              f'directory="{ntpath.dirname(args.xex)}"')


def fields(line):
    """The key=value pairs of a response line, bare words mapping to True.
    A quoted value can hold spaces."""
    return {key: value.strip('"') if eq else True
            for key, eq, value in re.findall(r'(\w+)(=?)("[^"]*"|\S*)', line)}


def filetime(hi, lo):
    ticks = (int(hi, 16) << 32) | int(lo, 16)
    if not ticks:
        return '0'
    when = datetime.datetime(1601, 1, 1) + datetime.timedelta(
        microseconds=ticks // 10)
    return f'{when:%Y-%m-%d %H:%M:%S}.{ticks % 10**7:07d} ({ticks:#x})'


def drives(c, args):
    for line in c.multiline('drivelist'):
        print(fields(line).get('drivename', line))


def dir_list(c, args):
    keys = ('name', 'sizehi', 'sizelo', 'createhi', 'createlo', 'changehi',
            'changelo')
    for line in c.multiline(f'dirlist name="{args.path}"'):
        f = fields(line)
        if not all(isinstance(f.get(key), str) for key in keys):
            raise XbdmError(f'dirlist: unexpected line {line!r}')
        size = (int(f['sizehi'], 16) << 32) | int(f['sizelo'], 16)
        kind = 'dir ' if f.get('directory') else 'file'
        print(f'{kind} {f["name"]}  size {size}\n'
              f'  create {filetime(f["createhi"], f["createlo"])}\n'
              f'  change {filetime(f["changehi"], f["changelo"])}')


def mem_peek(c, args):
    data = ''.join(c.multiline(f'getmem addr={int(args.addr, 0):#x} '
                               f'length=4'))
    if '?' in data:
        raise XbdmError(f'getmem: {args.addr} is not readable')
    print(f'0x{int(data, 16):08X}')


def mem_poke(c, args):
    value = struct.pack('>I', int(args.value, 0) & 0xFFFFFFFF)
    c.command(f'setmem addr={int(args.addr, 0):#x} data={value.hex()}')


def debug_watch(c, args):
    # reverse makes this connection the channel, so nothing has to listen.
    c.command(f'notify reconnectport={PORT} reverse', ok=('205',))
    deadline = time.time() + args.duration
    pending = b''
    with open(args.out, 'w') as out:
        while time.time() < deadline:
            # A timed out read would leave the socket's file unusable.
            if not select.select([c.sock], [], [], 0.5)[0]:
                continue
            chunk = c.sock.recv(4096)
            if not chunk:
                break
            pending += chunk
            *lines, pending = pending.split(b'\r\n')
            for raw in lines:
                line = raw.decode('latin-1')
                if line:
                    out.write(json.dumps({'Type': line.split()[0],
                                          'Raw': line}) + '\n')
                    out.flush()


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('title').set_defaults(run=title)
    fs = sub.add_parser('fs').add_subparsers(dest='fs', required=True)
    p = fs.add_parser('put')
    p.add_argument('--path', required=True)
    p.add_argument('--in', dest='input', required=True)
    p.set_defaults(run=fs_put)
    p = fs.add_parser('get')
    p.add_argument('--path', required=True)
    p.add_argument('--out', required=True)
    p.set_defaults(run=fs_get)
    p = fs.add_parser('rm')
    p.add_argument('--path', required=True)
    p.set_defaults(run=fs_rm)
    p = sub.add_parser('launch')
    p.add_argument('--xex', required=True)
    p.set_defaults(run=launch)
    sub.add_parser('drives').set_defaults(run=drives)
    p = sub.add_parser('dir')
    p.add_argument('--path', required=True)
    p.set_defaults(run=dir_list)
    mem = sub.add_parser('mem').add_subparsers(dest='mem', required=True)
    for name, run in (('peek', mem_peek), ('poke', mem_poke)):
        p = mem.add_parser(name)
        p.add_argument('--addr', required=True)
        p.add_argument('--type', choices=['u32'], required=True)
        if name == 'poke':
            p.add_argument('--value', required=True)
        p.set_defaults(run=run)
    debug = sub.add_parser('debug').add_subparsers(dest='debug', required=True)
    p = debug.add_parser('watch')
    p.add_argument('--duration', type=int, required=True)
    p.add_argument('--raw', action='store_true')
    p.add_argument('--out', required=True)
    p.set_defaults(run=debug_watch)
    args = parser.parse_args()
    try:
        if args.command == 'fs':
            args.run(args)
            return
        c = Connection()
        try:
            args.run(c, args)
        finally:
            c.close()
    except (XbdmError, *ftplib.all_errors) as e:
        sys.exit(f'xbdm: {e}')


if __name__ == '__main__':
    main()
