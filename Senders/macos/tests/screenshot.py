#!/usr/bin/env python3
"""Capture README screenshots on a macOS runner.

  screenshot.py window OWNER OUT.png     one window of an app, no shadow
  screenshot.py menu OWNER OUT.png       an open menu plus the menu bar above it
"""
import subprocess, sys, time
import Quartz

def windows(owner):
    info = Quartz.CGWindowListCopyWindowInfo(Quartz.kCGWindowListOptionOnScreenOnly, Quartz.kCGNullWindowID)
    return [w for w in info if w.get("kCGWindowOwnerName") == owner and w["kCGWindowBounds"]["Height"] > 40]

def wait_for(owner, pred=lambda w: True, timeout=20):
    end = time.time() + timeout
    while time.time() < end:
        ws = [w for w in windows(owner) if pred(w)]
        if ws:
            return ws
        time.sleep(0.5)
    all_owners = sorted({w.get("kCGWindowOwnerName") for w in Quartz.CGWindowListCopyWindowInfo(
        Quartz.kCGWindowListOptionOnScreenOnly, Quartz.kCGNullWindowID)})
    sys.exit(f"no window for {owner}; on screen: {all_owners}")

mode, owner, out = sys.argv[1:4]
if mode == "window":
    w = max(wait_for(owner, lambda w: w["kCGWindowLayer"] == 0),
            key=lambda w: w["kCGWindowBounds"]["Width"] * w["kCGWindowBounds"]["Height"])
    time.sleep(1)
    subprocess.check_call(["screencapture", "-x", "-o", "-l", str(w["kCGWindowNumber"]), out])
else:
    w = wait_for(owner, lambda w: w["kCGWindowLayer"] > 0)[0]
    b = w["kCGWindowBounds"]
    time.sleep(0.5)
    full = out + ".full.png"
    subprocess.check_call(["screencapture", "-x", full])
    screen_w = Quartz.CGDisplayBounds(Quartz.CGMainDisplayID()).size.width
    img = Quartz.CGImageSourceCreateImageAtIndex(
        Quartz.CGImageSourceCreateWithURL(Quartz.CFURLCreateWithFileSystemPath(None, full, 0, False), None), 0, None)
    scale = Quartz.CGImageGetWidth(img) / screen_w
    x0 = max(0, b["X"] - 60); x1 = min(screen_w, b["X"] + b["Width"] + 60)
    rect = Quartz.CGRectMake(x0 * scale, 0, (x1 - x0) * scale, (b["Y"] + b["Height"] + 16) * scale)
    crop = Quartz.CGImageCreateWithImageInRect(img, rect)
    dest = Quartz.CGImageDestinationCreateWithURL(
        Quartz.CFURLCreateWithFileSystemPath(None, out, 0, False), "public.png", 1, None)
    Quartz.CGImageDestinationAddImage(dest, crop, None)
    Quartz.CGImageDestinationFinalize(dest)
print("saved", out)
