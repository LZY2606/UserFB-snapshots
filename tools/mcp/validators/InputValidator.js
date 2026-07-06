/**
 * @module InputValidator
 * Schema-level validation for MCP tool input arguments.
 *
 * Keeps validation rules co-located with the schema rather than
 * scattered across handler functions.
 *
 * @example
 * InputValidator.assert(args, {
 *   projectId: { type: 'string', required: true },
 *   fps:       { type: 'number', min: 1, max: 240 },
 * });
 */

export class InputValidator {
  /**
   * Validate `args` against `schema`.
   *
   * @param {object} args
   * @param {Record<string, FieldRule>} schema
   * @returns {{ valid: boolean, errors: string[] }}
   *
   * @typedef {{ type?: string, required?: boolean, enum?: any[],
   *             min?: number, max?: number, minLength?: number }} FieldRule
   */
  static validate(args, schema) {
    const errors = [];

    for (const [field, rules] of Object.entries(schema)) {
      const value = args?.[field];
      const missing = value === undefined || value === null || value === '';

      if (rules.required && missing) {
        errors.push(`"${field}" is required`);
        continue;
      }

      if (missing) continue;

      if (rules.type === 'string' && typeof value !== 'string') {
        errors.push(`"${field}" must be a string, got ${typeof value}`);
        continue;
      }
      if (rules.type === 'number' && typeof value !== 'number') {
        errors.push(`"${field}" must be a number, got ${typeof value}`);
        continue;
      }
      if (rules.type === 'boolean' && typeof value !== 'boolean') {
        errors.push(`"${field}" must be a boolean, got ${typeof value}`);
        continue;
      }

      if (rules.enum && !rules.enum.includes(value)) {
        errors.push(`"${field}" must be one of [${rules.enum.join(', ')}], got "${value}"`);
      }

      if (rules.type === 'number') {
        if (rules.min !== undefined && value < rules.min) {
          errors.push(`"${field}" must be >= ${rules.min}, got ${value}`);
        }
        if (rules.max !== undefined && value > rules.max) {
          errors.push(`"${field}" must be <= ${rules.max}, got ${value}`);
        }
      }

      if (rules.type === 'string' && rules.minLength !== undefined && value.length < rules.minLength) {
        errors.push(`"${field}" must be at least ${rules.minLength} characters long`);
      }
    }

    return { valid: errors.length === 0, errors };
  }

  /**
   * Validate and throw on failure.
   *
   * @param {object} args
   * @param {Record<string, FieldRule>} schema
   * @throws {Error}
   */
  static assert(args, schema) {
    const { valid, errors } = InputValidator.validate(args, schema);
    if (!valid) {
      throw new Error(`Invalid tool arguments: ${errors.join('; ')}`);
    }
  }
}
