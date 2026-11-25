// The WebGL side of a snapshot (docs/snapshot.md, "GL"). WebGL objects are JavaScript objects, outside the wasm memory
// that a snapshot images, so the WebGL backend (engine/gl_webgl.c) turns them into a blob in engine memory before the
// image is taken (xj_gl_snap_save), and a restored run makes them again from it (xj_gl_snap_load), under the same
// emscripten GL names: the guest's names are those names, and the backend's C state (in the image) refers to them.
//
// WebGL2 cannot report a texture level's format and size, nor a shader's source once it is deleted, so
// xj_gl_snap_install wraps the context's upload, source and link calls to note them on the objects. The pixels of a
// texture are read back through a framebuffer (color-renderable formats); a buffer's bytes with getBufferSubData.
// Depth and stencil contents, queries, syncs and transform feedback are not kept.
//
// The blob: u32 JSON length, the JSON (UTF-8), then the binary parts the JSON points into ([offset, length]).
addToLibrary({
  $xjSnap: {
    // Wraps the calls whose arguments WebGL2 cannot report later. Idempotent per context.
    install: function (gl) {
      if (gl.__xjSnap) return;
      gl.__xjSnap = true;
      var face = function (target) {
        return target >= 0x8515 && target <= 0x851a ? target - 0x8515 : -1; // TEXTURE_CUBE_MAP_POSITIVE_X..NEGATIVE_Z
      };
      var bound = function (target) {
        var f = face(target);
        if (target === 0x0de1) return gl.getParameter(0x8069); // TEXTURE_BINDING_2D
        if (f >= 0 || target === 0x8513) return gl.getParameter(0x8514); // TEXTURE_BINDING_CUBE_MAP
        return null;
      };
      var note = function (target, level, info) {
        var t = bound(target);
        if (!t) return;
        if (!t.__lv) t.__lv = {};
        t.__cube = target !== 0x0de1;
        t.__lv[(face(target) < 0 ? 0 : face(target)) * 64 + level] = info;
      };
      var w = function (name, fn) {
        var orig = gl[name];
        gl[name] = function () {
          fn.apply(null, arguments);
          return orig.apply(gl, arguments);
        };
      };
      // The unsized one- and two-channel formats (ALPHA, LUMINANCE, LUMINANCE_ALPHA) cannot be attached to a framebuffer,
      // so their pixels cannot be read back: their uploads are kept in a copy (info.sh) instead.
      var shBpp = { 0x1906: 1, 0x1909: 1, 0x190a: 2 };
      var copyRows = function (dst, dw, x, y, width, height, bpp, src) {
        var align = gl.getParameter(0x0cf5); // UNPACK_ALIGNMENT
        var row = width * bpp, stride = Math.ceil(row / align) * align;
        var u8 = new Uint8Array(src.buffer, src.byteOffset, src.byteLength);
        for (var r = 0; r < height && (r * stride + row) <= u8.length; r++)
          dst.set(u8.subarray(r * stride, r * stride + row), ((y + r) * dw + x) * bpp);
      };
      // An upload from a pixel unpack buffer (the guest's glTexSubImage2DOffsetAEMU): its bytes, read from the buffer.
      var fromPbo = function (off, width, height, bpp) {
        if (typeof off !== 'number' || !gl.getParameter(0x88ef)) return null; // PIXEL_UNPACK_BUFFER_BINDING
        var align = gl.getParameter(0x0cf5);
        var row = width * bpp, stride = Math.ceil(row / align) * align;
        var tmp = new Uint8Array(stride * (height - 1) + row);
        try {
          gl.getBufferSubData(0x88ec, off, tmp);
        } catch (e) {
          return null;
        }
        return tmp;
      };
      var levelInfo = function (target, level) {
        var t = bound(target);
        return t && t.__lv ? t.__lv[(face(target) < 0 ? 0 : face(target)) * 64 + level] : null;
      };
      w('texImage2D', function (target, level, ifmt, width, height, border, fmt, type, pixels) {
        if (typeof width !== 'number' || arguments.length < 8) return;
        var info = { i: ifmt, w: width, h: height, f: fmt, t: type };
        var bpp = shBpp[ifmt];
        if (bpp && type === 0x1401 && width > 0 && height > 0) {
          info.sh = new Uint8Array(width * height * bpp);
          var src = pixels && ArrayBuffer.isView(pixels) ? pixels : fromPbo(pixels, width, height, bpp);
          if (src) copyRows(info.sh, width, 0, 0, width, height, bpp, src);
        }
        note(target, level, info);
      });
      w('texSubImage2D', function (target, level, x, y, width, height, fmt, type, pixels) {
        if (typeof width !== 'number' || arguments.length < 9) return;
        var info = levelInfo(target, level);
        if (!info || !info.sh || width <= 0 || height <= 0) return;
        var bpp = shBpp[info.i];
        var src = pixels && ArrayBuffer.isView(pixels) ? pixels : fromPbo(pixels, width, height, bpp);
        if (src) copyRows(info.sh, info.w, x, y, Math.min(width, info.w - x), Math.min(height, info.h - y), bpp, src);
      });
      w('copyTexImage2D', function (target, level, ifmt, x, y, width, height) {
        note(target, level, { i: ifmt, w: width, h: height, f: ifmt, t: 0x1401 });
      });
      w('compressedTexImage2D', function (target, level, ifmt, width, height) {
        note(target, level, { i: ifmt, w: width, h: height, f: 0, t: 0, c: 1 });
      });
      w('texStorage2D', function (target, levels, ifmt, width, height) {
        var faces = target === 0x8513 ? [0x8515, 0x8516, 0x8517, 0x8518, 0x8519, 0x851a] : [target];
        var t = bound(target);
        if (t) t.__storage = { levels: levels, i: ifmt, w: width, h: height };
        for (var l = 0; l < levels; l++)
          for (var k = 0; k < faces.length; k++)
            note(faces[k], l, { i: ifmt, w: Math.max(1, width >> l), h: Math.max(1, height >> l), s: 1 });
      });
      w('generateMipmap', function (target) {
        var t = bound(target);
        if (t) t.__mip = true;
      });
      // WebGL fixes a buffer's kind at its first binding: an index buffer (ELEMENT_ARRAY_BUFFER) can never be bound to
      // another kind's target, nor the other way round. The kind is noted so a restore makes each buffer as its kind.
      w('bindBuffer', function (target, b) {
        if (b && b.__elem === undefined && target !== 0x8f36 && target !== 0x8f37) b.__elem = target === 0x8893;
      });
      w('shaderSource', function (shader, src) {
        shader.__src = src;
      });
      w('linkProgram', function (program) {
        var sh = gl.getAttachedShaders(program) || [];
        program.__link = sh.map(function (s) {
          return { type: gl.getShaderParameter(s, 0x8b4f), src: s.__src || '' }; // SHADER_TYPE
        });
      });
      w('transformFeedbackVaryings', function (program, varyings, mode) {
        program.__tfv = { v: Array.prototype.slice.call(varyings), m: mode };
      });
    },
  },

  xj_gl_snap_install__deps: ['$GL', '$xjSnap'],
  xj_gl_snap_install: function () {
    if (GL.currentContext) xjSnap.install(GL.currentContext.GLctx);
  },

  // Returns a malloc'd blob (its length at *lenp), or 0 when there is no context.
  xj_gl_snap_save__deps: ['$GL', 'malloc'],
  xj_gl_snap_save: function (lenp) {
    var gl = GL.currentContext ? GL.currentContext.GLctx : null;
    if (!gl) return 0;
    var parts = [], plen = 0;
    var bin = function (u8) {
      var at = plen;
      parts.push(u8);
      plen += u8.length;
      return [at, u8.length];
    };
    var idOf = new Map();
    var tables = ['textures', 'buffers', 'framebuffers', 'renderbuffers', 'programs', 'shaders', 'vaos', 'samplers'];
    tables.forEach(function (tn) {
      var t = GL[tn] || [];
      for (var i = 1; i < t.length; i++) if (t[i]) idOf.set(t[i], i);
    });
    var id = function (o) {
      return o ? idOf.get(o) || 0 : 0;
    };
    var out = { counter: GL.counter, handle: GL.currentContext ? GL.currentContext.handle : 0 };
    // The engine's bindings come back from its own state; these are put back so the snapshot run is undisturbed.
    var keepFbo = gl.getParameter(0x8ca6), keepRfbo = gl.getParameter(0x8caa), keepVao = gl.getParameter(0x85b5);
    var keepTex = gl.getParameter(0x8069), keepUnit = gl.getParameter(0x84e0), keepAbuf = gl.getParameter(0x8894);
    var keepRead = gl.getParameter(0x0c02), keepPack = gl.getParameter(0x88ed), keepPackAlign = gl.getParameter(0x0d05);
    gl.bindBuffer(0x88eb, null); // PIXEL_PACK_BUFFER
    gl.pixelStorei(0x0d05, 1);
    var fbo = gl.createFramebuffer();

    out.textures = [];
    (GL.textures || []).forEach(function (t, i) {
      if (!t || !i) return;
      var rec = { id: i, cube: !!t.__cube, mip: !!t.__mip, st: t.__storage || null, lv: [], p: {} };
      var target = t.__cube ? 0x8513 : 0x0de1;
      var lv = t.__lv || {};
      if (Object.keys(lv).length || t.__storage) {
        gl.activeTexture(0x84c0);
        gl.bindTexture(target, t);
        [0x2801, 0x2800, 0x2802, 0x2803, 0x8072, 0x813a, 0x813b, 0x813c, 0x813d, 0x884c, 0x884d].forEach(function (p) {
          var v = gl.getTexParameter(target, p);
          if (v !== null && v !== undefined) rec.p[p] = v;
        });
      }
      Object.keys(lv).forEach(function (k) {
        var info = lv[k], key = Number(k), f = key >> 6, level = key & 63;
        var L = { k: key, i: info.i, w: info.w, h: info.h, f: info.f, t: info.t, s: info.s ? 1 : 0 };
        if (info.sh) L.raw = bin(info.sh);
        if (!info.c && info.w > 0 && info.h > 0) {
          gl.bindFramebuffer(0x8d40, fbo);
          gl.framebufferTexture2D(0x8d40, 0x8ce0, t.__cube ? 0x8515 + f : 0x0de1, t, level);
          if (gl.checkFramebufferStatus(0x8d40) === 0x8cd5) {
            var px = new Uint8Array(info.w * info.h * 4);
            gl.readBuffer(0x8ce0);
            gl.readPixels(0, 0, info.w, info.h, 0x1908, 0x1401, px);
            if (!gl.getError()) L.px = bin(px);
          }
          gl.framebufferTexture2D(0x8d40, 0x8ce0, 0x0de1, null, 0);
        }
        rec.lv.push(L);
      });
      out.textures.push(rec);
    });

    out.buffers = [];
    (GL.buffers || []).forEach(function (b, i) {
      if (!b || !i) return;
      gl.bindBuffer(0x8f36, b); // COPY_READ_BUFFER
      var size = gl.getBufferParameter(0x8f36, 0x8764) || 0, usage = gl.getBufferParameter(0x8f36, 0x8765) || 0x88e4;
      var rec = { id: i, size: size, usage: usage, elem: b.__elem ? 1 : 0 };
      if (size > 0) {
        var data = new Uint8Array(size);
        gl.getBufferSubData(0x8f36, 0, data);
        rec.d = bin(data);
      }
      out.buffers.push(rec);
    });
    gl.bindBuffer(0x8f36, null);

    out.renderbuffers = [];
    (GL.renderbuffers || []).forEach(function (r, i) {
      if (!r || !i) return;
      gl.bindRenderbuffer(0x8d41, r);
      out.renderbuffers.push({
        id: i, f: gl.getRenderbufferParameter(0x8d41, 0x8d44), w: gl.getRenderbufferParameter(0x8d41, 0x8d42),
        h: gl.getRenderbufferParameter(0x8d41, 0x8d43), n: gl.getRenderbufferParameter(0x8d41, 0x8cab) || 0,
      });
    });

    out.shaders = [];
    (GL.shaders || []).forEach(function (s, i) {
      if (!s || !i) return;
      out.shaders.push({ id: i, type: gl.getShaderParameter(s, 0x8b4f), src: s.__src || null,
        ok: s.__src != null && !!gl.getShaderParameter(s, 0x8b81) });
    });

    out.programs = [];
    (GL.programs || []).forEach(function (p, i) {
      if (!p || !i) return;
      var rec = { id: i, link: p.__link || null, tfv: p.__tfv || null, ok: !!gl.getProgramParameter(p, 0x8b82),
        attached: (gl.getAttachedShaders(p) || []).map(id).filter(function (x) { return x; }),
        attrs: [], u: [], blocks: [], ids: p.uniformSizeAndIdsByName || null, idc: p.uniformIdCounter || 1 };
      if (rec.ok) {
        var na = gl.getProgramParameter(p, 0x8b89); // ACTIVE_ATTRIBUTES
        for (var a = 0; a < na; a++) {
          var ai = gl.getActiveAttrib(p, a);
          if (ai) rec.attrs.push([ai.name, gl.getAttribLocation(p, ai.name)]);
        }
        var nu = gl.getProgramParameter(p, 0x8b86); // ACTIVE_UNIFORMS
        for (var u = 0; u < nu; u++) {
          var ui = gl.getActiveUniform(p, u);
          if (!ui) continue;
          var base = ui.name.replace(/\[0\]$/, '');
          for (var e = 0; e < ui.size; e++) {
            var nm = ui.size > 1 ? base + '[' + e + ']' : ui.name;
            var loc = gl.getUniformLocation(p, nm);
            if (!loc) continue; // a member of a uniform block
            var v = gl.getUniform(p, loc);
            if (v === null || v === undefined) continue;
            rec.u.push([nm, ui.type, typeof v === 'object' ? Array.prototype.slice.call(v) : v]);
          }
        }
        var nb = gl.getProgramParameter(p, 0x8a36) || 0; // ACTIVE_UNIFORM_BLOCKS
        for (var bi = 0; bi < nb; bi++) rec.blocks.push(gl.getActiveUniformBlockParameter(p, bi, 0x8a3f)); // UNIFORM_BLOCK_BINDING
      }
      out.programs.push(rec);
    });

    var attach = function (target) {
      var r = {};
      [0x8ce0, 0x8ce1, 0x8ce2, 0x8ce3, 0x8d00, 0x8d20, 0x821a].forEach(function (a) {
        var type = gl.getFramebufferAttachmentParameter(target, a, 0x8cd0); // OBJECT_TYPE
        if (!type) return;
        var o = gl.getFramebufferAttachmentParameter(target, a, 0x8cd1); // OBJECT_NAME
        var rec = { type: type, id: id(o) };
        if (type === 0x1702) {
          rec.level = gl.getFramebufferAttachmentParameter(target, a, 0x8cd2);
          var cf = gl.getFramebufferAttachmentParameter(target, a, 0x8cd3);
          rec.face = cf || 0;
        }
        r[a] = rec;
      });
      return r;
    };
    out.framebuffers = [];
    (GL.framebuffers || []).forEach(function (f, i) {
      if (!f || !i) return;
      gl.bindFramebuffer(0x8d40, f);
      out.framebuffers.push({ id: i, a: attach(0x8d40) });
    });

    out.vaos = [];
    var nattr = gl.getParameter(0x8869); // MAX_VERTEX_ATTRIBS
    (GL.vaos || []).forEach(function (v, i) {
      if (!v || !i) return;
      gl.bindVertexArray(v);
      var rec = { id: i, e: id(gl.getParameter(0x8895)), a: [] }; // ELEMENT_ARRAY_BUFFER_BINDING
      for (var k = 0; k < nattr; k++) {
        var b = gl.getVertexAttrib(k, 0x889f); // VERTEX_ATTRIB_ARRAY_BUFFER_BINDING
        var en = gl.getVertexAttrib(k, 0x8622);
        if (!b && !en) continue;
        rec.a.push({ k: k, b: id(b), en: en, s: gl.getVertexAttrib(k, 0x8623), st: gl.getVertexAttrib(k, 0x8624),
          t: gl.getVertexAttrib(k, 0x8625), n: gl.getVertexAttrib(k, 0x886a), i: gl.getVertexAttrib(k, 0x88fd),
          d: gl.getVertexAttrib(k, 0x88fe), o: gl.getVertexAttribOffset(k, 0x8645) });
      }
      out.vaos.push(rec);
    });
    gl.bindVertexArray(keepVao);

    out.samplers = [];
    (GL.samplers || []).forEach(function (s, i) {
      if (!s || !i) return;
      var p = {};
      [0x2801, 0x2800, 0x2802, 0x2803, 0x8072, 0x813a, 0x813b, 0x884c, 0x884d].forEach(function (q) {
        p[q] = gl.getSamplerParameter(s, q);
      });
      out.samplers.push({ id: i, p: p });
    });

    gl.deleteFramebuffer(fbo);
    gl.bindFramebuffer(0x8ca9, keepFbo);
    gl.bindFramebuffer(0x8ca8, keepRfbo);
    gl.readBuffer(keepRead);
    gl.activeTexture(0x84c0);
    gl.bindTexture(0x0de1, null);
    gl.activeTexture(keepUnit);
    gl.bindTexture(0x0de1, keepTex);
    gl.bindBuffer(0x8892, keepAbuf);
    gl.bindBuffer(0x88eb, keepPack);
    gl.pixelStorei(0x0d05, keepPackAlign);

    var json = new TextEncoder().encode(JSON.stringify(out));
    var total = 4 + json.length + plen;
    var p = _malloc(total);
    if (!p) return 0;
    HEAPU8[p] = json.length & 255;
    HEAPU8[p + 1] = (json.length >> 8) & 255;
    HEAPU8[p + 2] = (json.length >> 16) & 255;
    HEAPU8[p + 3] = (json.length >>> 24) & 255;
    HEAPU8.set(json, p + 4);
    var at = p + 4 + json.length;
    parts.forEach(function (u8) {
      HEAPU8.set(u8, at);
      at += u8.length;
    });
    HEAP32[lenp >> 2] = total;
    // What was kept, by format (stderr: the page's log): levels with their pixels, and levels without (not readable).
    var byFmt = {};
    out.textures.forEach(function (t) {
      t.lv.forEach(function (L) {
        var k = '0x' + L.i.toString(16) + (L.s ? 's' : '') + (L.px ? '' : L.raw ? ' copy' : ' no pixels');
        byFmt[k] = (byFmt[k] || 0) + 1;
      });
    });
    err('xiaoji: gl(webgl): snapshot: ' + out.textures.length + ' textures, ' + out.buffers.length + ' buffers, ' +
      out.programs.length + ' programs, ' + out.shaders.length + ' shaders, ' + out.framebuffers.length + ' framebuffers, ' +
      out.renderbuffers.length + ' renderbuffers, ' + out.vaos.length + ' vertex arrays; levels by internal format: ' +
      JSON.stringify(byFmt));
    return p;
  },

  // Makes the WebGL2 context on the worker's canvas again, and every object of the blob under its old name. Returns
  // 1, or 0 (no canvas, or no context).
  xj_gl_snap_load__deps: ['$GL', '$xjSnap'],
  xj_gl_snap_load: function (blob, len) {
    var canvas = globalThis.xjCanvas;
    if (!canvas) return 0;
    var jl = HEAPU8[blob] | (HEAPU8[blob + 1] << 8) | (HEAPU8[blob + 2] << 16) | (HEAPU8[blob + 3] << 24);
    var s = JSON.parse(new TextDecoder().decode(HEAPU8.slice(blob + 4, blob + 4 + jl)));
    var binAt = blob + 4 + jl;
    var bytes = function (r) {
      return r ? HEAPU8.subarray(binAt + r[0], binAt + r[0] + r[1]) : null;
    };
    var handle = GL.createContext(canvas, {
      majorVersion: 2, minorVersion: 0, alpha: false, depth: false, stencil: false, antialias: false,
      premultipliedAlpha: false, preserveDrawingBuffer: false, failIfMajorPerformanceCaveat: false,
      enableExtensionsByDefault: true, explicitSwapControl: false,
    });
    if (!handle) return 0;
    GL.makeContextCurrent(handle);
    var gl = GL.currentContext.GLctx;
    xjSnap.install(gl);
    var firstErr = null;
    var chk = function (what) {
      var e = gl.getError();
      if (e && !firstErr) firstErr = what + ': 0x' + e.toString(16);
    };
    var put = function (table, i, o) {
      for (var k = table.length; k < i; k++) table[k] = null;
      table[i] = o;
      if (o && table !== GL.shaders) o.name = i;
      return o;
    };
    var obj = {};
    var named = function (tn) {
      return function (i) {
        return i ? GL[tn][i] || null : null;
      };
    };
    var tex = named('textures'), buf = named('buffers'), rb = named('renderbuffers');
    gl.pixelStorei(0x0cf5, 1); // UNPACK_ALIGNMENT
    gl.bindBuffer(0x88ec, null); // PIXEL_UNPACK_BUFFER

    (s.shaders || []).forEach(function (r) {
      var sh = put(GL.shaders, r.id, gl.createShader(r.type));
      if (r.src != null) {
        gl.shaderSource(sh, r.src);
        if (r.ok) gl.compileShader(sh);
      }
    });
    (s.programs || []).forEach(function (r) {
      var p = put(GL.programs, r.id, gl.createProgram());
      p.maxUniformLength = p.maxAttributeLength = p.maxUniformBlockNameLength = 0;
      p.uniformIdCounter = 1;
      p.uniformLocsById = 0;
      p.uniformSizeAndIdsByName = {};
      if (r.link && r.ok) {
        var tmp = r.link.map(function (l) {
          var sh = gl.createShader(l.type);
          gl.shaderSource(sh, l.src);
          gl.compileShader(sh);
          gl.attachShader(p, sh);
          return sh;
        });
        r.attrs.forEach(function (a) {
          if (a[1] >= 0) gl.bindAttribLocation(p, a[1], a[0]);
        });
        if (r.tfv) gl.transformFeedbackVaryings(p, r.tfv.v, r.tfv.m);
        gl.linkProgram(p);
        tmp.forEach(function (sh) {
          gl.detachShader(p, sh);
          gl.deleteShader(sh);
        });
        // emscripten's uniform ids as the snapshot's driver numbered them, whatever this one enumerates.
        if (r.ids && Object.keys(r.ids).length) {
          p.uniformSizeAndIdsByName = r.ids;
          p.uniformIdCounter = r.idc;
          p.uniformLocsById = {};
          p.uniformArrayNamesById = {};
          Object.keys(r.ids).forEach(function (n) {
            var sz = r.ids[n][0], base = r.ids[n][1];
            for (var j = 0; j < sz; j++) {
              p.uniformLocsById[base + j] = j;
              p.uniformArrayNamesById[base + j] = n;
            }
          });
        }
        gl.useProgram(p);
        r.u.forEach(function (u) {
          var loc = gl.getUniformLocation(p, u[0]);
          if (!loc) return;
          var t = u[1], v = u[2], a = Array.isArray(v) ? v : [v];
          switch (t) {
          case 0x1406: gl.uniform1fv(loc, a); break;
          case 0x8b50: gl.uniform2fv(loc, a); break;
          case 0x8b51: gl.uniform3fv(loc, a); break;
          case 0x8b52: gl.uniform4fv(loc, a); break;
          case 0x8b5a: gl.uniformMatrix2fv(loc, false, a); break;
          case 0x8b5b: gl.uniformMatrix3fv(loc, false, a); break;
          case 0x8b5c: gl.uniformMatrix4fv(loc, false, a); break;
          case 0x8b65: gl.uniformMatrix2x3fv(loc, false, a); break;
          case 0x8b66: gl.uniformMatrix2x4fv(loc, false, a); break;
          case 0x8b67: gl.uniformMatrix3x2fv(loc, false, a); break;
          case 0x8b68: gl.uniformMatrix3x4fv(loc, false, a); break;
          case 0x8b69: gl.uniformMatrix4x2fv(loc, false, a); break;
          case 0x8b6a: gl.uniformMatrix4x3fv(loc, false, a); break;
          case 0x1405: gl.uniform1uiv(loc, a); break;
          case 0x8dc6: gl.uniform2uiv(loc, a); break;
          case 0x8dc7: gl.uniform3uiv(loc, a); break;
          case 0x8dc8: gl.uniform4uiv(loc, a); break;
          case 0x8b57: case 0x8b53: gl.uniform2iv(loc, a.map(Number)); break;
          case 0x8b58: case 0x8b54: gl.uniform3iv(loc, a.map(Number)); break;
          case 0x8b59: case 0x8b55: gl.uniform4iv(loc, a.map(Number)); break;
          default: gl.uniform1iv(loc, a.map(Number)); break; // int, bool and the samplers
          }
        });
        r.blocks.forEach(function (binding, bi) {
          gl.uniformBlockBinding(p, bi, binding);
        });
        chk('program ' + r.id);
      }
      r.attached.forEach(function (sid) {
        if (GL.shaders[sid]) gl.attachShader(p, GL.shaders[sid]);
      });
    });
    gl.useProgram(null);

    (s.textures || []).forEach(function (r) {
      var t = put(GL.textures, r.id, gl.createTexture());
      var target = r.cube ? 0x8513 : 0x0de1;
      if (!r.lv.length && !r.st) return; // never bound to a target with an image: bound later by the guest
      gl.bindTexture(target, t);
      if (r.st) gl.texStorage2D(target, r.st.levels, r.st.i, r.st.w, r.st.h);
      r.lv.forEach(function (L) {
        var f = L.k >> 6, level = L.k & 63, tgt = r.cube ? 0x8515 + f : 0x0de1;
        var px = bytes(L.px);
        if (L.s) {
          // Immutable storage: the pixels go in with the format its internal format takes for bytes.
          var sf = { 0x8229: [0x1903, [0]], 0x822b: [0x8227, [0, 1]], 0x8051: [0x1907, [0, 1, 2]], 0x8d62: [0x1907, [0, 1, 2]],
            0x8058: [0x1908, null], 0x8c43: [0x1908, null], 0x8056: [0x1908, null], 0x8057: [0x1908, null] }[L.i];
          if (px && sf) {
            var up = px;
            if (sf[1]) {
              var m = L.w * L.h, chs = sf[1];
              up = new Uint8Array(m * chs.length);
              for (var q2 = 0; q2 < m; q2++) for (var c2 = 0; c2 < chs.length; c2++) up[q2 * chs.length + c2] = px[q2 * 4 + chs[c2]];
            }
            gl.texSubImage2D(tgt, level, 0, 0, L.w, L.h, sf[0], 0x1401, up);
          }
          return;
        }
        var rawc = bytes(L.raw);
        if (rawc) { // an unsized one- or two-channel format: the copy of its uploads
          gl.texImage2D(tgt, level, L.i, L.w, L.h, 0, L.f, L.t, rawc);
          return;
        }
        if (px && (L.f === 0x1908 || L.f === 0x80e1) && L.t === 0x1401) { // RGBA (and BGRA) bytes: as read
          gl.texImage2D(tgt, level, L.i, L.w, L.h, 0, 0x1908, 0x1401, px);
          return;
        }
        var data = null;
        if (px && L.t === 0x1401) { // other byte formats: the channels taken from the RGBA read
          var n = L.w * L.h, ch = { 0x1903: [0], 0x8227: [0, 1], 0x1907: [0, 1, 2], 0x1909: [0], 0x190a: [0, 3], 0x1906: [3] }[L.f];
          if (ch) {
            data = new Uint8Array(n * ch.length);
            for (var q = 0; q < n; q++) for (var c = 0; c < ch.length; c++) data[q * ch.length + c] = px[q * 4 + ch[c]];
          }
        }
        if (L.c) return; // compressed: not kept (the image is undefined after a restore)
        gl.texImage2D(tgt, level, L.i, L.w, L.h, 0, L.f, L.t, data);
      });
      chk('texture ' + r.id + ' levels');
      Object.keys(r.p).forEach(function (p) {
        gl.texParameterf(target, Number(p), r.p[p]);
      });
      chk('texture ' + r.id + ' parameters');
      t.__mip = r.mip;
    });
    gl.bindTexture(0x0de1, null);
    gl.bindTexture(0x8513, null);

    // Each buffer is made through a binding of its kind (index buffers under a scratch vertex array, so no other
    // vertex array's element binding changes).
    var scratchVao = gl.createVertexArray();
    gl.bindVertexArray(scratchVao);
    (s.buffers || []).forEach(function (r) {
      var b = put(GL.buffers, r.id, gl.createBuffer());
      var target = r.elem ? 0x8893 : 0x8892;
      gl.bindBuffer(target, b);
      var d = bytes(r.d);
      if (d) gl.bufferData(target, d, r.usage);
      else if (r.size) gl.bufferData(target, r.size, r.usage);
    });
    gl.bindBuffer(0x8893, null);
    gl.bindBuffer(0x8892, null);
    gl.bindVertexArray(null);
    gl.deleteVertexArray(scratchVao);
    chk('buffers');

    (s.renderbuffers || []).forEach(function (r) {
      var o = put(GL.renderbuffers, r.id, gl.createRenderbuffer());
      if (!r.w || !r.h) return;
      gl.bindRenderbuffer(0x8d41, o);
      if (r.n) gl.renderbufferStorageMultisample(0x8d41, r.n, r.f, r.w, r.h);
      else gl.renderbufferStorage(0x8d41, r.f, r.w, r.h);
    });
    gl.bindRenderbuffer(0x8d41, null);
    chk('renderbuffers');

    (s.framebuffers || []).forEach(function (r) {
      var f = put(GL.framebuffers, r.id, gl.createFramebuffer());
      gl.bindFramebuffer(0x8d40, f);
      Object.keys(r.a).forEach(function (k) {
        var a = r.a[k], at = Number(k);
        if (a.type === 0x1702 && tex(a.id)) {
          var tt = GL.textures[a.id];
          gl.framebufferTexture2D(0x8d40, at, a.face ? a.face : 0x0de1, tt, a.level || 0);
        } else if (a.type === 0x8d41 && rb(a.id)) {
          gl.framebufferRenderbuffer(0x8d40, at, 0x8d41, GL.renderbuffers[a.id]);
        }
      });
    });
    gl.bindFramebuffer(0x8d40, null);
    chk('framebuffers');

    (s.vaos || []).forEach(function (r) {
      var v = put(GL.vaos, r.id, gl.createVertexArray());
      gl.bindVertexArray(v);
      r.a.forEach(function (a) {
        gl.bindBuffer(0x8892, buf(a.b));
        chk('vertex array ' + r.id + ' attribute ' + a.k + ' buffer ' + a.b + ' bind');
        if (a.b) {
          if (a.i) gl.vertexAttribIPointer(a.k, a.s, a.t, a.st, a.o);
          else gl.vertexAttribPointer(a.k, a.s, a.t, a.n, a.st, a.o);
        }
        chk('vertex array ' + r.id + ' attribute ' + a.k + ' pointer ' + JSON.stringify(a));
        if (a.en) gl.enableVertexAttribArray(a.k);
        if (a.d) gl.vertexAttribDivisor(a.k, a.d);
      });
      gl.bindBuffer(0x8893, buf(r.e));
      chk('vertex array ' + r.id + ' element buffer ' + r.e);
    });
    gl.bindVertexArray(null);
    gl.bindBuffer(0x8892, null);
    chk('vertex arrays');

    (s.samplers || []).forEach(function (r) {
      var o = put(GL.samplers, r.id, gl.createSampler());
      Object.keys(r.p).forEach(function (p) {
        if (r.p[p] !== null) gl.samplerParameterf(o, Number(p), r.p[p]);
      });
    });

    GL.counter = Math.max(GL.counter, s.counter);
    err('xiaoji: gl(webgl): restore: ' + s.textures.length + ' textures, ' + s.buffers.length + ' buffers, ' +
      s.programs.length + ' programs; ' + (firstErr ? 'first GL error at ' + firstErr : 'no GL error') + '; ' + gl.getError());
    return 1;
  },
});
