#!/usr/bin/env python3
"""gen-glproto: the engine's decoders for the emugen wire protocols (docs/graphics.md, G1).

  python3 -I tools/gen-glproto.py SPEC_DIR OUT_DIR [--check-guest GUEST_DIR]

SPEC_DIR holds the protocol specs as vulkan-cereal has them: renderControl/, gles2/, gles1/, each with
NAME.in (the calls), NAME.attrib (per-call attributes) and NAME.types (the wire types). The specs are read,
never copied: their licensing is not settled (see docs/graphics.md, G1).

OUT_DIR receives:
  gl_wire.h          the wire helpers the decoders use (reply buffer, reads), written by this script
  gl_backend.h       one struct of function pointers, one member per decoded call, for all protocols
  <name>_opcodes.h   OP_<call> = number, as emugen numbers them (base_opcode, then the .in order)
  <name>_dec.c       <name>_decode(): reads a packet, calls the backend, writes the reply
  <name>_report.txt  per call: opcode, status (decoded, or why not), and its fixed packet size

Wire rules (emugen README; checked against vulkan-cereal stream-servers/gles2_dec/gles2_dec.cpp):
  a packet is uint32 opcode, uint32 total length (header included), then the arguments in order.
  A scalar takes its width from NAME.types (bits / 8; GLboolean is one byte). An in or inout pointer is a
  uint32 length and that many bytes. An out pointer is a uint32 length only. A reply is the bytes of the
  out and inout pointers in order, then the return value if the call returns one.
  Checksums are off (the guest sends none), so the decoders have no checksum path.

Calls the decoders do not handle are listed in the report and make the decoder answer -1 (never a silent
skip): pointer return values, pointers to pointers, calls with flag custom_decoder or unsupported, and
types the table does not know. The guest's own encoders for those calls are not run by the engine.
"""

import os
import re
import sys

PROTOCOLS = [("renderControl", 10000), ("gles2", 2048), ("gles1", 1024)]

# C types of the scalar wire types. The backend struct uses these; pointers are void * (see the header).
C_TYPE = {
    "GLboolean": "uint8_t", "GLubyte": "uint8_t", "GLchar": "char", "GLshort": "int16_t",
    "GLclampx": "int32_t", "GLfixed": "int32_t", "GLint": "int32_t", "GLint32": "int32_t",
    "GLsizei": "int32_t", "GLuint": "uint32_t", "GLuint32": "uint32_t", "GLenum": "uint32_t",
    "GLbitfield": "uint32_t", "GLfloat": "float", "GLclampf": "float", "GLvoid": "void",
    "GLintptr": "int32_t", "GLsizeiptr": "int32_t", "GLint64": "int64_t", "GLuint64": "uint64_t",
    "GLsync": "uint64_t", "GLeglImageOES": "uint64_t", "uint32_t": "uint32_t", "uint64_t": "uint64_t",
    "int32_t": "int32_t", "EGLint": "int32_t", "EGLenum": "uint32_t", "EGLBoolean": "uint32_t",
    "EGLNativeWindowType": "uint64_t", "EGLNativeDisplayType": "uint64_t", "EGLNativePixmapType": "uint64_t",
    "EGLClientBuffer": "uint64_t", "EGLImageKHR": "uint64_t", "int": "int32_t", "char": "char",
}
# The unsigned wire type of each width, for reading a scalar before its C cast.
WIRE_UINT = {8: "uint8_t", 16: "uint16_t", 32: "uint32_t", 64: "uint64_t"}


def read_types(path):
    """NAME.types: 'name bits format is_pointer'. Returns {name: bits}."""
    table = {}
    with open(path, encoding="utf-8") as fh:
        for raw in fh:
            parts = raw.split()
            if len(parts) >= 2 and parts[1].isdigit():
                table[parts[0]] = int(parts[1])
    return table


def read_attrib(path):
    """NAME.attrib: returns (base_opcode or None, {call: {dir, len, flags}})."""
    base = None
    calls = {}
    cur = None
    with open(path, encoding="utf-8") as fh:
        for raw in fh:
            line = raw.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            if not line[0] in " \t":
                cur = line.strip()
                if cur != "GLOBAL":
                    calls.setdefault(cur, {"dir": {}, "len": {}, "flags": set(), "var_flag": {}})
                continue
            toks = line.split()
            if cur == "GLOBAL":
                if toks[0] == "base_opcode":
                    base = int(toks[1])
                continue
            info = calls[cur]
            if toks[0] == "dir" and len(toks) >= 3:
                info["dir"][toks[1]] = toks[2]
            elif toks[0] == "len" and len(toks) >= 3:
                info["len"][toks[1]] = " ".join(toks[2:])
            elif toks[0] == "var_flag" and len(toks) >= 3:
                info["var_flag"][toks[1]] = toks[2:]
            elif toks[0] == "flag":
                info["flags"].update(toks[1:])
    return base, calls


def split_top(s):
    """Splits a parameter list on commas (none of the specs nest commas inside parentheses)."""
    return [p.strip() for p in s.split(",")] if s.strip() else []


def parse_param(text, idx, types):
    """'const GLfloat *v' -> (name, ctype_text, base, ptr_depth). 'void' -> None."""
    text = text.strip()
    if text == "void" or text == "":
        return None
    words = text.replace("*", " * ").split()
    if len(words) >= 2 and words[-1] != "*" and words[-1] != "const" and words[-1] not in types:
        name = words[-1]
        ctype = " ".join(words[:-1])
    else:
        name = "a%d" % idx
        ctype = text
    depth = ctype.count("*")
    base = re.sub(r"\bconst\b|\*|\s", "", ctype)
    return name, ctype, base, depth


def read_in(path, types):
    """NAME.in: [(call, ret, [params])] in file order. A line is PREFIX(ret, name, params...)."""
    calls = []
    with open(path, encoding="utf-8") as fh:
        for raw in fh:
            line = raw.strip().rstrip(";").strip()
            if not line or line.startswith("#"):
                continue
            m = re.match(r"^\w+\s*\((.*)\)\s*$", line)
            if not m:
                raise SystemExit("gen-glproto: cannot parse %s: %r" % (path, line))
            parts = split_top(m.group(1))
            ret, name, rest = parts[0], parts[1], parts[2:]
            params = []
            for i, p in enumerate(rest):
                if p in ("void", ""):
                    continue
                params.append(parse_param(p, i, types))
            calls.append((name, ret, params))
    return calls


def classify(name, ret, params, attrib, types):
    """Returns (status, reason). status is 'decoded' or 'skipped'."""
    flags = attrib.get("flags", set())
    if "not_api" in flags:
        return "skipped", "not_api"
    if "unsupported" in flags:
        return "skipped", "flag unsupported"
    if any("DMA" in v for v in attrib.get("var_flag", {}).values()):
        # The DMA pointers travel as a 64-bit guest address, not as data; the DMA transport is not advertised.
        return "skipped", "var_flag DMA: the DMA transport is not advertised (docs/graphics.md 3.2)"
    rbase = re.sub(r"\bconst\b|\*|\s", "", ret)
    if "*" in ret:
        return "skipped", "pointer return value"
    if rbase != "void" and not known(rbase):
        return "skipped", "return type %s is not a known type" % rbase
    for pname, ctype, base, depth in params:
        if depth >= 2:
            return "skipped", "pointer to pointer (%s)" % pname
        if depth == 0 and (base == "void" or not known(base)):
            return "skipped", "scalar type %s is not known" % base
    if "custom_decoder" in flags:
        return "decoded", "custom_decoder: the backend provides the call"
    return "decoded", ""


def known(base):
    return base in attrib_bits or base in C_TYPE


def bits_of(base):
    """Wire width in bits: from the .types table, else the C type."""
    if base in attrib_bits:
        return attrib_bits[base]
    ct = C_TYPE.get(base, "uint32_t")
    return {"uint8_t": 8, "char": 8, "int16_t": 16, "uint64_t": 64, "int64_t": 64}.get(ct, 32)


def fixed_size(params, attrib):
    """The request size without variable data: 8 + scalars + 4 per pointer (the length word)."""
    size = 8
    for pname, ctype, base, depth in params:
        if depth >= 1:
            size += 4
        else:
            size += bits_of(base) // 8
    return size


attrib_bits = {}
FIXED = {}
SKIP_REASON = {}


def reason_of(attr):
    return "var_flag DMA" if any("DMA" in v for v in attr.get("var_flag", {}).values()) else ""


def c_scalar(base):
    return C_TYPE.get(base, "uint32_t")


def gen_all(spec_dir, out_dir, check_guest):
    global attrib_bits
    os.makedirs(out_dir, exist_ok=True)
    report_lines = []
    range_lines = []
    backend_members = []
    opcodes_by_proto = {}
    decoders = {}
    for proto, base_default in PROTOCOLS:
        d = os.path.join(spec_dir, proto)
        types = read_types(os.path.join(d, proto + ".types"))
        attrib_bits = types
        base_attr, attribs = read_attrib(os.path.join(d, proto + ".attrib"))
        base = base_attr if base_attr is not None else base_default
        calls = read_in(os.path.join(d, proto + ".in"), types)
        addon = os.path.join(d, proto + ".addon")
        addon_calls = read_in(addon, types) if os.path.exists(addon) else []
        # opcode per call: base, then one per .in line in order (emugen numbers every call)
        ops = []
        cases = []
        opcode_lines = []
        for i, (name, ret, params) in enumerate(calls):
            op = base + i
            ops.append((name, op))
            attr = attribs.get(name, {"dir": {}, "len": {}, "flags": set(), "var_flag": {}})
            status, reason = classify(name, ret, params, attr, types)
            fsize = fixed_size(params, attr)
            FIXED[(proto, name)] = fsize
            SKIP_REASON[(proto, name)] = reason_of(attr)
            opcode_lines.append("#define OP_%s %d" % (name, op))
            report_lines.append("%s %s %d %s fixed=%d %s" % (proto, name, op, status, fsize, reason))
            if status != "decoded":
                continue
            rbase = re.sub(r"\bconst\b|\*|\s", "", ret)
            rtype = C_TYPE.get(rbase, "void")
            members = []
            for pname, ctype, base_t, depth in params:
                members.append((pname, base_t, depth, attr["dir"].get(pname, "in")))
            ret_void = rbase == "void"
            backend_members.append((proto, name, rtype, ret_void, members))
            cases.append((name, op, rtype, ret_void, members))
        for name, ret, params in addon_calls:
            report_lines.append("%s %s - skipped fixed=- addon: its opcode numbers come from a gl.in that is not in the tree" % (proto, name))
        if calls:
            range_lines.append("#define OP_FIRST_%s %d" % (proto, base))
            range_lines.append("#define OP_LAST_%s %d" % (proto, base + len(calls) - 1))
        opcodes_by_proto[proto] = opcode_lines
        decoders[proto] = (ops, cases)
        with open(os.path.join(out_dir, proto + "_opcodes.h"), "w", encoding="utf-8") as fh:
            fh.write("/* Generated by tools/gen-glproto.py from the %s spec. Do not edit. */\n" % proto)
            fh.write("\n".join(opcode_lines) + "\n")

    with open(os.path.join(out_dir, "gl_ranges.h"), "w", encoding="utf-8") as fh:
        fh.write("/* Generated by tools/gen-glproto.py. Do not edit. First and last opcode of each protocol. */\n"
                 "#ifndef XIAOJI_GL_RANGES_H\n#define XIAOJI_GL_RANGES_H\n" + "\n".join(range_lines) + "\n#endif\n")
    with open(os.path.join(out_dir, "gl_wire.h"), "w", encoding="utf-8") as fh:
        fh.write(WIRE_H)
    with open(os.path.join(out_dir, "gl_backend.h"), "w", encoding="utf-8") as fh:
        fh.write(backend_header(backend_members))
    with open(os.path.join(out_dir, "gl_defaults.c"), "w", encoding="utf-8") as fh:
        fh.write(defaults_source(backend_members))
    for proto, _ in PROTOCOLS:
        ops, cases = decoders[proto]
        with open(os.path.join(out_dir, proto + "_dec.c"), "w", encoding="utf-8") as fh:
            fh.write(decoder_source(proto, cases))
    with open(os.path.join(out_dir, "gen_report.txt"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(report_lines) + "\n")

    counts = {}
    for line in report_lines:
        status = line.split()[3]
        counts[status] = counts.get(status, 0) + 1
    print("gen-glproto: %s" % ", ".join("%s %d" % kv for kv in sorted(counts.items())))
    if check_guest:
        check_against_guest(check_guest, decoders)


WIRE_H = r"""/* Generated by tools/gen-glproto.py. Do not edit.
 * The wire helpers of the decoders: a growable reply buffer, and bounds-checked reads. */
#ifndef XIAOJI_GL_WIRE_H
#define XIAOJI_GL_WIRE_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *buf;
    size_t len, cap;
} gl_reply;

static inline int gl_reply_put(gl_reply *r, const void *src, size_t n)
{
    if (r->len + n > r->cap) {
        size_t nc = r->cap ? r->cap : 64;
        while (nc < r->len + n)
            nc *= 2;
        uint8_t *nb = realloc(r->buf, nc);
        if (!nb)
            return -1;
        r->buf = nb;
        r->cap = nc;
    }
    if (n)
        memcpy(r->buf + r->len, src, n);
    r->len += n;
    return 0;
}

/* Takes n bytes of a request at *q (with *left bytes remaining). Returns 0, or -1 if short. */
static inline int gl_take(const uint8_t **q, size_t *left, size_t n, const uint8_t **out)
{
    if (*left < n)
        return -1;
    *out = *q;
    *q += n;
    *left -= n;
    return 0;
}

/* A default (no-op) backend call ran: gl_host.c logs each call name once. */
void gl_note_default(const char *name);

static inline uint64_t gl_load(const uint8_t *p, size_t n)
{
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

#endif
"""


def backend_header(members):
    out = [
        "/* Generated by tools/gen-glproto.py from the renderControl, gles2 and gles1 specs. Do not edit.",
        " * The backend: one function pointer per decoded call. Every function takes the stream's context",
        " * first. Pointer arguments are void *: an in pointer points at the request bytes (read only), an out",
        " * pointer at a zeroed buffer of the length the guest asked for (its contents are the reply). A NULL",
        " * member is answered as an error by the decoder. */",
        "#ifndef XIAOJI_GL_BACKEND_H",
        "#define XIAOJI_GL_BACKEND_H",
        "",
        "#include <stdint.h>",
        '#include "gl_wire.h"',
        "",
    ]
    for proto, _ in PROTOCOLS:
        out.append("typedef struct %s_backend {" % proto)
        for mproto, name, rtype, ret_void, params in members:
            if mproto != proto:
                continue
            out.append("    %s (*%s)(%s);" % (rtype, name, ", ".join(backend_args(params))))
        out.append("} %s_backend_t;" % proto)
        out.append("")
    for proto, _ in PROTOCOLS:
        out.append("int %s_decode(const %s_backend_t *be, void *ctx, const uint8_t *pkt, size_t len, gl_reply *rep);" % (proto, proto))
    out += ["", "#endif", ""]
    return "\n".join(out)


def backend_args(params):
    """Each pointer is followed by its wire length (the guest's byte count), so a backend never writes past it."""
    args = ["void *ctx"]
    for pname, base, depth, direction in params:
        if depth >= 1:
            args.append("%s void *%s" % ("const" if direction == "in" else "", pname))
            args.append("uint32_t %s_len" % pname)
        else:
            args.append("%s %s" % (C_TYPE.get(base, "uint32_t"), pname))
    return args


def defaults_source(members):
    """gl_defaults.c: every decoded call with a no-op implementation (void, or zero), per protocol."""
    out = [
        "/* Generated by tools/gen-glproto.py. Do not edit.",
        " * The default implementation of every decoded call: void calls do nothing, others return 0. The",
        " * null backend (engine/gl/gl_null.c) overrides the calls that need a real answer. */",
        '#include "gl_backend.h"',
        "",
    ]
    for proto, _ in PROTOCOLS:
        table = []
        for mproto, name, rtype, ret_void, params in members:
            if mproto != proto:
                continue
            args = backend_args(params)
            fn = "null_%s_%s" % (proto, name)
            body = "" if ret_void else "    return 0;"
            out.append("static %s %s(%s)" % (rtype, fn, ", ".join(args)))
            out.append("{")
            for pname, base, depth, direction in params:
                out.append("    (void)%s;" % pname)
            out.append("    (void)ctx;")
            out.append("    gl_note_default(\"%s\");" % name)
            if body:
                out.append(body)
            out.append("}")
            out.append("")
            table.append("    .%s = %s," % (name, fn))
        out.append("const %s_backend_t %s_null_defaults = {" % (proto, proto))
        out += table
        out += ["};", ""]
    return "\n".join(out)


def decoder_source(proto, cases):
    out = [
        "/* Generated by tools/gen-glproto.py from the %s spec. Do not edit. */" % proto,
        '#include "gl_backend.h"',
        '#include "gl_wire.h"',
        '#include "%s_opcodes.h"' % proto,
        "",
        "/* Decodes one %s request of total length len (header included) and calls the backend. The reply" % proto,
        " * is appended to rep. Returns 0, or -1 for a malformed request, an opcode this decoder does not"
        " handle, or a call the backend does not implement. */",
        "int %s_decode(const %s_backend_t *be, void *ctx, const uint8_t *pkt, size_t len, gl_reply *rep)" % (proto, proto),
        "{",
        "    if (len < 8)",
        "        return -1;",
        "    const uint8_t *q = pkt + 8;",
        "    size_t left = len - 8;",
        "    const uint8_t *tp;",
        "    (void)tp;",
        "    switch ((uint32_t)gl_load(pkt, 4)) {",
    ]
    for name, op, rtype, ret_void, params in cases:
        out.append("    case OP_%s: {" % name)
        out.append("        if (!be->%s) return -1;" % name)  # before any allocation
        out.append("        /* scalars and pointer lengths, in wire order */")
        vars_ = []
        for pname, base, depth, direction in params:
            if depth >= 1:
                out.append("        uint32_t n_%s;" % pname)
                out.append("        if (gl_take(&q, &left, 4, &tp) != 0) return -1;")
                out.append("        n_%s = (uint32_t)gl_load(tp, 4);" % pname)
                if direction in ("in", "inout"):
                    out.append("        if (gl_take(&q, &left, n_%s, &tp) != 0) return -1;" % pname)
                    out.append("        void *v_%s = (void *)tp;" % pname)
                    if direction == "inout":
                        out.append("        void *o_%s = calloc(1, n_%s ? n_%s : 1);" % (pname, pname, pname))
                        out.append("        if (!o_%s) return -1;" % pname)
                        out.append("        memcpy(o_%s, tp, n_%s);" % (pname, pname))
                        out.append("        v_%s = o_%s;" % (pname, pname))
                else:
                    out.append("        void *v_%s = calloc(1, n_%s ? n_%s : 1);" % (pname, pname, pname))
                    out.append("        if (!v_%s) return -1;" % pname)
                vars_.append((pname, True, direction))
            else:
                bits = bits_of(base)
                nb = bits // 8
                ct = C_TYPE.get(base, "uint32_t")
                out.append("        if (gl_take(&q, &left, %d, &tp) != 0) return -1;" % nb)
                if ct in ("float", "double"):
                    # The wire holds the value's bits (IEEE 754, little-endian): reinterpret them, do not convert the
                    # integer (B18: a conversion made every float argument the integer value of its bit pattern).
                    out.append("        %s a_%s;" % (ct, pname))
                    out.append("        { %s b_ = (%s)gl_load(tp, %d); memcpy(&a_%s, &b_, %d); }"
                               % (WIRE_UINT[bits], WIRE_UINT[bits], nb, pname, nb))
                else:
                    out.append("        %s a_%s = (%s)(%s)gl_load(tp, %d);" % (ct, pname, ct, WIRE_UINT[bits], nb))
                vars_.append((pname, False, "in"))
        call_args = ["ctx"]
        for pname, is_ptr, direction in vars_:
            if is_ptr:
                call_args += ["v_%s" % pname, "n_%s" % pname]
            else:
                call_args.append("a_%s" % pname)
        out.append("        if (left != 0) return -1;  /* the packet has bytes the call does not use */")
        if ret_void:
            out.append("        be->%s(%s);" % (name, ", ".join(call_args)))
        else:
            out.append("        %s ret = be->%s(%s);" % (rtype, name, ", ".join(call_args)))
        # The reply: the out and inout buffers in order, then the return value. Every buffer is freed.
        frees = []
        for pname, is_ptr, direction in vars_:
            if is_ptr and direction in ("out", "inout"):
                out.append("        if (gl_reply_put(rep, v_%s, n_%s) != 0) { free(v_%s); return -1; }" % (pname, pname, pname))
                frees.append("v_%s" % pname)
        if not ret_void:
            out.append("        if (gl_reply_put(rep, &ret, sizeof ret) != 0) { %s return -1; }" % "".join("free(%s); " % f for f in frees))
        for f in frees:
            out.append("        free(%s);" % f)
        out.append("        return 0;")
        out.append("    }")
    out += [
        "    default:",
        "        return -1;",
        "    }",
        "}",
        "",
    ]
    return "\n".join(out)


def check_against_guest(guest_dir, decoders):
    """Diffs the opcode numbers and the fixed request sizes with the guest's generated encoders."""
    bad = 0
    checked = 0
    for proto, _ in PROTOCOLS:
        hdr = {}
        opath = os.path.join(guest_dir, proto + "_opcodes.h")
        if not os.path.exists(opath):
            print("check: %s not found" % opath)
            bad += 1
            continue
        with open(opath, encoding="utf-8") as fh:
            for raw in fh:
                m = re.match(r"#define OP_(\w+)\s+(\d+)", raw)
                if m:
                    hdr[m.group(1)] = int(m.group(2))
        ops, _cases = decoders[proto]
        for name, op in ops:
            if name in hdr:
                checked += 1
                if hdr[name] != op:
                    print("check: %s %s opcode %d, guest says %d" % (proto, name, op, hdr[name]))
                    bad += 1
        missing = [n for n, _ in ops if n not in hdr]
        if missing:
            print("check: %s: calls not in the guest header: %s" % (proto, " ".join(missing)))
            bad += 1
    print("check: %d opcodes compared with the guest headers, %d problems" % (checked, bad))
    sized, mismatched = check_sizes(guest_dir, decoders)
    bad += mismatched
    print("check: %d fixed request sizes compared with the guest encoders, %d mismatches" % (sized, mismatched))
    if bad:
        raise SystemExit(1)


ENC_FILES = {"renderControl": "renderControl_enc.cpp", "gles2": "gl2_enc.cpp", "gles1": "gl_enc.cpp"}


def check_sizes(guest_dir, decoders):
    """Each guest encoder fixes its request size with 'sizeWithoutChecksum = <expr>;' before 'OP_<name>'.
    Those expressions are plain arithmetic (or use sizeof, which is skipped). They must equal our fixed size."""
    sized = 0
    mismatched = 0
    for proto, _ in PROTOCOLS:
        path = os.path.join(guest_dir, ENC_FILES[proto])
        if not os.path.exists(path):
            print("check: %s not found" % path)
            mismatched += 1
            continue
        text = open(path, encoding="utf-8").read()
        for m in re.finditer(r"sizeWithoutChecksum = ([^;]+);", text):
            expr = m.group(1).strip()
            nxt = re.search(r"OP_(\w+)", text[m.end():m.end() + 900])
            if not nxt or "sizeof" in expr or not re.fullmatch(r"[0-9+*()\s]+", expr):
                continue
            name = nxt.group(1)
            if (proto, name) not in FIXED or SKIP_REASON.get((proto, name)):
                continue  # DMA calls are not advertised, so the guest never sends them
            want = eval(expr)
            got = FIXED[(proto, name)]
            sized += 1
            if want != got:
                mismatched += 1
                print("check: %s %s request size %d, the guest encoder says %d (%s)" % (proto, name, got, want, expr))
    return sized, mismatched


def main(argv):
    args = argv[1:]
    check = None
    if "--check-guest" in args:
        i = args.index("--check-guest")
        check = args[i + 1]
        del args[i:i + 2]
    if len(args) != 2:
        raise SystemExit(__doc__)
    gen_all(args[0], args[1], check)


if __name__ == "__main__":
    main(sys.argv)
