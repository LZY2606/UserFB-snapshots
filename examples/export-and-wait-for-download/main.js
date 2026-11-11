import { mkdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { readFileSync } from 'node:fs';

import puppeteer from 'puppeteer';

import {
  createRunner,
  parse,
  PuppeteerRunnerExtension,
  stringify,
} from '../../lib/main.js';

// An absolute directory dedicated to this flow. It is created for you, but
// creating it ahead of time is also fine.
const downloadPath = join(tmpdir(), 'replay-report-downloads');
mkdirSync(downloadPath, { recursive: true });

// The recording itself only describes the steps; the absolute downloadPath
// is attached here so the same recording.json stays machine independent.
const rawRecording = JSON.parse(
  readFileSync(new URL('./recording.json', import.meta.url), 'utf8')
);
const recording = parse({ ...rawRecording, downloadPath });

const browser = await puppeteer.launch({ headless: true });
try {
  const page = await browser.newPage();
  const extension = new PuppeteerRunnerExtension(browser, page);
  const runner = await createRunner(recording, extension);

  // Replay clicks "Export" and blocks until the browser reports the download
  // as completed AND downloadPath/<guid> is a readable file. No fixed delay
  // is inserted into the recording.
  await runner.run();

  // One entry per settled download, in completion order:
  // {guid, url, suggestedFilename, path}. Records published by successful
  // steps survive a failure in a later step.
  for (const download of extension.completedDownloads) {
    console.log('settled download:', download);
  }
} finally {
  // Direct replay never closes a browser it did not create, so do it here.
  await browser.close();
}

// The exported standalone script runs the same correlation and settlement
// rules without importing private Replay helpers, creates/closes its own
// browser (also on failure) and exposes the same `completedDownloads` array.
console.log('\n--- equivalent exported script ---');
console.log(await stringify(recording));
