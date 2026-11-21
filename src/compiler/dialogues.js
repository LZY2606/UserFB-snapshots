import { compileText } from './text.js';

export function compileDialogues({ styles, dialogues, editableStyles }) {
  let minLayer = Infinity;
  const results = [];
  for (let i = 0; i < dialogues.length; i++) {
    const dia = dialogues[i];
    if (dia.Start >= dia.End) {
      continue;
    }
    if (!styles[dia.Style]) {
      dia.Style = 'Default';
    }
    const stl = styles[dia.Style].style;
    const compiledText = compileText({
      styles,
      style: dia.Style,
      parsed: dia.Text.parsed,
      start: dia.Start,
      end: dia.End,
      editableStyles,
    });
    const alignment = compiledText.alignment || stl.Alignment;
    minLayer = Math.min(minLayer, dia.Layer);
    const compiled = Object.assign({
      layer: dia.Layer,
      start: dia.Start,
      end: dia.End,
      style: dia.Style,
      name: dia.Name,
      // reset style by `\r` will not effect margin and alignment
      margin: {
        left: dia.MarginL || stl.MarginL,
        right: dia.MarginR || stl.MarginR,
        vertical: dia.MarginV || stl.MarginV,
      },
      effect: dia.Effect,
    }, compiledText, { alignment });
    if (editableStyles) {
      // 0 means "inherit from the style" and is not an explicit override.
      compiled.marginMeta = {
        left: !!dia.MarginL,
        right: !!dia.MarginR,
        vertical: !!dia.MarginV,
        style: dia.Style,
      };
    }
    results.push(compiled);
  }
  for (let i = 0; i < results.length; i++) {
    results[i].layer -= minLayer;
  }
  return results.sort((a, b) => a.start - b.start || a.end - b.end);
}
