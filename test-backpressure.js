import EventEmitter from 'node:events';
import {test} from 'node:test';
import {strict as assert} from 'node:assert';
import {pEventIterator} from './index.js';

// Lets microtasks queued by the filter chain run, without any timer.
const flush = async (count = 1) => {
	for (let index = 0; index < count; index++) {
		// eslint-disable-next-line no-await-in-loop -- each round has to observe the microtasks of the previous one.
		await Promise.resolve();
	}
};

// The library runs filters one at a time, so a reading queued behind a pending filter has not had its filter called yet. The gate therefore exists from the moment the reading is emitted, and the filter only looks it up, which keeps every reading releasable by hand while it occupies a backpressure slot.
const makeFilterHarness = () => {
	const gates = [];
	const send = (emitter, eventName, value) => {
		const gate = {value, ...Promise.withResolvers()};
		gates.push(gate);
		emitter.emit(eventName, value);
	};

	const filter = value => gates.find(gate => gate.value === value).promise;
	const resolve = (index, verdict) => {
		gates[index].resolve(verdict);
	};

	return {
		gates,
		send,
		filter,
		resolve,
	};
};

const makeBackpressure = (overrides = {}) => ({
	highWaterMark: 2,
	lowWaterMark: 0,
	pause: () => undefined,
	resume: () => undefined,
	...overrides,
});

test('pauses at the high water mark while filters are pending and resumes at the low water mark', async () => {
	const emitter = new EventEmitter();
	const harness = makeFilterHarness();
	const notifications = [];
	const iterator = pEventIterator(emitter, 'data', {
		filter: harness.filter,
		resolutionEvents: ['end'],
		backpressure: {
			highWaterMark: 3,
			lowWaterMark: 1,
			pause() {
				notifications.push('pause');
			},
			resume() {
				notifications.push('resume');
			},
		},
	});

	harness.send(emitter, 'data', 1);
	harness.send(emitter, 'data', 2);
	harness.send(emitter, 'data', 3);

	// The third admission reaches the high water mark synchronously, while every reading is still inside or queued for the filter.
	assert.equal(harness.gates.length, 3);
	assert.deepEqual(notifications, ['pause']);

	// Only two of the three readings pass validation.
	harness.resolve(0, true);
	harness.resolve(1, true);
	harness.resolve(2, false);
	await flush(12);

	// Two accepted readings are buffered and none is consumed, so the low water mark has not been reached.
	assert.deepEqual(notifications, ['pause']);

	assert.deepEqual(await iterator.next(), {done: false, value: 1});

	// One slot is left, which is exactly the low water mark, so resume fires once.
	assert.deepEqual(notifications, ['pause', 'resume']);

	assert.deepEqual(await iterator.next(), {done: false, value: 2});

	// Already resumed: draining to zero must not notify again.
	assert.deepEqual(notifications, ['pause', 'resume']);

	harness.send(emitter, 'end', 'done');
	harness.resolve(3, true);
	await flush(12);

	assert.deepEqual(await iterator.next(), {done: true, value: 'done'});
	assert.deepEqual(notifications, ['pause', 'resume']);
	assert.deepEqual(emitter.eventNames(), []);
});

test('a filter that drops everything never leaves the source paused', async () => {
	const emitter = new EventEmitter();
	const harness = makeFilterHarness();
	const notifications = [];
	const iterator = pEventIterator(emitter, 'data', {
		filter: harness.filter,
		backpressure: makeBackpressure({
			pause() {
				notifications.push('pause');
			},
			resume() {
				notifications.push('resume');
			},
		}),
	});

	harness.send(emitter, 'data', 1);
	harness.send(emitter, 'data', 2);
	assert.deepEqual(notifications, ['pause']);

	harness.resolve(0, false);
	harness.resolve(1, false);
	await flush(12);

	// Both slots were released by the dropped verdicts, so the pause is undone instead of sticking forever.
	assert.deepEqual(notifications, ['pause', 'resume']);

	// A later burst can pause and recover again.
	harness.send(emitter, 'data', 3);
	harness.send(emitter, 'data', 4);
	assert.deepEqual(notifications, ['pause', 'resume', 'pause']);

	harness.resolve(2, false);
	harness.resolve(3, false);
	await flush(12);
	assert.deepEqual(notifications, ['pause', 'resume', 'pause', 'resume']);

	await iterator.return();
});

test('pending reads do not cover readings that are still in the filter', async () => {
	const emitter = new EventEmitter();
	const harness = makeFilterHarness();
	const notifications = [];
	const iterator = pEventIterator(emitter, 'data', {
		filter: harness.filter,
		backpressure: makeBackpressure({
			pause() {
				notifications.push('pause');
			},
			resume() {
				notifications.push('resume');
			},
		}),
	});

	// Two reads are waiting before the readings even arrive.
	const reads = [iterator.next(), iterator.next()];
	await flush();

	harness.send(emitter, 'data', 'a');
	harness.send(emitter, 'data', 'b');

	// The unfinished filters still own both slots, so the waiting reads cannot hide them and the source is paused.
	assert.deepEqual(notifications, ['pause']);

	harness.resolve(0, true);
	await flush(12);
	assert.deepEqual(await reads[0], {done: false, value: 'a'});

	// One reading was consumed, the other is still queued in the filter: above the low water mark.
	assert.deepEqual(notifications, ['pause']);

	harness.resolve(1, true);
	await flush(12);
	assert.deepEqual(await reads[1], {done: false, value: 'b'});
	await flush();

	assert.deepEqual(notifications, ['pause', 'resume']);
	await iterator.return();
});

test('data emitted re-entrantly from pause is kept and only notifies once', async () => {
	const emitter = new EventEmitter();
	const harness = makeFilterHarness();
	let pauseCount = 0;
	const iterator = pEventIterator(emitter, 'data', {
		filter: harness.filter,
		backpressure: makeBackpressure({
			pause() {
				pauseCount++;
				// The source ignores the pause and pushes another reading from inside the notification.
				harness.send(emitter, 'data', 'extra');
			},
		}),
	});

	harness.send(emitter, 'data', 1);
	harness.send(emitter, 'data', 2);

	assert.equal(pauseCount, 1);
	assert.equal(harness.gates.length, 3);

	for (let index = 0; index < harness.gates.length; index++) {
		harness.resolve(index, true);
	}

	await flush(16);

	const values = [];
	for (let index = 0; index < 3; index++) {
		// eslint-disable-next-line no-await-in-loop -- the reads have to follow the delivery order.
		const result = await iterator.next();
		values.push(result.value);
	}

	// Filters run in enqueue order, and the re-entrant reading is enqueued while the second reading's handler is still on the stack, so it is validated first. The order changes but nothing is lost and the pause fired only once.
	assert.deepEqual(values, [1, 'extra', 2]);
	await iterator.return();
});

test('a batch emitted from resume can reach the high water mark again', async () => {
	const emitter = new EventEmitter();
	const harness = makeFilterHarness();
	const notifications = [];
	let didRefill = false;
	const iterator = pEventIterator(emitter, 'data', {
		filter: harness.filter,
		backpressure: makeBackpressure({
			pause() {
				notifications.push('pause');
			},
			resume() {
				notifications.push('resume');

				// The hook only refills on the first recovery; the resume triggered by the final reads must not start another cycle.
				if (didRefill) {
					return;
				}

				didRefill = true;
				harness.send(emitter, 'data', 'x');
				harness.send(emitter, 'data', 'y');
			},
		}),
	});

	harness.send(emitter, 'data', 1);
	harness.send(emitter, 'data', 2);
	assert.deepEqual(notifications, ['pause']);

	harness.resolve(0, false);
	harness.resolve(1, false);
	await flush(12);

	// The resume hook immediately pushes two readings, which reach the high water mark and pause again.
	assert.deepEqual(notifications, ['pause', 'resume', 'pause']);
	assert.equal(harness.gates.length, 4);

	harness.resolve(2, true);
	harness.resolve(3, true);
	await flush(12);

	const reads = [iterator.next(), iterator.next()];
	const values = await Promise.all(reads);
	await flush();

	assert.deepEqual(values, [{done: false, value: 'x'}, {done: false, value: 'y'}]);
	assert.deepEqual(notifications, ['pause', 'resume', 'pause', 'resume']);
	await iterator.return();
});

test('return() while paused removes the listeners first and releases the pause exactly once', async () => {
	const emitter = new EventEmitter();
	const harness = makeFilterHarness();
	const notifications = [];
	const iterator = pEventIterator(emitter, 'data', {
		filter: harness.filter,
		resolutionEvents: ['end'],
		backpressure: makeBackpressure({
			pause() {
				notifications.push('pause');
			},
			resume() {
				// The listeners have to be gone before the pause is released.
				assert.equal(emitter.listenerCount('data'), 0);
				notifications.push('resume');
			},
		}),
	});

	harness.send(emitter, 'data', 1);
	harness.send(emitter, 'data', 2);
	assert.deepEqual(notifications, ['pause']);

	assert.deepEqual(await iterator.return('stopped'), {done: true, value: 'stopped'});
	assert.deepEqual(emitter.eventNames(), []);
	assert.deepEqual(notifications, ['pause', 'resume']);

	// Late verdicts and late events must not produce another notification.
	harness.resolve(0, true);
	harness.resolve(1, false);
	await flush(12);
	emitter.emit('data', 3);
	emitter.emit('end', 'done');
	await flush(12);

	assert.deepEqual(notifications, ['pause', 'resume']);
	assert.deepEqual(await iterator.next(), {done: true, value: undefined});
});

test('abort while paused clears the listeners and leaves no late notifications', async () => {
	const emitter = new EventEmitter();
	const controller = new AbortController();
	const harness = makeFilterHarness();
	const notifications = [];
	const iterator = pEventIterator(emitter, 'data', {
		filter: harness.filter,
		signal: controller.signal,
		backpressure: makeBackpressure({
			pause() {
				notifications.push('pause');
			},
			resume() {
				assert.equal(emitter.listenerCount('data'), 0);
				notifications.push('resume');
			},
		}),
	});

	harness.send(emitter, 'data', 1);
	harness.send(emitter, 'data', 2);
	assert.deepEqual(notifications, ['pause']);

	controller.abort(new Error('aborted'));

	assert.deepEqual(emitter.eventNames(), []);
	assert.deepEqual(notifications, ['pause', 'resume']);
	await assert.rejects(iterator.next(), {message: 'aborted'});

	harness.resolve(0, true);
	harness.resolve(1, true);
	await flush(12);
	assert.deepEqual(notifications, ['pause', 'resume']);
});

test('an error while paused releases the pause once and still rejects the read', async () => {
	const emitter = new EventEmitter();
	const harness = makeFilterHarness();
	const notifications = [];
	const iterator = pEventIterator(emitter, 'data', {
		filter: harness.filter,
		backpressure: makeBackpressure({
			pause() {
				notifications.push('pause');
			},
			resume() {
				assert.equal(emitter.listenerCount('data'), 0);
				notifications.push('resume');
			},
		}),
	});

	harness.send(emitter, 'data', 1);
	harness.send(emitter, 'data', 2);
	assert.deepEqual(notifications, ['pause']);

	emitter.emit('error', new Error('boom'));

	assert.deepEqual(emitter.eventNames(), []);
	assert.deepEqual(notifications, ['pause', 'resume']);
	await assert.rejects(iterator.next(), {message: 'boom'});

	harness.resolve(0, true);
	harness.resolve(1, false);
	await flush(12);
	assert.deepEqual(notifications, ['pause', 'resume']);
});

test('limit counts filtered-out events and ending while paused releases the pause', async () => {
	const emitter = new EventEmitter();
	const harness = makeFilterHarness();
	const notifications = [];
	const iterator = pEventIterator(emitter, 'data', {
		filter: harness.filter,
		limit: 2,
		backpressure: makeBackpressure({
			pause() {
				notifications.push('pause');
			},
			resume() {
				notifications.push('resume');
			},
		}),
	});

	harness.send(emitter, 'data', 1);
	harness.send(emitter, 'data', 2);
	assert.deepEqual(notifications, ['pause']);

	// Both are dropped, but both count towards the limit, so the iterator ends.
	harness.resolve(0, false);
	harness.resolve(1, false);
	await flush(12);

	assert.deepEqual(notifications, ['pause', 'resume']);
	assert.deepEqual(await iterator.next(), {done: true, value: undefined});
	assert.deepEqual(emitter.eventNames(), []);
});

test('ending while paused still delivers accepted values and the end payload, which takes no slot', async () => {
	const emitter = new EventEmitter();
	const harness = makeFilterHarness();
	const notifications = [];
	const iterator = pEventIterator(emitter, 'data', {
		filter: harness.filter,
		resolutionEvents: ['end'],
		backpressure: makeBackpressure({
			pause() {
				notifications.push('pause');
			},
			resume() {
				notifications.push('resume');
			},
		}),
	});

	harness.send(emitter, 'data', 1);
	harness.send(emitter, 'data', 2);

	// The end payload goes through the filter too, but it is not a reading and must not take a slot.
	harness.send(emitter, 'end', 'done');
	assert.equal(harness.gates.length, 3);
	assert.deepEqual(notifications, ['pause']);

	harness.resolve(0, true);
	harness.resolve(1, true);
	harness.resolve(2, true);
	await flush(16);

	assert.deepEqual(notifications, ['pause', 'resume']);
	assert.deepEqual(await iterator.next(), {done: false, value: 1});
	assert.deepEqual(await iterator.next(), {done: false, value: 2});
	assert.deepEqual(await iterator.next(), {done: true, value: 'done'});
	assert.deepEqual(emitter.eventNames(), []);
});

test('ending the iteration from inside pause leaves it closed with a single resume', async () => {
	// First, a resolution event emitted by the pause hook.
	const emitter = new EventEmitter();
	const first = makeFilterHarness();
	const firstNotifications = [];
	const firstIterator = pEventIterator(emitter, 'data', {
		filter: first.filter,
		resolutionEvents: ['end'],
		backpressure: makeBackpressure({
			pause() {
				firstNotifications.push('pause');
				first.send(emitter, 'end', 'finished');
			},
			resume() {
				firstNotifications.push('resume');
			},
		}),
	});

	first.send(emitter, 'data', 1);
	first.send(emitter, 'data', 2);
	for (let index = 0; index < first.gates.length; index++) {
		first.resolve(index, true);
	}

	await flush(16);

	// The end is emitted while the second reading's handler is still on the stack, so its filter job is queued behind the end's job. The reading already validated before the end is still delivered, the second reading's pending verdict is discarded exactly as without backpressure, and the end payload follows, with one pause and exactly one release.
	assert.deepEqual(firstNotifications, ['pause', 'resume']);
	assert.deepEqual(await firstIterator.next(), {done: false, value: 1});
	assert.deepEqual(await firstIterator.next(), {done: true, value: 'finished'});
	assert.deepEqual(await firstIterator.next(), {done: true, value: undefined});

	// Then, `return()` called from the pause hook. A holder keeps a constant name the hook can close over before the iterator exists.
	const otherEmitter = new EventEmitter();
	const second = makeFilterHarness();
	const secondNotifications = [];
	const holder = {};
	holder.iterator = pEventIterator(otherEmitter, 'data', {
		filter: second.filter,
		backpressure: makeBackpressure({
			pause() {
				secondNotifications.push('pause');
				holder.iterator.return();
			},
			resume() {
				secondNotifications.push('resume');
			},
		}),
	});

	second.send(otherEmitter, 'data', 1);
	second.send(otherEmitter, 'data', 2);

	assert.deepEqual(secondNotifications, ['pause', 'resume']);
	assert.deepEqual(otherEmitter.eventNames(), []);

	second.resolve(0, true);
	second.resolve(1, true);
	await flush(12);

	assert.deepEqual(secondNotifications, ['pause', 'resume']);
	assert.deepEqual(await holder.iterator.next(), {done: true, value: undefined});
});

test('a source that ignores pause loses nothing and never overflows', async () => {
	const emitter = new EventEmitter();
	const notifications = [];
	const iterator = pEventIterator(emitter, 'data', {
		backpressure: {
			highWaterMark: 2,
			lowWaterMark: 1,
			pause() {
				notifications.push('pause');
			},
			resume() {
				notifications.push('resume');
			},
		},
	});

	for (const value of [1, 2, 3, 4, 5]) {
		emitter.emit('data', value);
	}

	assert.deepEqual(notifications, ['pause']);

	const values = [];
	for (let index = 0; index < 5; index++) {
		// eslint-disable-next-line no-await-in-loop -- each read has to observe the updated water mark.
		const result = await iterator.next();
		values.push(result.value);
	}

	assert.deepEqual(values, [1, 2, 3, 4, 5]);
	assert.deepEqual(notifications, ['pause', 'resume']);
	await iterator.return();
});

test('invalid backpressure options throw a TypeError before any listener is attached', async () => {
	const emitter = new EventEmitter();
	const valid = {
		highWaterMark: 3,
		lowWaterMark: 1,
		pause: () => undefined,
		resume: () => undefined,
	};
	const invalid = [
		{...valid, highWaterMark: 1},
		{...valid, lowWaterMark: 3},
		{...valid, lowWaterMark: -1},
		{...valid, highWaterMark: 1.5},
		{...valid, lowWaterMark: 2 ** 53},
		{...valid, highWaterMark: Infinity},
		{...valid, pause: undefined},
		{...valid, resume: 'not a function'},
		{highWaterMark: 3, lowWaterMark: 1},
	];

	// Kept in a helper so the assertions run unconditionally for every bad configuration.
	const assertRejectedBeforeAttach = backpressure => {
		assert.throws(() => pEventIterator(emitter, 'data', {backpressure}), TypeError);
		assert.equal(emitter.listenerCount('data'), 0);
	};

	for (const backpressure of invalid) {
		assertRejectedBeforeAttach(backpressure);
	}

	const iterator = pEventIterator(emitter, 'data', {backpressure: valid});
	assert.equal(emitter.listenerCount('data'), 1);
	await iterator.return();
});

test('regression: without backpressure, readings and the resolution payload are delivered as before', async () => {
	const emitter = new EventEmitter();
	const iterator = pEventIterator(emitter, 'data', {resolutionEvents: ['end']});

	const first = iterator.next();
	emitter.emit('data', 'a');
	assert.deepEqual(await first, {done: false, value: 'a'});

	emitter.emit('data', 'b');
	emitter.emit('data', 'c');
	emitter.emit('end', 'finished');

	assert.deepEqual(await iterator.next(), {done: false, value: 'b'});
	assert.deepEqual(await iterator.next(), {done: false, value: 'c'});
	assert.deepEqual(await iterator.next(), {done: true, value: 'finished'});
	assert.deepEqual(await iterator.next(), {done: true, value: undefined});
	assert.deepEqual(emitter.eventNames(), []);
});
