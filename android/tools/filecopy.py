#!/usr/bin/env python3
"""文件复制：APFS 用 clonefile，btrfs/XFS 用 FICLONE，否则普通复制。

出包链路会整份复制几十 GB 的 ROM 数据（make_pack 的产物包、game_apk.py --resources 的资源包）。
在同一文件系统上，复制后的两份内容各自独立（写任一份都触发 CoW），所以共享数据块对调用方是透明的；
宿主或卷不支持克隆时自动退回 shutil.copy2。

    from filecopy import copy_file
    copy_file(src, dst)   # True = 共享了数据块；False = 普通复制。两种情况下内容/权限/时间戳一致。
"""
import os
import shutil
import sys
try:
    import fcntl
except ImportError:   # Windows
    fcntl = None

# Linux 的 ioctl 号。Darwin 不调用这个 ioctl。
_FICLONE = getattr(fcntl, 'FICLONE', 0x40049409)
_clonefile = None
if sys.platform == 'darwin':
    import ctypes

    _clonefile = getattr(ctypes.CDLL(None), 'clonefile', None)
    if _clonefile is not None:
        _clonefile.argtypes = (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32)
        _clonefile.restype = ctypes.c_int


def copy_file(src, dst):
    """复制 src 到 dst；成功共享数据块返回 True，普通复制返回 False。"""
    os.makedirs(os.path.dirname(dst) or '.', exist_ok=True)
    if sys.platform == 'darwin':
        # clonefile 只能新建目标。目标已存在时，沿用 copy2 的覆盖语义。
        if _clonefile is not None and os.path.isfile(src) and _clonefile(os.fsencode(src), os.fsencode(dst), 0) == 0:
            shutil.copystat(src, dst)
            return True
    elif fcntl is not None:
        try:
            with open(src, 'rb') as a, open(dst, 'wb') as b:
                fcntl.ioctl(b.fileno(), _FICLONE, a.fileno())
            shutil.copystat(src, dst)
            return True
        except OSError:
            pass
    shutil.copy2(src, dst)
    return False
