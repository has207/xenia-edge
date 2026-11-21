"""Xex corpus: the load flags XexLoadImage takes. Without LOAD_DLL (0x8) or
0x80 it refuses before it looks for the module, leaving the handle alone.

Left out: with LOAD_DLL the console also fails the missing name with
STATUS_ACCESS_DENIED, writing a null handle, where Xenia fails it with
STATUS_NO_SUCH_FILE.

The handle's slot starts out as 0xCD, so a call that leaves it alone shows.
"""

TITLE_PROCESS = 0x1
MISSING = 'game:\\xgmissing.xex'


def build(corpus):
    s = corpus.scenario('XexLoadImage flags')
    name = s.string(MISSING, 'name')
    for flags, label in ((0, 'no flags'), (TITLE_PROCESS, 'title process')):
        handle = s.alloc(4, 'handle')
        s.call('XexLoadImage', name, flags, 0, handle, name=label,
               capture=[handle])
