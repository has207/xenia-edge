"""Silverlight corpus: exports the Silverlight apps call that can run on a
console without changing anything outside the title: the token requests
before XAuth is started, the optional media update check, the closed
captioning state and settings, XeKeysAesCbc with the roamable key, reading
biometric data and the remote control media state.

Outputs start filled with 0xCD so a capture shows whether they were written.
An overlapped starts with 0xCD in the fields the system writes and 0 in the
event and completion routine. The optional media update (title FFFE07DF,
marketplace content) has to be installed on both sides.
"""

import struct

ROAMABLE_KEY = 0x19
USER_NONE = 0xFF
URL = 'http://www.example.com/path'
APPLIES_TO = 'http://www.example.com/'
CLEAR = bytes(range(0x20))


def overlapped(s):
    return s.alloc(0x1C, 'overlapped', struct.pack(
        '>IIIIIII', 0xCDCDCDCD, 0xCDCDCDCD, 0xCDCDCDCD, 0, 0, 0, 0xCDCDCDCD))


def applies_to(s, url, name, length=None):
    text = s.string(url, 'url', size=max(len(url) + 1, 0x410))
    token = s.alloc(4, 'token')
    s.call('XamGetAppliesTo', text, len(url) if length is None else length,
           token, name=name, capture=[token])


def get_token(s, user, name, applies='default', length=None, out=True,
              ov=False):
    text = 0 if applies is None else s.string(APPLIES_TO, 'applies to',
                                              size=0x410)
    token = s.alloc(4, 'token') if out else 0
    args = [user, text, len(APPLIES_TO) if length is None else length, token]
    capture = [token] if out else []
    ovl = overlapped(s) if ov else None
    if ovl:
        capture.append(ovl)
    args.append(ovl or 0)
    s.call('XamGetToken', *args, name=name, capture=capture)


def aes(s, size, feed, encrypt, name, data=CLEAR):
    source = s.alloc(0x20, 'input', data)
    output = s.alloc(0x20, 'output')
    capture = [output]
    feed_ptr = 0
    if feed:
        feed_ptr = s.alloc(0x10, 'feed', fill=0)
        capture.append(feed_ptr)
    s.call('XeKeysAesCbc', ROAMABLE_KEY, source, size, output, feed_ptr,
           encrypt, name=name, capture=capture)
    return output


def build(corpus):
    s = corpus.scenario('applies to')
    applies_to(s, URL, 'url')
    applies_to(s, '://www.example.com/', 'empty scheme')
    applies_to(s, 'www.example.com/', 'no scheme')
    applies_to(s, 'http:/www.example.com/', 'one slash')
    applies_to(s, 'http', 'short', length=3)
    applies_to(s, URL, 'too long', length=0x401)
    token = s.alloc(4, 'token')
    s.call('XamGetAppliesTo', 0, len(URL), token, name='no url',
           capture=[token])
    s.call('XamGetAppliesTo', s.string(URL, 'url'), len(URL), 0,
           name='no token pointer')

    s = corpus.scenario('get token')
    get_token(s, USER_NONE, 'any user')
    get_token(s, 0, 'user 0')
    get_token(s, 4, 'user 4')
    get_token(s, 5, 'user 5')
    get_token(s, USER_NONE, 'no token pointer', out=False)
    get_token(s, USER_NONE, 'no applies to', applies=None)
    get_token(s, USER_NONE, 'too long', length=0x401)
    get_token(s, USER_NONE, 'overlapped', ov=True)

    # Both sides have the optional media update installed, which holds
    # CodecPack01.xex and CodecPack02.xex. YouTube asks for version 0x37CA.
    s = corpus.scenario('optional media update')
    for version in (0, 1):
        s.call('XamIsOptionalMediaUpdateInstalled', 0, version,
               name=f'no name, version {version:#x}')
    for module in ('CodecPack01', 'CodecPack02', 'xg_no_such_module'):
        text = s.string(module, module)
        for version in (0, 0x37CA, 0xFFFFFFFF):
            s.call('XamIsOptionalMediaUpdateInstalled', text, version,
                   name=f'{module} version {version:#x}')

    s = corpus.scenario('closed captioning')
    s.call('XamGetClosedCaptioningState', name='state')
    s.call('XamGetClosedCaptioningSettings', 0, name='settings, no pointer')
    settings = s.alloc(4, 'settings')
    s.call('XamGetClosedCaptioningSettings', settings, name='settings',
           capture=[settings])

    s = corpus.scenario('aes')
    ecb = aes(s, 0x20, False, 1, 'ecb encrypt')
    aes(s, 0x20, True, 1, 'cbc encrypt')
    aes(s, 0x14, False, 1, 'partial block')
    aes(s, 0, False, 1, 'no data')
    # The first step's output decrypts back to the clear text.
    back = s.alloc(0x20, 'decrypted')
    s.call('XeKeysAesCbc', ROAMABLE_KEY, ecb, 0x20, back, 0, 0,
           name='ecb decrypt', capture=[back])

    s = corpus.scenario('biometric data')
    xuid = s.alloc(8, 'xuid')
    own = s.variable('xuid')
    s.call('XamUserGetXUID', 0, 1, xuid, store=[(own, xuid)],
           name='own xuid')
    value = s.alloc(4, 'value')
    buffer = s.alloc(0x40, 'buffer')
    size = s.alloc(4, 'size', struct.pack('>I', 0x40))
    for name, args in (('no value pointer', (0, buffer, size)),
                       ('no buffer', (value, 0, size)),
                       ('no size pointer', (value, buffer, 0))):
        s.call('XamReadBiometricData', own, *args, 0, name=name,
               capture=[value, size])
    s.call('XamReadBiometricData', own, value, buffer, size, 0, name='read',
           capture=[value, buffer.prefix(0x10), size])
    ovl = overlapped(s)
    s.call('XamReadBiometricData', own, value, buffer, size, ovl,
           name='read overlapped', wait=ovl,
           capture=[value, buffer.prefix(0x10), size, ovl])

    s = corpus.scenario('media state')
    s.call('XamLrcSetMediaState', 0, name='no state')
