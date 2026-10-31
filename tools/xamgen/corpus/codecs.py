"""Codecs corpus: the extra AV codec loaders, ordinals 0x7DC/0x7DD and
0x7E3/0x7E4, given no pointer for the module, then loading each module twice,
unloading it once more than it was loaded and loading it again.

Each load captures the export it hands back. Xenia needs the two modules in
its sysext folder.
"""

LOADERS = (('XamLoadExtraAVCodecs', 'XamUnloadExtraAVCodecs'),
           ('XamLoadExtraAVCodecs2', 'XamUnloadExtraAVCodecs2'))


def load(s, export, name):
    entry = s.alloc(4, 'export')
    s.call(export, entry, name=name, capture=[entry])


def build(corpus):
    s = corpus.scenario('null module pointer')
    for loader, _ in LOADERS:
        s.call(loader, 0, name=loader)

    for loader, unloader in LOADERS:
        s = corpus.scenario(f'{loader} references')
        load(s, loader, 'load')
        load(s, loader, 'load again')
        s.call(unloader, name='unload', void=True)
        s.call(unloader, name='unload again', void=True)
        s.call(unloader, name='unload with none left', void=True)
        load(s, loader, 'load after unloading')
        s.call(unloader, name='unload last', void=True)
