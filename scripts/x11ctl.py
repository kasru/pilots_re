#!/usr/bin/env python3
"""Minimal X11/EWMH driver via ctypes + libX11 (no xdotool/wmctrl on the host).

Used to reproduce window-manager interactions (maximize / restore / fullscreen)
against PILOT_RV on DISPLAY=:0 from a script.

Usage:
  x11ctl.py list                     - client windows: <id> <name>
  x11ctl.py state <id> <action>      - _NET_WM_STATE: add|max|restore|fs|unfs
  x11ctl.py active <id>              - _NET_ACTIVE_WINDOW (focus/raise)
  x11ctl.py iconify <id>             - WM_CHANGE_STATE IconicState (minimize)
  x11ctl.py warp <id> <x> <y>        - pointer into window (relative)
  x11ctl.py xtest-motion <rx> <ry>   - hardware-like motion, root coords (libXtst)
  x11ctl.py fake-motion <id> <x> <y> - synthetic MotionNotify straight to the window
                                       (bypasses covering overlays like the screen locker;
                                        delivered only if a client - SDL - selected it there)
  x11ctl.py fake-click <id> <x> <y>  - synthetic ButtonPress+Release at window coords
  x11ctl.py pointer                  - XQueryPointer: root coords + child under pointer
  x11ctl.py sniff <id> <seconds>     - count pointer events arriving at THIS client
                                       (verifies event delivery on the X server side)
"""
import ctypes
import ctypes.util
import sys

lib = ctypes.CDLL(ctypes.util.find_library("X11") or "libX11.so.6")

lib.XOpenDisplay.argtypes = [ctypes.c_char_p]
lib.XOpenDisplay.restype = ctypes.c_void_p
lib.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
lib.XDefaultRootWindow.restype = ctypes.c_ulong
lib.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
lib.XInternAtom.restype = ctypes.c_ulong
lib.XGetWindowProperty.argtypes = [
    ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong,   # dpy, window, property
    ctypes.c_long, ctypes.c_long, ctypes.c_int,        # offset, length, delete
    ctypes.c_ulong,                                    # req_type (AnyPropertyType=0)
    ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_int),   # actual_type, format
    ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong), # nitems, bytes_after
    ctypes.POINTER(ctypes.c_void_p)]                   # prop
lib.XGetWindowProperty.restype = ctypes.c_int
lib.XFree.argtypes = [ctypes.c_void_p]
lib.XSendEvent.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int,
                           ctypes.c_long, ctypes.c_void_p]
lib.XSendEvent.restype = ctypes.c_int
lib.XFlush.argtypes = [ctypes.c_void_p]
lib.XWarpPointer.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong,
                             ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_uint,
                             ctypes.c_int, ctypes.c_int]

ClientMessage = 33
SubstructureRedirectMask = 0x100000
SubstructureNotifyMask = 0x200000


class XClientMessageEvent(ctypes.Structure):
    # prefix fields must match Xlib's XClientMessageEvent exactly;
    # data kept oversized (long[20]) so XSendEvent never reads past the object
    _fields_ = [
        ("type", ctypes.c_int),
        ("serial", ctypes.c_ulong),
        ("send_event", ctypes.c_int),
        ("display", ctypes.c_void_p),
        ("window", ctypes.c_ulong),
        ("message_type", ctypes.c_ulong),
        ("format", ctypes.c_int),
        ("data", ctypes.c_long * 20),
    ]


class XEvent(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int), ("pad", ctypes.c_long * 24)]


class XMotionNotifyEvent(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_int),
        ("serial", ctypes.c_ulong),
        ("send_event", ctypes.c_int),
        ("display", ctypes.c_void_p),
        ("window", ctypes.c_ulong),
        ("root", ctypes.c_ulong),
        ("subwindow", ctypes.c_ulong),
        ("time", ctypes.c_ulong),
        ("x", ctypes.c_int),
        ("y", ctypes.c_int),
        ("x_root", ctypes.c_int),
        ("y_root", ctypes.c_int),
        ("state", ctypes.c_uint),
        ("same_screen", ctypes.c_int),
        ("pad", ctypes.c_ulong * 5),
    ]


class XButtonEvent(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_int),
        ("serial", ctypes.c_ulong),
        ("send_event", ctypes.c_int),
        ("display", ctypes.c_void_p),
        ("window", ctypes.c_ulong),
        ("root", ctypes.c_ulong),
        ("subwindow", ctypes.c_ulong),
        ("time", ctypes.c_ulong),
        ("x", ctypes.c_int),
        ("y", ctypes.c_int),
        ("x_root", ctypes.c_int),
        ("y_root", ctypes.c_int),
        ("state", ctypes.c_uint),
        ("button", ctypes.c_uint),
        ("same_screen", ctypes.c_int),
        ("pad", ctypes.c_ulong * 5),
    ]


def get_prop(dpy, win, name):
    """XGetWindowProperty any type -> (format, [items]) or None."""
    atom = lib.XInternAtom(dpy, name.encode(), 0)
    if not atom:
        return None
    actual = ctypes.c_ulong()
    fmt = ctypes.c_int()
    nitems = ctypes.c_ulong()
    after = ctypes.c_ulong()
    prop = ctypes.c_void_p()
    if lib.XGetWindowProperty(dpy, win, atom, 0, 4096, 0, 0,
                              ctypes.byref(actual), ctypes.byref(fmt),
                              ctypes.byref(nitems), ctypes.byref(after),
                              ctypes.byref(prop)) != 0 or not prop.value:
        return None
    try:
        if fmt.value == 32:
            arr = ctypes.cast(prop, ctypes.POINTER(ctypes.c_long))
            return fmt.value, [arr[i] for i in range(nitems.value)]
        raw = ctypes.string_at(prop.value, nitems.value)
        return fmt.value, [raw]
    finally:
        lib.XFree(prop)


def send_cm(dpy, win, msg_atom, l0, l1, l2):
    ev = XClientMessageEvent()
    ev.type = ClientMessage
    ev.window = win
    ev.message_type = msg_atom
    ev.format = 32
    ev.data[0] = l0
    ev.data[1] = l1
    ev.data[2] = l2
    ev.data[3] = 1                      # source indication: application
    mask = SubstructureRedirectMask | SubstructureNotifyMask
    lib.XSendEvent(dpy, lib.XDefaultRootWindow(dpy), 0, mask, ctypes.byref(ev))
    lib.XFlush(dpy)


def main():
    dpy = lib.XOpenDisplay(None)
    if not dpy:
        sys.exit("cannot open display")
    root = lib.XDefaultRootWindow(dpy)
    cmd = sys.argv[1] if len(sys.argv) > 1 else "list"

    if cmd == "list":
        got = get_prop(dpy, root, "_NET_CLIENT_LIST")
        wins = got[1] if got else []
        for w in wins:
            name = ""
            for prop in ("_NET_WM_NAME", "WM_NAME"):
                g = get_prop(dpy, w, prop)
                if g:
                    name = g[1][0].decode("utf-8", "replace") if isinstance(g[1][0], bytes) else str(g[1][0])
                    break
            print("0x%x %s" % (w & 0xFFFFFFFF, name))
    elif cmd == "state":
        win = int(sys.argv[2], 0)
        act = sys.argv[3]
        NET_WM_STATE = lib.XInternAtom(dpy, b"_NET_WM_STATE", 0)
        VERT = lib.XInternAtom(dpy, b"_NET_WM_STATE_MAXIMIZED_VERT", 0)
        HORZ = lib.XInternAtom(dpy, b"_NET_WM_STATE_MAXIMIZED_HORZ", 0)
        FS = lib.XInternAtom(dpy, b"_NET_WM_STATE_FULLSCREEN", 0)
        # 0=remove 1=add 2=toggle (EWMH)
        if act in ("add", "max"):
            send_cm(dpy, win, NET_WM_STATE, 1, VERT, HORZ)
        elif act in ("restore", "unmax"):
            send_cm(dpy, win, NET_WM_STATE, 0, VERT, HORZ)
        elif act == "fs":
            send_cm(dpy, win, NET_WM_STATE, 1, FS, 0)
        elif act == "unfs":
            send_cm(dpy, win, NET_WM_STATE, 0, FS, 0)
        else:
            sys.exit("unknown state action: " + act)
        print("sent %s to 0x%x" % (act, win & 0xFFFFFFFF))
    elif cmd == "active":
        win = int(sys.argv[2], 0)
        NET_ACTIVE = lib.XInternAtom(dpy, b"_NET_ACTIVE_WINDOW", 0)
        ev = XClientMessageEvent()
        ev.type = ClientMessage
        ev.window = win
        ev.message_type = NET_ACTIVE
        ev.format = 32
        ev.data[0] = 2                      # source: pager
        mask = SubstructureRedirectMask | SubstructureNotifyMask
        lib.XSendEvent(dpy, root, 0, mask, ctypes.byref(ev))
        lib.XFlush(dpy)
        print("activated 0x%x" % (win & 0xFFFFFFFF))
    elif cmd == "pointer":
        rootwin = lib.XDefaultRootWindow(dpy)
        # XQueryPointer(dpy, w, &root, &child, &root_x, &root_y, &win_x, &win_y, &mask)
        lib.XQueryPointer.argtypes = [ctypes.c_void_p, ctypes.c_ulong,
                                      ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong),
                                      ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
                                      ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
                                      ctypes.POINTER(ctypes.c_uint)]
        lib.XQueryPointer.restype = ctypes.c_int
        r = ctypes.c_ulong(); c = ctypes.c_ulong()
        rx = ctypes.c_int(); ry = ctypes.c_int(); wx = ctypes.c_int(); wy = ctypes.c_int()
        mask = ctypes.c_uint()
        same = lib.XQueryPointer(dpy, rootwin, ctypes.byref(r), ctypes.byref(c),
                                 ctypes.byref(rx), ctypes.byref(ry),
                                 ctypes.byref(wx), ctypes.byref(wy), ctypes.byref(mask))
        print("pointer root=(%d,%d) child=0x%x same_screen=%d mask=0x%x"
              % (rx.value, ry.value, c.value, same, mask.value))
    elif cmd == "sniff":
        # listen for pointer events on a window and print what actually arrives
        win = int(sys.argv[2], 0)
        dur = float(sys.argv[3]) if len(sys.argv) > 3 else 5.0
        MOTION, ENTER, LEAVE = 0x40, 0x10, 0x20
        lib.XSelectInput.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_long]
        lib.XSelectInput(dpy, win, MOTION | ENTER | LEAVE | 0x20000)  # + StructureNotify
        lib.XFlush(dpy)
        lib.XPending.argtypes = [ctypes.c_void_p]
        lib.XPending.restype = ctypes.c_int
        lib.XNextEvent.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        lib.XConnectionNumber.argtypes = [ctypes.c_void_p]
        lib.XConnectionNumber.restype = ctypes.c_int
        import select as sel
        import time as tm
        counts = {}
        deadline = tm.time() + dur
        while tm.time() < deadline:
            fd = lib.XConnectionNumber(dpy)
            sel.select([fd], [], [], max(0.0, min(0.2, deadline - tm.time())))
            while lib.XPending(dpy):
                ev = XEvent()
                lib.XNextEvent(dpy, ctypes.byref(ev))
                t = ev.type
                counts[t] = counts.get(t, 0) + 1
        names = {2: "PRESS", 3: "RELEASE", 6: "MOTION", 7: "ENTER", 8: "LEAVE",
                 9: "FOCUS_IN", 10: "FOCUS_OUT", 12: "EXPOSE", 17: "UNMAP",
                 16: "MAP", 18: "REPARENT"}
        print("sniff: " + (", ".join("%s=%d" % (names.get(k, "type%d" % k), v)
                                     for k, v in sorted(counts.items())) or "nothing"))
    elif cmd == "xtest-motion":
        # real pointer motion (as if from hardware) in ROOT coordinates
        xt = ctypes.CDLL("libXtst.so.6")
        xt.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                            ctypes.c_int, ctypes.c_int, ctypes.c_ulong]
        xt.XTestFakeMotionEvent.restype = ctypes.c_int
        lib.XDefaultScreen.argtypes = [ctypes.c_void_p]
        lib.XDefaultScreen.restype = ctypes.c_int
        rx, ry = int(sys.argv[2]), int(sys.argv[3])
        scr = lib.XDefaultScreen(dpy)
        xt.XTestFakeMotionEvent(dpy, scr, rx, ry, 0)
        lib.XFlush(dpy)
        print("xtest motion -> (%d,%d)" % (rx, ry))
    elif cmd == "fake-motion":
        # synthetic MotionNotify sent straight to the window (bypasses overlays;
        # delivered only if a client selected PointerMotionMask there - i.e. SDL did)
        win = int(sys.argv[2], 0)
        x, y = int(sys.argv[3]), int(sys.argv[4])
        ev = XMotionNotifyEvent()
        ev.type = 6                              # MotionNotify
        ev.send_event = 1
        ev.display = dpy
        ev.window = win
        ev.root = root
        ev.same_screen = 1
        ev.x, ev.y = x, y
        ev.x_root, ev.y_root = x, y              # approximate (root coords ignored by game)
        lib.XSendEvent(dpy, win, 0, 0x40, ctypes.byref(ev))   # PointerMotionMask
        lib.XFlush(dpy)
        print("fake motion -> %d,%d" % (x, y))
    elif cmd == "fake-click":
        # synthetic ButtonPress+ButtonRelease at window-relative coords
        win = int(sys.argv[2], 0)
        x, y = int(sys.argv[3]), int(sys.argv[4])
        for typ, button in ((4, 1), (5, 1)):     # ButtonPress, ButtonRelease
            ev = XButtonEvent()
            ev.type = typ
            ev.send_event = 1
            ev.display = dpy
            ev.window = win
            ev.root = root
            ev.same_screen = 1
            ev.x, ev.y = x, y
            ev.x_root, ev.y_root = x, y
            ev.button = button
            lib.XSendEvent(dpy, win, 0, 0xC, ctypes.byref(ev))  # press|release masks: one delivery
            lib.XFlush(dpy)
        print("fake click -> %d,%d" % (x, y))
    elif cmd == "iconify":
        # WM_CHANGE_STATE -> IconicState (the classic minimize request)
        win = int(sys.argv[2], 0)
        WM_CHANGE_STATE = lib.XInternAtom(dpy, b"WM_CHANGE_STATE", 0)
        ev = XClientMessageEvent()
        ev.type = ClientMessage
        ev.window = win
        ev.message_type = WM_CHANGE_STATE
        ev.format = 32
        ev.data[0] = 3                          # IconicState
        mask = SubstructureRedirectMask | SubstructureNotifyMask
        lib.XSendEvent(dpy, root, 0, mask, ctypes.byref(ev))
        lib.XFlush(dpy)
        print("iconified 0x%x" % (win & 0xFFFFFFFF))
    elif cmd == "warp":
        win = int(sys.argv[2], 0)
        x, y = int(sys.argv[3]), int(sys.argv[4])
        lib.XWarpPointer(dpy, 0, win, 0, 0, 0, 0, x, y)
        lib.XFlush(dpy)
        print("warped 0x%x -> %d,%d" % (win & 0xFFFFFFFF, x, y))
    else:
        sys.exit("unknown command: " + cmd)


if __name__ == "__main__":
    main()
