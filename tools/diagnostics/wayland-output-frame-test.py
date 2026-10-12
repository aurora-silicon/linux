#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Show a bounded animation on one Wayland output and report frame feedback."""
import argparse
import json
import time

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
from gi.repository import Gdk, GLib, Gtk  # noqa: E402

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--connector", default="DP-1")
parser.add_argument("--seconds", type=int, default=8, choices=range(3, 61))
parser.add_argument("--pattern", choices=("moving", "gamma"), default="moving")
args = parser.parse_args()
Gtk.init()
display = Gdk.Display.get_default()
if display is None:
    raise SystemExit("No display connection")
monitors = display.get_monitors()
matches = [monitors.get_item(i) for i in range(monitors.get_n_items())
           if monitors.get_item(i).get_connector() == args.connector]
if len(matches) != 1:
    raise SystemExit("Requested output is absent or ambiguous")

loop = GLib.MainLoop()
window = Gtk.Window(title="Aurora external display frame test")
area = Gtk.DrawingArea()
window.set_child(area)
started = time.monotonic()
stats = {"connector": args.connector, "pattern": args.pattern, "ticks": 0, "draws": 0,
         "actual_connector": None, "physical_visibility": "requires user observation"}
feedback = set()
complete_frames = set()


def draw(_area, context, width, height):
    surface = window.get_surface()
    monitor = display.get_monitor_at_surface(surface) if surface else None
    if monitor:
        stats["actual_connector"] = monitor.get_connector()
    if args.pattern == "gamma":
        context.set_source_rgb(0.08, 0.08, 0.08)
        context.paint()
        context.set_source_rgb(1, 1, 1)
        context.select_font_face("Sans")
        context.set_font_size(24)
        context.move_to(24, 48)
        context.show_text("Match a solid grey to the checkerboard brightness; view from a distance")
        y, h = int(height * 0.25), int(height * 0.5)
        w = int(width * 0.15)
        left = int(width * 0.04)
        for row in range(0, h, 2):
            for col in range(0, w, 2):
                value = ((row // 2) + (col // 2)) % 2
                context.set_source_rgb(value, value, value)
                context.rectangle(left + col, y + row, 2, 2)
                context.fill()
        for index, value in enumerate((0.50, 0.67, 0.735, 0.80)):
            x = int(width * (0.24 + index * 0.19))
            context.set_source_rgb(value, value, value)
            context.rectangle(x, y, w, h)
            context.fill()
            context.set_source_rgb(1, 1, 1)
            context.move_to(x, y + h + 40)
            context.show_text(f"{value * 100:g}%")
        stats["draws"] += 1
        return
    phase = (time.monotonic() - started) % 2 / 2
    context.set_source_rgb(0.06, 0.10, 0.17)
    context.paint()
    context.set_source_rgb(0.15, 0.80, 0.55)
    context.rectangle(phase * width, height * 0.25, width * 0.15, height * 0.5)
    context.fill()
    stats["draws"] += 1


def tick(widget, clock):
    stats["ticks"] += 1
    counter = clock.get_frame_counter()
    for frame in range(max(0, counter - 16), counter):
        previous = clock.get_timings(frame)
        if previous is not None and previous.get_complete():
            complete_frames.add(frame)
            presented = previous.get_presentation_time()
            if presented:
                feedback.add(presented)
    surface = window.get_surface()
    monitor = display.get_monitor_at_surface(surface) if surface else None
    if monitor:
        stats["actual_connector"] = monitor.get_connector()
    if args.pattern == "moving" or stats["actual_connector"] != args.connector:
        widget.queue_draw()
    return GLib.SOURCE_CONTINUE


def finish():
    window.destroy()
    loop.quit()
    return GLib.SOURCE_REMOVE


area.set_draw_func(draw)
area.add_tick_callback(tick)
window.connect("close-request", lambda _window: finish())
window.fullscreen_on_monitor(matches[0])
window.present()
GLib.timeout_add_seconds(args.seconds, finish)
loop.run()
stats["elapsed_s"] = round(time.monotonic() - started, 3)
stats["presentation_feedback_frames"] = len(feedback)
stats["complete_timings"] = len(complete_frames)
stats["callback_pass"] = stats["ticks"] >= args.seconds * 15
stats["draw_pass"] = stats["draws"] >= (args.seconds * 15 if args.pattern == "moving" else 1)
stats["presentation_pass"] = len(feedback) >= args.seconds * 10
stats["target_pass"] = stats["actual_connector"] == args.connector
stats["verification_scope"] = "compositor feedback and output placement; physical scanout is not measured"
print(json.dumps(stats, sort_keys=True))
checks = ("callback_pass", "draw_pass", "presentation_pass", "target_pass") if args.pattern == "moving" else ("draw_pass", "target_pass")
raise SystemExit(0 if all(stats[k] for k in checks) else 1)
