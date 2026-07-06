/**
 * @module ValidationService
 * Validate VideoForge projects and generated export outputs.
 *
 * Project-level validation delegates to VideoForge's InterchangeValidator.
 * Export-level validation performs structural checks on the generated text
 * (XML well-formedness heuristics, EDL header integrity, etc.).
 *
 * @typedef {{ valid: boolean, errors: string[], warnings: string[] }} ValidationResult
 */

import { InterchangeValidator, TimelineConverter } from 'videoforge';

export class ValidationService {
  /**
   * @param {import('./ProjectService.js').ProjectService} projectService
   * @param {import('./StorageService.js').StorageService} storage
   */
  constructor(projectService, storage) {
    this._projects = projectService;
    this._storage = storage;
    this._validator = new InterchangeValidator();
    this._converter = new TimelineConverter();
  }

  // ── Project validation ────────────────────────────────────────────────────────

  /**
   * Run full project-level validation using VideoForge's InterchangeValidator.
   *
   * @param {string} projectId
   * @returns {ValidationResult}
   */
  validateProject(projectId) {
    const project = this._projects.getProject(projectId);
    return this._validator.validateProject(project);
  }

  /**
   * Validate the ITR representation produced by TimelineConverter.
   *
   * @param {string} projectId
   * @returns {ValidationResult}
   */
  validateTimeline(projectId) {
    const project = this._projects.getProject(projectId);
    const itr = this._converter.convert(project);
    return this._validator.validateTimeline(itr);
  }

  // ── Export validation ─────────────────────────────────────────────────────────

  /**
   * Validate a stored export by ID.
   * Dispatches to the appropriate format-specific validator.
   *
   * @param {string} exportId
   * @returns {ValidationResult}
   */
  validateExport(exportId) {
    const data = this._storage.getExport(exportId);
    if (!data) {
      return {
        valid: false,
        errors: [`Export not found: "${exportId}"`],
        warnings: [],
      };
    }

    switch (data.format) {
      case 'premiere': return this._validatePremiereXml(data.content);
      case 'fcpxml':   return this._validateFcpxml(data.content);
      case 'edl':      return this._validateEdl(data.content);
      default:
        return {
          valid: false,
          errors: [`Unknown export format: "${data.format}"`],
          warnings: [],
        };
    }
  }

  // ── Format validators ─────────────────────────────────────────────────────────

  /**
   * @param {string} content
   * @returns {ValidationResult}
   */
  _validatePremiereXml(content) {
    const errors = [];
    const warnings = [];

    if (!content.includes('<xmeml'))        errors.push('Missing <xmeml> root element');
    if (!content.includes('version="5"'))   warnings.push('Expected XMEML version="5"');
    if (!content.includes('<sequence'))     errors.push('Missing <sequence> element');
    if (!content.includes('<video>'))       warnings.push('No <video> section found');
    if (!content.includes('<audio>'))       warnings.push('No <audio> section found');
    if (!content.endsWith('</xmeml>') && !content.trimEnd().endsWith('</xmeml>')) {
      warnings.push('Document does not end with </xmeml>');
    }
    if (content.includes('NaN') || content.includes('>undefined<')) {
      errors.push('Export contains NaN or undefined — possible serialization error');
    }

    return { valid: errors.length === 0, errors, warnings };
  }

  /**
   * @param {string} content
   * @returns {ValidationResult}
   */
  _validateFcpxml(content) {
    const errors = [];
    const warnings = [];

    if (!content.includes('<fcpxml'))         errors.push('Missing <fcpxml> root element');
    if (!content.includes('version="1.10"'))  warnings.push('Expected FCPXML version="1.10"');
    if (!content.includes('<resources'))      warnings.push('Missing <resources> block');
    if (!content.includes('<library'))        warnings.push('Missing <library> element');
    if (!content.includes('<sequence'))       errors.push('Missing <sequence> element');
    if (content.includes('NaN') || content.includes('>undefined<')) {
      errors.push('Export contains NaN or undefined — possible serialization error');
    }

    return { valid: errors.length === 0, errors, warnings };
  }

  /**
   * @param {string} content
   * @returns {ValidationResult}
   */
  _validateEdl(content) {
    const errors = [];
    const warnings = [];

    if (!content.includes('TITLE:')) errors.push('Missing TITLE: header line');
    if (!content.includes('FCM:'))   warnings.push('Missing FCM: (Frame Count Mode) header');

    const lines = content.split('\n');
    const events = lines.filter((l) => /^\s*\d{3}\s/.test(l));

    if (events.length === 0) {
      warnings.push('No edit events found (expected lines starting with NNN)');
    }

    const seen = new Set();
    for (const evt of events) {
      const num = evt.trim().slice(0, 3);
      if (seen.has(num)) errors.push(`Duplicate event number: ${num}`);
      seen.add(num);
    }

    return { valid: errors.length === 0, errors, warnings };
  }
}
