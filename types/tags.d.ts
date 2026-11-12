export interface ParsedTag {
    a?: 0 | 1 | 2 | 3 | 5 | 6 | 7 | 9 | 10 | 11;
    a1?: string;
    a2?: string;
    a3?: string;
    a4?: string;
    alpha?: string;
    an?: 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9;
    b?: 0 | 1;
    be?: number;
    blur?: number;
    bord?: number;
    c1?: string;
    c2?: string;
    c3?: string;
    c4?: string;
    clip?: {
        inverse: boolean;
        scale: number;
        drawing?: string[][];
        dots?: [number, number, number, number];
    }
    fad?: [number, number];
    fade?: [number, number, number, number, number, number];
    fax?: number;
    fay?: number;
    fe?: number;
    fn?: string;
    fr?: number;
    frx?: number;
    fry?: number;
    frz?: number;
    fs?: string;
    fscx?: number;
    fscy?: number;
    fsp?: number;
    i?: 0 | 1;
    k?: number;
    kf?: number;
    ko?: number;
    kt?: number;
    K?: number;
    move?: [number, number, number, number] | [number, number, number, number, number, number];
    org?: [number, number];
    p?: number;
    pbo?: number;
    pos?: [number, number];
    q?: 0 | 1 | 2 | 3;
    r?: string;
    s?: 0 | 1;
    shad?: number;
    t?: {
        t1: number;
        t2: number;
        accel: number;
        tags: { [K in keyof ParsedTag]: ParsedTag[K]; }[];
    }
    u?: 0 | 1;
    xbord?: number;
    xshad?: number;
    ybord?: number;
    yshad?: number;
}

export interface CompiledTag {
    a1?: string;
    a2?: string;
    a3?: string;
    a4?: string;
    an?: 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9;
    b?: 0 | 1;
    be?: number;
    blur?: number;
    c1?: string;
    c2?: string;
    c3?: string;
    c4?: string;
    fax?: number;
    fay?: number;
    fe?: number;
    fn?: string;
    frx?: number;
    fry?: number;
    frz?: number;
    fs?: number;
    fscx?: number;
    fscy?: number;
    fsp?: number;
    i?: 0 | 1;
    k?: number;
    kf?: number;
    ko?: number;
    kt?: number;
    p?: number;
    pbo?: number;
    q?: 0 | 1 | 2 | 3;
    s?: 0 | 1;
    t?: {
        t1: number;
        t2: number;
        accel: number;
        tag: CompiledTag;
    }[];
    u?: 0 | 1;
    xbord?: number;
    xshad?: number;
    ybord?: number;
    yshad?: number;
}

/**
 * Fully resolved style tag returned by `sampleDialogue`. Every key is present
 * because sampling starts from the owning style's defaults. Colors are BBGGRR
 * strings and alphas are two-digit hex transparency bytes, same as in ASS.
 */
export interface SampledDialogueTag {
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
    b: number;
    i: number;
    u: number;
    s: number;
    fscx: number;
    fscy: number;
    fsp: number;
    frz: number;
    frx: number;
    fry: number;
    fax: number;
    fay: number;
    blur: number;
    xbord: number;
    ybord: number;
    xshad: number;
    yshad: number;
    fe: number;
    q: 0 | 1 | 2 | 3;
}

/** One ordered entry of the per-dialogue animation timeline. */
export type AnimationTimelineOperation = {
    /** A static override, applied as an immediate replacement. */
    type: 'set';
    tag: Partial<SampledDialogueTag>;
} | {
    /** A `\t` transform; milliseconds are relative to the dialogue start. */
    type: 't';
    t1: number;
    t2: number;
    accel: number;
    tag: Partial<SampledDialogueTag>;
} | {
    /** A `\r` / `\rStyle` reset; the animation clock keeps running. */
    type: 'reset';
    style: string;
};
