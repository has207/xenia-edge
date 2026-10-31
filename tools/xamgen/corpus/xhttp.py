"""XHttp corpus: what XHttp does before a request goes out. Argument checks of
XHttpOpen, XHttpConnect and XHttpOpenRequest, every option queried on each
handle type and the ones that can be set, header queries before a response,
the calls a request refuses before it's sent, closing callbacks, closing a
connect before its request and the performance counters.

XHttp reports through the last error so every call is followed by
GetLastError. A call that returns a handle keeps it in a variable rather than
recording it, since its value differs between runs. Each scenario starts and
shuts down XHttp itself, as Open needs it started on a console. Nothing here
sends a request so no server is involved.
"""

import struct

CALLER = 1  # XNCALLER_TITLE
ASYNC = 0x10000000
SECURE = 0x00800000

QUERY_REQUEST_HEADERS = 0x80000000
QUERY_SYSTEMTIME = 0x40000000
QUERY_NUMBER = 0x20000000
QUERY_CONTENT_LENGTH = 9
QUERY_HOST = 15
QUERY_RAW_HEADERS_CRLF = 22
QUERY_STATUS_CODE = 0xFFFE
QUERY_CUSTOM = 0xFFFF

HANDLE_CLOSING = 0x800
AUTH_BASIC = 1

OPTION_RECEIVE_TIMEOUT = 6
OPTION_KEEP_CONNECTION = 22
OPTION_CONTEXT_VALUE = 45
OPTION_DISABLE_FEATURE = 63
OPTION_REDIRECT_POLICY = 88
OPTION_MAX_REDIRECTS = 89
OPTION_USER_VALUE = 103
OPTION_NULL_HANDLE = 104
# Every option either xam or WinHTTP names that a 4-byte query can show.
OPTIONS = (1, 2, 3, 5, 6, 12, 22, 23, 31, 34, 45, 63, 88, 89, 90, 100, 101,
           103, 104, 105, 106, 108, 110, 113, 114)

SERVER = 'example.com'


def http(s, export, *args, name, handle=None, capture=()):
    """Calls an XHttp export as a title does, then GetLastError. With
    `handle`, the returned handle goes to that variable instead of being
    recorded."""
    if handle:
        s.call(export, CALLER, *args, name=name, capture=capture,
               store=[(handle, 'r3')], void=True)
    else:
        s.call(export, CALLER, *args, name=name, capture=capture)
    s.call('GetLastError', name=f'{name}: error')


def start(s):
    s.call('NetDll_XHttpStartup', CALLER, 0, 0, name='startup')


def stop(s):
    # The console leaves r3 as it was.
    s.call('NetDll_XHttpShutdown', CALLER, name='shutdown', void=True)


def open_session(s, name='open'):
    session = s.variable('session')
    http(s, 'NetDll_XHttpOpen', s.string('xgen', 'user agent'), 0, 0, 0,
         ASYNC, name=name, handle=session)
    return session


def connect(s, session, server=SERVER, flags=0, name='connect',
            variable='connect'):
    handle = s.variable(variable)
    http(s, 'NetDll_XHttpConnect', session, s.string(server, 'server'), 0,
         flags, name=name, handle=handle)
    return handle


def open_request(s, connect_handle, name='open request', variable='request'):
    handle = s.variable(variable)
    http(s, 'NetDll_XHttpOpenRequest', connect_handle, s.string('GET', 'verb'),
         s.string('/', 'object name'), 0, 0, 0, 0, name=name, handle=handle)
    return handle


def close(s, handle, name):
    http(s, 'NetDll_XHttpCloseHandle', handle, name=name)


def query_option(s, handle, option, name, size=4):
    value = s.alloc(size, 'value')
    length = s.alloc(4, 'length', struct.pack('>I', size))
    http(s, 'NetDll_XHttpQueryOption', handle, option, value, length,
         name=name, capture=[value, length])


def set_option(s, handle, option, value, name, length=4):
    data = s.alloc(max(length, 4), 'value', struct.pack('>I', value))
    http(s, 'NetDll_XHttpSetOption', handle, option, data, length, name=name)


def query_headers(s, request, level, name, header=None, size=0x40,
                  length=None, index=None):
    buffer = s.alloc(size, 'buffer') if size else 0
    length_ptr = s.alloc(4, 'length', struct.pack(
        '>I', size if length is None else length))
    capture = [length_ptr]
    if buffer:
        capture.insert(0, buffer)
    index_ptr = 0
    if index is not None:
        index_ptr = s.alloc(4, 'index', struct.pack('>I', index))
        capture.append(index_ptr)
    header_ptr = s.string(header, 'header') if header else 0
    http(s, 'NetDll_XHttpQueryHeaders', request, level, header_ptr, buffer,
         length_ptr, index_ptr, name=name, capture=capture)


def callback_calls(s, callback, calls, name):
    """Records how many times `callback` ran, then each call's context,
    status and info length (its handle and info pointer differ between
    runs)."""
    capture = [callback.log.prefix(4)]
    for i in range(calls):
        entry = callback.log + (8 + 8 * callback.args * i)
        for arg in (1, 2, 4):
            capture.append((entry + 8 * arg).prefix(8))
    s.call('GetLastError', name=name, capture=capture)


def build(corpus):
    s = corpus.scenario('open')
    start(s)
    session = open_session(s)
    agent = s.string('xgen', 'user agent')
    http(s, 'NetDll_XHttpOpen', agent, 2, 0, 0, ASYNC, name='access type 2')
    http(s, 'NetDll_XHttpOpen', agent, 1, 0, s.string('*', 'bypass'), ASYNC,
         name='proxy bypass')
    http(s, 'NetDll_XHttpOpen', agent, 0, 0, 0, 0, name='not async')
    http(s, 'NetDll_XHttpOpen', agent, 0, 0, 0, ASYNC | 1, name='other flag')
    close(s, session, 'close')
    close(s, 0, 'close no handle')
    stop(s)

    s = corpus.scenario('connect')
    start(s)
    session = open_session(s)
    handle = connect(s, session)
    name = s.string(SERVER, 'server')
    http(s, 'NetDll_XHttpConnect', session, 0, 0, 0, name='no server')
    http(s, 'NetDll_XHttpConnect', session, s.string('', 'empty'), 0, 0,
         name='empty server')
    http(s, 'NetDll_XHttpConnect', session, s.string('a' * 0x100, 'long'), 0,
         0, name='server too long')
    longest = connect(s, session, 'a' * 0xFF, name='longest server',
                      variable='longest')
    close(s, longest, 'close longest')
    http(s, 'NetDll_XHttpConnect', session, name, 0, 0x10,
         name='unknown flag')
    secure = connect(s, session, flags=SECURE, name='secure',
                     variable='secure')
    close(s, secure, 'close secure')
    http(s, 'NetDll_XHttpConnect', handle, name, 0, 0,
         name='connect handle as session')
    close(s, handle, 'close connect')
    close(s, session, 'close session')
    stop(s)

    s = corpus.scenario('open request')
    start(s)
    session = open_session(s)
    handle = connect(s, session)
    request = open_request(s, handle)
    verb = s.string('GET', 'verb')
    root = s.string('/', 'object name')
    http(s, 'NetDll_XHttpOpenRequest', handle, verb, root, 0, 0, 0, 0,
         name='second request')
    http(s, 'NetDll_XHttpOpenRequest', session, verb, root, 0, 0, 0, 0,
         name='session as connect')
    close(s, request, 'close request')
    http(s, 'NetDll_XHttpOpenRequest', handle, verb, 0, 0, 0, 0, 0,
         name='no object name')
    http(s, 'NetDll_XHttpOpenRequest', handle, verb,
         s.string('x', 'relative'), 0, 0, 0, 0, name='relative object name')
    memory = s.alloc(0x5D0, 'memory', fill=0)
    http(s, 'NetDll_XHttpOpenRequestUsingMemory', handle, verb, root, 0, 0, 0,
         0, 0x100, 0, name='size without memory')
    http(s, 'NetDll_XHttpOpenRequestUsingMemory', handle, verb, root, 0, 0, 0,
         memory, 0x5CF, 0, name='small memory')
    in_memory = s.variable('request')
    http(s, 'NetDll_XHttpOpenRequestUsingMemory', handle, verb, root, 0, 0, 0,
         memory, 0x5D0, 0, name='memory', handle=in_memory)
    close(s, in_memory, 'close memory request')
    close(s, handle, 'close connect')
    close(s, session, 'close session')
    stop(s)

    # Children copy the callback their parent has when they're opened.
    s = corpus.scenario('status callback')
    start(s)
    session = open_session(s)
    callback = s.recorder('callback', args=5, calls=3)
    http(s, 'NetDll_XHttpSetStatusCallback', session, callback, 0xFFFFFFFF, 0,
         name='set')
    http(s, 'NetDll_XHttpSetStatusCallback', session, callback,
         HANDLE_CLOSING, 0, name='set again')
    http(s, 'NetDll_XHttpSetStatusCallback', 0, callback, HANDLE_CLOSING, 0,
         name='no handle')
    handle = connect(s, session)
    request = open_request(s, handle)
    query_option(s, request, 1, 'request callback')
    close(s, request, 'close request')
    close(s, handle, 'close connect')
    close(s, session, 'close session')
    callback_calls(s, callback, 3, 'closing callbacks')
    stop(s)

    s = corpus.scenario('query options')
    start(s)
    session = open_session(s)
    handle = connect(s, session)
    request = open_request(s, handle)
    for kind, target in (('session', session), ('connect', handle),
                         ('request', request)):
        for option in OPTIONS:
            query_option(s, target, option, f'{kind} {option}')
    close(s, request, 'close request')
    close(s, handle, 'close connect')
    close(s, session, 'close session')
    stop(s)

    s = corpus.scenario('set options')
    start(s)
    session = open_session(s)
    handle = connect(s, session)
    request = open_request(s, handle)
    set_option(s, 0, OPTION_RECEIVE_TIMEOUT, 5000, 'no handle')
    set_option(s, 0, OPTION_NULL_HANDLE, 0, 'no handle 104')
    set_option(s, request, OPTION_RECEIVE_TIMEOUT, 5000, 'receive timeout')
    query_option(s, request, OPTION_RECEIVE_TIMEOUT, 'receive timeout back')
    set_option(s, request, OPTION_RECEIVE_TIMEOUT, 5000, 'receive timeout 8',
               length=8)
    set_option(s, session, OPTION_RECEIVE_TIMEOUT, 5000,
               'session receive timeout')
    set_option(s, handle, OPTION_RECEIVE_TIMEOUT, 5000,
               'connect receive timeout')
    set_option(s, request, OPTION_MAX_REDIRECTS, 3, 'max redirects')
    query_option(s, request, OPTION_REDIRECT_POLICY, 'policy with max 3')
    set_option(s, request, OPTION_REDIRECT_POLICY, 0, 'policy 0 with max 3')
    set_option(s, request, OPTION_REDIRECT_POLICY, 2, 'policy 2')
    query_option(s, request, OPTION_REDIRECT_POLICY, 'policy 2 back')
    query_option(s, request, OPTION_MAX_REDIRECTS, 'max redirects back')
    set_option(s, request, OPTION_MAX_REDIRECTS, 0, 'max redirects 0')
    query_option(s, request, OPTION_REDIRECT_POLICY, 'policy with max 0')
    set_option(s, request, OPTION_REDIRECT_POLICY, 1, 'policy 1 with max 0')
    query_option(s, request, OPTION_MAX_REDIRECTS, 'max redirects after 1')
    set_option(s, request, OPTION_REDIRECT_POLICY, 4, 'policy 4')
    set_option(s, request, OPTION_DISABLE_FEATURE, 0x8, 'disable 8')
    set_option(s, request, OPTION_DISABLE_FEATURE, 0x10, 'disable 10')
    query_option(s, request, OPTION_DISABLE_FEATURE, 'disable back')
    set_option(s, request, OPTION_DISABLE_FEATURE, 0x1, 'disable 1')
    set_option(s, request, OPTION_KEEP_CONNECTION, 0, 'keep connection')
    set_option(s, request, OPTION_CONTEXT_VALUE, 0x1234, 'context value')
    set_option(s, session, OPTION_USER_VALUE, 0x12345678, 'session 103')
    query_option(s, session, OPTION_USER_VALUE, 'session 103 back')
    query_option(s, handle, OPTION_USER_VALUE, 'connect 103')
    callback = s.recorder('callback', args=5, calls=1)
    data = s.alloc(8, 'callback value')
    s.write(data, callback)
    http(s, 'NetDll_XHttpSetOption', handle, 1, data, 4,
         name='connect callback set')
    http(s, 'NetDll_XHttpSetOption', handle, 1, data, 8,
         name='connect callback 8')
    query_option(s, handle, 1, 'connect callback back')
    close(s, request, 'close request')
    close(s, handle, 'close connect')
    close(s, session, 'close session')
    stop(s)

    s = corpus.scenario('headers before response')
    start(s)
    session = open_session(s)
    handle = connect(s, session)
    request = open_request(s, handle)
    query_headers(s, request, QUERY_CONTENT_LENGTH | QUERY_NUMBER,
                  'content length', size=8)
    query_headers(s, request, QUERY_RAW_HEADERS_CRLF, 'raw headers')
    query_headers(s, request, QUERY_STATUS_CODE | QUERY_NUMBER,
                  'status code', size=8)
    query_headers(s, request, QUERY_REQUEST_HEADERS | QUERY_HOST, 'host')
    query_headers(s, request, QUERY_REQUEST_HEADERS | QUERY_HOST,
                  'host, short buffer', size=4)
    query_headers(s, request, QUERY_REQUEST_HEADERS | QUERY_HOST,
                  'host, no buffer', size=0, length=0)
    query_headers(s, request, QUERY_REQUEST_HEADERS | QUERY_HOST,
                  'host with index', index=0)
    query_headers(s, request, QUERY_REQUEST_HEADERS | QUERY_HOST,
                  'host with index 1', index=1)
    query_headers(s, request, QUERY_REQUEST_HEADERS, 'request accept')
    query_headers(s, request, QUERY_REQUEST_HEADERS | QUERY_CUSTOM,
                  'request custom host', header='Host')
    query_headers(s, request, QUERY_CUSTOM, 'custom without name')
    query_headers(s, request, QUERY_CONTENT_LENGTH, 'name with level',
                  header='X')
    query_headers(s, request,
                  QUERY_CONTENT_LENGTH | QUERY_NUMBER | QUERY_SYSTEMTIME,
                  'number and systemtime')
    http(s, 'NetDll_XHttpQueryHeaders', request, QUERY_CONTENT_LENGTH, 0,
         s.alloc(8, 'buffer'), 0, 0, name='no length pointer')
    query_headers(s, handle, QUERY_RAW_HEADERS_CRLF, 'connect handle')
    close(s, request, 'close request')
    close(s, handle, 'close connect')
    close(s, session, 'close session')
    stop(s)

    s = corpus.scenario('before send')
    start(s)
    session = open_session(s)
    handle = connect(s, session)
    request = open_request(s, handle)
    buffer = s.alloc(0x10, 'buffer')
    http(s, 'NetDll_XHttpReceiveResponse', request, 0, name='receive')
    http(s, 'NetDll_XHttpReadData', request, buffer, 0x10, 0, name='read',
         capture=[buffer])
    http(s, 'NetDll_XHttpWriteData', request, buffer, 0x10, 0, name='write')
    http(s, 'NetDll_XHttpSendRequest', request, 0, 0,
         s.string('body', 'body'), 4, 0, 0x1234,
         name='optional data without total')
    schemes = [s.alloc(4, n) for n in ('supported', 'first', 'target')]
    http(s, 'NetDll_XHttpQueryAuthSchemes', request, *schemes,
         name='auth schemes', capture=schemes)
    user = s.string('user', 'user')
    password = s.string('password', 'password')
    set_option(s, request, OPTION_MAX_REDIRECTS, 5, 'max redirects')
    http(s, 'NetDll_XHttpSetCredentials', request, 0, AUTH_BASIC, user,
         password, 0, name='basic credentials')
    query_option(s, request, OPTION_MAX_REDIRECTS, 'max redirects after')
    http(s, 'NetDll_XHttpSetCredentials', request, 0, AUTH_BASIC,
         s.string('u' * 0x100, 'long user'), password, 0,
         name='basic credentials too long')
    counters = s.alloc(0x1C, 'counters')
    # The counters don't set the last error so it stays the previous call's.
    http(s, 'NetDll_XHttpGetPerfCounters', counters, name='perf counters',
         capture=[counters])
    http(s, 'NetDll_XHttpGetPerfCounters', 0, name='perf counters, no pointer')
    close(s, request, 'close request')
    close(s, handle, 'close connect')
    close(s, session, 'close session')
    stop(s)

    # A connect closed first leaves its request to be closed later. A session
    # isn't closed before its connects, which would leave them pointing at
    # freed memory on a console.
    s = corpus.scenario('close order')
    start(s)
    session = open_session(s)
    callback = s.recorder('callback', args=5, calls=3)
    http(s, 'NetDll_XHttpSetStatusCallback', session, callback,
         HANDLE_CLOSING, 0, name='set callback')
    handle = connect(s, session)
    request = open_request(s, handle)
    close(s, handle, 'close connect')
    counters = s.alloc(0x1C, 'counters')
    http(s, 'NetDll_XHttpGetPerfCounters', counters,
         name='perf counters after connect', capture=[counters])
    close(s, request, 'close request')
    close(s, session, 'close session')
    callback_calls(s, callback, 3, 'closing callbacks')
    stop(s)
