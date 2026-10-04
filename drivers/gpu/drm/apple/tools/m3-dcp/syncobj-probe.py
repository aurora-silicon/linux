#!/usr/bin/env python3
"""CPU-only DRM timeline/import/eventfd check; does not submit GPU work.

Usage: syncobj-probe.py DISPLAY_NODE RENDER_NODE
The JSON result identifies both nodes. Exercise imported timelines in both
directions, requiring an unsignaled point to time out before CPU signaling it.
"""
import ctypes as C
import errno
import json
import os
import select
import sys

drm = C.CDLL('libdrm.so.2', use_errno=True)
U32, U64, INT = C.c_uint32, C.c_uint64, C.c_int
P32, P64 = C.POINTER(U32), C.POINTER(U64)
signatures = {
    'drmGetCap': [INT, U64, P64],
    'drmSyncobjCreate': [INT, U32, P32],
    'drmSyncobjDestroy': [INT, U32],
    'drmSyncobjHandleToFD': [INT, U32, C.POINTER(INT)],
    'drmSyncobjFDToHandle': [INT, INT, P32],
    'drmSyncobjTimelineSignal': [INT, P32, P64, U32],
    'drmSyncobjTimelineWait': [INT, P32, P64, U32, C.c_int64, U32, P32],
    'drmSyncobjEventfd': [INT, U32, U64, INT, U32],
}
for name, args in signatures.items():
    fn = getattr(drm, name)
    fn.argtypes, fn.restype = args, INT


def check(result):
    if result:
        raise OSError(C.get_errno(), f'libdrm returned {result}')


assert len(sys.argv) == 3, __doc__
fds, owned, handles = [], [], []
report = {'scope': 'CPU-signaled synchronization; no GPU execution', 'nodes': [],
          'directions': []}
try:
    for path in sys.argv[1:]:
        fd = os.open(path, os.O_RDWR | os.O_CLOEXEC)
        fds.append(fd)
        caps = {}
        for name, cap in [('syncobj', 0x13), ('timeline', 0x14)]:
            value = U64()
            check(drm.drmGetCap(fd, cap, C.byref(value)))
            caps[name] = value.value
            assert value.value == 1, (path, name, value.value)
        report['nodes'].append({'path': os.path.realpath(path), **caps})
    for src, dst in [(fds[0], fds[1]), (fds[1], fds[0])]:
        original, imported, object_fd = U32(), U32(), INT(-1)
        check(drm.drmSyncobjCreate(src, 0, C.byref(original)))
        handles.append((src, original.value))
        check(drm.drmSyncobjHandleToFD(src, original, C.byref(object_fd)))
        owned.append(object_fd.value)
        check(drm.drmSyncobjFDToHandle(dst, object_fd, C.byref(imported)))
        handles.append((dst, imported.value))
        point = U64(7)
        C.set_errno(0)
        result = drm.drmSyncobjTimelineWait(dst, C.byref(imported),
            C.byref(point), 1, 0, 2, None)  # WAIT_FOR_SUBMIT, zero deadline
        assert result == -errno.ETIME, (result, C.get_errno())
        event = os.eventfd(0, os.EFD_CLOEXEC | os.EFD_NONBLOCK)
        owned.append(event)
        check(drm.drmSyncobjEventfd(dst, imported, point, event, 0))
        assert not select.select([event], [], [], 0)[0]
        check(drm.drmSyncobjTimelineSignal(src, C.byref(original), C.byref(point), 1))
        check(drm.drmSyncobjTimelineWait(dst, C.byref(imported),
            C.byref(point), 1, 0, 2, None))
        assert select.select([event], [], [], 1)[0] == [event]
        assert os.eventfd_read(event) == 1
        report['directions'].append({'source': fds.index(src),
            'destination': fds.index(dst), 'point': point.value,
            'pre_signal_timeout': True, 'post_signal_wait': True, 'eventfd': True})
    report['status'] = 'Pass'
    print(json.dumps(report, indent=2))
finally:
    for fd, handle in reversed(handles):
        drm.drmSyncobjDestroy(fd, handle)
    for fd in owned + fds:
        os.close(fd)
