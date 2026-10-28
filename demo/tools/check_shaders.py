"""The demo's shaders must be the plugin's shaders, character for character.

    python3 demo/tools/check_shaders.py

Called from `tools/verify.sh`. Exit code 1 means the two copies have drifted.

------------------------------------------------------------------- why

`demo/plugin.js` holds the five GLSL strings of `source/Shaders.cpp` -- the
version line, the vertex body and the capture, resample and output bodies --
and the numbers the hands, the projector and the print are made of. That is
two copies of the same text, and two copies drift quietly: a demo that renders
a *plausible* flickering, sepia, scratched print looks exactly like one that
renders the right one. The page's claim is that the loop of film, the print and
the shutter's sum running in your browser are the plugin's, so the claim needs
something enforcing it. `hmtest` drives the real plugin class and has never
heard of this page.

------------------------------------------------------------------- what it does

1. Pulls each `R"( ... )"` body out of Shaders.cpp and each matching backtick
   literal out of plugin.js, and compares them exactly -- no whitespace
   normalisation, no comment stripping. A comment updated on one side only is
   drift worth catching; the comments carry the reasoning (why the bilinear is
   written out, why the host's picture is read with texelFetch).

   The one transformation is a decode, not a normalisation. A template literal
   cannot hold a raw backtick, backslash or `${`, so `sync_shaders.py` escapes
   those three; this undoes exactly those three and REJECTS any other backslash
   on the JS side, which could only be somebody hiding a difference in the
   decoder.

2. Regenerates the rest of the generated block -- Shaders.h's print-data layout,
   every constant and enum of Model.h and Model.cpp's hash streams, Spectral.h's
   weights, the control counts and option lists, Handleman.cpp's constants and
   the print seed -- from the C++ and compares it with plugin.js, line for
   line.

------------------------------------------------------------------- what it cannot

Nothing here checks the PORT. The hash, the two hands, `CumulativeOpen` and
`Segments`, the weave, `Scratches`, `Dust`, `HairAt`, `FogExposure`, the Reel,
`ToneLaw`, every control's law, FilmRing's growth and
Handleman::ProcessOpenGL's clock, cranks, pull-downs, camera, frame table,
print upload and uniforms in plugin.js are a hand translation of Model.cpp,
Controls.cpp, FilmRing.cpp and Handleman.cpp, and only a reader can tell
whether they still agree (or demo/tools/crosscheck, which renders the page and
`hmtest --pipe` from the same frames and compares them, when somebody runs it).
Change one of those and change the page by hand to match.

If this fails because a shader changed in the plugin: `python3 demo/tools/sync_shaders.py`,
never an edit of plugin.js by hand.
"""
import os
import re
import sys

# No __pycache__ beside the page's sources.
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sync_shaders  # noqa: E402

REPO = sync_shaders.REPO


def from_js(source, name):
    match = re.search(r"^const " + name + r" = `(.*?)`;$", source, re.S | re.M)
    if match is None:
        return None, None
    body = match.group(1)
    stray = re.search(r"\\(?![`\\$])", body)
    if stray is not None:
        upto = body[: stray.start()]
        return None, f"backslash that is not an escaped backtick, backslash or dollar, at line {upto.count(chr(10)) + 1}"
    decoded = re.sub(r"\\([`\\$])", r"\1", body)
    return decoded, None


def main():
    with open(os.path.join(REPO, "demo", "plugin.js")) as handle:
        js = handle.read()
    shaders_cpp = sync_shaders.read("source/Shaders.cpp")

    problems = 0

    # The version line assemble() prepends.
    want = sync_shaders.cpp_version(shaders_cpp)
    got = re.search(r'^const K_VERSION = "(.*?)";$', js, re.M)
    if got is None or got.group(1) != want:
        print("FAIL  K_VERSION is not Shaders.cpp's kVersion")
        problems += 1
    else:
        print(f"ok    {'K_VERSION':<14} matches kVersion")

    for name, symbol in sync_shaders.SHADERS:
        cpp_text = sync_shaders.cpp_shader(shaders_cpp, symbol)
        js_text, complaint = from_js(js, name)
        if complaint is not None:
            print(f"FAIL  {name} in demo/plugin.js has a {complaint}")
            problems += 1
            continue
        if js_text is None:
            print(f"FAIL  {name} not found in demo/plugin.js")
            problems += 1
            continue
        if cpp_text == js_text:
            print(f"ok    {name:<14} matches {symbol} ({len(cpp_text)} chars)")
            continue
        problems += 1
        print(f"FAIL  {name} has drifted from {symbol} in source/Shaders.cpp")
        a_lines, b_lines = cpp_text.splitlines(), js_text.splitlines()
        for i in range(max(len(a_lines), len(b_lines))):
            a = a_lines[i] if i < len(a_lines) else "<missing>"
            b = b_lines[i] if i < len(b_lines) else "<missing>"
            if a != b:
                print(f"        first difference at line {i + 1}")
                print(f"          C++: {a}")
                print(f"          js : {b}")
                break

    # The tables and constants: the whole generated block, regenerated.
    where = sync_shaders.region(js)
    if where is None:
        print("FAIL  demo/plugin.js has no generated block")
        problems += 1
    else:
        have = js[where[0]: where[1]].splitlines()
        want_lines = sync_shaders.block().splitlines()
        if have == want_lines:
            constants = sum(1 for line in want_lines if re.match(r"^[A-Z_]+\.(k|seed)", line))
            print(f"ok    the generated block matches source/ (Shaders.h, Model.h, Spectral.h, Controls, Handleman, {constants} constants)")
        else:
            problems += 1
            for i in range(max(len(have), len(want_lines))):
                a = want_lines[i] if i < len(want_lines) else "<missing>"
                b = have[i] if i < len(have) else "<missing>"
                if a != b:
                    print(f"FAIL  the generated block differs from source/ at its line {i + 1}")
                    print(f"          source: {a[:120]}")
                    print(f"          js    : {b[:120]}")
                    break

    print()
    if problems:
        print(f"{problems} piece(s) differ -- run python3 demo/tools/sync_shaders.py, do not edit plugin.js by hand")
        return 1
    print(f"all {len(sync_shaders.SHADERS) + 1} shader pieces and every copied constant are identical to the plugin's")
    return 0


if __name__ == "__main__":
    sys.exit(main())
