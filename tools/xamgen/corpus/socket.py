"""Socket corpus: TCP over the loopback address. A non-blocking connect to a
port nobody listens on and the select sets it fails in, socket and TCP option
values set and read back at every length, and a non-blocking connect to a
listener followed by a receive with no data.

Each call is preceded by WSASetLastError(0) and followed by WSAGetLastError,
so a call that fails shows its error and one that succeeds shows 0. Socket
handles go to variables rather than being recorded, since their values differ
between runs. select gets its sets filled in with RtlFillMemoryUlong and only
their counts are recorded.

The XEX has a LAN key, so Xenia makes its TCP sockets System Link streams
unless System Link is off. Run it both ways: as is, and for host sockets with
`run.py xenia OUT --arg=--systemlink_transport=off`. A Xenia without
non-blocking connects blocks in the last scenario's receive.
"""

import struct

CALLER = 1  # XNCALLER_TITLE

AF_INET = 2
SOCK_STREAM = 1
IPPROTO_TCP = 6
FIONBIO = 0x8004667E

SOL_SOCKET = 0xFFFF
SO_LINGER = 0x0080
SO_SNDBUF = 0x1001
SO_RCVBUF = 0x1002
SO_SNDTIMEO = 0x1005
SO_RCVTIMEO = 0x1006
SO_ERROR = 0x1007
SO_TYPE = 0x1008
TCP_NODELAY = 0x0001

LOOPBACK = 0x7F000001
CLOSED_PORT = 31233
LISTEN_PORT = 31234
FD_SET_SIZE = 4 + 4 * 64
SELECT_SECONDS = 5


def net(s, export, *args, name, handle=None, capture=()):
    """Calls a socket export as a title does, between clearing and reading
    the last error. With `handle`, the returned handle goes to that variable
    instead of being recorded."""
    s.call('NetDll_WSASetLastError', 0, name=f'{name}: clear', void=True)
    if handle:
        s.call(export, CALLER, *args, name=name, capture=capture,
               store=[(handle, 'r3')], void=True)
    else:
        s.call(export, CALLER, *args, name=name, capture=capture)
    s.call('NetDll_WSAGetLastError', name=f'{name}: error')


def start(s, params=0):
    s.call('NetDll_XNetStartup', CALLER, params, name='xnet startup')
    s.call('NetDll_WSAStartup', CALLER, 0x0202, s.alloc(0x200, 'wsadata'),
           name='wsa startup')


def stop(s):
    s.call('NetDll_WSACleanup', CALLER, name='wsa cleanup')
    s.call('NetDll_XNetCleanup', CALLER, 0, name='xnet cleanup')


def open_socket(s, variable, name):
    handle = s.variable(variable)
    net(s, 'NetDll_socket', AF_INET, SOCK_STREAM, IPPROTO_TCP, name=name,
        handle=handle)
    return handle


def close(s, handle, name):
    net(s, 'NetDll_closesocket', handle, name=name)


def non_blocking(s, handle, name):
    net(s, 'NetDll_ioctlsocket', handle, FIONBIO,
        s.alloc(4, 'on', struct.pack('>I', 1)), name=name)


def address(s, addr, port):
    return s.alloc(16, 'address', struct.pack('>HHI', AF_INET, port, addr),
                   fill=0)


def connect(s, handle, port, name):
    net(s, 'NetDll_connect', handle, address(s, LOOPBACK, port), 16, name=name)


def set_option(s, handle, level, option, data, name, length=None):
    value = s.alloc(max(len(data), 4), 'value', data)
    net(s, 'NetDll_setsockopt', handle, level, option, value,
        len(data) if length is None else length, name=name)


def get_option(s, handle, level, option, name, length=4):
    value = s.alloc(8, 'value')
    length_ptr = s.alloc(4, 'length', struct.pack('>I', length))
    net(s, 'NetDll_getsockopt', handle, level, option, value, length_ptr,
        name=name, capture=[value, length_ptr])


def word(value):
    return struct.pack('>I', value)


class Sets:
    """select's read, write and error sets, refilled with one socket for each
    call that passes them."""

    def __init__(self, s):
        self.sets = {kind: s.alloc(FD_SET_SIZE, f'{kind} set', fill=0)
                     for kind in ('read', 'write', 'error')}
        self.timeout = s.alloc(8, 'timeout',
                               struct.pack('>II', SELECT_SECONDS, 0))

    def select(self, s, handle, kinds, name):
        args = []
        for kind in ('read', 'write', 'error'):
            if kind not in kinds:
                args.append(0)
                continue
            fds = self.sets[kind]
            s.write(fds.prefix(4), word(1))
            s.call('RtlFillMemoryUlong', fds + 4, 4, handle,
                   name=f'{name}: fill {kind}', void=True)
            args.append(fds)
        net(s, 'NetDll_select', 0, *args, self.timeout, name=name,
            capture=[self.sets[kind].prefix(4) for kind in kinds])


def build(corpus):
    corpus.lan_key = bytes(range(16))

    s = corpus.scenario('refused connect')
    start(s)
    handle = open_socket(s, 'socket', 'socket')
    non_blocking(s, handle, 'non-blocking')
    connect(s, handle, CLOSED_PORT, 'connect')
    sets = Sets(s)
    sets.select(s, handle, ('read', 'write', 'error'), 'select all')
    sets.select(s, handle, ('write',), 'select write')
    sets.select(s, handle, ('read',), 'select read')
    sets.select(s, handle, ('error',), 'select error')
    sets.select(s, handle, ('read', 'write'), 'select read and write')
    get_option(s, handle, SOL_SOCKET, SO_ERROR, 'error option')
    close(s, handle, 'close')
    stop(s)

    s = corpus.scenario('options')
    start(s)
    handle = open_socket(s, 'socket', 'socket')
    get_option(s, handle, SOL_SOCKET, SO_RCVBUF, 'default receive buffer')
    get_option(s, handle, SOL_SOCKET, SO_SNDBUF, 'default send buffer')
    get_option(s, handle, IPPROTO_TCP, TCP_NODELAY, 'default no delay')
    get_option(s, handle, SOL_SOCKET, SO_LINGER, 'default linger')
    set_option(s, handle, SOL_SOCKET, SO_RCVBUF, word(0x8000),
               'receive buffer')
    get_option(s, handle, SOL_SOCKET, SO_RCVBUF, 'receive buffer back')
    set_option(s, handle, SOL_SOCKET, SO_SNDBUF, word(0x8000), 'send buffer')
    get_option(s, handle, SOL_SOCKET, SO_SNDBUF, 'send buffer back')
    for size in (0x100000, 0, 0xFFFFFFFF):
        for option, kind in ((SO_RCVBUF, 'receive'), (SO_SNDBUF, 'send')):
            set_option(s, handle, SOL_SOCKET, option, word(size),
                       f'{kind} buffer {size:#x}')
            get_option(s, handle, SOL_SOCKET, option,
                       f'{kind} buffer {size:#x} back')
    set_option(s, handle, SOL_SOCKET, SO_RCVTIMEO, word(1234),
               'receive timeout')
    get_option(s, handle, SOL_SOCKET, SO_RCVTIMEO, 'receive timeout back')
    get_option(s, handle, SOL_SOCKET, SO_RCVTIMEO, 'receive timeout, 8 bytes',
               length=8)
    get_option(s, handle, SOL_SOCKET, SO_RCVTIMEO, 'receive timeout, 2 bytes',
               length=2)
    set_option(s, handle, SOL_SOCKET, SO_SNDTIMEO, word(5678) + word(0x1111),
               'send timeout, 8 bytes')
    get_option(s, handle, SOL_SOCKET, SO_SNDTIMEO, 'send timeout back')
    set_option(s, handle, IPPROTO_TCP, TCP_NODELAY, b'\1', 'no delay, 1 byte')
    get_option(s, handle, IPPROTO_TCP, TCP_NODELAY, 'no delay back')
    get_option(s, handle, IPPROTO_TCP, TCP_NODELAY, 'no delay, 1 byte back',
               length=1)
    set_option(s, handle, IPPROTO_TCP, TCP_NODELAY, word(0x100),
               'no delay 0x100')
    get_option(s, handle, IPPROTO_TCP, TCP_NODELAY, 'no delay 0x100 back')
    set_option(s, handle, SOL_SOCKET, SO_LINGER, struct.pack('>HH', 1, 5),
               'linger')
    get_option(s, handle, SOL_SOCKET, SO_LINGER, 'linger back')
    get_option(s, handle, SOL_SOCKET, SO_TYPE, 'type')
    get_option(s, handle, SOL_SOCKET, SO_ERROR, 'error option')
    close(s, handle, 'close')
    stop(s)

    # The default buffer sizes come from XNetStartup's parameters, here 8 KB
    # for receives and 4 KB for sends.
    s = corpus.scenario('startup buffer sizes')
    params = s.alloc(13, 'params', bytes([13, 0, 0, 0, 8, 4]), fill=0)
    start(s, params)
    handle = open_socket(s, 'socket', 'socket')
    get_option(s, handle, SOL_SOCKET, SO_RCVBUF, 'default receive buffer')
    get_option(s, handle, SOL_SOCKET, SO_SNDBUF, 'default send buffer')
    close(s, handle, 'close')
    stop(s)

    # The receive comes last: on a Xenia whose connect drops FIONBIO it
    # blocks.
    s = corpus.scenario('accepted connect')
    start(s)
    listener = open_socket(s, 'listener', 'listener')
    non_blocking(s, listener, 'listener non-blocking')
    set_option(s, listener, SOL_SOCKET, SO_LINGER, struct.pack('>HH', 1, 7),
               'listener linger')
    net(s, 'NetDll_bind', listener, address(s, 0, LISTEN_PORT), 16,
        name='bind')
    net(s, 'NetDll_listen', listener, 1, name='listen')
    client = open_socket(s, 'client', 'client')
    non_blocking(s, client, 'client non-blocking')
    set_option(s, client, SOL_SOCKET, SO_RCVTIMEO, word(1234),
               'receive timeout')
    connect(s, client, LISTEN_PORT, 'connect')
    sets = Sets(s)
    sets.select(s, client, ('write', 'error'), 'select write and error')
    connect(s, client, LISTEN_PORT, 'connect again')
    get_option(s, client, SOL_SOCKET, SO_RCVTIMEO,
               'receive timeout after connect')
    get_option(s, client, SOL_SOCKET, SO_ERROR, 'error option')
    accepted = s.variable('accepted')
    net(s, 'NetDll_accept', listener, 0, 0, name='accept', handle=accepted)
    get_option(s, accepted, SOL_SOCKET, SO_LINGER, 'accepted linger')
    buffer = s.alloc(0x10, 'buffer')
    net(s, 'NetDll_recv', client, buffer, 0x10, 0, name='receive nothing',
        capture=[buffer])
    close(s, accepted, 'close accepted')
    close(s, client, 'close client')
    close(s, listener, 'close listener')
    stop(s)
