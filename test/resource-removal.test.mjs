import assert from 'node:assert/strict';
import { Texture } from '../src/core/Texture.js';
import { RenderTarget } from '../src/core/RenderTarget.js';
import { Renderer } from '../src/core/Renderer.js';
import { Program } from '../src/core/Program.js';

/*
 * Recording WebGL stand-in. Only the GL is faked: Texture, RenderTarget,
 * Renderer and Program run their real entry points, including remove().
 * Every created handle keeps its identity and a delete counter so ownership
 * and double-release behaviour can be verified.
 */

const CONSTANTS = {
    TEXTURE_2D: 3553,
    TEXTURE_3D: 32879,
    TEXTURE_2D_ARRAY: 35866,
    TEXTURE_CUBE_MAP: 34067,
    TEXTURE_CUBE_MAP_POSITIVE_X: 34069,
    TEXTURE0: 33984,
    UNSIGNED_BYTE: 5121,
    UNSIGNED_INT: 5125,
    UNSIGNED_INT_24_8: 36269,
    RGBA: 6408,
    DEPTH_COMPONENT: 6402,
    DEPTH_COMPONENT16: 33189,
    DEPTH_STENCIL: 34041,
    DEPTH24_STENCIL8: 35056,
    COLOR_ATTACHMENT0: 36064,
    DEPTH_ATTACHMENT: 36096,
    STENCIL_ATTACHMENT: 36128,
    DEPTH_STENCIL_ATTACHMENT: 33306,
    FRAMEBUFFER: 36160,
    RENDERBUFFER: 36161,
    STENCIL_INDEX8: 36168,
    CLAMP_TO_EDGE: 33071,
    REPEAT: 10497,
    LINEAR: 9729,
    NEAREST: 9728,
    NEAREST_MIPMAP_LINEAR: 9986,
    VERTEX_SHADER: 35633,
    FRAGMENT_SHADER: 35632,
    LINK_STATUS: 35714,
    ACTIVE_UNIFORMS: 35718,
    ACTIVE_ATTRIBUTES: 35721,
    MAX_COMBINED_TEXTURE_IMAGE_UNITS: 35661,
    ONE: 1,
    ZERO: 0,
    FUNC_ADD: 32774,
    LEQUAL: 515,
    CCW: 2305,
    CW: 2304,
    BACK: 1029,
    DEPTH_TEST: 2929,
    CULL_FACE: 2884,
    BLEND: 3042,
    STENCIL_TEST: 2960,
    COLOR_BUFFER_BIT: 16384,
    DEPTH_BUFFER_BIT: 256,
    STENCIL_BUFFER_BIT: 1024,
    UNPACK_FLIP_Y_WEBGL: 37440,
    UNPACK_PREMULTIPLY_ALPHA_WEBGL: 37441,
    UNPACK_ALIGNMENT: 3317,
    TEXTURE_MIN_FILTER: 10241,
    TEXTURE_MAG_FILTER: 10240,
    TEXTURE_WRAP_S: 10242,
    TEXTURE_WRAP_T: 10243,
    TEXTURE_WRAP_R: 32882,
};

const allRegistry = [];

function createHarness() {
    const calls = [];
    const counts = {};
    const registry = [];
    allRegistry.push(registry);

    const record = (name, args = []) => {
        calls.push({ name, args: args.slice() });
        counts[name] = (counts[name] || 0) + 1;
    };

    let handleId = 0;
    const createHandle = (kind) => {
        const handle = { kind, id: ++handleId, deleted: false, deleteCount: 0 };
        registry.push(handle);
        record(`create${kind}`);
        return handle;
    };

    const deleteHandle = (name, kind, handle) => {
        assert.ok(handle && handle.kind === kind, `${name} received a ${handle && handle.kind} handle`);
        handle.deleted = true;
        handle.deleteCount++;
        record(name, [handle]);
    };

    const gl = {
        canvas: { width: 8, height: 8 },
        ...CONSTANTS,

        createTexture: () => createHandle('Texture'),
        createFramebuffer: () => createHandle('Framebuffer'),
        createRenderbuffer: () => createHandle('Renderbuffer'),
        createProgram: () => createHandle('Program'),
        createShader: () => createHandle('Shader'),

        deleteTexture: (h) => deleteHandle('deleteTexture', 'Texture', h),
        deleteFramebuffer: (h) => deleteHandle('deleteFramebuffer', 'Framebuffer', h),
        deleteRenderbuffer: (h) => deleteHandle('deleteRenderbuffer', 'Renderbuffer', h),
        deleteProgram: (h) => deleteHandle('deleteProgram', 'Program', h),

        getParameter: (p) => (p === CONSTANTS.MAX_COMBINED_TEXTURE_IMAGE_UNITS ? 16 : 0),
        getExtension: () => null,
        getShaderInfoLog: () => '',
        getProgramParameter: (program, p) => {
            if (p === CONSTANTS.LINK_STATUS) return true;
            return 0; // ACTIVE_UNIFORMS / ACTIVE_ATTRIBUTES
        },
        getUniformLocation: () => null,
    };

    for (const name of [
        'bindTexture',
        'texParameteri',
        'texParameterf',
        'pixelStorei',
        'texImage2D',
        'texImage3D',
        'compressedTexImage2D',
        'generateMipmap',
        'activeTexture',
        'framebufferTexture2D',
        'bindRenderbuffer',
        'renderbufferStorage',
        'framebufferRenderbuffer',
        'bindFramebuffer',
        'viewport',
        'enable',
        'disable',
        'cullFace',
        'frontFace',
        'depthMask',
        'depthFunc',
        'blendFunc',
        'blendEquation',
        'stencilFunc',
        'stencilMask',
        'stencilOp',
        'clear',
        'scissor',
        'useProgram',
        'attachShader',
        'shaderSource',
        'compileShader',
        'linkProgram',
        'uniform1i',
        'uniform1iv',
    ]) {
        gl[name] = (...args) => record(name, args);
    }

    // Renderer constructor needs a DOM canvas; exercise the real prototype on a
    // hand-initialised instance instead, exactly like removal-baseline.mjs
    const renderer = Object.create(Renderer.prototype);
    renderer.gl = gl;
    gl.renderer = renderer;
    renderer.isWebgl2 = true;
    renderer.dpr = 1;
    renderer.width = 8;
    renderer.height = 8;
    renderer.autoClear = true;
    renderer.color = true;
    renderer.depth = true;
    renderer.stencil = false;
    renderer.extensions = {};
    renderer.parameters = { maxTextureUnits: 16, maxAnisotropy: 0 };
    renderer.state = {
        blendFunc: { src: gl.ONE, dst: gl.ZERO },
        blendEquation: { modeRGB: gl.FUNC_ADD },
        cullFace: false,
        frontFace: gl.CCW,
        depthMask: true,
        depthFunc: gl.LEQUAL,
        premultiplyAlpha: false,
        flipY: false,
        unpackAlignment: 4,
        framebuffer: null,
        viewport: { x: 0, y: 0, width: null, height: null },
        textureUnits: [],
        activeTextureUnit: 0,
        boundBuffer: null,
        uniformLocations: new Map(),
        currentProgram: null,
    };
    renderer.drawBuffers = () => {};

    return {
        gl,
        renderer,
        registry,
        count: (name) => counts[name] || 0,
        calls: () => calls.slice(),
        handles: (kind) => registry.filter((h) => h.kind === kind),
        lastCall: (name) => calls.filter((c) => c.name === name).pop(),
    };
}

const expectThrow = (fn, match) => {
    let error;
    assert.throws(() => {
        try {
            fn();
        } catch (e) {
            error = e;
            throw e;
        }
    }, match);
    return error;
};

let passed = 0;
let failed = 0;
function test(name, fn) {
    try {
        fn();
        passed++;
        console.log(`  ok - ${name}`);
    } catch (e) {
        failed++;
        console.error(`  FAIL - ${name}\n        ${e.stack.split('\n').slice(1, 6).join('\n        ')}`);
    }
}

/* ------------------------------------------------------------------ */
/* Texture                                                             */
/* ------------------------------------------------------------------ */

test('same texture cached on two units; remove invalidates every unit but leaves neighbours alone', () => {
    const h = createHarness();
    const { gl, renderer } = h;

    const tex = new Texture(gl, { width: 4, height: 4, generateMipmaps: false, flipY: false });
    let onUpdateCount = 0;
    tex.onUpdate = () => onUpdateCount++;
    tex.update(1);
    tex.update(2);

    const neighbour = new Texture(gl, { width: 2, height: 2, generateMipmaps: false, flipY: false });
    neighbour.update(0);

    assert.equal(renderer.state.textureUnits[1], tex.id);
    assert.equal(renderer.state.textureUnits[2], tex.id);
    assert.equal(renderer.state.textureUnits[0], neighbour.id);

    const handle = tex.texture;
    const snapshot = ['activeTexture', 'bindTexture', 'texImage2D', 'texImage3D', 'generateMipmap'].map((n) => h.count(n));
    const activeBefore = renderer.state.activeTextureUnit;

    tex.remove();

    // Exact handle identity, exactly one GL delete
    assert.equal(h.count('deleteTexture'), 1);
    assert.equal(h.lastCall('deleteTexture').args[0], handle);
    assert.equal(handle.deleteCount, 1);
    assert.equal(handle.deleted, true);

    // All units referencing this texture are invalidated, neighbour cache untouched
    assert.equal(renderer.state.textureUnits[1], null);
    assert.equal(renderer.state.textureUnits[2], null);
    assert.equal(renderer.state.textureUnits[0], neighbour.id);

    // No unit switch, upload, mip generation or onUpdate during release
    assert.equal(renderer.state.activeTextureUnit, activeBefore);
    ['activeTexture', 'bindTexture', 'texImage2D', 'texImage3D', 'generateMipmap'].forEach((n, i) =>
        assert.equal(h.count(n), snapshot[i], `${n} must not be called by remove`),
    );
    assert.equal(onUpdateCount, 0);

    // Neighbour still works, and a fresh texture can take over the freed unit
    neighbour.update(0);
    const replacement = new Texture(gl, { width: 1, height: 1, generateMipmaps: false, flipY: false });
    replacement.update(2);
    assert.equal(renderer.state.textureUnits[2], replacement.id);
    assert.equal(replacement.texture.deleted, false);
});

test('Texture.remove is idempotent and never recreates the handle', () => {
    const h = createHarness();
    const { gl } = h;
    const tex = new Texture(gl, { width: 4, height: 4, generateMipmaps: false });
    const handle = tex.texture;

    tex.remove();
    tex.remove();
    tex.remove();
    assert.equal(handle.deleteCount, 1);
    assert.equal(tex.removed, true);

    // Reuse after removal always errors, even with a changed image: no cache
    // branch escape, no new GL texture
    const createdBefore = h.count('createTexture');
    const textureError = expectThrow(() => tex.bind(), /Texture has been removed/);
    expectThrow(() => tex.update(0), /Texture has been removed/);
    tex.image = { width: 8, height: 8 };
    expectThrow(() => tex.update(0), /Texture has been removed/);
    assert.equal(h.count('createTexture'), createdBefore);
    assert.equal(tex.texture, handle);
    assert.equal(handle.deleteCount, 1);

    // Same error wording must surface through Program.use samplers
    const program = new Program(gl, { vertex: 'a', fragment: 'b', uniforms: { t: { value: tex } } });
    program.uniformLocations.set({ name: 't', uniformName: 't', nameComponents: [], type: 35678}, {});
    assert.equal(expectThrow(() => program.use()).message, textureError.message);

    program.uniforms.t.value = [tex, tex];
    assert.equal(expectThrow(() => program.use()).message, textureError.message);
});

test('mipmapped texture removal triggers no extra uploads or mip generation', () => {
    const h = createHarness();
    const { gl } = h;
    const tex = new Texture(gl, {
        image: new Uint8Array(16),
        width: 2,
        height: 2,
        generateMipmaps: true,
        flipY: false,
    });
    tex.update();
    assert.ok(h.count('generateMipmap') >= 1);
    const mips = h.count('generateMipmap');
    const uploads = h.count('texImage2D');
    tex.remove();
    assert.equal(h.count('generateMipmap'), mips);
    assert.equal(h.count('texImage2D'), uploads);
    assert.equal(tex.texture.deleteCount, 1);
});

/* ------------------------------------------------------------------ */
/* RenderTarget: depth storage variants                                */
/* ------------------------------------------------------------------ */

test('depth-only target releases framebuffer, color texture and depth renderbuffer', () => {
    const h = createHarness();
    const target = new RenderTarget(h.gl, { width: 4, height: 4, depth: true, stencil: false });
    assert.ok(target.depthBuffer);
    assert.equal(target.stencilBuffer, undefined);
    assert.equal(target.depthStencilBuffer, undefined);
    const { buffer, depthBuffer, texture } = target;

    target.remove();
    assert.equal(buffer.deleteCount, 1);
    assert.equal(depthBuffer.deleteCount, 1);
    assert.equal(texture.texture.deleteCount, 1);
    assert.equal(h.count('deleteRenderbuffer'), 1);
});

test('stencil-only target releases the stencil renderbuffer', () => {
    const h = createHarness();
    const target = new RenderTarget(h.gl, { width: 4, height: 4, depth: false, stencil: true });
    assert.ok(target.stencilBuffer);
    assert.equal(target.depthBuffer, undefined);
    target.remove();
    assert.equal(target.stencilBuffer.deleteCount, 1);
    assert.equal(h.count('deleteRenderbuffer'), 1);
});

test('combined depth-stencil target releases one merged renderbuffer', () => {
    const h = createHarness();
    const target = new RenderTarget(h.gl, { width: 8, height: 4, color: 2, depth: true, stencil: true });
    assert.ok(target.depthStencilBuffer);
    assert.equal(target.depthBuffer, undefined);
    assert.equal(target.stencilBuffer, undefined);
    target.remove();
    assert.equal(target.depthStencilBuffer.deleteCount, 1);
    assert.equal(h.count('deleteRenderbuffer'), 1);
    assert.equal(h.count('deleteFramebuffer'), 1);
});

test('depth texture storage (depth-only and depth-stencil) is released as a texture', () => {
    for (const stencil of [false, true]) {
        const h = createHarness();
        const target = new RenderTarget(h.gl, { width: 4, height: 4, depth: true, stencil, depthTexture: true });
        assert.ok(target.depthTexture);
        assert.equal(target.depthBuffer, undefined);
        assert.equal(target.stencilBuffer, undefined);
        assert.equal(target.depthStencilBuffer, undefined);
        const depthHandle = target.depthTexture.texture;

        target.remove();
        assert.equal(depthHandle.deleteCount, 1);
        assert.equal(h.count('deleteRenderbuffer'), 0);
        assert.equal(h.count('deleteTexture'), 1 + target.textures.length);
    }
});

test('target without depth or stencil releases no renderbuffers', () => {
    const h = createHarness();
    const target = new RenderTarget(h.gl, { width: 4, height: 4, depth: false, stencil: false });
    target.remove();
    assert.equal(h.count('deleteRenderbuffer'), 0);
    assert.equal(h.count('deleteFramebuffer'), 1);
    assert.equal(target.texture.texture.deleteCount, 1);
});

/* ------------------------------------------------------------------ */
/* RenderTarget: ownership, aliasing and binding cache                 */
/* ------------------------------------------------------------------ */

test('attachment removed first is not deleted again by target.remove; texture alias releases once', () => {
    const h = createHarness();
    const target = new RenderTarget(h.gl, { width: 4, height: 4, color: 2, depth: true, stencil: true });
    assert.equal(target.texture, target.textures[0]);

    const first = target.textures[0].texture;
    const second = target.textures[1].texture;
    const depthStencil = target.depthStencilBuffer;
    const framebuffer = target.buffer;

    target.textures[1].remove();
    assert.equal(second.deleteCount, 1);

    target.remove();
    assert.equal(second.deleteCount, 1, 'pre-removed attachment must not be deleted twice');
    assert.equal(first.deleteCount, 1, 'alias texture must be released exactly once');
    assert.equal(depthStencil.deleteCount, 1);
    assert.equal(framebuffer.deleteCount, 1);
    assert.equal(h.count('deleteTexture'), 2);
    assert.equal(h.count('deleteFramebuffer'), 1);
    assert.equal(h.count('deleteRenderbuffer'), 1);

    // Repeated whole-target release changes nothing
    target.remove();
    assert.equal(framebuffer.deleteCount, 1);
    assert.equal(first.deleteCount, 1);
    assert.equal(second.deleteCount, 1);
});

test('removing the currently bound target falls back to the default framebuffer and syncs cache', () => {
    const h = createHarness();
    const { renderer, gl } = h;
    const target = new RenderTarget(gl, { width: 4, height: 4 });
    renderer.bindFramebuffer(target);
    assert.equal(renderer.state.framebuffer, target.buffer);

    target.remove();
    assert.equal(renderer.state.framebuffer, null);
    assert.deepEqual(h.lastCall('bindFramebuffer').args, [gl.FRAMEBUFFER, null]);
    assert.equal(target.buffer.deleteCount, 1);
});

test('A/B targets interleave: removing an unused target never disturbs the target still drawing', () => {
    const h = createHarness();
    const { renderer, gl } = h;
    const a = new RenderTarget(gl, { width: 4, height: 4, depth: true });
    const b = new RenderTarget(gl, { width: 4, height: 4, depth: true });

    renderer.bindFramebuffer(b);
    const bindsBefore = h.count('bindFramebuffer');
    a.remove();

    // No framebuffer rebind while B stays current; B and its attachments survive
    assert.equal(h.count('bindFramebuffer'), bindsBefore);
    assert.equal(renderer.state.framebuffer, b.buffer);
    assert.equal(b.buffer.deleted, false);
    b.textures.forEach((t) => assert.equal(t.texture.deleted, false));
    assert.equal(b.depthBuffer.deleted, false);

    // B can still be rendered to
    const scene = { updateMatrixWorld() {}, traverse() {} };
    renderer.render({ scene, target: b });
    assert.equal(h.lastCall('bindFramebuffer').args[1], b.buffer);
    assert.deepEqual(h.lastCall('viewport').args, [0, 0, 4, 4]);

    // Now removing B restores the default framebuffer
    b.remove();
    assert.equal(renderer.state.framebuffer, null);
    assert.equal(a.buffer.deleteCount, 1);
    assert.equal(b.buffer.deleteCount, 1);
});

test('removed target keeps diagnostic references but is rejected by setSize and renderer.render', () => {
    const h = createHarness();
    const { renderer, gl } = h;
    const target = new RenderTarget(gl, { width: 6, height: 3, color: 2, depth: true });
    const buffer = target.buffer;
    const attachments = target.textures.slice();

    target.remove();

    // Fields stay for debugging, but buffer must not be nulled (that would look like the canvas)
    assert.equal(target.buffer, buffer);
    assert.equal(target.width, 6);
    assert.equal(target.height, 3);
    assert.deepEqual(target.textures, attachments);
    assert.equal(target.removed, true);

    // setSize refuses even when dimensions are unchanged
    expectThrow(() => target.setSize(6, 3), /RenderTarget has been removed/);
    expectThrow(() => target.setSize(8, 8), /RenderTarget has been removed/);

    // render refuses before binding, viewport work or any scene access
    const guardedScene = {
        updateMatrixWorld() {
            throw new Error('scene was touched');
        },
        traverse() {
            throw new Error('scene was touched');
        },
    };
    const binds = h.count('bindFramebuffer');
    const viewports = h.count('viewport');
    expectThrow(() => renderer.render({ scene: guardedScene, target }), /RenderTarget has been removed/);
    assert.equal(h.count('bindFramebuffer'), binds);
    assert.equal(h.count('viewport'), viewports);
});

/* ------------------------------------------------------------------ */
/* Lifecycles that must keep working: create, resize, lazy upload      */
/* ------------------------------------------------------------------ */

test('normal target creation, resize and same-size no-op still work', () => {
    const h = createHarness();
    const target = new RenderTarget(h.gl, { width: 8, height: 8, color: 1, depth: true });
    assert.equal(target.width, 8);
    assert.equal(target.textures[0].width, 8);

    const uploads = h.count('texImage2D');
    target.setSize(4, 2);
    assert.equal(target.width, 4);
    assert.equal(target.height, 2);
    assert.equal(target.textures[0].width, 4);
    assert.ok(h.count('texImage2D') > uploads);
    assert.deepEqual(h.lastCall('renderbufferStorage').args, [h.gl.RENDERBUFFER, h.gl.DEPTH_COMPONENT16, 4, 2]);

    const callsBefore = h.calls().length;
    target.setSize(4, 2);
    assert.equal(h.calls().length, callsBefore, 'same-size resize must be a no-op');

    target.remove();
    assert.equal(target.buffer.deleteCount, 1);
});

test('standalone texture lazy upload and repeated-bind optimisation are unchanged', () => {
    const h = createHarness();
    const { renderer, gl } = h;
    const tex = new Texture(gl, { width: 2, height: 2, generateMipmaps: false, flipY: false });

    tex.update(0);
    assert.ok(h.count('texImage2D') >= 1);
    const uploads = h.count('texImage2D');

    // Unchanged image on the same unit: cache branch, no re-upload
    tex.update(0);
    assert.equal(h.count('texImage2D'), uploads);

    // bind() repeats on the active unit must not hit GL
    renderer.activeTexture(0);
    tex.bind();
    const binds = h.count('bindTexture');
    tex.bind();
    assert.equal(h.count('bindTexture'), binds);

    // Switching units rebinds once
    renderer.activeTexture(3);
    tex.bind();
    assert.equal(h.count('bindTexture'), binds + 1);
    assert.equal(renderer.state.textureUnits[3], tex.id);
});

test('Program.use drives a live sampler and Program.remove releases its program', () => {
    const h = createHarness();
    const { gl } = h;
    const tex = new Texture(gl, { width: 2, height: 2, generateMipmaps: false, flipY: false });
    const program = new Program(gl, { vertex: 'a', fragment: 'b', uniforms: { t: { value: tex } } });
    program.uniformLocations.set({ name: 't', uniformName: 't', nameComponents: [], type: 35678}, {});

    program.use();
    assert.ok(h.count('uniform1i') >= 1);
    assert.equal(tex.texture.deleted, false);

    program.remove();
    assert.equal(program.program.deleteCount, 1);
});

/* ------------------------------------------------------------------ */
/* Summary                                                             */
/* ------------------------------------------------------------------ */

const kinds = ['Texture', 'Framebuffer', 'Renderbuffer', 'Program', 'Shader'];
console.log(`\n${passed} passed, ${failed} failed`);
for (const kind of kinds) {
    const created = allRegistry.flat().filter((handle) => handle.kind === kind);
    const deleted = created.filter((handle) => handle.deleted);
    console.log(`${kind.padEnd(13)} created=${String(created.length).padStart(2)}  deleted=${String(deleted.length).padStart(2)}  alive=${created.length - deleted.length}`);
}

// Resources owned by the new remove() paths must never be released twice
for (const kind of ['Texture', 'Framebuffer', 'Renderbuffer']) {
    const doubleDeleted = allRegistry.flat().filter((handle) => handle.kind === kind && handle.deleteCount > 1);
    assert.equal(doubleDeleted.length, 0, `${kind} handles must never be deleted twice`);
}

if (failed) process.exit(1);
