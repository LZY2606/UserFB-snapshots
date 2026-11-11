#include "zgl.h"

void glopClearColor(GLParam* p) {
	GLContext* c = gl_get_context();
	c->clear_color.v[0] = p[1].f;
	c->clear_color.v[1] = p[2].f;
	c->clear_color.v[2] = p[3].f;
	c->clear_color.v[3] = p[4].f;
}
void glopClearDepth(GLParam* p) {
	GLContext* c = gl_get_context();
	c->clear_depth = p[1].f;
}

void glClearStencil(GLint s) {
	GLParam p[2];
#include "error_check_no_context.h"
	p[0].op = OP_ClearStencil;
	p[1].i = s;

	gl_add_op(p);
}

void glopClearStencil(GLParam* p) {
	GLContext* c = gl_get_context();
	c->zb->stencil_clear = (GLubyte)(p[1].i & 0xff);
}

void glStencilFunc(GLenum func, GLint ref, GLuint mask) {
	GLParam p[4];
#include "error_check_no_context.h"
	p[0].op = OP_StencilFunc;
	p[1].i = func;
	p[2].i = ref;
	p[3].ui = mask;

	gl_add_op(p);
}

void glopStencilFunc(GLParam* p) {
	GLContext* c = gl_get_context();
	GLint ref = p[2].i;
	c->zb->stencil_func = (GLenum)p[1].i;
	/* the reference is clamped to 0..255 before any masking */
	if (ref < 0)
		ref = 0;
	else if (ref > 255)
		ref = 255;
	c->zb->stencil_ref = (GLubyte)ref;
	c->zb->stencil_value_mask = (GLubyte)(p[3].ui & 0xff);
}

void glStencilMask(GLuint mask) {
	GLParam p[2];
#include "error_check_no_context.h"
	p[0].op = OP_StencilMask;
	p[1].ui = mask;

	gl_add_op(p);
}

void glopStencilMask(GLParam* p) {
	GLContext* c = gl_get_context();
	c->zb->stencil_write_mask = (GLubyte)(p[1].ui & 0xff);
}

void glStencilOp(GLenum fail, GLenum zfail, GLenum zpass) {
	GLParam p[4];
#include "error_check_no_context.h"
	p[0].op = OP_StencilOp;
	p[1].i = fail;
	p[2].i = zfail;
	p[3].i = zpass;

	gl_add_op(p);
}

void glopStencilOp(GLParam* p) {
	GLContext* c = gl_get_context();
	c->zb->stencil_op_sfail = (GLenum)p[1].i;
	c->zb->stencil_op_dpfail = (GLenum)p[2].i;
	c->zb->stencil_op_dppass = (GLenum)p[3].i;
}

void glopClear(GLParam* p) {
	GLContext* c = gl_get_context();
	GLint mask = p[1].i;
	GLint z = 0;
	GLint r = (GLint)(c->clear_color.v[0] * COLOR_MULT_MASK);
	GLint g = (GLint)(c->clear_color.v[1] * COLOR_MULT_MASK);
	GLint b = (GLint)(c->clear_color.v[2] * COLOR_MULT_MASK);

	/* TODO : correct value of Z */

	/* the stencil clear obeys the stencil write mask but not the
	   GL_STENCIL_TEST enable; unselected buffers are left untouched */
	ZB_clear(c->zb, mask & GL_DEPTH_BUFFER_BIT, z, mask & GL_COLOR_BUFFER_BIT, r, g, b,
		 mask & GL_STENCIL_BUFFER_BIT, c->zb->stencil_clear);
}
