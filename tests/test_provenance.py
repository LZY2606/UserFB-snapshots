# -*- coding: utf-8 -*-
#
# Tests for the optional provenance mode of podcastparser.
#
# Permission to use, copy, modify, and/or distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#

import glob
import io
import json
import os

import pytest

import podcastparser


FEED_URL = 'http://example.com/feed.xml'
ITUNES_NS = 'http://www.itunes.com/dtds/podcast-1.0.dtd'
EXT_NS = 'http://example.com/extensions'

CANDIDATE_KEYS = {
    'namespace', 'element', 'path', 'normalizer', 'attribute',
    'source_line', 'source_column', 'raw_value', 'value',
    'accepted', 'rejected_reason',
}


def parse(feed, **kwargs):
    return podcastparser.parse(FEED_URL, io.StringIO(feed), **kwargs)


def strip_provenance(result):
    return {key: value for key, value in result.items()
            if key != 'provenance'}


# ---------------------------------------------------------------------------
# Mode switching and parity with the classic API
# ---------------------------------------------------------------------------

def test_provenance_disabled_by_default():
    feed = '<rss><channel><title>T</title></channel></rss>'
    result = parse(feed)
    assert 'provenance' not in result
    assert strip_provenance(parse(feed, provenance=False)) == result
    assert strip_provenance(parse(feed, provenance=True)) == result


@pytest.mark.parametrize('rss_filename',
                         glob.glob(os.path.join('tests', 'data', '*.rss')))
def test_final_values_identical_to_classic_api(rss_filename):
    basename, _ = os.path.splitext(rss_filename)
    json_filename = basename + '.json'
    if not os.path.exists(json_filename):
        pytest.skip('no expected JSON for %s' % rss_filename)

    param_filename = basename + '.param.json'
    params = (json.load(open(param_filename))
              if os.path.exists(param_filename) else {})
    expected = json.load(open(json_filename))

    normalized = rss_filename
    if os.sep == '\\':
        normalized = normalized.replace(os.sep, '/')
    url = 'file://' + normalized

    classic = podcastparser.parse(url, open(rss_filename), **params)
    provenance = podcastparser.parse(url, open(rss_filename),
                                     provenance=True, **params)

    assert classic == expected
    assert strip_provenance(provenance) == classic
    assert len(provenance['provenance']['episodes']) == \
        len(provenance['episodes'])


def test_provenance_structure():
    feed = '''<rss xmlns:it="%s"><channel><title>T</title>
    <item><guid>g</guid><it:duration>00:20:00</it:duration>
    <enclosure url="a.mp3" type="audio/mpeg" length="10"/></item>
    </channel></rss>''' % ITUNES_NS
    result = parse(feed, provenance=True)
    provenance = result['provenance']

    assert set(provenance) == {'feed', 'episodes', 'extensions'}
    assert set(provenance['extensions']) == {'feed', 'episodes'}

    candidates = provenance['episodes'][0]['total_time']
    assert len(candidates) == 1
    candidate = candidates[0]
    assert set(candidate) == CANDIDATE_KEYS
    assert candidate['namespace'] == ITUNES_NS
    assert candidate['element'] == 'it:duration'
    assert candidate['path'] == 'rss/channel/item/itunes:duration'
    assert candidate['normalizer'] == 'parse_time'
    assert candidate['attribute'] is None
    assert candidate['raw_value'] == '00:20:00'
    assert candidate['value'] == 1200
    assert candidate['accepted'] is True
    assert candidate['rejected_reason'] is None
    assert candidate['source_line'] == 2
    assert candidate['source_column'] is not None


# ---------------------------------------------------------------------------
# Namespace handling: prefixes are irrelevant, URIs are not
# ---------------------------------------------------------------------------

def test_renamed_namespace_prefix_gives_same_result():
    template = '''<rss xmlns:%s="%s"><channel><title>T</title>
    <item><guid>g</guid><%s:duration>00:20:00</%s:duration>
    <enclosure url="a.mp3" type="audio/mpeg" length="10"/></item>
    </channel></rss>'''

    original = parse(template % ('it', ITUNES_NS, 'it', 'it'),
                     provenance=True)
    renamed = parse(template % ('zebra', ITUNES_NS, 'zebra', 'zebra'),
                    provenance=True)

    assert strip_provenance(renamed) == strip_provenance(original)

    original_candidate = original['provenance']['episodes'][0][
        'total_time'][0]
    renamed_candidate = renamed['provenance']['episodes'][0][
        'total_time'][0]
    assert original_candidate['namespace'] == ITUNES_NS
    assert renamed_candidate['namespace'] == ITUNES_NS
    assert renamed_candidate['element'] == 'zebra:duration'
    assert renamed_candidate['path'] == original_candidate['path']
    assert renamed_candidate['value'] == original_candidate['value']


def test_default_namespace_matches_prefixed_namespace():
    prefixed = parse(
        '<rss xmlns:it="%s"><channel><title>T</title>'
        '<item><guid>g</guid><it:duration>00:20:00</it:duration>'
        '<enclosure url="a.mp3" type="audio/mpeg" length="10"/>'
        '</item></channel></rss>' % ITUNES_NS, provenance=True)
    defaulted = parse(
        '<rss><channel><title>T</title>'
        '<item><guid>g</guid>'
        '<duration xmlns="%s">00:20:00</duration>'
        '<enclosure url="a.mp3" type="audio/mpeg" length="10"/>'
        '</item></channel></rss>' % ITUNES_NS, provenance=True)

    assert strip_provenance(defaulted) == strip_provenance(prefixed)
    assert defaulted['provenance']['episodes'][0]['total_time'][0][
        'namespace'] == ITUNES_NS


# ---------------------------------------------------------------------------
# Candidate ordering and rejection reasons
# ---------------------------------------------------------------------------

def test_swapped_element_order_is_stable():
    head = ('<rss xmlns:it="%s"><channel><title>T</title>'
            '<item><title>E</title><guid>g</guid>' % ITUNES_NS)
    tail = ('<enclosure url="a.mp3" type="audio/mpeg" length="10"/>'
            '</item></channel></rss>')
    first_summary = parse(head +
                          '<it:summary>SUMMARY</it:summary>'
                          '<description>DESCRIPTION</description>' + tail,
                          provenance=True)
    first_description = parse(head +
                              '<description>DESCRIPTION</description>'
                              '<it:summary>SUMMARY</it:summary>' + tail,
                              provenance=True)

    # Swapping the element order must not change the final value: both
    # documents describe the same episode and return the same result.
    assert strip_provenance(first_summary) == strip_provenance(
        first_description)
    assert first_summary['episodes'][0]['description'] == 'SUMMARY'

    for result, first_element in ((first_summary, 'it:summary'),
                                  (first_description, 'description')):
        candidates = result['provenance']['episodes'][0]['description']
        # Candidates are kept in document order, independently of which
        # source ended up winning.
        assert [c['element'] for c in candidates] == [
            c['element'] for c in
            sorted(candidates, key=lambda c: (c['source_line'],
                                              c['source_column']))]
        assert candidates[0]['element'] == first_element
        # The summary source always wins the final value and is accepted.
        assert any(c['accepted'] and c['value'] == 'SUMMARY'
                   for c in candidates)
        # Both sources are traceable through their namespace
        namespaces = {c['namespace'] for c in candidates}
        assert ITUNES_NS in namespaces and None in namespaces

    # When the RSS description appears after the summary, the non-overwriting
    # RSSItemDescription records an explicit rejection.
    rejected = [c for c in first_summary['provenance']['episodes'][0][
        'description'] if not c['accepted']]
    assert len(rejected) == 1 and rejected[0]['element'] == 'description'
    assert rejected[0]['rejected_reason']
    # When the RSS description appears first, both candidates are accepted
    # and the overwriting summary still ends up with the same final value.
    assert all(c['accepted'] for c in first_description['provenance'][
        'episodes'][0]['description'])


def test_overwrite_false_records_rejection():
    feed = '''<rss xmlns:it="%s"><channel>
    <description>primary</description>
    <it:summary>ignored</it:summary>
    </channel></rss>''' % ITUNES_NS
    result = parse(feed, provenance=True)
    candidates = result['provenance']['feed']['description']
    assert result['description'] == 'primary'
    assert [c['accepted'] for c in candidates] == [True, False]
    assert 'overwrite disabled' in candidates[1]['rejected_reason']


def test_invalid_values_are_rejected_with_reason():
    feed = '''<rss xmlns:it="%s"><channel><title>T</title>
    <itunes:explicit xmlns:it="%s">maybe</itunes:explicit>
    <item><title>E</title><guid>g</guid>
    <it:episode>not-a-number</it:episode>
    <it:episodeType>special</it:episodeType>
    <enclosure url="a.mp3" type="audio/mpeg" length="10"/>
    </item></channel></rss>''' % (ITUNES_NS, ITUNES_NS)
    result = parse(feed, provenance=True)

    explicit = result['provenance']['feed']['explicit'][0]
    assert explicit['accepted'] is False
    assert explicit['rejected_reason']

    episode_bucket = result['provenance']['episodes'][0]
    assert not episode_bucket['number'][0]['accepted']
    assert 'integer' in episode_bucket['number'][0]['rejected_reason']
    assert not episode_bucket['type'][0]['accepted']
    assert 'allowed set' in episode_bucket['type'][0]['rejected_reason']


# ---------------------------------------------------------------------------
# Blank values
# ---------------------------------------------------------------------------

def test_blank_values_are_recorded():
    feed = '''<rss><channel><title>T</title>
    <item><guid>g</guid><title>   </title>
    <duration xmlns="%s">  </duration>
    <enclosure url="a.mp3" type="audio/mpeg" length="10"/>
    </item></channel></rss>''' % ITUNES_NS
    result = parse(feed, provenance=True)
    episode = result['episodes'][0]

    title_candidate = result['provenance']['episodes'][0]['title'][0]
    assert title_candidate['raw_value'] == '   '
    assert title_candidate['value'] == ''
    assert title_candidate['accepted'] is True
    assert episode['title'] == ''

    duration_candidate = result['provenance']['episodes'][0][
        'total_time'][0]
    assert duration_candidate['raw_value'] == '  '
    assert duration_candidate['value'] == 0


# ---------------------------------------------------------------------------
# Enclosures
# ---------------------------------------------------------------------------

def test_duplicate_enclosures_both_become_candidates():
    feed = '''<rss><channel><title>T</title>
    <item><title>E</title><guid>g</guid>
    <enclosure url="a.mp3" type="audio/mpeg" length="10"/>
    <enclosure url="a.mp3" type="audio/mpeg" length="10"/>
    </item></channel></rss>'''
    result = parse(feed, provenance=True)

    candidates = result['provenance']['episodes'][0]['enclosures']
    assert len(candidates) == 2
    assert all(c['accepted'] for c in candidates)
    assert all(c['attribute'] is None for c in candidates)
    assert [c['value'] for c in candidates] == \
        result['episodes'][0]['enclosures']
    assert candidates[0]['raw_value'] == {
        'url': 'a.mp3', 'length': '10', 'type': 'audio/mpeg'}


def test_enclosure_without_url_is_rejected():
    feed = '''<rss><channel><title>T</title>
    <item><title>E</title><guid>g</guid>
    <enclosure type="audio/mpeg" length="10"/>
    </item></channel></rss>'''
    result = parse(feed, provenance=True)
    candidate = result['provenance']['episodes'][0]['enclosures'][0]
    assert candidate['accepted'] is False
    assert '@url' in candidate['rejected_reason']


# ---------------------------------------------------------------------------
# Relative URLs
# ---------------------------------------------------------------------------

def test_relative_urls_keep_raw_and_resolved_value():
    feed = '''<rss><channel>
    <image><url>img/cover.jpg</url></image>
    <item><title>E</title><guid>g</guid>
    <enclosure url="ep/a.mp3" type="audio/mpeg" length="1"/>
    </item></channel></rss>'''
    result = parse(feed, provenance=True)

    cover = result['provenance']['feed']['cover_url'][0]
    assert cover['raw_value'] == 'img/cover.jpg'
    assert cover['value'] == 'http://example.com/img/cover.jpg'
    assert cover['normalizer'].startswith('urljoin/')

    enclosure = result['provenance']['episodes'][0]['enclosures'][0]
    assert enclosure['raw_value']['url'] == 'ep/a.mp3'
    assert enclosure['value']['url'] == 'http://example.com/ep/a.mp3'


# ---------------------------------------------------------------------------
# Unknown extensions: recorded, isolated, bounded
# ---------------------------------------------------------------------------

def test_unknown_extensions_are_recorded_but_never_pollute():
    feed = '''<rss xmlns:foo="%s"><channel>
    <title>T</title><foo:title>FAKE TITLE</foo:title>
    <item><title>E</title><guid>g</guid>
    <foo:duration>999</foo:duration>
    <enclosure url="a.mp3" type="audio/mpeg" length="1"/>
    </item></channel></rss>''' % EXT_NS
    result = parse(feed, provenance=True)

    assert result['title'] == 'T'
    assert result['episodes'][0]['total_time'] == 0

    feed_ext = result['provenance']['extensions']['feed']
    assert [r['element'] for r in feed_ext] == ['foo:title']
    assert feed_ext[0]['namespace'] == EXT_NS
    assert feed_ext[0]['text'] == 'FAKE TITLE'
    assert feed_ext[0]['accepted'] is False
    assert feed_ext[0]['rejected_reason'] == \
        'no handler registered for namespace'

    episode_ext = result['provenance']['extensions']['episodes'][0]
    assert [r['element'] for r in episode_ext] == ['foo:duration']

    for bucket in result['provenance']['episodes'][0].values():
        assert all(c['namespace'] != EXT_NS for c in bucket)
    for bucket in result['provenance']['feed'].values():
        assert all(c['namespace'] != EXT_NS for c in bucket)


def test_deeply_nested_extension_is_bounded():
    depth = 500
    nested = ('<foo:bar xmlns:foo="%s">' % EXT_NS
              + ''.join('<foo:n%d>' % i for i in range(depth))
              + 'X' * (podcastparser.EXTENSION_TEXT_LIMIT * 2)
              + ''.join('</foo:n%d>' % i
                        for i in range(depth - 1, -1, -1))
              + '</foo:bar>')
    feed = ('<rss><channel><title>T</title>%s'
            '<item><title>E</title><guid>g</guid>'
            '<enclosure url="a.mp3" type="audio/mpeg" length="1"/>'
            '</item></channel></rss>') % nested

    result = parse(feed, provenance=True)
    record = result['provenance']['extensions']['feed'][0]
    assert record['namespace'] == EXT_NS
    assert record['depth'] == 3
    assert record['truncated'] is True
    assert len(record['text']) == podcastparser.EXTENSION_TEXT_LIMIT
    # The standard result is untouched and still complete
    assert result['title'] == 'T'
    assert len(result['episodes']) == 1


def test_extensions_are_split_between_feed_and_episode_scope():
    feed = '''<rss xmlns:foo="%s"><channel><title>T</title>
    <foo:channel-ext>a</foo:channel-ext>
    <item><title>E</title><guid>g</guid>
    <foo:item-ext>b</foo:item-ext>
    <enclosure url="a.mp3" type="audio/mpeg" length="1"/>
    </item></channel></rss>''' % EXT_NS
    result = parse(feed, provenance=True)

    assert [r['text'] for r in
            result['provenance']['extensions']['feed']] == ['a']
    assert [r['text'] for r in
            result['provenance']['extensions']['episodes'][0]] == ['b']


def test_inline_html_inside_known_element_is_not_an_extension():
    feed = '''<rss xmlns:content="http://purl.org/rss/1.0/modules/content/">
    <channel><title>T</title>
    <item><title>E</title><guid>g</guid>
    <content:encoded><h1>Header <em>inside</em> text</h1></content:encoded>
    <enclosure url="a.mp3" type="audio/mpeg" length="1"/>
    </item></channel></rss>'''
    result = parse(feed, provenance=True)

    # The HTML child elements have no namespace URI and must not show up as
    # unknown extensions.
    assert result['provenance']['extensions'] == {
        'feed': [], 'episodes': [[]]}
    # The standard parsing result is unchanged by provenance mode.
    assert strip_provenance(result) == parse(feed)


def test_extension_subtree_may_contain_no_namespace_children():
    feed = '''<rss xmlns:foo="%s"><channel><title>T</title>
    <foo:wrap><foo:inner><b>mix</b>tail</foo:inner></foo:wrap>
    </channel></rss>''' % EXT_NS
    result = parse(feed, provenance=True)

    records = result['provenance']['extensions']['feed']
    # Only the top-level extension element opens a record...
    assert [r['element'] for r in records] == ['foo:wrap']
    # ...and all nested text, including the no-namespace child, is captured.
    assert records[0]['text'] == 'mixtail'


# ---------------------------------------------------------------------------
# Extension handlers registered by namespace URI
# ---------------------------------------------------------------------------

def test_extension_handler_dispatched_by_namespace_uri():
    def handler(ctx):
        return {'element': ctx.element, 'text': ctx.text,
                'attrs': ctx.attrs, 'scope': ctx.scope}

    feed = '''<rss xmlns:zz="%s"><channel><title>T</title>
    <zz:one lang="en">hello</zz:one>
    <item><title>E</title><guid>g</guid>
    <enclosure url="a.mp3" type="audio/mpeg" length="1"/>
    </item></channel></rss>''' % EXT_NS
    result = parse(feed, provenance=True,
                   extension_handlers={EXT_NS: handler})

    record = result['provenance']['extensions']['feed'][0]
    assert record['accepted'] is True
    assert record['handler'] == 'handler'
    assert record['value'] == {
        'element': 'zz:one', 'text': 'hello',
        'attrs': {'lang': 'en'}, 'scope': 'feed'}


def test_global_handler_registry_ignores_prefixes():
    calls = []

    def handler(ctx):
        calls.append(ctx.element)
        return True

    podcastparser.register_extension_handler(EXT_NS, handler)
    try:
        result = parse(
            '<rss xmlns:anything="%s"><channel><title>T</title>'
            '<anything:one>x</anything:one></channel></rss>' % EXT_NS,
            provenance=True)
    finally:
        podcastparser.unregister_extension_handler(EXT_NS)

    assert calls == ['anything:one']
    assert result['provenance']['extensions']['feed'][0]['accepted'] is True


def test_handler_exception_only_affects_its_own_candidate():
    def bad_handler(ctx):
        raise RuntimeError('boom')

    def good_handler(ctx):
        return 'ok'

    feed = '''<rss xmlns:foo="%s" xmlns:bar="http://example.com/other">
    <channel><title>T</title>
    <foo:one>bad</foo:one><bar:two>good</bar:two>
    </channel></rss>''' % EXT_NS
    result = parse(feed, provenance=True,
                   extension_handlers={EXT_NS: bad_handler,
                                       'http://example.com/other': good_handler})

    records = result['provenance']['extensions']['feed']
    bad, good = records
    assert bad['accepted'] is False
    assert 'RuntimeError' in bad['rejected_reason']
    assert 'boom' in bad['rejected_reason']
    assert good['accepted'] is True
    assert good['value'] == 'ok'
    # Standard fields and the rest of the parse are unaffected
    assert result['title'] == 'T'


def test_normalizer_exception_only_rejects_that_candidate():
    target = podcastparser.MAPPING['rss/channel/item/itunes:duration']
    original = target.filter_func

    def exploding_filter(text):
        raise RuntimeError('normalizer exploded')

    target.filter_func = exploding_filter
    try:
        feed = '''<rss xmlns:it="%s"><channel><title>T</title>
        <item><title>E</title><guid>g</guid>
        <it:duration>00:20:00</it:duration>
        <enclosure url="a.mp3" type="audio/mpeg" length="10"/>
        </item></channel></rss>''' % ITUNES_NS
        result = parse(feed, provenance=True)
    finally:
        target.filter_func = original

    candidate = result['provenance']['episodes'][0]['total_time'][0]
    assert candidate['accepted'] is False
    assert 'normalizer exploded' in candidate['rejected_reason']
    assert result['episodes'][0]['title'] == 'E'


def test_enclosure_target_exception_only_rejects_that_candidate():
    original_parse_length = podcastparser.parse_length
    calls = {'n': 0}

    def failing_parse_length(text):
        calls['n'] += 1
        if calls['n'] == 1:
            raise RuntimeError('length exploded')
        return original_parse_length(text)

    podcastparser.parse_length = failing_parse_length
    try:
        feed = '''<rss><channel><title>T</title>
        <item><title>E</title><guid>g</guid>
        <enclosure url="a.mp3" type="audio/mpeg" length="10"/>
        <enclosure url="b.mp3" type="audio/mpeg" length="20"/>
        </item></channel></rss>'''
        result = parse(feed, provenance=True)
    finally:
        podcastparser.parse_length = original_parse_length

    candidates = result['provenance']['episodes'][0]['enclosures']
    assert len(candidates) == 2
    assert candidates[0]['accepted'] is False
    assert 'length exploded' in candidates[0]['rejected_reason']
    assert candidates[1]['accepted'] is True
    # The second enclosure still made it into the standard result.
    assert [e['url'] for e in result['episodes'][0]['enclosures']] == \
        ['http://example.com/b.mp3']


# ---------------------------------------------------------------------------
# Episode lifecycle
# ---------------------------------------------------------------------------

def test_max_episodes_keeps_provenance_aligned():
    params_path = os.path.join('tests', 'data', 'max_episodes.param.json')
    params = json.load(open(params_path))
    feed_path = os.path.join('tests', 'data', 'max_episodes.rss')
    result = podcastparser.parse(
        'file://' + feed_path, open(feed_path),
        provenance=True, **params)
    assert len(result['episodes']) == params['max_episodes']
    assert len(result['provenance']['episodes']) == len(result['episodes'])
    assert len(result['provenance']['extensions']['episodes']) == \
        len(result['episodes'])


def test_dropped_episode_does_not_leave_provenance_behind():
    feed_path = os.path.join('tests', 'data',
                             'no_title_multi_enclosure.rss')
    result = podcastparser.parse(
        'file://' + feed_path, open(feed_path), provenance=True)
    assert result['episodes'] == []
    assert result['provenance']['episodes'] == []
    assert result['provenance']['extensions']['episodes'] == []


def test_invalid_feed_still_fails_in_provenance_mode():
    with pytest.raises(podcastparser.FeedParseError):
        parse('<html><body/></html>', provenance=True)
