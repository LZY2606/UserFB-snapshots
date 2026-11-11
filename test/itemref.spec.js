/* eslint-env jest */

const parser = require('../src');

describe('itemref', () => {
  test('parses itemrefs', () => {
    const html =
      '<div>' +
      '  <div id="ref1">' +
      '    <div itemprop="name">Jan</div>' +
      '  </div>' +
      '  <div id="ref2" itemprop="name">Potoms</div>' +
      '  <div itemscope itemtype="http://schema.org/Person" itemref="ref1 ref2"></div>' +
      '</div>';
    const result = parser.toJson(html);
    expect(Array.isArray(result.items)).toBe(true);
    expect(result.items.length).toBe(1);
    expect(result.items[0].properties).toEqual({
      name: ['Jan', 'Potoms']
    });
  });

  test('parses multiple items with the same ref', () => {
    const html =
      '<div id="ref" itemprop="name">Jan</div>' +
      '<div itemscope itemtype="http://schema.org/Person" itemref="ref"></div>' +
      '<div itemscope itemtype="http://schema.org/Person" itemref="ref"></div>';
    const result = parser.toJson(html);
    expect(Array.isArray(result.items)).toBe(true);
    expect(result.items.length).toBe(2);
    expect(result.items[0].properties).toEqual({
      name: ['Jan']
    });
    expect(result.items[1].properties).toEqual({
      name: ['Jan']
    });
  });

  test.skip('parses nested reffed items', function () {
    const html =
      '<div itemscope itemid="#item1">' +
      '  <div id="ref" itemprop="property1" itemscope itemid="#sub-item1"></div>' +
      '  <div itemprop="property2" itemscope itemref="ref" itemid="#sub-item2">' +
      '  </div>' +
      '</div>';
    const result = parser.toJson(html);
    expect(Array.isArray(result.items)).toBe(true);
    expect(result.items.length).toBe(1);
    expect(result).toHaveProperty('.items[0].properties.property1[0].id', '#sub-item1');
    expect(result).toHaveProperty('.items[0].properties.property2[0].id', '#sub-item2');
    expect(result).toHaveProperty(
      '.items[0].properties.property2[0].properties.property1[0].id',
      '#sub-item1'
    );
  });

  test('handle top-level circular structure', () => {
    const html =
      '<div id="ref">' +
      '  <div itemscope itemref="ref">' +
      '    <div itemprop="name">Jan</div>' +
      '  </div>' +
      '</div>';
    const result = parser.toJson(html);
    expect(Array.isArray(result.items)).toBe(true);
    expect(result.items.length).toBe(1);
    expect(result.items[0].properties).toEqual({
      name: ['Jan']
    });
  });

  test('handle nested item circular structure', () => {
    const html =
      '<div itemscope>' +
      '  <div id="ref">' +
      '    <div itemprop="name">Jan</div>' +
      '    <div itemprop="friend" itemscope>' +
      '      <div itemprop="name">Other Jan</div>' +
      '      <div itemprop="friend" itemscope itemref="ref"></div>' +
      '    </div>' +
      '  </div>' +
      '</div>';
    const result = parser.toJson(html);
    expect(Array.isArray(result.items)).toBe(true);
    expect(result.items.length).toBe(1);
    expect(result.items[0].properties).toEqual({
      name: ['Jan'],
      friend: [{
        properties: {
          name: ['Other Jan'],
          friend: ['ERROR']
        }
      }]
    });
  });

  test('shares one item across several itemprop tokens without duplicating its fields', () => {
    const html =
      '<div itemscope itemtype="https://schema.org/Product">' +
      '  <div itemscope itemprop="seller brand" itemtype="https://schema.org/Organization">' +
      '    <span itemprop="name">ACME</span>' +
      '  </div>' +
      '</div>';
    for (const strict of [true, false]) {
      const product = parser.toJson(html, { strict }).items[0];
      const seller = product.properties.seller[0];
      // both properties point at the very same entry
      expect(seller).toBe(product.properties.brand[0]);
      // the single <span> is counted once, the type is emitted once
      expect(seller.properties.name).toEqual(['ACME']);
      expect(seller.type).toEqual(['https://schema.org/Organization']);
    }
  });

  test('keeps real repeated values while sharing a referenced item', () => {
    const html =
      '<div itemscope itemtype="https://schema.org/Product">' +
      '  <div itemscope itemprop="seller brand" itemtype="https://schema.org/Organization">' +
      '    <span itemprop="name">ACME</span><span itemprop="name">ACME</span>' +
      '  </div>' +
      '</div>';
    const seller = parser.toJson(html, { strict: true }).items[0].properties.seller[0];
    expect(seller.properties.name).toEqual(['ACME', 'ACME']);
  });

  test('inherits each parent vocabulary once on a shared typeless item', () => {
    const html =
      '<div itemscope itemtype="https://a.test/Owner" itemref="shared"></div>' +
      '<div itemscope itemtype="https://b.test/Owner" itemref="shared"></div>' +
      '<span id="shared" itemscope itemprop="node"><span itemprop="name">N</span></span>';
    const items = parser.toJson(html, { strict: true }).items;
    expect(items.length).toBe(2);
    const first = items[0].properties.node[0];
    const second = items[1].properties.node[0];
    // one shared entry, but a predicate for every inherited vocabulary
    expect(first).toBe(second);
    expect(first.properties).toEqual({
      'https://a.test/name': ['N'],
      'https://b.test/name': ['N']
    });
  });

  test('does not merge two independent items that happen to have identical content', () => {
    const html =
      '<div itemscope itemtype="https://schema.org/Product">' +
      '  <div itemscope itemprop="seller" itemtype="https://schema.org/Organization">' +
      '    <span itemprop="name">ACME</span>' +
      '  </div>' +
      '  <div itemscope itemprop="brand" itemtype="https://schema.org/Organization">' +
      '    <span itemprop="name">ACME</span>' +
      '  </div>' +
      '</div>';
    const product = parser.toJson(html, { strict: true }).items[0];
    const seller = product.properties.seller[0];
    const brand = product.properties.brand[0];
    // distinct itemscope elements stay distinct entries even if equal
    expect(seller).not.toBe(brand);
    expect(seller.properties.name).toEqual(['ACME']);
    expect(brand.properties.name).toEqual(['ACME']);
    expect(seller.type).toEqual(['https://schema.org/Organization']);
    expect(brand.type).toEqual(['https://schema.org/Organization']);
  });

  test('still throws on an ancestral itemref cycle in strict mode', () => {
    const html =
      '<main itemscope itemtype="https://schema.org/Thing" itemref="a"></main>' +
      '<div id="a" itemscope itemprop="next" itemref="b"><span itemprop="name">A</span></div>' +
      '<div id="b" itemscope itemprop="next" itemref="a"></div>';
    expect(() => parser.toJson(html, { strict: true })).toThrow(/Cyclic structure/);
  });
});
