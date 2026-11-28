import { ParsedTag, CompiledTag } from './tags';

interface ScriptInfo {
    Title: string;
    ScriptType: 'V4.00' | 'V4.00+' | string;
    WrapStyle: '0' | '1' | '2' | '3';
    PlayResX: string;
    PlayResY: string;
    ScaledBorderAndShadow: 'yes' | 'no';
    Collisions: 'Normal' | 'Reverse';
    [name: string]: string;
}

interface ParsedASSStyles {
    format: string[];
    style: {
        Name: string;
        Fontname: string;
        Fontsize: string;
        PrimaryColour: string;
        SecondaryColour: string;
        OutlineColour: string;
        BackColour: string;
        Bold: string;
        Italic: string;
        Underline: string;
        StrikeOut: string;
        ScaleX: string;
        ScaleY: string;
        Spacing: string;
        Angle: string;
        BorderStyle: string;
        Outline: string;
        Shadow: string;
        Alignment: string;
        MarginL: string;
        MarginR: string;
        MarginV: string;
        Encoding: string;
    }[];
}

interface ParsedASSEventTextParsed {
    tags: { [K in keyof ParsedTag]: ParsedTag[K]; }[];
    text: string;
    drawing: string[][];
}

interface EffectBanner {
    name: 'banner';
    delay: number;
    leftToRight: number;
    fadeAwayWidth: number;
}

interface EffectScroll {
    name: 'scroll up' | 'scroll down';
    y1: number;
    y2: number;
    delay: number;
    fadeAwayHeight: number;
}

interface EffectUnknown {
    name: string;
}

interface ParsedASSEventText {
    raw: string;
    combined: string;
    parsed: ParsedASSEventTextParsed[];
}

interface ParsedASSEvent {
    Layer: number;
    Start: number;
    End: number;
    Style: string;
    Name: string;
    MarginL: number;
    MarginR: number;
    MarginV: number;
    Effect?: EffectBanner | EffectScroll | EffectUnknown;
    Text: ParsedASSEventText;
}

interface ParsedASSEvents {
    format: string[];
    comment: ParsedASSEvent[];
    dialogue: ParsedASSEvent[];
}

export interface ParsedASS {
    info: ScriptInfo;
    styles: ParsedASSStyles;
    events: ParsedASSEvents;
}

/**
 * Parse ASS string.
 * @param text
 */
export function parse(text: string): ParsedASS;

export function stringify(obj: ParsedASS): string;

export interface CompiledASSStyleTag {
    fn: string;
    fs: number;
    c1: string;
    a1: string;
    c2: string;
    a2: string;
    c3: string;
    a3: string;
    c4: string;
    a4: string;
    b: 0 | 1;
    i: 0 | 1;
    u: 0 | 1;
    s: 0 | 1;
    fscx: number;
    fscy: number;
    fsp: number;
    frz: number;
    xbord: number;
    ybord: number;
    xshad: number;
    yshad: number;
    fe: number;
    // TODO: [breaking change] delete `q`
    q: 0 | 1 | 2 | 3;
}

export interface CompiledASSStyle {
    style: {
        Name: string;
        Fontname: string;
        Fontsize: number;
        PrimaryColour: string;
        SecondaryColour: string;
        OutlineColour: string;
        BackColour: string;
        Bold: -1 | 0;
        Italic: -1 | 0;
        Underline: -1 | 0;
        StrikeOut: -1 | 0;
        ScaleX: number;
        ScaleY: number;
        Spacing: number;
        Angle: number;
        BorderStyle: 1 | 3;
        Outline: number;
        Shadow: number;
        Alignment: number;
        MarginL: number;
        MarginR: number;
        MarginV: number;
        Encoding: number;
    }
    tag: CompiledASSStyleTag;
}

export interface DialogueDrawingInstruction {
    type: 'M' | 'L' | 'C';
    points: {
        x: number;
        y: number;
    }[];
}

export interface DialogueDrawing {
    instructions: DialogueDrawingInstruction[];
    d: string;
    minX: number;
    minY: number;
    width: number;
    height: number;
}

export interface DialogueFragment {
    tag: CompiledTag;
    text: string;
    drawing?: DialogueDrawing;
    // only present when compiled with `editableStyles: true`
    origin?: FragmentOrigin;
}

export interface DialogueSlice {
    style: string;
    fragments: DialogueFragment[];
}

export interface Dialogue {
    layer: number;
    start: number;
    end: number;
    style: string;
    name: string;
    margin: {
        left: number;
        right: number;
        vertical: number;
    }
    // only present when compiled with `editableStyles: true`
    origin?: DialogueOrigin;
    effect?: EffectBanner | EffectScroll | EffectUnknown;
    alignment: number;
    q: 0 | 1 | 2 | 3;
    slices: DialogueSlice[];
    pos?: {
        x: number;
        y: number;
    };
    org?: {
        x: number;
        y: number;
    };
    move?: {
        x1: number;
        y1: number;
        x2: number;
        y2: number;
        t1: number;
        t2: number;
    };
    fade?: {
        type: 'fad';
        t1: number;
        t2: number;
    } | {
        type: 'fade';
        a1: number;
        a2: number;
        a3: number;
        t1: number;
        t2: number;
        t3: number;
        t4: number;
    };
    clip?: {
        inverse: boolean;
        scale: number;
        drawing?: DialogueDrawing;
        dots?: {
            x1: number;
            y1: number;
            x2: number;
            y2: number;
        }
    }
}

export interface CompiledASS {
    info: ScriptInfo;
    width: number;
    height: number;
    collisions: 'Normal' | 'Reverse';
    wrapStyle: 0 | 1 | 2 | 3;
    styles: { [styleName: string]: CompiledASSStyle };
    dialogues: Dialogue[];
    // only present when compiled with `editableStyles: true`
    editableStyles?: boolean;
}

export interface CompileOptions {
    defaultInfo?: Partial<ScriptInfo>;
    defaultStyle?: object;
    // keep per-fragment style origins so `updateStyles` can re-project
    // them against patched styles later (default: false)
    editableStyles?: boolean;
}

export function compile(text: string, options?: CompileOptions): CompiledASS;

export function decompile(obj: CompiledASS): string;

export type FontsizeOrigin =
    | { type: 'inherit' }
    | { type: 'value'; value: number }
    | { type: 'relative'; factor: number; base: FontsizeOrigin };

export type StyleValueOrigin =
    | { type: 'inherit' }
    | { type: 'value'; value: string };

export interface FragmentOrigin {
    fs: FontsizeOrigin;
    c1: StyleValueOrigin;
    a1: StyleValueOrigin;
    projected: {
        fs: number;
        c1: string;
        a1: string;
    };
}

export interface DialogueOrigin {
    margin: {
        left: number | null;
        right: number | null;
        vertical: number | null;
    };
    projectedMargin: {
        left: number;
        right: number;
        vertical: number;
    };
}

export interface StylePatch {
    // a positive finite number
    Fontsize?: number;
    // `&HAABBGGRR` or `&HAABBGGRR&`
    PrimaryColour?: string;
    // non-negative integers
    MarginL?: number;
    MarginR?: number;
    MarginV?: number;
}

/**
 * Apply a style patch to a document compiled with `editableStyles: true`
 * and return a fully independent new document. Every fragment is
 * re-projected from its saved origin against the patched styles.
 */
export function updateStyles(
    document: CompiledASS,
    changes: { [styleName: string]: StylePatch }
): CompiledASS;
