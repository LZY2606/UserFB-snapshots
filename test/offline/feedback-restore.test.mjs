///////////////////////////////////////////////////////////////////////////////////
// The MIT License (MIT)
//
// Copyright (c) 2026 Tarek Sherif
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of
// this software and associated documentation files (the "Software"), to deal in
// the Software without restriction, including without limitation the rights to
// use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
// the Software, and to permit persons to whom the Software is furnished to do so,
// subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
// FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
// IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
///////////////////////////////////////////////////////////////////////////////////

// Offline (no browser) verification of TransformFeedback association
// retention across resource restoration. Run with Node 22+:
//     node test/offline/feedback-restore.test.mjs
//
// The offline backend records, per transform feedback handle, the
// index -> buffer handle attachments, plus the begin/draw/end event order.
// VertexBuffer, TransformFeedback, DrawCall and Program are the real library
// classes; only GL shader compilation is stubbed, and the feedback
// associations are stored by the TransformFeedback objects themselves.

import { GL } from "../../src/constants.js";
import { VertexBuffer } from "../../src/vertex-buffer.js";
import { TransformFeedback } from "../../src/transform-feedback.js";
import { DrawCall } from "../../src/draw-call.js";
import { Program } from "../../src/program.js";

let passes = 0;

function check(condition, message) {
    if (!condition) {
        throw new Error(`CHECK FAILED: ${message}`);
    }
    passes++;
    console.log(`  ok - ${message}`);
}

function checkThrows(fn, messageFragment, message) {
    let threw = false;
    try {
        fn();
    } catch (e) {
        threw = true;
        check(e.message.includes(messageFragment), `${message} (got: ${e.message})`);
    }
    check(threw, `${message} threw`);
}

function describeAttachments(attachmentMap) {
    let entries = [];
    for (let [index, handle] of attachmentMap) {
        entries.push(`${index}:#${handle ? handle.id : "null"}`);
    }
    return `{${entries.join(", ")}}`;
}

// Recording offline WebGL backend.
function createBackend(name, maxSeparateAttribs = 4) {
    let nextId = 0;
    let currentTF = null;
    let capturing = false;

    // transform feedback handle -> Map(index -> buffer handle)
    let attachments = new Map();
    let events = [];
    let captures = [];
    let counts = {
        bindTransformFeedback: 0,
        bindBufferBase: 0,
        bindBufferBaseAttach: 0,
        begin: 0,
        draw: 0,
        end: 0
    };

    let gl = {
        backendName: name,
        STATIC_DRAW: GL.STATIC_DRAW,
        DYNAMIC_DRAW: GL.DYNAMIC_DRAW,

        getParameter(param) {
            if (param === GL.MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS) {
                return maxSeparateAttribs;
            }
            return 0;
        },

        createBuffer() {
            return { id: ++nextId, kind: "buffer" };
        },
        deleteBuffer() {},
        bindBuffer() {},
        bufferData() {},
        bufferSubData() {},

        createTransformFeedback() {
            let handle = { id: ++nextId, kind: "transformFeedback" };
            attachments.set(handle, new Map());
            return handle;
        },
        deleteTransformFeedback(handle) {
            attachments.delete(handle);
            if (currentTF === handle) {
                currentTF = null;
            }
        },
        bindTransformFeedback(target, handle) {
            counts.bindTransformFeedback++;
            currentTF = handle;
        },
        bindBufferBase(target, index, handle) {
            counts.bindBufferBase++;
            if (handle) {
                counts.bindBufferBaseAttach++;
            }
            if (currentTF) {
                attachments.get(currentTF).set(index, handle);
            }
        },

        beginTransformFeedback(primitive) {
            counts.begin++;
            capturing = true;
            events.push({ name: "begin", tf: currentTF.id });
        },
        drawArraysInstanced() {
            counts.draw++;
            if (capturing) {
                events.push({ name: "draw", tf: currentTF.id });
                // Snapshot of the outputs this capture actually wrote to.
                captures.push(new Map(attachments.get(currentTF)));
            }
        },
        endTransformFeedback() {
            counts.end++;
            capturing = false;
            events.push({ name: "end", tf: currentTF.id });
        },

        // Minimal shader/program stubs.
        createShader() {
            return { id: ++nextId, kind: "shader" };
        },
        shaderSource() {},
        compileShader() {},
        getShaderParameter() {
            return true;
        },
        deleteShader() {},
        createProgram() {
            return { id: ++nextId, kind: "program" };
        },
        attachShader() {},
        transformFeedbackVaryings() {},
        bindAttribLocation() {},
        linkProgram() {},
        useProgram() {},
        getProgramParameter(program, param) {
            if (param === GL.LINK_STATUS) {
                return true;
            }
            if (param === GL.COMPLETION_STATUS_KHR) {
                return true;
            }
            return 0;
        }
    };

    return {
        name,
        gl,
        events,
        captures,
        counts,
        attachments,
        currentTF: () => currentTF,
        snapshotCounts: () => ({ ...counts })
    };
}

function createAppState() {
    return {
        program: null,
        vertexArray: null,
        transformFeedback: null
    };
}

// Real Program with stubbed shader compilation. Feedback relationships are
// not stored here; each TransformFeedback keeps its own.
function createProgram(gl, appState) {
    let program = new Program(
        gl,
        appState,
        "#version 300 es\nvoid main() {}",
        "#version 300 es\nvoid main() {}",
        ["output0"],
        null,
        GL.SEPARATE_ATTRIBS
    );
    return program.link().checkLinkage();
}

function createBuffer(gl, appState, values) {
    return new VertexBuffer(gl, appState, GL.FLOAT, values.length, new Float32Array(values));
}

function runDraw(program, appState, transformFeedback) {
    let drawCall = new DrawCall(program.gl, appState, program);
    drawCall
        .primitive(GL.POINTS)
        .transformFeedback(transformFeedback)
        .draw();
    return drawCall;
}

function checkEventOrder(backend, message) {
    let n = backend.events.length;
    check(n >= 3, `${message}: enough events recorded`);
    let last = backend.events.slice(-3).map((e) => e.name);
    check(last.join(",") === "begin,draw,end", `${message}: begin -> draw -> end order (${last.join(" -> ")})`);
}

console.log("== TransformFeedback restore association tests ==");

// ---------------------------------------------------------------------------
// 1. Sparse indices, buffer-restore-first ordering, no resend on unchanged draws
// ---------------------------------------------------------------------------
console.log("\n[1] sparse indices, buffer restore before feedback restore");
{
    let backend = createBackend("backend-1");
    let state = createAppState();
    let program = createProgram(backend.gl, state);

    let b0 = createBuffer(backend.gl, state, [1, 2]);
    let b2 = createBuffer(backend.gl, state, [3, 4]);
    let tf = new TransformFeedback(backend.gl, state);

    check(tf.feedbackBuffer(0, b0) === tf, "feedbackBuffer returns the TransformFeedback for chaining");
    tf.feedbackBuffer(2, b2);

    runDraw(program, state, tf);
    let round1 = backend.captures[0];
    console.log(`    round 1 captured: ${describeAttachments(round1)}`);
    check(round1.get(0) === b0.buffer, "round 1: index 0 captures current b0 handle");
    check(round1.get(2) === b2.buffer, "round 1: sparse index 2 captures current b2 handle");
    check(round1.get(1) === undefined, "round 1: unused index 1 is not compressed into");
    check([...round1.keys()].join(",") === "0,2", "sparse index positions are preserved");
    checkEventOrder(backend, "round 1");

    // Unchanged second draw: nothing may be re-sent.
    let before = backend.snapshotCounts();
    runDraw(program, state, tf);
    let unchanged = backend.counts.bindBufferBase - before.bindBufferBase;
    check(unchanged === 0, `unchanged draw issues no bindBufferBase (${unchanged})`);
    check(backend.counts.bindTransformFeedback - before.bindTransformFeedback === 0, "unchanged draw does not rebind the transform feedback");

    // Restore order: buffers first, feedback second.
    let old0 = b0.buffer;
    let old2 = b2.buffer;
    b0.restore(new Float32Array([5, 6]));
    b2.restore(new Float32Array([7, 8]));
    tf.restore();
    check(tf.transformFeedback !== null, "feedback restore allocates a new handle");
    check(b0.buffer !== old0 && b2.buffer !== old2, "buffer restore allocates new handles");

    before = backend.snapshotCounts();
    runDraw(program, state, tf);
    let resent = backend.counts.bindBufferBaseAttach - before.bindBufferBaseAttach;
    check(resent === 2, `rebuilt feedback re-attaches both outputs (${resent} buffer attaches)`);
    let round2 = backend.captures[2];
    console.log(`    round 2 captured: ${describeAttachments(round2)}`);
    check(round2.get(0) === b0.buffer, "round 2: index 0 uses the restored buffer handle, not the old one");
    check(round2.get(2) === b2.buffer, "round 2: index 2 uses the restored buffer handle, not the old one");
    check(![...round2.values()].includes(old0) && ![...round2.values()].includes(old2), "round 2: no stale registration handle is captured");
    checkEventOrder(backend, "round 2");

    before = backend.snapshotCounts();
    runDraw(program, state, tf);
    check(backend.counts.bindBufferBase - before.bindBufferBase === 0, "following unchanged draw again sends nothing");
}

// ---------------------------------------------------------------------------
// 2. Feedback-restore-first ordering
// ---------------------------------------------------------------------------
console.log("\n[2] feedback restore before buffer restore");
{
    let backend = createBackend("backend-2");
    let state = createAppState();
    let program = createProgram(backend.gl, state);

    let b0 = createBuffer(backend.gl, state, [1]);
    let b2 = createBuffer(backend.gl, state, [2]);
    let tf = new TransformFeedback(backend.gl, state)
        .feedbackBuffer(0, b0)
        .feedbackBuffer(2, b2);

    runDraw(program, state, tf);

    // Restore order: feedback first, buffers afterwards, then draw.
    tf.restore();
    let newB0 = b0.restore(new Float32Array([9])).buffer;
    let newB2 = b2.restore(new Float32Array([10])).buffer;

    runDraw(program, state, tf);
    let captured = backend.captures[1];
    console.log(`    post-restore captured: ${describeAttachments(captured)}`);
    check(captured.get(0) === newB0, "feedback-first order: index 0 resolves to the latest buffer handle");
    check(captured.get(2) === newB2, "feedback-first order: index 2 resolves to the latest buffer handle");
    checkEventOrder(backend, "feedback-first restore");
}

// ---------------------------------------------------------------------------
// 3. One output buffer shared by two transform feedbacks
// ---------------------------------------------------------------------------
console.log("\n[3] shared output buffer across two transform feedbacks");
{
    let backend = createBackend("backend-3");
    let state = createAppState();
    let program = createProgram(backend.gl, state);

    let shared = createBuffer(backend.gl, state, [1, 2, 3]);
    let tfA = new TransformFeedback(backend.gl, state).feedbackBuffer(1, shared);
    let tfB = new TransformFeedback(backend.gl, state).feedbackBuffer(0, shared);

    let dcA = new DrawCall(backend.gl, state, program).primitive(GL.POINTS).transformFeedback(tfA);
    let dcB = new DrawCall(backend.gl, state, program).primitive(GL.POINTS).transformFeedback(tfB);
    dcA.draw();
    dcB.draw();
    check(backend.captures[0].get(1) === shared.buffer, "tfA captures shared buffer at its own index 1");
    check(backend.captures[1].get(0) === shared.buffer, "tfB captures shared buffer at its own index 0");

    let oldHandle = shared.buffer;
    shared.restore(new Float32Array([4, 5, 6]));

    let before = backend.snapshotCounts();
    dcA.draw();
    dcB.draw();
    let resent = backend.counts.bindBufferBaseAttach - before.bindBufferBaseAttach;
    check(resent === 2, `each feedback re-attaches the rebuilt shared buffer once (${resent} buffer attaches)`);

    let capA = backend.captures[2];
    let capB = backend.captures[3];
    console.log(`    tfA captured: ${describeAttachments(capA)}`);
    console.log(`    tfB captured: ${describeAttachments(capB)}`);
    check(capA.get(1) === shared.buffer && capA.get(1) !== oldHandle, "tfA submits the new shared handle");
    check(capB.get(0) === shared.buffer && capB.get(0) !== oldHandle, "tfB submits the new shared handle");

    // Identities must not bleed between feedback objects.
    let other = createBuffer(backend.gl, state, [7]);
    tfA.feedbackBuffer(1, other);
    check(tfB.feedbackBuffers.get(0) === shared, "replacing an output on tfA does not touch tfB's association");
    check(tfA.feedbackBuffers.get(1) === other, "tfA holds the replacement");
}

// ---------------------------------------------------------------------------
// 4. Replacing a single sparse entry only re-binds that index
// ---------------------------------------------------------------------------
console.log("\n[4] single item replacement");
{
    let backend = createBackend("backend-4");
    let state = createAppState();
    let program = createProgram(backend.gl, state);

    let b0 = createBuffer(backend.gl, state, [1]);
    let b1 = createBuffer(backend.gl, state, [2]);
    let b2 = createBuffer(backend.gl, state, [3]);
    let tf = new TransformFeedback(backend.gl, state)
        .feedbackBuffer(0, b0)
        .feedbackBuffer(1, b1)
        .feedbackBuffer(2, b2);

    runDraw(program, state, tf);

    let replacement = createBuffer(backend.gl, state, [8]);
    let before = backend.snapshotCounts();
    tf.feedbackBuffer(1, replacement);
    check(tf.feedbackBuffers.get(1) === replacement, "replacement registered at the same index");

    runDraw(program, state, tf);
    let resent = backend.counts.bindBufferBaseAttach - before.bindBufferBaseAttach;
    check(resent === 1, `only the replaced index is re-sent (${resent} buffer attach)`);

    let captured = backend.captures[1];
    console.log(`    captured: ${describeAttachments(captured)}`);
    check(captured.get(1) === replacement.buffer, "replaced index captures the new buffer");
    check(captured.get(0) === b0.buffer && captured.get(2) === b2.buffer, "untouched indices keep their buffers and positions");
}

// ---------------------------------------------------------------------------
// 5. Validation and deleted-buffer failures: capture never starts,
//    legal associations survive and the next draw succeeds.
// ---------------------------------------------------------------------------
console.log("\n[5] failures do not start capture or drop legal associations");
{
    let backend = createBackend("backend-5");
    let otherBackend = createBackend("backend-5-other");
    let state = createAppState();
    let program = createProgram(backend.gl, state);

    let b0 = createBuffer(backend.gl, state, [1]);
    let b1 = createBuffer(backend.gl, state, [2]);
    let b2 = createBuffer(backend.gl, state, [3]);
    let tf = new TransformFeedback(backend.gl, state)
        .feedbackBuffer(0, b0)
        .feedbackBuffer(1, b1)
        .feedbackBuffer(2, b2);

    runDraw(program, state, tf);
    let beginBefore = backend.counts.begin;

    checkThrows(() => tf.feedbackBuffer(1.5, b0), "integer", "fractional index rejected");
    checkThrows(() => tf.feedbackBuffer(-1, b0), "integer", "negative index rejected");
    checkThrows(() => tf.feedbackBuffer(4, b0), "MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS", "index at the limit rejected");
    let foreignBuffer = createBuffer(otherBackend.gl, createAppState(), [9]);
    checkThrows(() => tf.feedbackBuffer(0, foreignBuffer), "same WebGL context", "buffer from another gl rejected");

    check(tf.feedbackBuffers.size === 3, "all legal associations retained after validation errors");
    check(state.transformFeedback === tf, "failed validation leaves appState untouched");
    runDraw(program, state, tf);
    let captured = backend.captures[1];
    check(captured.get(0) === b0.buffer && captured.get(1) === b1.buffer && captured.get(2) === b2.buffer,
        "draw after validation errors still captures all registered outputs");

    // Deleted buffer discovered at capture time.
    b1.delete();
    check(b1.buffer === null, "buffer delete releases its handle");
    checkThrows(() => runDraw(program, state, tf), "has been deleted", "draw with a deleted feedback buffer rejected");
    check(backend.counts.begin === beginBefore + 1, "beginTransformFeedback was not called for the failed capture");
    check(tf.feedbackBuffers.size === 3, "failed capture keeps the other legal associations");
    check(state.transformFeedback === tf, "failed capture leaves the feedback bound (capture itself never started)");

    // Business re-uploads, then the next draw works.
    b1.restore(new Float32Array([12]));
    runDraw(program, state, tf);
    let recovered = backend.captures[2];
    console.log(`    recovered capture: ${describeAttachments(recovered)}`);
    check(recovered.get(0) === b0.buffer && recovered.get(1) === b1.buffer && recovered.get(2) === b2.buffer,
        "draw succeeds once the deleted buffer is restored");
    checkEventOrder(backend, "recovered draw");
}

// ---------------------------------------------------------------------------
// 6. Two full restore rounds
// ---------------------------------------------------------------------------
console.log("\n[6] two restore rounds");
{
    let backend = createBackend("backend-6");
    let state = createAppState();
    let program = createProgram(backend.gl, state);

    let b0 = createBuffer(backend.gl, state, [1]);
    let b2 = createBuffer(backend.gl, state, [2]);
    let tf = new TransformFeedback(backend.gl, state)
        .feedbackBuffer(0, b0)
        .feedbackBuffer(2, b2);

    let handleHistory = [];
    runDraw(program, state, tf);
    handleHistory.push([...backend.captures[0].values()]);

    for (let round = 1; round <= 2; round++) {
        b0.restore(new Float32Array([round * 10]));
        b2.restore(new Float32Array([round * 20]));
        tf.restore();
        runDraw(program, state, tf);
        let captured = backend.captures[round];
        console.log(`    round ${round + 1} captured: ${describeAttachments(captured)}`);
        check(captured.get(0) === b0.buffer && captured.get(2) === b2.buffer,
            `round ${round + 1}: latest handles captured`);
        handleHistory.push([...captured.values()]);
    }

    let flat = handleHistory.flat();
    check(new Set(flat).size === flat.length, "every restore round produced distinct captured handles");
    check(tf.feedbackBuffers.size === 2, "associations survive both restore rounds");
}

// ---------------------------------------------------------------------------
// 7. bind() followed by feedbackBuffer(): cached and native state must agree
// ---------------------------------------------------------------------------
console.log("\n[7] bind then feedbackBuffer keeps appState and native binding consistent");
{
    let backend = createBackend("backend-7");
    let state = createAppState();
    let program = createProgram(backend.gl, state);

    let b0 = createBuffer(backend.gl, state, [1]);
    let tf = new TransformFeedback(backend.gl, state);

    tf.bind();
    check(state.transformFeedback === tf, "bind caches the feedback");
    check(backend.currentTF() === tf.transformFeedback, "bind matches the native transform feedback target");

    tf.feedbackBuffer(0, b0);
    check(state.transformFeedback === null, "feedbackBuffer clears the cached target after its temporary bind");
    check(backend.currentTF() === null, "native transform feedback target is also null after configuration");

    tf.bind();
    check(state.transformFeedback === tf, "subsequent bind is not skipped by stale cache");
    check(backend.currentTF() === tf.transformFeedback, "subsequent bind re-binds the native target");
    runDraw(program, state, tf);
    let captured = backend.captures[0];
    check(captured.get(0) === b0.buffer, "output registered between binds is actually captured");
}

// ---------------------------------------------------------------------------
// 8. Transform feedback deletion does not own its output buffers;
//    a DrawCall without feedback unbinds as before.
// ---------------------------------------------------------------------------
console.log("\n[8] delete ownership and no-feedback draw call unbinding");
{
    let backend = createBackend("backend-8");
    let state = createAppState();
    let program = createProgram(backend.gl, state);

    let b0 = createBuffer(backend.gl, state, [1]);
    let b2 = createBuffer(backend.gl, state, [2]);
    let tf = new TransformFeedback(backend.gl, state)
        .feedbackBuffer(0, b0)
        .feedbackBuffer(2, b2);

    runDraw(program, state, tf);
    tf.delete();
    check(tf.transformFeedback === null, "delete releases the transform feedback handle");
    check(state.transformFeedback === null, "delete resets the cached target");
    check(b0.buffer !== null && b2.buffer !== null, "deleting the feedback does not delete its output buffers");
    check(tf.feedbackBuffers.size === 2, "deleting keeps the registered associations for a later restore");

    tf.restore();
    runDraw(program, state, tf);
    let captured = backend.captures[1];
    console.log(`    capture after delete+restore: ${describeAttachments(captured)}`);
    check(captured.get(0) === b0.buffer && captured.get(2) === b2.buffer,
        "associations re-attach after delete followed by restore");

    // Ordinary draw call without transform feedback: existing unbind path.
    let plain = new DrawCall(backend.gl, state, program).primitive(GL.POINTS);
    let eventsBefore = backend.events.length;
    plain.draw();
    check(state.transformFeedback === null, "draw without feedback clears the cached feedback");
    check(backend.currentTF() === null, "draw without feedback clears the native feedback target");
    check(backend.events.length === eventsBefore, "draw without feedback records no begin/draw/end feedback events");
}

console.log(`\nALL PASS: ${passes} checks`);
