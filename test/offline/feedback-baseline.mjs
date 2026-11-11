import assert from 'node:assert/strict';
import {GL} from '../../src/constants.js';
import {VertexBuffer} from '../../src/vertex-buffer.js';
import {TransformFeedback} from '../../src/transform-feedback.js';
let id=0,current=null;const bindings=new Map();
const gl={STATIC_DRAW:GL.STATIC_DRAW,
 createBuffer(){return {id:++id};},bindBuffer(){},bufferData(){},
 createTransformFeedback(){const h={id:++id};bindings.set(h,new Map());return h;},
 bindTransformFeedback(t,h){current=h;},
 bindBufferBase(t,i,h){if(current)bindings.get(current).set(i,h);}};
const state={vertexArray:null,transformFeedback:null};
const buffer=new VertexBuffer(gl,state,GL.FLOAT,2,new Float32Array([1,2,3,4]));
const tf=new TransformFeedback(gl,state).feedbackBuffer(2,buffer);
tf.bind();assert.equal(bindings.get(current).get(2),buffer.buffer);
console.log('initial sparse index2 captures current buffer handle');
tf.feedbackBuffer(2,buffer);tf.bind();
assert.equal(state.transformFeedback,tf);assert.equal(current,null);
console.log('existing feedbackBuffer leaves cached TF active while native binding is null');
const old=buffer.buffer;buffer.restore(new Float32Array([5,6,7,8]));tf.restore();tf.bind();
assert.notEqual(old,buffer.buffer);assert.equal(bindings.get(current).size,0);
console.log('existing restore loses sparse feedback association despite restored buffer');
