///////////////////////////////////////////////////////////////////////////////////
// The MIT License (MIT)
//
// Copyright (c) 2017 Tarek Sherif
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

import { GL } from "./constants.js";

/**
    Tranform feedback object.

    @class TransformFeedback
    @prop {WebGLRenderingContext} gl The WebGL context.
    @prop {WebGLTransformFeedback} transformFeedback Transform feedback object.
    @prop {Map} feedbackBuffers Registered output bindings, as a map of varying
        index to VertexBuffer. Buffer objects are tracked rather than their GL
        handles, so associations survive both buffer and transform feedback
        restoration and re-attach to the buffers' current handles.
    @prop {Map} boundHandles GL handles currently attached to this transform
        feedback object, as a map of varying index to WebGLBuffer. Used to
        avoid re-issuing bindBufferBase for unchanged bindings.
    @prop {Object} appState Tracked GL state.
*/
export class TransformFeedback {

    constructor(gl, appState) {
        this.gl = gl;
        this.appState = appState;
        this.transformFeedback = null;
        this.feedbackBuffers = new Map();
        this.boundHandles = new Map();

        this.restore();
    }

    /**
        Restore transform feedback after context loss. Registered feedback
        buffer associations are retained and re-applied against the buffers'
        current GL handles the next time this object is bound.

        @method
        @return {TransformFeedback} The TransformFeedback object.
    */
    restore() {
        if (this.appState.transformFeedback === this) {
            this.appState.transformFeedback = null;
        }

        this.transformFeedback = this.gl.createTransformFeedback();

        // The new transform feedback object starts with no output bindings.
        this.boundHandles.clear();

        return this;
    }

    /**
        Bind a feedback buffer to capture transform output. The association is
        retained across restore() calls on this object and on the buffer, and
        is re-applied using the buffer's current GL handle.

        @method
        @param {number} index Index of transform feedback varying to capture.
        @param {VertexBuffer} buffer Buffer to record output into.
        @return {TransformFeedback} The TransformFeedback object.
    */
    feedbackBuffer(index, buffer) {
        let maxAttribs = this.gl.getParameter ?
            this.gl.getParameter(GL.MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS) : null;
        if (!maxAttribs) {
            // Minimum guaranteed by the WebGL 2 spec.
            maxAttribs = 4;
        }

        if (!Number.isInteger(index) || index < 0 || index >= maxAttribs) {
            throw new Error("TransformFeedback.feedbackBuffer: index must be an integer in the range [0, MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS).");
        }

        if (!buffer || buffer.gl !== this.gl) {
            throw new Error("TransformFeedback.feedbackBuffer: buffer must be a VertexBuffer created from the same WebGL context.");
        }

        this.gl.bindTransformFeedback(GL.TRANSFORM_FEEDBACK, this.transformFeedback);
        this.appState.transformFeedback = this;

        this.gl.bindBufferBase(GL.TRANSFORM_FEEDBACK_BUFFER, index, buffer.buffer);
        this.feedbackBuffers.set(index, buffer);
        this.boundHandles.set(index, buffer.buffer);

        // TODO(Tarek): Firefox doesn't properly unbind TRANSFORM_FEEDBACK_BUFFER
        // bindings when TRANSFORM_FEEDBACK is unbound.
        // https://bugzilla.mozilla.org/show_bug.cgi?id=1541396
        this.gl.bindTransformFeedback(GL.TRANSFORM_FEEDBACK, null);
        this.gl.bindBufferBase(GL.TRANSFORM_FEEDBACK_BUFFER, index, null);

        // The temporary configuration binding is fully unbound, so the cached
        // state must match the actual GL state instead of still pointing here.
        this.appState.transformFeedback = null;

        return this;
    }

    /**
        Delete this transform feedback. Registered feedback buffer
        associations are retained, and the output buffers themselves
        are not deleted.

        @method
        @return {TransformFeedback} The TransformFeedback object.
    */
    delete() {
        if (this.transformFeedback) {
            this.gl.deleteTransformFeedback(this.transformFeedback);
            this.transformFeedback = null;
            this.boundHandles.clear();

            if (this.appState.transformFeedback === this) {
                this.gl.bindTransformFeedback(GL.TRANSFORM_FEEDBACK, null);
                this.appState.transformFeedback = null;
            }
        }

        return this;
    }

    /**
        Bind this transform feedback and sync any registered output buffer
        whose GL handle has changed. Unchanged bindings are not re-issued,
        so consecutive unchanged draws carry no bindBufferBase overhead.

        @method
        @ignore
        @return {TransformFeedback} The TransformFeedback object.
    */
    bind() {
        if (this.appState.transformFeedback !== this) {
            this.gl.bindTransformFeedback(GL.TRANSFORM_FEEDBACK, this.transformFeedback);
            this.appState.transformFeedback = this;
        }

        this.syncFeedbackBuffers();

        return this;
    }

    // Re-attach registered output buffers whose current GL handles differ
    // from the handles attached to this transform feedback object. Throws
    // if a registered buffer has been deleted, in which case no transform
    // feedback capture is started and the registered associations are
    // left intact.
    syncFeedbackBuffers() {
        this.feedbackBuffers.forEach((buffer, index) => {
            if (!buffer.buffer) {
                throw new Error(`TransformFeedback.bind: feedback buffer registered at index ${index} has been deleted.`);
            }

            if (this.boundHandles.get(index) !== buffer.buffer) {
                this.gl.bindBufferBase(GL.TRANSFORM_FEEDBACK_BUFFER, index, buffer.buffer);
                this.boundHandles.set(index, buffer.buffer);
            }
        });
    }

}
