import { compileText } from './text.js';

export function compileDialogues({ styles, dialogues, editable }) {
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
      editable,
    });
    const alignment = compiledText.alignment || stl.Alignment;
    minLayer = Math.min(minLayer, dia.Layer);
    const margin = {
      left: dia.MarginL || stl.MarginL,
      right: dia.MarginR || stl.MarginR,
      vertical: dia.MarginV || stl.MarginV,
    };
    const compiled = Object.assign({
      layer: dia.Layer,
      start: dia.Start,
      end: dia.End,
      style: dia.Style,
      name: dia.Name,
      // reset style by `\r` will not effect margin and alignment
      margin,
      effect: dia.Effect,
    }, compiledText, { alignment });
    if (editable) {
      // a zero dialogue margin inherits from the style, a non-zero one is
      // an explicit value even if it happens to equal the style margin
      compiled.origin = {
        margin: {
          left: dia.MarginL || null,
          right: dia.MarginR || null,
          vertical: dia.MarginV || null,
        },
        projectedMargin: {
          left: margin.left,
          right: margin.right,
          vertical: margin.vertical,
        },
      };
    }
    results.push(compiled);
  }
  for (let i = 0; i < results.length; i++) {
    results[i].layer -= minLayer;
  }
  return results.sort((a, b) => a.start - b.start || a.end - b.end);
}
