import { compile, decompile, updateStyles } from '../src/index.js';

const text = `[Script Info]
PlayResX: 640
PlayResY: 360

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, MarginL, MarginR, MarginV
Style: Default,Arial,20,&H00FFFFFF,10,10,10
Style: Alt,Arial,30,&H000000FF,20,20,20

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:01.00,0:00:03.00,Default,,10,0,0,,A{\\fs20}B{\\rAlt\\fs+2}C
`;

const report = (label, doc) => {
  const values = doc.dialogues[0].slices
    .flatMap((slice) => slice.fragments.map((fragment) => `${fragment.text}=${fragment.tag.fs}`));
  console.log(`${label}: ${values.join(' ')} (styles ${doc.styles.Default.tag.fs}/${doc.styles.Alt.tag.fs})`);
};

// 1. Compile with editable sources, then patch the two style sizes.
const doc = compile(text, { editableStyles: true });
report('original ', doc);

const bigger = updateStyles(doc, { Default: { Fontsize: 40 }, Alt: { Fontsize: 50 } });
report('updated  ', bigger); // A=40 B=20 C=60

// 2. Update again: relative sizes are recomputed from their source, so C is
//    1.2 * 25 = 30 rather than 1.2 * 60.
const smaller = updateStyles(bigger, { Alt: { Fontsize: 25 } });
report('rescaled ', smaller); // A=40 B=20 C=30

// 3. Save as ordinary ASS and continue editing after reloading it.
const saved = decompile(bigger);
console.log('\nsaved dialogue:');
console.log(saved.split('\n').find((line) => line.startsWith('Dialogue')));

const reloaded = compile(saved, { editableStyles: true });
const continued = updateStyles(reloaded, { Alt: { Fontsize: 25 } });
report('reloaded ', continued); // A=40 B=20 C=30
