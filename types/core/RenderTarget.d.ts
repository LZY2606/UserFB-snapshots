import { Texture } from './Texture.js';

import type { OGLRenderingContext } from './Renderer.js';

export interface RenderTargetOptions {
    width: number;
    height: number;
    target: GLenum;
    color: number;
    depth: boolean;
    stencil: boolean;
    depthTexture: boolean;
    wrapS: GLenum;
    wrapT: GLenum;
    wrapR: GLenum;
    minFilter: GLenum;
    magFilter: GLenum;
    type: GLenum;
    format: GLenum;
    internalFormat: GLenum;
    unpackAlignment: number;
    premultiplyAlpha: boolean;
}

/**
 * A render target.
 * @see {@link https://github.com/oframe/ogl/blob/master/src/core/RenderTarget.js | Source}
 */
export class RenderTarget {
    gl: OGLRenderingContext;
    width: number;
    height: number;
    depth: boolean;
    stencil: boolean;
    buffer: WebGLFramebuffer;
    target: number;

    textures: Texture[];
    texture: Texture;
    depthTexture: Texture;
    depthBuffer: WebGLRenderbuffer;
    stencilBuffer: WebGLRenderbuffer;
    depthStencilBuffer: WebGLRenderbuffer;

    /** True once {@link remove} has released the framebuffer and its attachments. */
    removed: boolean;

    constructor(gl: OGLRenderingContext, options?: Partial<RenderTargetOptions>);

    setSize(width: number, height: number): void;

    /**
     * Delete the framebuffer together with the color textures, optional depth
     * texture, and depth/stencil renderbuffers created by this target. Safe to
     * call repeatedly; a removed target can no longer be resized or rendered to.
     */
    remove(): void;
}
