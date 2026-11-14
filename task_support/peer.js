'use strict';
const {once} = require('node:events');
const WebSocket = require('ws');
const CDP = require('../');

async function openPeer() {
    const server = new WebSocket.Server({host: '127.0.0.1', port: 0});
    await once(server, 'listening');
    const incoming = [];
    const waiters = [];
    const connections = once(server, 'connection');
    const clientPromise = CDP({
        target: `ws://127.0.0.1:${server.address().port}`,
        protocol: require('../lib/protocol.json')
    });
    const [socket] = await connections;
    socket.on('message', raw => {
        const message = JSON.parse(raw);
        if (waiters.length) waiters.shift()(message);
        else incoming.push(message);
    });
    const client = await clientPromise;
    return {
        client, socket, server,
        next() { return incoming.length ? Promise.resolve(incoming.shift()) : new Promise(resolve => waiters.push(resolve)); },
        reply(request, result = {}) { socket.send(JSON.stringify({id: request.id, result})); },
        reject(request, message = 'controlled rejection') { socket.send(JSON.stringify({id: request.id, error: {code: -32000, message}})); },
        event(method, params, sessionId) { socket.send(JSON.stringify({method, params, sessionId})); },
        async close() {
            if (socket.readyState === WebSocket.OPEN) await client.close();
            socket.terminate();
            await new Promise(resolve => server.close(resolve));
        }
    };
}
module.exports = {openPeer};
