'use strict';
const assert = require('node:assert/strict');
const {openPeer} = require('./peer');
(async () => {
    const p = await openPeer();
    try {
        const event = p.client.Input.dragIntercepted('child');
        const data = {items:[{mimeType:'text/plain',data:'order-17'}],files:[],dragOperationsMask:17};
        const sent = p.client.Input.dispatchMouseEvent({type:'mousePressed',x:12,y:34,button:'left',buttons:1,clickCount:1}, 'child');
        const m = await p.next();
        assert.equal(m.method, 'Input.dispatchMouseEvent');
        assert.equal(m.sessionId, 'child');
        assert.deepEqual(m.params, {type:'mousePressed',x:12,y:34,button:'left',buttons:1,clickCount:1});
        p.reply(m);
        p.event('Input.dragIntercepted', {data}, 'child');
        assert.deepEqual(await sent, {});
        assert.deepEqual(await event, {data});
        const released = p.client.send('Input.dispatchMouseEvent', {type:'mouseReleased',x:12,y:34,button:'left',buttons:0,clickCount:1}, 'child');
        const up = await p.next();
        assert.equal(up.params.type, 'mouseReleased');
        assert.equal(up.params.buttons, 0);
        p.reply(up);
        await released;
        // This is an existing-function baseline, never a claim of feature completion.
        console.log('NATIVE_INPUT_BASELINE_OK press release generated-session-event');
        console.log('dragAndDrop_available=' + String(typeof p.client.dragAndDrop === 'function'));
    } finally { await p.close(); }
})().catch(error => { console.error(error); process.exitCode=1; });
