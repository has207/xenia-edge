"""Voice category corpus: the volumes titles scale their voices by and the
per-client masks that say which categories were set.

Setting a category's volume marks it changed for every render client, even
when the value is the same. Reading a client's mask clears it, and a new
client starts with every category marked. The scenario uses category 30,
which nothing on the console sets, and puts it back to 1.0 at the end.
Category 1 holds the music volume, 0 while the user's own music plays.
"""

import struct

CATEGORY = 30
OUT_OF_RANGE = 32


def setter(s):
    """Calls XAudioSetVoiceCategoryVolume(r3, the float at r4), which takes
    its volume in f1."""
    def emit(a, r, thunk):
        a.label(r.entry)
        a.lfs(1, 0, 4)
        a.load32(12, thunk('XAudioSetVoiceCategoryVolume'))
        a.mtctr(12)
        a.bctr()
    return s.routine('set category volume', emit)


def build(corpus):
    s = corpus.scenario('voice category')
    set_volume = setter(s)

    def register(name):
        registration = s.alloc(8, f'{name} registration', fill=0)
        s.write(registration, s.recorder(f'{name} callback'))
        out = s.alloc(4, f'{name} handle')
        handle = s.variable(name)
        s.call('XAudioRegisterRenderDriverClient', registration, out,
               store=[(handle, out)], name=f'register {name}')
        return handle

    def mask(handle, name):
        out = s.alloc(4, f'{name} mask', fill=0xCD)
        s.call('XAudioGetVoiceCategoryVolumeChangeMask', handle, out,
               capture=[out], name=name)

    def get(category, name):
        out = s.alloc(4, f'{name} volume', fill=0xCD)
        s.call('XAudioGetVoiceCategoryVolume', category, out, capture=[out],
               name=name)

    def set_to(category, volume, name):
        value = s.alloc(4, f'{name} value', struct.pack('>f', volume))
        s.call(set_volume, category, value, name=name)

    first = register('first')
    second = register('second')
    mask(first, 'first mask at registration')
    mask(first, 'first mask read again')
    mask(second, 'second mask at registration')
    get(0, 'category 0')
    get(CATEGORY, 'category 30')
    get(31, 'category 31')

    set_to(CATEGORY, 0.5, 'set 30 to 0.5')
    mask(first, 'first mask after set')
    mask(second, 'second mask after set')
    mask(first, 'first mask read again after set')
    get(CATEGORY, 'category 30 after set')
    set_to(CATEGORY, 0.5, 'set 30 to 0.5 again')
    mask(first, 'first mask after same value')
    set_to(CATEGORY, 1.5, 'set 30 to 1.5')
    get(CATEGORY, 'category 30 above 1')

    set_to(OUT_OF_RANGE, 0.5, 'set 32')
    get(OUT_OF_RANGE, 'category 32')
    mask(first, 'first mask after set 32')
    mask(0, 'mask of handle 0')

    set_to(CATEGORY, 1.0, 'restore 30')
    s.call('XAudioUnregisterRenderDriverClient', first,
           name='unregister first')
    mask(second, 'second mask after restore')
    third = register('third')
    mask(third, 'third mask at registration')
    s.call('XAudioUnregisterRenderDriverClient', second,
           name='unregister second')
    s.call('XAudioUnregisterRenderDriverClient', third,
           name='unregister third')
