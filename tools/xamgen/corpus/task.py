"""XamTaskSchedule corpus: the thread a task's callback runs on and its
priority, for the task types and flags titles pass.

The callback is common.probe. A type 2 task gets a thread of its own, type 8
goes to XAM's worker pool, whose worker that takes it, and so its CPU, varies
from run to run, so a type 8 task's CPU isn't recorded. XAM keeps 0x00080000
only for a title with the ForegroundTasks privilege, which this XEX lacks, or
with 0x20000000 also set.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import message, probe, without_cpu  # noqa: E402

POOLED = 8

TASKS = (
    ('type 2, foreground without the privilege', 0x02080002, 0),
    ('type 2, foreground kept by 0x20000000', 0x22080002, 0),
    ('type 2, no other flags', 0x00000002, 0),
    ('type 2, CPU from value2', 0x80000002, 3),
    ('type 8', 0x00000008, 0),
    ('type 8, 0x04000008', 0x04000008, 0),
    ('type 8, 0x00402008', 0x00402008, 0),
)


def build(corpus):
    for name, flags, value2 in TASKS:
        s = corpus.scenario(name)
        callback = probe(s)
        msg = message(s)
        args = s.alloc(8, 'args', struct.pack('>II', flags, value2))
        handle_out = s.alloc(4, 'task handle')
        task = s.variable('task')
        capture = without_cpu(msg) if flags & 0x1F == POOLED else [msg]
        s.call('XamTaskSchedule', callback, msg, args, handle_out, wait=msg,
               capture=capture, store=[(task, handle_out)], name='schedule')
        s.call('XamTaskCloseHandle', task, name='close')
