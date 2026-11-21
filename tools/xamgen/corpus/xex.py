"""Xex corpus: the load flags XexLoadImage takes, and what a load of a module
that isn't there fails with. Without LOAD_DLL (0x8) or 0x80 it refuses before
it looks for the module, leaving the handle alone. Past that it zeroes the
handle, and a module goes into the title process with TITLE_PROCESS (0x1) and
into the system process without it, which a title's thread can't load into.
A missing module then fails as opening its file would.

The handle's slot starts out as 0xCD, so a call that leaves it alone shows.
"""

TITLE_PROCESS = 0x1
LOAD_DLL = 0x8
MISSING_NAME = 'game:\\xgmissing.xex'
MISSING_FOLDER = 'game:\\xgmissing\\xgmissing.xex'


def build(corpus):
    s = corpus.scenario('XexLoadImage flags')
    name = s.string(MISSING_NAME, 'name')
    for flags, label in ((0, 'no flags'), (TITLE_PROCESS, 'title process'),
                         (LOAD_DLL, 'LOAD_DLL in the system process')):
        handle = s.alloc(4, 'handle')
        s.call('XexLoadImage', name, flags, 0, handle, name=label,
               capture=[handle])

    s = corpus.scenario('XexLoadImage missing module')
    for path, label in ((MISSING_NAME, 'missing name'),
                        (MISSING_FOLDER, 'missing folder')):
        name = s.string(path, 'name')
        handle = s.alloc(4, 'handle')
        s.call('XexLoadImage', name, LOAD_DLL | TITLE_PROCESS, 0, handle,
               name=label, capture=[handle])
