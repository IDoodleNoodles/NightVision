"""Turn an image with transparency into a SWF the game can show as a NightVision overlay.

Usage: python make_overlay.py <input.png or .dds> <output.swf>

The game does not draw bitmaps embedded in a SWF, so only the alpha channel is used: it is traced into
black vector layers that rebuild the soft edge. Colours in the image are ignored. The SWF also holds a
stub document class, because an ActionScript 3 movie needs one to build its timeline, and a looping
clip of random specks named grain, which sits under the mask. The plugin sets its alpha.

The plugin stretches the stage over the whole screen, so draw for the final aspect ratio, or for a
square that is stretched to fit. Needs Pillow, numpy and contourpy.
"""
import random
import struct
import sys

import contourpy
import numpy
from PIL import Image

STEP = 0.1
LEVELS = [(i + 0.5) * STEP for i in range(10)]

GRAIN_FRAMES = 8
GRAIN_DOTS = 4000
GRAIN_ID = 100


class Bits:
    def __init__(self):
        self.value = 0
        self.count = 0

    def add(self, value, width):
        self.value = (self.value << width) | (value & ((1 << width) - 1))
        self.count += width

    def bytes(self):
        pad = -self.count % 8
        return (self.value << pad).to_bytes((self.count + pad) // 8, "big")


def signed_bits(*values):
    return max(len(bin(abs(value))) - 2 + 1 for value in values)


def rect(xmax, ymax):
    width = signed_bits(xmax, ymax)
    bits = Bits()
    bits.add(width, 5)
    for value in (0, xmax, 0, ymax):
        bits.add(value, width)
    return bits.bytes()


def matrix(scale=None):
    bits = Bits()
    bits.add(1 if scale else 0, 1)
    if scale:
        fixed = scale << 16
        width = signed_bits(fixed)
        bits.add(width, 5)
        bits.add(fixed, width)
        bits.add(fixed, width)
    bits.add(0, 1)  # no rotate
    bits.add(0, 5)  # no translate
    return bits.bytes()


def tag(code, body=b""):
    if len(body) < 63:
        return struct.pack("<H", code << 6 | len(body)) + body
    return struct.pack("<HI", code << 6 | 63, len(body)) + body


def mask_shape(shape_id, rings, opacity, size):
    """A black shape covering the stage and more, with each ring cut out of it."""
    margin = 32 * 20
    far = (size + 32) * 20
    body = struct.pack("<H", shape_id) + rect(far, far)
    body += b"\x01\x00" + bytes((0, 0, 0, round(opacity * 255)))  # one solid RGBA fill
    body += b"\x00"  # no line styles
    bits = Bits()
    bits.add(1, 4)  # fill index bits
    bits.add(0, 4)  # line index bits

    def start(x, y, first):
        bits.add(0, 1)  # style change record
        bits.add(0b00101 if first else 0b00001, 5)  # fill style 1 on the first path, then move
        width = max(signed_bits(x), signed_bits(y))
        bits.add(width, 5)
        bits.add(x, width)
        bits.add(y, width)
        if first:
            bits.add(1, 1)

    def line(dx, dy):
        if dx == 0 and dy == 0:
            return
        width = max(signed_bits(dx), signed_bits(dy), 2)
        bits.add(1, 1)  # edge
        bits.add(1, 1)  # straight
        bits.add(width - 2, 4)
        bits.add(1, 1)  # general line
        bits.add(dx, width)
        bits.add(dy, width)

    def path(points, first=False):
        x, y = points[0]
        start(x, y, first)
        for next_x, next_y in points[1:] + points[:1]:
            line(next_x - x, next_y - y)
            x, y = next_x, next_y

    # Fill style 1 sits on the right of the direction of travel, so the outside runs clockwise on
    # screen and each hole runs the other way.
    path([(-margin, -margin), (far, -margin), (far, far), (-margin, far)], True)
    for ring in rings:
        path(ring)
    bits.add(0, 6)  # end of shape
    return body + bits.bytes()


def grain_shape(shape_id, size, seed):
    """Random specks. They are narrow because the square stage is stretched wide."""
    rng = random.Random(seed)
    far = size * 20
    body = struct.pack("<H", shape_id) + rect(far, far)
    body += b"\x01\x00" + bytes((255, 255, 255, 255)) + b"\x00"
    bits = Bits()
    bits.add(1, 4)
    bits.add(0, 4)
    width, height = 24, 40
    for dot in range(GRAIN_DOTS):
        x = rng.randrange(far - width)
        y = rng.randrange(far - height)
        bits.add(0, 1)
        bits.add(0b00101 if dot == 0 else 0b00001, 5)
        move = max(signed_bits(x), signed_bits(y))
        bits.add(move, 5)
        bits.add(x, move)
        bits.add(y, move)
        if dot == 0:
            bits.add(1, 1)
        for vertical, delta in ((0, width), (1, height), (0, -width), (1, -height)):
            size_bits = signed_bits(delta)
            bits.add(1, 1)
            bits.add(1, 1)
            bits.add(size_bits - 2, 4)
            bits.add(0, 1)
            bits.add(vertical, 1)
            bits.add(delta, size_bits)
    bits.add(0, 6)
    return body + bits.bytes()


def grain_tags(size):
    """Define the speck shapes and the clip that loops through them at 15 frames a second."""
    tags = b""
    frames = b""
    for frame in range(GRAIN_FRAMES):
        shape_id = GRAIN_ID + 1 + frame
        tags += tag(32, grain_shape(shape_id, size, frame))
        flags = b"\x02" if frame == 0 else b"\x03"  # place, then replace the character
        frames += tag(26, flags + struct.pack("<HH", 1, shape_id)) + tag(1) + tag(1)
    frames += tag(0)
    return tags + tag(39, struct.pack("<HH", GRAIN_ID, GRAIN_FRAMES * 2) + frames)


def signed_area(points):
    return sum(x0 * y1 - x1 * y0 for (x0, y0), (x1, y1) in zip(points, points[1:] + points[:1]))


def trace(alpha, level):
    """Closed rings, in twips, around every area of the mask below this alpha."""
    pad = 16
    padded = numpy.pad(alpha, pad, constant_values=255)  # closes holes that run off the image
    rings = []
    for line in contourpy.contour_generator(z=padded).lines(level * 255):
        points = []
        for x, y in line[:-1][::3]:
            point = (round((x - pad) * 20), round((y - pad) * 20))
            if not points or point != points[-1]:
                points.append(point)
        if len(points) < 3:
            continue
        if signed_area(points) > 0:  # clockwise on screen, because y runs down
            points.reverse()
        rings.append(points)
    return rings


def u30(value):
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        out.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(out)


def text(value):
    data = value.encode()
    return u30(len(data)) + data


def abc():
    """ActionScript bytecode for: public class MainTimeline extends MovieClip {}"""
    strings = ["", "flash.display", "flash.events", "MainTimeline", "Object", "EventDispatcher", "DisplayObject",
               "InteractiveObject", "DisplayObjectContainer", "Sprite", "MovieClip"]
    index = {name: i + 1 for i, name in enumerate(strings)}

    package = 0x16
    protected = 0x18
    namespaces = [(package, ""), (package, "flash.display"), (package, "flash.events"), (protected, "MainTimeline")]
    public, display, events = 1, 2, 3

    names = [("Object", public), ("EventDispatcher", events), ("DisplayObject", display), ("InteractiveObject", display),
             ("DisplayObjectContainer", display), ("Sprite", display), ("MovieClip", display), ("MainTimeline", public)]
    chain = [i + 1 for i in range(7)]
    movie_clip = chain[-1]
    main_timeline = 8

    pool = b"".join((u30(0), u30(0), u30(0)))  # no ints, uints or doubles
    pool += u30(len(strings) + 1) + b"".join(text(name) for name in strings)
    pool += u30(len(namespaces) + 1) + b"".join(bytes((kind,)) + u30(index[name]) for kind, name in namespaces)
    pool += u30(0)  # no namespace sets
    pool += u30(len(names) + 1) + b"".join(b"\x07" + u30(ns) + u30(index[name]) for name, ns in names)

    methods = u30(3) + b"\x00\x00\x00\x00" * 3  # no params, any return, no name, no flags
    instance = u30(main_timeline) + u30(movie_clip) + b"\x09" + u30(4) + u30(0) + u30(1) + u30(0)  # sealed, protected ns
    class_info = u30(0) + u30(0)
    script = u30(2) + u30(1) + u30(main_timeline) + b"\x04" + u30(1) + u30(0)  # class trait

    def body(method, stack, locals_, init, scope, code):
        return u30(method) + u30(stack) + u30(locals_) + u30(init) + u30(scope) + u30(len(code)) + code + u30(0) + u30(0)

    getlex = lambda name: b"\x60" + u30(name)
    pushscope = b"\x30"
    script_code = b"\xd0" + pushscope + b"\x65\x00"
    script_code += b"".join(getlex(name) + pushscope for name in chain)
    script_code += getlex(movie_clip) + b"\x58\x00" + b"\x1d" * len(chain) + b"\x68" + u30(main_timeline) + b"\x47"

    bodies = u30(3)
    bodies += body(0, 1, 1, 8, 9, b"\xd0\x30\x47")  # class init
    bodies += body(1, 1, 1, 9, 10, b"\xd0\x30\xd0\x49\x00\x47")  # constructor: super()
    bodies += body(2, 2, 1, 1, 9, script_code)

    return struct.pack("<HH", 16, 46) + pool + methods + u30(0) + u30(1) + instance + class_info + u30(1) + script + bodies


def build(image):
    width, height = image.size
    alpha = numpy.asarray(image.getchannel("A"), dtype=float)

    # Stacked layers, each covering more of the edge's fade, add up to the mask's soft alpha.
    tags = tag(69, struct.pack("<I", 0x08))  # FileAttributes: ActionScript 3
    tags += grain_tags(width)
    placed = tag(26, b"\x26" + struct.pack("<HH", 1, GRAIN_ID) + matrix() + b"grain\x00")  # PlaceObject2 with a name
    covered = 0.0
    for layer, level in enumerate(LEVELS):
        target = min((layer + 1) * STEP, 1.0)
        opacity = 1.0 - (1.0 - target) / (1.0 - covered)
        covered = target
        shape_id = layer + 1
        tags += tag(32, mask_shape(shape_id, trace(alpha, level), opacity, width))  # DefineShape3
        placed += tag(26, b"\x06" + struct.pack("<HH", layer + 2, shape_id) + matrix())  # PlaceObject2

    tags += tag(82, struct.pack("<I", 1) + b"\x00" + abc())  # DoABC
    tags += tag(76, struct.pack("<HH", 1, 0) + b"MainTimeline\x00")  # SymbolClass: the root
    tags += placed
    tags += tag(1)  # ShowFrame
    tags += tag(0)  # End

    body = rect(width * 20, height * 20) + struct.pack("<HH", 30 << 8, 1) + tags
    return b"FWS" + bytes((15,)) + struct.pack("<I", 8 + len(body)) + body


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    image = Image.open(sys.argv[1]).convert("RGBA")
    if image.width != image.height:
        sys.exit("The image must be square")
    with open(sys.argv[2], "wb") as out:
        out.write(build(image))


if __name__ == "__main__":
    main()
