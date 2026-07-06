/**
 * @module ExportParser
 * Parse VideoForge export content (Premiere XML, FCPXML, EDL) for statistics.
 *
 * Uses string-based analysis rather than a DOM parser, which keeps this module
 * dependency-free and suitable for any Node.js environment.  The counts are
 * accurate enough for inspection and regression assertions; they are not
 * intended to replace schema-aware XML tooling.
 */

export class ExportParser {

  // ── Public API ────────────────────────────────────────────────────────────────

  /**
   * Parse Premiere Pro XML (XMEML v5) and extract structure statistics.
   *
   * @param {string} xml
   * @returns {PremiereStats}
   */
  parsePremiere(xml) {
    return {
      format: 'premiere',
      xmemlVersion: this._attr(xml, 'xmeml', 'version'),
      sequenceCount: this._count(xml, '<sequence'),
      videoTrackCount: this._countInsideFirst(xml, '<video>', '</video>', '<track>'),
      audioTrackCount: this._countInsideFirst(xml, '<audio>', '</audio>', '<track>'),
      clipItemCount: this._count(xml, '<clipitem'),
      transitionItemCount: this._count(xml, '<transitionitem'),
      generatorItemCount: this._count(xml, '<generatoritem'),
      keyframeCount: this._count(xml, '<keyframe>'),
      markerCount: this._count(xml, '<marker>'),
      size: xml.length,
      lineCount: xml.split('\n').length,
    };
  }

  /**
   * Parse FCPXML 1.10 and extract structure statistics.
   *
   * @param {string} xml
   * @returns {FcpxmlStats}
   */
  parseFcpxml(xml) {
    // asset tag can be self-closing (<asset .../>)  or have a space before />
    const assetCount = this._count(xml, '<asset ') + this._count(xml, '<asset\n');

    // VideoForge FCPXML exporter emits <clip> elements inside the spine.
    // <asset-clip> is an alternative form used by some exporters; count both.
    const clipCount = this._count(xml, '<clip ') + this._count(xml, '<clip\n');
    const assetClipCount = this._count(xml, '<asset-clip');

    return {
      format: 'fcpxml',
      fcpxmlVersion: this._attr(xml, 'fcpxml', 'version'),
      assetCount,
      formatCount: this._count(xml, '<format '),
      sequenceCount: this._count(xml, '<sequence'),
      clipCount,
      assetClipCount,
      videoTrackCount: this._count(xml, '<video>'),
      audioTrackCount: this._count(xml, '<audio>'),
      titleCount: this._count(xml, '<title'),
      captionCount: this._count(xml, '<caption'),
      keyframeCount: this._count(xml, '<keyframe'),
      markerCount: this._count(xml, '<marker'),
      size: xml.length,
      lineCount: xml.split('\n').length,
    };
  }

  /**
   * Parse CMX3600 EDL and extract structure statistics.
   *
   * @param {string} edl
   * @returns {EdlStats}
   */
  parseEdl(edl) {
    const lines = edl.split('\n');
    const title = lines.find((l) => l.startsWith('TITLE:'))?.slice(6).trim() ?? '';
    const fcm   = lines.find((l) => l.startsWith('FCM:'))?.slice(4).trim() ?? '';

    // Event lines start with a 3-digit event number
    const eventLines     = lines.filter((l) => /^\s*\d{3}\s/.test(l));
    const videoEvents    = eventLines.filter((l) => / V /.test(l));
    const audioEvents    = eventLines.filter((l) => / A[^V]/.test(l));
    const avEvents       = eventLines.filter((l) => / AV /.test(l));
    const commentLines   = lines.filter((l) => l.startsWith('*'));

    return {
      format: 'edl',
      title,
      fcm,
      eventCount: eventLines.length,
      videoEventCount: videoEvents.length + avEvents.length,
      audioEventCount: audioEvents.length + avEvents.length,
      audioVideoEventCount: avEvents.length,
      commentLineCount: commentLines.length,
      size: edl.length,
      lineCount: lines.length,
    };
  }

  // ── Helpers ───────────────────────────────────────────────────────────────────

  /**
   * Count all occurrences of a literal substring.
   * @param {string} content
   * @param {string} needle
   * @returns {number}
   */
  _count(content, needle) {
    let n = 0;
    let pos = 0;
    while ((pos = content.indexOf(needle, pos)) !== -1) {
      n++;
      pos += needle.length;
    }
    return n;
  }

  /**
   * Count occurrences of `inner` within the FIRST occurrence of `open`…`close`.
   * Returns 0 if the outer boundary is not found.
   *
   * @param {string} content
   * @param {string} open
   * @param {string} close
   * @param {string} inner
   * @returns {number}
   */
  _countInsideFirst(content, open, close, inner) {
    const start = content.indexOf(open);
    if (start === -1) return 0;
    const end = content.indexOf(close, start + open.length);
    if (end === -1) return 0;
    return this._count(content.slice(start, end), inner);
  }

  /**
   * Extract a named attribute value from the first matching element.
   *
   * @param {string} content
   * @param {string} tagName
   * @param {string} attrName
   * @returns {string | null}
   */
  _attr(content, tagName, attrName) {
    const re = new RegExp(`<${tagName}[^>]*\\s${attrName}="([^"]*)"`, 'i');
    return content.match(re)?.[1] ?? null;
  }
}
