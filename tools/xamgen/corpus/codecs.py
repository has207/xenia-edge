"""Codecs corpus: the extra AV codec loaders, ordinals 0x7DC/0x7DD and
0x7E3/0x7E4, given no pointer for the module.

Only that check is compared. The console loads the real module and hands back
its export 1, and its first load returns 0x10000000 from xam's module loader
and later ones 0. Xenia has no module, hands back 0 and always returns 0.
"""


def build(corpus):
    s = corpus.scenario('null module pointer')
    for load in ('XamLoadExtraAVCodecs', 'XamLoadExtraAVCodecs2'):
        s.call(load, 0, name=load)
