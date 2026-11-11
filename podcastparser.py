# -*- coding: utf-8 -*-
#
# Podcastparser: A simple, fast and efficient podcast parser
# Copyright (c) 2012, 2013, 2014, 2018, 2020 Thomas Perl <m@thp.io>
# Copyright (c) 2016, 2017, 2018, 2019, 2020 Eric Le Lay <elelay@macports.org>
# Copyright (c) 2020 E.S. Rosenberg <es.rosenberg+openu@gmail.com>
#
# Permission to use, copy, modify, and/or distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
# REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
# AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
# INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
# LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
# OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
# PERFORMANCE OF THIS SOFTWARE.
#

""" Simplified, fast RSS parser """

# Will be parsed by setup.py to determine package metadata
__author__ = 'Thomas Perl <m@thp.io>'
__version__ = '0.6.11'
__website__ = 'http://gpodder.org/podcastparser/'
__license__ = 'ISC License'

from xml import sax

import re
import os
import time

from html.entities import entitydefs

from urllib import parse as urlparse
from email.utils import mktime_tz, parsedate_tz

import logging
logger = logging.getLogger(__name__)


# Sentinel for "no raw value / no value" (as opposed to None or '')
_UNSET = object()


def strip_filter(value):
    """Default text filter: trim surrounding whitespace."""
    return value.strip()

# Maximum amount of extension text kept in a provenance record, so a
# malicious extension cannot make the result grow without bounds
EXTENSION_TEXT_LIMIT = 10000


# ---------------------------------------------------------------------------
# Provenance support
# ---------------------------------------------------------------------------

class Candidate(object):
    """A single candidate value considered while building a field.

    Candidate objects are only created when provenance mode is enabled.
    They are converted to plain dicts in the parsing result, but the class
    is part of the public API so callers can register extension handlers
    and inspect records programmatically.
    """

    def __init__(self, namespace_uri, element, path, normalizer=None,
                 attribute=None, value=_UNSET, raw_value=_UNSET,
                 source_line=None, source_column=None):
        # Namespace URI the element/attribute belongs to. This is the real
        # XML namespace, independent of the prefix chosen by the document.
        self.namespace_uri = namespace_uri
        # Element name as written in the document (prefix included, if any)
        self.element = element
        # Normalized MAPPING path of the element, e.g.
        # 'rss/channel/item/itunes:duration'
        self.path = path
        # Name of the normalizer applied to the raw value, if any
        self.normalizer = normalizer
        # Attribute name when the candidate comes from an attribute
        self.attribute = attribute
        # The value as it would be (or was) written into the result
        self.value = value
        # The raw value found in the document (text or attribute value)
        self.raw_value = raw_value
        # Position of the element in the source document
        self.source_line = source_line
        self.source_column = source_column
        # None means the candidate was accepted; otherwise a human readable
        # explanation of why it did not make it into the field
        self.rejected_reason = None

    @property
    def accepted(self):
        return self.rejected_reason is None

    def as_dict(self):
        result = {
            'namespace': self.namespace_uri,
            'element': self.element,
            'path': self.path,
            'normalizer': self.normalizer,
            'attribute': self.attribute,
            'source_line': self.source_line,
            'source_column': self.source_column,
            'raw_value': None if self.raw_value is _UNSET else self.raw_value,
            'value': None if self.value is _UNSET else self.value,
            'accepted': self.accepted,
            'rejected_reason': self.rejected_reason,
        }
        return result


class ExtensionContext(object):
    """Context passed to a registered extension handler.

    Handlers only get access to the element they were registered for, never
    to the standard feed/episode fields, so an extension cannot pollute the
    regular parsing result. Values returned by the handler are stored in the
    extension's provenance record.
    """

    def __init__(self, scope, namespace_uri, element, path, attrs, text,
                 base, source_line, source_column, depth):
        self.scope = scope
        self.namespace_uri = namespace_uri
        self.element = element
        self.path = path
        self.attrs = dict(attrs)
        self.text = text
        self.base = base
        self.source_line = source_line
        self.source_column = source_column
        self.depth = depth


# Extension handlers registered process-wide, keyed by namespace URI (never
# by prefix). Per-parse handlers are merged on top of this registry.
_EXTENSION_HANDLERS = {}


def register_extension_handler(namespace_uri, handler):
    """Register an extension handler for a namespace URI.

    The handler is called with an :class:`ExtensionContext` for every
    top-level element of the given namespace that podcastparser does not
    handle itself. Registration is by namespace URI, so renaming the prefix
    in a feed does not affect dispatch.
    """
    if not isinstance(namespace_uri, str) or not namespace_uri:
        raise ValueError('Extension handlers must be registered by namespace URI')
    _EXTENSION_HANDLERS[namespace_uri] = handler


def unregister_extension_handler(namespace_uri):
    """Remove a previously registered extension handler."""
    _EXTENSION_HANDLERS.pop(namespace_uri, None)


class _ProvenanceRecorder(object):
    """Collects candidates and unknown-extension records for one parse run."""

    def __init__(self, extension_handlers=None):
        # key -> list[Candidate], feed-level fields
        self.feed_candidates = {}
        # one dict (key -> list[Candidate]) per episode, aligned with
        # handler.episodes (entries are pushed/popped together)
        self.episode_candidates = []
        self.feed_extensions = []
        self.episode_extensions = []
        handlers = dict(_EXTENSION_HANDLERS)
        if extension_handlers:
            handlers.update(extension_handlers)
        self.extension_handlers = handlers

    def begin_episode(self):
        self.episode_candidates.append({})
        self.episode_extensions.append([])

    def drop_episode(self):
        self.episode_candidates.pop()
        self.episode_extensions.pop()

    def trim_episodes(self, count):
        del self.episode_candidates[count:]
        del self.episode_extensions[count:]

    def candidates_for(self, scope, key):
        bucket = self.feed_candidates if scope == 'feed' else self.episode_candidates[-1]
        return bucket.setdefault(key, [])

    def extensions_for(self, scope):
        if scope == 'feed':
            return self.feed_extensions
        return self.episode_extensions[-1]

    def as_dict(self):
        return {
            'feed': {key: [c.as_dict() for c in candidates]
                     for key, candidates in self.feed_candidates.items()},
            'episodes': [
                {key: [c.as_dict() for c in candidates]
                 for key, candidates in bucket.items()}
                for bucket in self.episode_candidates
            ],
            'extensions': {
                'feed': list(self.feed_extensions),
                'episodes': [list(entries) for entries in self.episode_extensions],
            },
        }


class Target(object):
    WANT_TEXT = False

    def __init__(self, key=None, filter_func=strip_filter, overwrite=True):
        self.key = key
        self.filter_func = filter_func
        self.overwrite = overwrite

    @property
    def normalizer_name(self):
        filter_func = getattr(self, 'filter_func', None)
        if filter_func is None:
            return None
        return getattr(filter_func, '__name__', None)

    def start(self, handler, attrs):
        pass

    def end(self, handler, text):
        pass


class RSS(Target):
    def start(self, handler, attrs):
        if 'xml:base' in attrs.keys():
            handler.set_base(attrs.get('xml:base'))


class PodcastItem(Target):
    def end(self, handler, text):
        by_published = lambda entry: entry.get('published')
        order = 'type' not in handler.data or handler.data['type'] != 'serial'
        handler.sort_episodes(by_published, reverse=order)
        if handler.max_episodes:
            episodes = handler.data['episodes'][:handler.max_episodes]
            handler.data['episodes'] = episodes
            if handler.recorder is not None:
                handler.recorder.trim_episodes(handler.max_episodes)


class PodcastAttr(Target):
    WANT_TEXT = True

    def end(self, handler, text):
        handler.set_podcast_attr(
            self.key, self.filter_func(text),
            overwrite=self.overwrite, raw=text,
            normalizer=self.normalizer_name)


class PodcastAttrList(Target):
    WANT_TEXT = True

    def end(self, handler, text):
        if not self.overwrite and handler.get_podcast_attr(self.key):
            handler.reject_podcast_attr(
                self.key, text, self.normalizer_name,
                'overwrite disabled: field already set')
            return
        handler.set_podcast_attr(
            self.key, self.filter_func(text).split(', '),
            overwrite=self.overwrite, raw=text,
            normalizer=self.normalizer_name + '/split')


class PodcastAttrType(Target):
    WANT_TEXT = True

    def end(self, handler, text):
        if not self.overwrite and handler.get_podcast_attr(self.key):
            handler.reject_podcast_attr(
                self.key, text, self.normalizer_name,
                'overwrite disabled: field already set')
            return
        value = self.filter_func(text)
        if value in ('episodic', 'serial'):
            handler.set_podcast_attr(self.key, value, raw=text,
                                     normalizer=self.normalizer_name)
        else:
            handler.reject_podcast_attr(
                self.key, text, self.normalizer_name,
                'value not in allowed set (episodic, serial)')


class PodcastAttrRelativeLink(PodcastAttr):
    @property
    def normalizer_name(self):
        base = super(PodcastAttrRelativeLink, self).normalizer_name
        return 'urljoin/' + (base or 'strip')

    def end(self, handler, text):
        raw = text
        text = urlparse.urljoin(handler.base, text.strip())
        handler.set_podcast_attr(
            self.key, self.filter_func(text), overwrite=self.overwrite,
            raw=raw, normalizer=self.normalizer_name)


class PodcastAttrFromHref(Target):
    ATTRIBUTE = 'href'

    @property
    def normalizer_name(self):
        return 'urljoin/' + getattr(self.filter_func, '__name__', 'strip')

    def start(self, handler, attrs):
        if not self.overwrite and handler.get_podcast_attr(self.key):
            handler.reject_podcast_attr(
                self.key, attrs.get(self.ATTRIBUTE), self.normalizer_name,
                'overwrite disabled: field already set',
                attribute=self.ATTRIBUTE)
            return
        value = attrs.get(self.ATTRIBUTE)
        if not value:
            handler.reject_podcast_attr(
                self.key, value, self.normalizer_name,
                'missing or empty attribute "@%s"' % self.ATTRIBUTE,
                attribute=self.ATTRIBUTE)
            return
        raw = value
        value = urlparse.urljoin(handler.base, value)
        handler.set_podcast_attr(self.key, self.filter_func(value), raw=raw,
                                 normalizer=self.normalizer_name,
                                 attribute=self.ATTRIBUTE)


class PodcastAttrFromUrl(PodcastAttrFromHref):
    ATTRIBUTE = 'url'


class EpisodeItem(Target):
    def start(self, handler, attrs):
        handler.add_episode()

    def end(self, handler, text):
        handler.validate_episode()


class EpisodeAttr(Target):
    WANT_TEXT = True

    def end(self, handler, text):
        if not self.overwrite and handler.get_episode_attr(self.key):
            handler.reject_episode_attr(
                self.key, text, self.normalizer_name,
                'overwrite disabled: field already set')
            return
        value = self.filter_func(text)
        if value != '' or handler.get_episode_attr(self.key) is None:
            handler.set_episode_attr(
                self.key, value, raw=text,
                normalizer=self.normalizer_name)
        else:
            handler.reject_episode_attr(
                self.key, text, self.normalizer_name,
                'blank value and field already set')


class EpisodeAttrRelativeLink(EpisodeAttr):
    @property
    def normalizer_name(self):
        base = super(EpisodeAttrRelativeLink, self).normalizer_name
        return 'urljoin/' + (base or 'strip')

    def end(self, handler, text):
        raw = text
        text = urlparse.urljoin(handler.base, text)
        if not self.overwrite and handler.get_episode_attr(self.key):
            handler.reject_episode_attr(
                self.key, raw, self.normalizer_name,
                'overwrite disabled: field already set')
            return
        value = self.filter_func(text)
        if value != '' or handler.get_episode_attr(self.key) is None:
            handler.set_episode_attr(
                self.key, value, raw=raw,
                normalizer=self.normalizer_name)
        else:
            handler.reject_episode_attr(
                self.key, raw, self.normalizer_name,
                'blank value and field already set')

class EpisodeGuid(EpisodeAttr):
    def start(self, handler, attrs):
        if attrs.get('isPermaLink', 'true').lower() == 'true':
            handler.set_episode_attr('_guid_is_permalink', True)
        else:
            handler.set_episode_attr('_guid_is_permalink', False)

    def end(self, handler, text):
        def filter_func(guid):
            guid = guid.strip()
            if handler.get_episode_attr('_guid_is_permalink'):
                return urlparse.urljoin(handler.base, guid)
            return guid

        filter_func.__name__ = 'guid_permalink_urljoin'
        self.filter_func = filter_func
        EpisodeAttr.end(self, handler, text)


class EpisodeAttrFromHref(Target):
    ATTRIBUTE = 'href'

    @property
    def normalizer_name(self):
        return 'urljoin/' + getattr(self.filter_func, '__name__', 'strip')

    def start(self, handler, attrs):
        value = attrs.get(self.ATTRIBUTE)
        if not value:
            handler.reject_episode_attr(
                self.key, value, self.normalizer_name,
                'missing or empty attribute "@%s"' % self.ATTRIBUTE,
                attribute=self.ATTRIBUTE)
            return
        raw = value
        value = urlparse.urljoin(handler.base, value)
        handler.set_episode_attr(self.key, self.filter_func(value), raw=raw,
                                 normalizer=self.normalizer_name,
                                 attribute=self.ATTRIBUTE)


class EpisodeAttrFromUrl(EpisodeAttrFromHref):
    ATTRIBUTE = 'url'


class EpisodeAttrSeason(EpisodeAttr):
    def end(self, handler, text):
        try:
            episode_season = int(text)
        except ValueError:
            episode_season = 0
        handler.set_episode_attr(self.key, episode_season, raw=text,
                                 normalizer='int')


class EpisodeAttrNumber(EpisodeAttr):
    def end(self, handler, text):
        value = self.filter_func(text)
        try:
            episode_num = int(value)
        except ValueError:
            handler.reject_episode_attr(
                self.key, text, 'int', 'not an integer')
            return
        if episode_num > 0:
            handler.set_episode_attr(self.key, episode_num, raw=text,
                                     normalizer='int')
        else:
            handler.reject_episode_attr(
                self.key, text, 'int', 'number must be positive')


class EpisodeAttrType(EpisodeAttr):
    def end(self, handler, text):
        value = self.filter_func(text).lower()
        if value in ('full', 'trailer', 'bonus'):
            handler.set_episode_attr(self.key, value, raw=text,
                                     normalizer=self.normalizer_name)
        else:
            handler.reject_episode_attr(
                self.key, text, self.normalizer_name,
                'value not in allowed set (full, trailer, bonus)')


class Enclosure(Target):
    NORMALIZER = 'parse_url/parse_length/parse_type'

    def __init__(self, file_size_attribute):
        Target.__init__(self)
        self.file_size_attribute = file_size_attribute

    @property
    def normalizer_name(self):
        return self.NORMALIZER

    def start(self, handler, attrs):
        url = attrs.get('url')
        if url is None:
            handler.reject_episode_attr(
                'enclosures', None, self.normalizer_name,
                'missing attribute "@url"', attribute='url')
            return

        normalized_url = parse_url(urlparse.urljoin(handler.base, url.lstrip()))
        file_size = parse_length(attrs.get(self.file_size_attribute))
        mime_type = parse_type(attrs.get('type'))

        handler.add_enclosure(normalized_url, file_size, mime_type,
                              raw_url=url,
                              raw_length=attrs.get(self.file_size_attribute),
                              raw_type=attrs.get('type'),
                              normalizer=self.normalizer_name)


class AtomLink(Target):
    NORMALIZER = 'parse_url/parse_length/parse_type'

    @property
    def normalizer_name(self):
        return self.NORMALIZER

    def start(self, handler, attrs):
        rel = attrs.get('rel', 'alternate')
        href = attrs.get('href')
        url = parse_url(urlparse.urljoin(handler.base, href))
        mime_type = parse_type(attrs.get('type', 'text/html'))
        file_size = parse_length(attrs.get('length', '0'))

        if rel == 'enclosure':
            handler.add_enclosure(url, file_size, mime_type,
                                  raw_url=href,
                                  raw_length=attrs.get('length', '0'),
                                  raw_type=attrs.get('type', 'text/html'),
                                  normalizer=self.normalizer_name)
        elif rel == 'payment':
            handler.set_episode_attr('payment_url', url, raw=href,
                                     normalizer=self.normalizer_name,
                                     attribute='href')
        elif mime_type == 'text/html':
            if rel in ('self', 'alternate'):
                if not handler.get_episode_attr('link'):
                    handler.set_episode_attr('link', url, raw=href,
                                             normalizer=self.normalizer_name,
                                             attribute='href')
                else:
                    handler.reject_episode_attr(
                        'link', href, self.normalizer_name,
                        'field already set', attribute='href')


class PodcastAtomLink(AtomLink):
    def start(self, handler, attrs):
        rel = attrs.get('rel', 'alternate')
        href = attrs.get('href')
        url = parse_url(urlparse.urljoin(handler.base, href))
        mime_type = parse_type(attrs.get('type'))

        # RFC 5005 (http://podlove.org/paged-feeds/)
        if rel == 'first':
            handler.set_podcast_attr('paged_feed_first', url, raw=href,
                                     normalizer=self.normalizer_name,
                                     attribute='href')
        elif rel == 'next':
            handler.set_podcast_attr('paged_feed_next', url, raw=href,
                                     normalizer=self.normalizer_name,
                                     attribute='href')
        elif rel == 'payment':
            handler.set_podcast_attr('payment_url', url, raw=href,
                                     normalizer=self.normalizer_name,
                                     attribute='href')
        elif mime_type == 'text/html':
            if rel in ('self', 'alternate'):
                handler.set_podcast_attr('link', url, raw=href,
                                         normalizer=self.normalizer_name,
                                         attribute='href')


class AtomContent(Target):
    WANT_TEXT = True

    def __init__(self):
        self._want_content = False

    def start(self, handler, attrs):
        self._mime_type = attrs.get('type', 'text')

    @property
    def normalizer_name(self):
        return 'atom-content-type/' + getattr(self, '_mime_type', 'text')

    def end(self, handler, text):
        if self._mime_type == 'html':
            handler.set_episode_attr('description_html', text, raw=text,
                                     normalizer=self.normalizer_name)
        elif self._mime_type == 'text':
            handler.set_episode_attr('description',
                                     squash_whitespace_not_nl(text),
                                     raw=text,
                                     normalizer=self.normalizer_name)


class RSSItemDescription(Target):
    """
    RSS 2.0 almost encourages to put html content in item/description
    but content:encoded is the better source of html content and itunes:summary
    is known to contain the short textual description of the item.
    So use a heuristic to attribute text to either description or description_html,
    without overriding existing values.
    """
    WANT_TEXT = True

    def __init__(self):
        self._want_content = False

    @property
    def normalizer_name(self):
        return 'is_html/squash_whitespace_not_nl'

    def end(self, handler, text):
        if is_html(text):
            if not handler.get_episode_attr('description_html'):
                handler.set_episode_attr('description_html', text.strip(),
                                         raw=text,
                                         normalizer=self.normalizer_name)
            else:
                handler.reject_episode_attr(
                    'description_html', text, self.normalizer_name,
                    'field already set')
        elif not handler.get_episode_attr('description'):
            # don't overwrite itunes:summary?
            handler.set_episode_attr('description',
                                     squash_whitespace_not_nl(text),
                                     raw=text,
                                     normalizer=self.normalizer_name)
        else:
            handler.reject_episode_attr(
                'description', text, self.normalizer_name,
                'field already set')


class PodloveChapters(Target):
    SUPPORTED_VERSIONS = ('1.1', '1.2')

    def start(self, handler, attrs):
        version = attrs.get('version', '1.1')
        if version not in PodloveChapters.SUPPORTED_VERSIONS:
            logger.warning('Possible incompatible chapters version: %s', version)


class PodloveChapter(Target):
    @property
    def normalizer_name(self):
        return 'parse_time'

    def start(self, handler, attrs):
        # Both the start and title attributes are mandatory
        if attrs.get('start') is None or attrs.get('title') is None:
            logger.warning('Invalid chapter (missing start and/or and title)')
            handler.reject_episode_attr(
                'chapters', None, self.normalizer_name,
                'missing required attribute "@start" and/or "@title"')
            return

        chapter = {
            'start': parse_time(attrs.get('start')),
            'title': attrs.get('title'),
        }

        for optional in ('href', 'image'):
            value = attrs.get(optional)
            if value:
                chapter[optional] = value

        handler.append_chapter(chapter, raw=dict(attrs),
                               normalizer=self.normalizer_name)


class EpisodePersonAttr(Target):
    WANT_TEXT = True

    def start(self, handler, attrs):
        if not handler.get_episode_attr("persons"):
            handler.add_episode_persons()
        person = {
            "name": None,
            "role": "host",
            "group": "cast",
            "href": None,
            "img": None,
        }
        for optional in ("group", "role", "href", "img"):
            value = attrs.get(optional)
            if value:
                if optional in ("role", "group"):
                    value = value.lower()
                person[optional] = value
        handler.append_episode_person(person, raw=dict(attrs))

    def end(self, handler, text):
        handler.get_episode_attr("persons")[-1]["name"] = text
        handler.note_episode_person_name(text)


class ItunesOwnerAttr(Target):
    WANT_TEXT = True

    def end(self, handler, text):
        if not self.overwrite and handler.get_podcast_attr(self.key):
            handler.reject_podcast_attr(
                self.key, text, self.normalizer_name,
                'overwrite disabled: field already set')
            return
        value = self.filter_func(text)
        handler.append_itunes_owner(self.key, value, raw=text,
                                    normalizer=self.normalizer_name)


class ItunesCategoryAttr(Target):
    def start(self, handler, attrs):
        # Let's use an empty string as a fallback for first-level categories
        # in case there is a valid sub-category.
        value = attrs.get('text', '')
        handler.append_itunes_category(self.key, self.filter_func(value),
                                       raw=value, attribute='text')


class ItunesSubCategoryAttr(Target):
    def start(self, handler, attrs):
        value = attrs.get('text')
        if not value:
            handler.reject_podcast_attr(
                self.key, value, self.normalizer_name,
                'missing or empty attribute "@text"', attribute='text')
            return
        handler.append_itunes_subcategory(self.key, self.filter_func(value),
                                          raw=value, attribute='text')


class ItunesOwnerItem(Target):
    def start(self, handler, attrs):
        handler.add_itunes_owner()


class PodcastAttrExplicit(Target):
    WANT_TEXT = True
    _VALUES_MAPPER = {
        'yes': True,
        'explicit': True,
        'true': True,
        'no': False,
        'clean': False,
        'false': False,
    }

    def end(self, handler, text):
        value = self.filter_func(text).lower()
        if value in self._VALUES_MAPPER:
            handler.set_podcast_attr(self.key, self._VALUES_MAPPER[value],
                                     raw=text,
                                     normalizer='explicit-value-mapping')
        else:
            handler.reject_podcast_attr(
                self.key, text, 'explicit-value-mapping',
                'value not in allowed explicit set')


class EpisodeAttrExplicit(PodcastAttrExplicit):
    def end(self, handler, text):
        value = self.filter_func(text).lower()
        if value in self._VALUES_MAPPER:
            handler.set_episode_attr(self.key, self._VALUES_MAPPER[value],
                                     raw=text,
                                     normalizer='explicit-value-mapping')
        else:
            handler.reject_episode_attr(
                self.key, text, 'explicit-value-mapping',
                'value not in allowed explicit set')


class Namespace():
    # Mapping of XML namespaces to prefixes as used in MAPPING below
    NAMESPACES = {
        # iTunes Podcasting, http://www.apple.com/itunes/podcasts/specs.html
        'http://www.itunes.com/dtds/podcast-1.0.dtd': 'itunes',
        'http://www.itunes.com/DTDs/Podcast-1.0.dtd': 'itunes',

        # Atom Syndication Format, http://tools.ietf.org/html/rfc4287
        'http://www.w3.org/2005/Atom': 'atom',
        'http://www.w3.org/2005/Atom/': 'atom',

        # Media RSS, http://www.rssboard.org/media-rss
        'http://search.yahoo.com/mrss/': 'media',

        # From http://www.rssboard.org/media-rss#namespace-declaration:
        #   "Note: There is a trailing slash in the namespace, although
        #    there has been confusion around this in earlier versions."
        'http://search.yahoo.com/mrss': 'media',

        # Podlove Simple Chapters, http://podlove.org/simple-chapters
        'http://podlove.org/simple-chapters': 'psc',
        'http://podlove.org/simple-chapters/': 'psc',

        # Purl RSS Content module
        'http://purl.org/rss/1.0/modules/content/': 'content',

        # Podcast Index podcast namespace
        # https://github.com/Podcastindex-org/podcast-namespace/blob/main/docs/1.0.md
        'https://github.com/Podcastindex-org/podcast-namespace/blob/main/docs/1.0.md': 'podcast',
        'https://github.com/Podcastindex-org/podcast-namespace/blob/main/docs/1.0.md': 'podcast',
    }

    def __init__(self, attrs, parent=None):
        self.namespaces = self.parse_namespaces(attrs)
        self.parent = parent

    @staticmethod
    def parse_namespaces(attrs):
        """Parse namespace definitions from XML attributes

        >>> expected = {'': 'example'}
        >>> Namespace.parse_namespaces({'xmlns': 'example'}) == expected
        True

        >>> expected = {'foo': 'http://example.com/bar'}
        >>> Namespace.parse_namespaces({'xmlns:foo':
        ...     'http://example.com/bar'}) == expected
        True

        >>> expected = {'': 'foo', 'a': 'bar', 'b': 'bla'}
        >>> Namespace.parse_namespaces({'xmlns': 'foo',
        ...     'xmlns:a': 'bar', 'xmlns:b': 'bla'}) == expected
        True
        """
        result = {}

        for key in attrs.keys():
            if key == 'xmlns':
                result[''] = attrs[key]
            elif key.startswith('xmlns:'):
                result[key[6:]] = attrs[key]

        return result

    def lookup(self, prefix):
        """Look up a namespace URI based on the prefix"""
        current = self
        while current is not None:
            result = current.namespaces.get(prefix, None)
            if result is not None:
                return result
            current = current.parent

        return None

    def map_provenance(self, name):
        """Map a tag name to its canonical MAPPING name.

        Returns a tuple ``(mapped_name, namespace_uri)`` where the namespace
        URI is the resolved URI of the element (independent of its prefix)
        and ``mapped_name`` is the name used for MAPPING lookups.

        >>> namespace = Namespace({'xmlns:it':
        ...    'http://www.itunes.com/dtds/podcast-1.0.dtd'}, None)
        >>> namespace.map_provenance('it:duration')
        ('itunes:duration', 'http://www.itunes.com/dtds/podcast-1.0.dtd')
        >>> parent = Namespace({'xmlns:m': 'http://search.yahoo.com/mrss/',
        ...                     'xmlns:x': 'http://example.com/'}, None)
        >>> child = Namespace({}, parent)
        >>> child.map_provenance('m:content')
        ('media:content', 'http://search.yahoo.com/mrss/')
        >>> child.map_provenance('x:y') # Unknown namespace URI
        ('!x:y', 'http://example.com/')
        >>> child.map_provenance('atom:link') # Undefined prefix
        ('atom:link', None)
        """
        if ':' not in name:
            # <duration xmlns="http://..."/>
            namespace = ''
            namespace_uri = self.lookup(namespace)
        else:
            # <itunes:duration/>
            namespace, name = name.split(':', 1)
            namespace_uri = self.lookup(namespace)
            if namespace_uri is None:
                # Use of "itunes:duration" without xmlns:itunes="..."
                logger.warning('No namespace defined for "%s:%s"', namespace,
                            name)
                return '%s:%s' % (namespace, name), None

        if namespace_uri is not None:
            prefix = self.NAMESPACES.get(namespace_uri)
            if prefix is None and namespace:
                # Proper use of namespaces, but unknown namespace. We prefix
                # the tag name here to make sure that it does not match any
                # other tag below if we can't recognize the namespace
                name = '!%s:%s' % (namespace, name)
            else:
                name = '%s:%s' % (prefix, name)

        return name, namespace_uri

    def map(self, name):
        """Apply namespace prefixes for a given tag

        >>> namespace = Namespace({'xmlns:it':
        ...    'http://www.itunes.com/dtds/podcast-1.0.dtd'}, None)
        >>> namespace.map('it:duration')
        'itunes:duration'
        >>> parent = Namespace({'xmlns:m': 'http://search.yahoo.com/mrss/',
        ...                     'xmlns:x': 'http://example.com/'}, None)
        >>> child = Namespace({}, parent)
        >>> child.map('m:content')
        'media:content'
        >>> child.map('x:y') # Unknown namespace URI
        '!x:y'
        >>> child.map('atom:link') # Undefined prefix
        'atom:link'
        """
        return self.map_provenance(name)[0]


def file_basename_no_extension(filename):
    """ Returns filename without extension

    >>> file_basename_no_extension('/home/me/file.txt')
    'file'

    >>> file_basename_no_extension('file')
    'file'
    """
    base = os.path.basename(filename)
    name, extension = os.path.splitext(base)
    return name


def squash_whitespace(text):
    """ Combine multiple whitespaces into one, trim trailing/leading spaces

    >>> squash_whitespace(' some\\t   text  with a    lot of   spaces ')
    'some text with a lot of spaces'
    """
    return re.sub(r'\s+', ' ', text.strip())


def squash_whitespace_not_nl(text):
    """ Like squash_whitespace, but don't squash linefeeds and carriage returns

    >>> squash_whitespace_not_nl(' linefeeds\\ncarriage\\r  returns')
    'linefeeds\\ncarriage\\r returns'
    """
    return re.sub(r'[^\S\r\n]+', ' ', text.strip())


def parse_time(value):
    """Parse a time string into seconds

    See RFC2326, 3.6 "Normal Play Time" (HH:MM:SS.FRACT)

    >>> parse_time('0')
    0
    >>> parse_time('128')
    128
    >>> parse_time('00:00')
    0
    >>> parse_time('00:00:00')
    0
    >>> parse_time('00:20')
    20
    >>> parse_time('00:00:20')
    20
    >>> parse_time('01:00:00')
    3600
    >>> parse_time(' 03:02:01')
    10921
    >>> parse_time('61:08')
    3668
    >>> parse_time('25:03:30 ')
    90210
    >>> parse_time('25:3:30')
    90210
    >>> parse_time('61.08')
    61
    >>> parse_time('01:02:03.500')
    3723
    >>> parse_time(' ')
    0
    """
    value = value.strip()

    if value == '':
        return 0

    hours = minutes = seconds = fraction = 0
    parsed = False

    m = re.match(r'(\d+)[:](\d\d?)[:](\d\d?)([.]\d+)?$', value)
    if not parsed and m:
        hours, minutes, seconds, fraction = m.groups()
        fraction = float(fraction or 0.0)
        parsed = True

    m = re.match(r'(\d+)[:](\d\d?)([.]\d+)?$', value)
    if not parsed and m:
        minutes, seconds, fraction = m.groups()
        fraction = float(fraction or 0.0)
        parsed = True

    m = re.match(r'(\d+)([.]\d+)?$', value)
    if not parsed and m:
        seconds, fraction = m.groups()
        fraction = float(fraction or 0.0)
        parsed = True

    if not parsed:
        try:
            seconds = int(value)
        except ValueError:
            logger.warning('Could not parse time value: "%s"', value)
            return 0

    return (int(hours) * 60 + int(minutes)) * 60 + int(seconds)


def parse_url(text):
    return normalize_feed_url(text.strip())


def parse_length(text):
    """ Parses a file length

    >>> parse_length(None)
    -1

    >>> parse_length('0')
    -1

    >>> parse_length('unknown')
    -1

    >>> parse_length('100')
    100
    """

    if text is None:
        return -1

    try:
        return int(text.strip()) or -1
    except ValueError:
        return -1


def parse_type(text):
    """ "normalize" a mime type

    >>> parse_type('text/plain')
    'text/plain'

    >>> parse_type('text')
    'application/octet-stream'

    >>> parse_type('')
    'application/octet-stream'

    >>> parse_type(None)
    'application/octet-stream'
    """

    if not text or '/' not in text:
        # Maemo bug 10036
        return 'application/octet-stream'

    return text


def parse_pubdate(text):
    """Parse a date string into a Unix timestamp

    >>> parse_pubdate('Fri, 21 Nov 1997 09:55:06 -0600')
    880127706

    >>> parse_pubdate('2003-12-13T00:00:00+02:00')
    1071266400

    >>> parse_pubdate('2003-12-13T18:30:02Z')
    1071340202

    >>> parse_pubdate('Mon, 02 May 1960 09:05:01 +0100')
    -305049299

    >>> parse_pubdate('')
    0

    >>> parse_pubdate('unknown')
    0
    """
    if not text:
        return 0

    parsed = parsedate_tz(text)
    if parsed is not None:
        try:
            pubtimeseconds = int(mktime_tz(parsed))
            return pubtimeseconds
        except(OverflowError, ValueError):
            logger.warning('bad pubdate %s is before epoch or after end of time (2038)', parsed)
            return 0

    try:
        parsed = time.strptime(text[:19], '%Y-%m-%dT%H:%M:%S')
        if parsed is not None:
            m = re.match(r'^(?:Z|([+-])([0-9]{2})[:]([0-9]{2}))$', text[19:])
            if m:
                parsed = list(iter(parsed))
                if m.group(1):
                    offset = 3600 * int(m.group(2)) + 60 * int(m.group(3))
                    if m.group(1) == '-':
                        offset = 0 - offset
                else:
                    offset = 0
                parsed.append(offset)
                return int(mktime_tz(tuple(parsed)))
            else:
                return int(time.mktime(parsed))
    except Exception:
        pass

    logger.error('Cannot parse date: %s', repr(text))
    return 0


# If you modify the mapping, don't forget to also update the documentation
# section "Supported XML Elements and Attributes" in doc/index.rst
MAPPING = {
    'rss': RSS(),
    'rss/channel': PodcastItem(),
    'rss/channel/title': PodcastAttr('title', squash_whitespace),
    'rss/channel/link': PodcastAttrRelativeLink('link'),
    'rss/channel/description': PodcastAttr('description', squash_whitespace_not_nl),
    'rss/channel/itunes:summary': PodcastAttr('description', squash_whitespace_not_nl, overwrite=False),
    'rss/channel/podcast:funding': PodcastAttrFromUrl('funding_url'),
    'rss/channel/podcast:locked': PodcastAttrExplicit('import_prohibited'),
    'rss/channel/image/url': PodcastAttrRelativeLink('cover_url'),
    'rss/channel/itunes:image': PodcastAttrFromHref('cover_url'),
    'rss/channel/itunes:type': PodcastAttrType('type', squash_whitespace),
    'rss/channel/atom:link': PodcastAtomLink(),
    'rss/channel/generator': PodcastAttr('generator', squash_whitespace),
    'rss/channel/language': PodcastAttr('language', squash_whitespace),

    'rss/channel/itunes:category': ItunesCategoryAttr('itunes_categories'),
    'rss/channel/itunes:category/itunes:category': ItunesSubCategoryAttr('itunes_categories'),
    'rss/channel/itunes:category/itunes:category/itunes:category': ItunesSubCategoryAttr('itunes_categories'),

    'rss/channel/itunes:author': PodcastAttr('itunes_author', squash_whitespace),
    'rss/channel/itunes:owner': ItunesOwnerItem('itunes_owner', squash_whitespace),
    'rss/channel/itunes:explicit': PodcastAttrExplicit('explicit', squash_whitespace),
    'rss/channel/itunes:new-feed-url': PodcastAttr('new_url', squash_whitespace),
    'rss/channel/itunes:keywords': PodcastAttrList('itunes_keywords', squash_whitespace),
    'rss/redirect/newLocation': PodcastAttr('new_url', squash_whitespace),

    'rss/channel/itunes:owner/itunes:email': ItunesOwnerAttr('email', squash_whitespace),
    'rss/channel/itunes:owner/itunes:name': ItunesOwnerAttr('name', squash_whitespace),

    'rss/channel/item': EpisodeItem(),
    'rss/channel/item/guid': EpisodeGuid('guid'),
    'rss/channel/item/title': EpisodeAttr('title', squash_whitespace),
    'rss/channel/item/link': EpisodeAttrRelativeLink('link'),
    'rss/channel/item/description': RSSItemDescription(),
    'rss/channel/item/itunes:summary': EpisodeAttr('description', squash_whitespace_not_nl),
    'rss/channel/item/media:description': EpisodeAttr('description', squash_whitespace_not_nl),
    'rss/channel/item/itunes:subtitle': EpisodeAttr('subtitle', squash_whitespace),
    'rss/channel/item/content:encoded': EpisodeAttr('description_html'),
    'rss/channel/item/itunes:duration': EpisodeAttr('total_time', parse_time),
    'rss/channel/item/pubDate': EpisodeAttr('published', parse_pubdate),
    'rss/channel/item/atom:link': AtomLink(),
    'rss/channel/item/itunes:explicit': EpisodeAttrExplicit('explicit', squash_whitespace),
    'rss/channel/item/itunes:author': EpisodeAttr('itunes_author', squash_whitespace),
    'rss/channel/item/itunes:season': EpisodeAttrSeason('season', squash_whitespace),
    'rss/channel/item/itunes:episode': EpisodeAttrNumber('number', squash_whitespace),
    'rss/channel/item/itunes:episodeType': EpisodeAttrType('type', squash_whitespace),

    'rss/channel/item/itunes:image': EpisodeAttrFromHref('episode_art_url'),
    'rss/channel/item/media:thumbnail': EpisodeAttrFromUrl('episode_art_url'),
    'rss/channel/item/media:group/media:thumbnail': EpisodeAttrFromUrl('episode_art_url'),

    'rss/channel/item/media:content': Enclosure('fileSize'),
    'rss/channel/item/media:group/media:content': Enclosure('fileSize'),
    'rss/channel/item/enclosure': Enclosure('length'),
    'rss/channel/item/psc:chapters': PodloveChapters(),
    'rss/channel/item/psc:chapters/psc:chapter': PodloveChapter(),
    'rss/channel/item/podcast:transcript': EpisodeAttrFromUrl("transcript_url"),
    'rss/channel/item/podcast:chapters': EpisodeAttrFromUrl("chapters_json_url"),
    'rss/channel/item/podcast:person': EpisodePersonAttr(),

    # Basic support for Atom feeds
    'atom:feed': PodcastItem(),
    'atom:feed/atom:title': PodcastAttr('title', squash_whitespace),
    'atom:feed/atom:subtitle': PodcastAttr('description', squash_whitespace_not_nl),
    'atom:feed/atom:icon': PodcastAttrRelativeLink('cover_url'),
    'atom:feed/atom:link': PodcastAtomLink(),
    'atom:feed/atom:entry': EpisodeItem(),
    'atom:feed/atom:entry/atom:id': EpisodeAttr('guid'),
    'atom:feed/atom:entry/atom:title': EpisodeAttr('title', squash_whitespace),
    'atom:feed/atom:entry/atom:link': AtomLink(),
    'atom:feed/atom:entry/atom:content': AtomContent(),
    'atom:feed/atom:entry/content:encoded': EpisodeAttr('description_html'),
    'atom:feed/atom:entry/atom:published': EpisodeAttr('published', parse_pubdate),
    'atom:feed/atom:entry/atom:updated': EpisodeAttr('published', parse_pubdate, overwrite=False),
    'atom:feed/atom:entry/media:group/media:description': EpisodeAttr('description', squash_whitespace_not_nl),

    'atom:feed/atom:entry/media:thumbnail': EpisodeAttrFromUrl('episode_art_url'),
    'atom:feed/atom:entry/media:group/media:thumbnail': EpisodeAttrFromUrl('episode_art_url'),

    'atom:feed/atom:entry/psc:chapters': PodloveChapters(),
    'atom:feed/atom:entry/psc:chapters/psc:chapter': PodloveChapter(),
}

# Derive valid root elements from the supported MAPPINGs
VALID_ROOTS = set(path.split('/')[0] for path in MAPPING.keys())


class FeedParseError(sax.SAXParseException, ValueError):
    """
    Exception raised when asked to parse an invalid feed

    This exception allows users of this library to catch exceptions
    without having to import the XML parsing library themselves.
    """
    pass


class PodcastHandler(sax.handler.ContentHandler):
    def __init__(self, url, max_episodes, provenance=False, extension_handlers=None):
        self.url = url
        self.max_episodes = max_episodes
        self.base = url
        self.text = None
        self.episodes = []
        self.data = {
            'title': file_basename_no_extension(url),
            'episodes': self.episodes,
        }
        self.path_stack = []
        self.namespace = None
        self.provenance = provenance
        self.recorder = _ProvenanceRecorder(extension_handlers) if provenance else None
        # Depth (in elements) of the current topmost unknown element
        self.extension_depth = 0
        # One bool per open element: True when the element is part of an
        # unknown extension subtree, so endElement mirrors startElement
        self.extension_flags = []
        # Provenance record of the element that opened the unknown subtree
        self.current_extension = None
        # Origin information of the MAPPING target currently being processed
        self.current_origin = None
        # > 0 while inside an item/entry element that has not been dropped
        self.episode_depth = 0
        # Set by validate_episode() when it rejects the current episode, so
        # endElement() knows not to decrement episode_depth again
        self.episode_dropped = False

    @property
    def current_scope(self):
        return 'episode' if self.episode_depth else 'feed'

    def set_base(self, base):
        self.base = base

    # -- candidate construction -------------------------------------------

    def _make_candidate(self, key, value=_UNSET, raw=_UNSET, normalizer=None,
                        attribute=None):
        origin = self.current_origin or {}
        return Candidate(
            origin.get('namespace_uri'),
            origin.get('element'),
            origin.get('path'),
            normalizer=normalizer,
            attribute=attribute,
            value=value,
            raw_value=raw,
            source_line=origin.get('line'),
            source_column=origin.get('column'),
        )

    def _accept(self, scope, key, candidate, value):
        candidate.value = value
        candidate.rejected_reason = None
        self.recorder.candidates_for(scope, key).append(candidate)

    def _reject(self, scope, key, candidate, reason):
        candidate.rejected_reason = reason
        self.recorder.candidates_for(scope, key).append(candidate)
        logger.debug('Rejected candidate for "%s" (%s): %s', key, scope, reason)

    # -- feed-level field setters -----------------------------------------

    def set_podcast_attr(self, key, value, overwrite=True, raw=_UNSET,
                         normalizer=None, attribute=None):
        if not overwrite and self.get_podcast_attr(key):
            if self.recorder is not None:
                candidate = self._make_candidate(key, value=value, raw=raw,
                                                 normalizer=normalizer,
                                                 attribute=attribute)
                self._reject('feed', key, candidate,
                             'overwrite disabled: field already set')
            return
        if self.recorder is not None:
            candidate = self._make_candidate(key, value=value, raw=raw,
                                             normalizer=normalizer,
                                             attribute=attribute)
            self._accept('feed', key, candidate, value)
        self.data[key] = value

    def reject_podcast_attr(self, key, raw, normalizer, reason, attribute=None):
        if self.recorder is None:
            return
        candidate = self._make_candidate(key, raw=raw, normalizer=normalizer,
                                         attribute=attribute)
        self._reject('feed', key, candidate, reason)

    def get_podcast_attr(self, key, default=None):
        return self.data.get(key, default)

    # -- episode-level field setters --------------------------------------

    def set_episode_attr(self, key, value, raw=_UNSET, normalizer=None,
                         attribute=None):
        # Internal bookkeeping keys are not part of the public result
        if self.recorder is not None and not key.startswith('_'):
            candidate = self._make_candidate(key, value=value, raw=raw,
                                             normalizer=normalizer,
                                             attribute=attribute)
            self._accept('episode', key, candidate, value)
        self.episodes[-1][key] = value

    def reject_episode_attr(self, key, raw, normalizer, reason, attribute=None):
        if self.recorder is None:
            return
        candidate = self._make_candidate(key, raw=raw, normalizer=normalizer,
                                         attribute=attribute)
        self._reject('episode', key, candidate, reason)

    def get_episode_attr(self, key, default=None):
        return self.episodes[-1].get(key, default)

    def add_episode_persons(self):
        self.episodes[-1]['persons'] = []

    def sort_episodes(self, key, reverse=False):
        """Sort episodes and keep provenance records aligned with them."""
        if self.recorder is None:
            self.episodes.sort(key=key, reverse=reverse)
            return

        indexed = list(enumerate(self.episodes))
        indexed.sort(key=lambda item: key(item[1]), reverse=reverse)
        order = [index for index, _ in indexed]
        self.episodes = [self.episodes[index] for index in order]
        self.data['episodes'] = self.episodes
        self.recorder.episode_candidates = [
            self.recorder.episode_candidates[index] for index in order]
        self.recorder.episode_extensions = [
            self.recorder.episode_extensions[index] for index in order]

    def append_episode_person(self, value, raw=_UNSET):
        if self.recorder is not None:
            candidate = self._make_candidate('persons', value=value, raw=raw)
            self._accept('episode', 'persons', candidate, value)
        self.episodes[-1]['persons'].append(value)

    def note_episode_person_name(self, name):
        if self.recorder is None:
            return
        candidates = self.recorder.candidates_for('episode', 'persons')
        if candidates:
            candidates[-1].value = dict(self.episodes[-1]['persons'][-1])

    def add_episode(self):
        self.episodes.append({
            # title
            'description': '',
            # url
            'published': 0,
            # guid
            'link': '',
            'total_time': 0,
            'payment_url': None,
            'enclosures': [],
            '_guid_is_permalink': False,
            'chapters': [],
        })
        if self.recorder is not None:
            self.recorder.begin_episode()
        self.episode_depth += 1
        self.episode_dropped = False

    def validate_episode(self):
        entry = self.episodes[-1]

        if len(entry['chapters']) == 0:
            del entry['chapters']

        # Ensures `description` does not contain HTML
        if is_html(entry['description']):
            if 'description_html' not in entry:
                entry['description_html'] = entry['description']
            entry['description'] = ''

        # Sets `description` to stripped `description_html` when empty
        if 'description_html' in entry and not entry['description']:
            entry['description'] = remove_html_tags(entry['description_html'])

        if 'guid' not in entry:
            if entry.get('link'):
                # Link element can serve as GUID
                entry['guid'] = entry['link']
            else:
                if len(set(enclosure['url'] for enclosure in entry['enclosures'])) != 1:
                    # Multi-enclosure feeds MUST have a GUID or the same URL for all enclosures
                    self.episodes.pop()
                    if self.recorder is not None:
                        self.recorder.drop_episode()
                    self.episode_depth -= 1
                    self.episode_dropped = True
                    return

                # Maemo bug 12073
                entry['guid'] = entry['enclosures'][0]['url']

        if 'title' not in entry:
            if len(entry['enclosures']) != 1:
                self.episodes.pop()
                if self.recorder is not None:
                    self.recorder.drop_episode()
                self.episode_depth -= 1
                self.episode_dropped = True
                return

            entry['title'] = file_basename_no_extension(
                entry['enclosures'][0]['url'])

        if not entry.get('link') and entry.get('_guid_is_permalink'):
            entry['link'] = entry['guid']

        del entry['_guid_is_permalink']

    def add_enclosure(self, url, file_size, mime_type, raw_url=_UNSET,
                      raw_length=_UNSET, raw_type=_UNSET, normalizer=None):
        enclosure = {
            'url': url,
            'file_size': file_size,
            'mime_type': mime_type,
        }
        self.episodes[-1]['enclosures'].append(enclosure)
        if self.recorder is not None:
            candidate = self._make_candidate(
                'enclosures', value=enclosure,
                raw={'url': raw_url if raw_url is not _UNSET else None,
                     'length': raw_length if raw_length is not _UNSET else None,
                     'type': raw_type if raw_type is not _UNSET else None},
                normalizer=normalizer)
            self._accept('episode', 'enclosures', candidate, enclosure)

    def append_chapter(self, chapter, raw=_UNSET, normalizer=None):
        self.episodes[-1]['chapters'].append(chapter)
        if self.recorder is not None:
            candidate = self._make_candidate('chapters', value=chapter,
                                             raw=raw, normalizer=normalizer)
            self._accept('episode', 'chapters', candidate, chapter)

    def add_itunes_owner(self):
        self.data['itunes_owner'] = {}

    def append_itunes_owner(self, key, value, raw=_UNSET, normalizer=None):
        self.data['itunes_owner'][key] = value
        if self.recorder is not None:
            candidate = self._make_candidate('itunes_owner', value=value,
                                             raw=raw, normalizer=normalizer)
            self._accept('feed', 'itunes_owner', candidate,
                         dict(self.data['itunes_owner']))

    def add_itunes_categories(self):
        self.data['itunes_categories'] = []

    def append_itunes_category(self, key, value, raw=_UNSET, attribute=None):
        self.data.setdefault('itunes_categories', []).append([value])
        if self.recorder is not None:
            candidate = self._make_candidate(key, value=[value], raw=raw,
                                             attribute=attribute)
            self._accept('feed', key, candidate, [value])

    def append_itunes_subcategory(self, key, value, raw=_UNSET, attribute=None):
        entry = self.data['itunes_categories'][-1]
        entry.append(value)
        if self.recorder is not None:
            candidate = self._make_candidate(key, value=list(entry), raw=raw,
                                             attribute=attribute)
            self._accept('feed', key, candidate, list(entry))

    # -- unknown extensions -----------------------------------------------

    def _record_extension_start(self, name, namespace_uri, path,
                                attrs, depth):
        locator = self._locator
        line = locator.getLineNumber() if locator is not None else None
        column = locator.getColumnNumber() if locator is not None else None
        record = {
            'namespace': namespace_uri,
            'element': name,
            'path': path,
            'attrs': dict(attrs),
            'text': '',
            'truncated': False,
            'depth': depth,
            'source_line': line,
            'source_column': column,
            'handler': None,
            'value': None,
            'accepted': False,
            'rejected_reason': None,
        }
        self.current_extension = record
        self.recorder.extensions_for(self.current_scope).append(record)

    def _record_extension_text(self, chars):
        record = self.current_extension
        if record is None:
            return
        remaining = EXTENSION_TEXT_LIMIT - len(record['text'])
        if remaining > 0:
            record['text'] += chars[:remaining]
            if len(chars) > remaining:
                record['truncated'] = True
        else:
            record['truncated'] = True

    def _finalize_extension(self):
        record = self.current_extension
        self.current_extension = None
        handler = self.recorder.extension_handlers.get(record['namespace'])
        if handler is None:
            # Not a rejection of the data itself: nobody claimed the
            # namespace, so the element is kept as an unhandled record.
            record['rejected_reason'] = \
                'no handler registered for namespace'
            return
        record['handler'] = getattr(handler, '__name__', repr(handler))
        context = ExtensionContext(
            self.current_scope, record['namespace'], record['element'],
            record['path'], record['attrs'], record['text'], self.base,
            record['source_line'], record['source_column'], record['depth'])
        try:
            record['value'] = handler(context)
            record['accepted'] = True
        except Exception as e:
            # A failing handler only invalidates its own candidate; the
            # regular parse and all other candidates keep going.
            record['rejected_reason'] = 'handler exception: %s: %s' % (
                type(e).__name__, e)
            logger.warning('Extension handler for "%s" failed: %s',
                           record['namespace'], record['rejected_reason'])

    # -- SAX plumbing ------------------------------------------------------

    def _record_target_failure(self, target, phase, error):
        logger.warning('Target %r failed during %s: %s: %s', target, phase,
                       type(error).__name__, error)
        if self.recorder is None or self.current_origin is None:
            return
        key = getattr(target, 'key', None)
        if key is None:
            # Enclosure/link targets don't carry a key; file their failure
            # against the field they would have written into.
            if isinstance(target, Enclosure):
                key = 'enclosures'
            else:
                return
        if key.startswith('_'):
            return
        candidate = self._make_candidate(
            key, raw=None,
            normalizer=getattr(target, 'normalizer_name', None))
        self._reject(self.current_scope, key, candidate,
                     'handler exception: %s: %s' % (type(error).__name__, error))

    def startElement(self, name, attrs):
        self.namespace = Namespace(attrs, self.namespace)
        mapped_name, namespace_uri = self.namespace.map_provenance(name)
        if not self.path_stack and mapped_name not in VALID_ROOTS:
            raise FeedParseError(
                msg='Unsupported feed type: {}'.format(mapped_name),
                exception=None,
                locator=self._locator,
            )
        self.path_stack.append(mapped_name)
        path = '/'.join(self.path_stack)

        target = MAPPING.get(path)
        if target is None:
            is_extension = (
                namespace_uri is not None or self.extension_depth > 0)
            if is_extension and self.recorder is not None:
                # Unknown no-namespace elements outside an extension
                # subtree (e.g. inline HTML tags inside content:encoded)
                # stay invisible like in the non-provenance parse path.
                if self.extension_depth == 0:
                    self._record_extension_start(
                        name, namespace_uri, path, attrs,
                        len(self.path_stack))
                self.extension_depth += 1
                self.extension_flags.append(True)
            else:
                self.extension_flags.append(False)
            return
        self.extension_flags.append(False)

        if self.recorder is not None:
            locator = self._locator
            self.current_origin = {
                'namespace_uri': namespace_uri,
                'element': name,
                'path': path,
                'line': locator.getLineNumber() if locator is not None else None,
                'column': locator.getColumnNumber() if locator is not None else None,
            }
            try:
                target.start(self, attrs)
            except Exception as e:
                self._record_target_failure(target, 'start', e)
        else:
            target.start(self, attrs)

        if target.WANT_TEXT:
            self.text = []

    def characters(self, chars):
        if self.text is not None:
            self.text.append(chars)
        elif self.recorder is not None and self.extension_depth:
            self._record_extension_text(chars)

    def endElement(self, name):
        path = '/'.join(self.path_stack)
        target = MAPPING.get(path)
        in_extension = bool(self.extension_flags and self.extension_flags[-1])
        if target is not None:
            if self.recorder is not None:
                content = ''.join(self.text) if self.text is not None else ''
                try:
                    target.end(self, content)
                except Exception as e:
                    self._record_target_failure(target, 'end', e)
                self.current_origin = None
            else:
                content = ''.join(self.text) if self.text is not None else ''
                target.end(self, content)
            self.text = None
        elif self.recorder is not None and in_extension:
            self.extension_depth -= 1
            if self.extension_depth == 0:
                self._finalize_extension()

        if self.namespace is not None:
            self.namespace = self.namespace.parent
        self.path_stack.pop()
        if self.extension_flags:
            self.extension_flags.pop()

        if type(target) is EpisodeItem and not self.episode_dropped:
            self.episode_depth -= 1
        self.episode_dropped = False


def parse(url, stream, max_episodes=0, provenance=False, extension_handlers=None):
    """Parse a podcast feed from the given URL and stream

    :param url: the URL of the feed. Will be used to resolve relative links
    :param stream: file-like object containing the feed content
    :param max_episodes: maximum number of episodes to return. 0 (default)
                         means no limit
    :param provenance: when True, the result additionally contains a
                       ``provenance`` dict with the candidate values that
                       were considered for every feed and episode field:
                       their namespace, element/attribute position, the
                       normalizer used and rejection reasons. Unknown
                       extensions are recorded there as well and never
                       touch standard fields. When False (default) the
                       returned data is byte-for-byte identical to a parse
                       without this argument.
    :param extension_handlers: optional dict mapping namespace URIs to
                               handler callables (see
                               :func:`register_extension_handler`). Handlers
                               are only invoked when ``provenance`` is True
                               and are always dispatched by namespace URI,
                               never by prefix.
    :returns: a dict with the parsed contents of the feed
    """
    handler = PodcastHandler(url, max_episodes, provenance=provenance,
                             extension_handlers=extension_handlers)
    try:
        sax.parse(stream, handler)
    except sax.SAXParseException as e:
        raise FeedParseError(e.getMessage(), e.getException(), e._locator)
    if provenance:
        handler.data['provenance'] = handler.recorder.as_dict()
    return handler.data


def normalize_feed_url(url):
    """
    Normalize and convert a URL. If the URL cannot be converted
    (invalid or unknown scheme), None is returned.

    This will also normalize feed:// and itpc:// to http://.

    >>> normalize_feed_url('itpc://example.org/podcast.rss')
    'http://example.org/podcast.rss'

    If no URL scheme is defined (e.g. "curry.com"), we will
    simply assume the user intends to add a http:// feed.

    >>> normalize_feed_url('curry.com')
    'http://curry.com/'

    It will also take care of converting the domain name to
    all-lowercase (because domains are not case sensitive):

    >>> normalize_feed_url('http://Example.COM/')
    'http://example.com/'

    Some other minimalistic changes are also taken care of,
    e.g. a ? with an empty query is removed:

    >>> normalize_feed_url('http://example.org/test?')
    'http://example.org/test'

    Leading and trailing whitespace is removed

    >>> normalize_feed_url(' http://example.com/podcast.rss ')
    'http://example.com/podcast.rss'

    Incomplete (too short) URLs are not accepted

    >>> normalize_feed_url('http://') is None
    True

    Unknown protocols are not accepted

    >>> normalize_feed_url('gopher://gopher.hprc.utoronto.ca/file.txt') is None
    True
    """
    url = url.strip()
    if not url or len(url) < 8:
        return None

    # Assume HTTP for URLs without scheme
    if '://' not in url:
        url = 'http://' + url

    scheme, netloc, path, query, fragment = urlparse.urlsplit(url)

    # Schemes and domain names are case insensitive
    scheme, netloc = scheme.lower(), netloc.lower()

    # Normalize empty paths to "/"
    if path == '':
        path = '/'

    # feed://, itpc:// and itms:// are really http://
    if scheme in ('feed', 'itpc', 'itms'):
        scheme = 'http'

    if scheme not in ('http', 'https', 'ftp', 'file'):
        return None

    # urlunsplit might return "a slighty different, but equivalent URL"
    return urlparse.urlunsplit((scheme, netloc, path, query, fragment))


HTML_TEST = re.compile(r'<[a-z][a-z0-9]*(?:\s.*?>|\/?>)', re.IGNORECASE | re.DOTALL)


def is_html(text):
    """Heuristically tell if text is HTML

    By looking for an open tag (more or less:)
    >>> is_html('<h1>HELLO</h1>')
    True
    >>> is_html('a < b < c')
    False
    """
    return bool(HTML_TEST.search(text))


def remove_html_tags(html):
    """
    Remove HTML tags from a string and replace numeric and
    named entities with the corresponding character, so the
    HTML text can be displayed in a simple text view.
    """
    if html is None:
        return None

    # If we would want more speed, we could make these global
    re_strip_tags = re.compile(r'<[^>]*>')
    re_unicode_entities = re.compile(r'&#(\d{2,4});')
    re_html_entities = re.compile(r'&(.{2,8});')
    re_newline_tags = re.compile(r'(<br[^>]*>|<[/]?ul[^>]*>|</li>)', re.I)
    re_listing_tags = re.compile(r'<li[^>]*>', re.I)

    result = html

    # Convert common HTML elements to their text equivalent
    result = re_newline_tags.sub(r'\n', result)
    result = re_listing_tags.sub(r'\n * ', result)
    result = re.sub(r'<[Pp]>', r'\n\n', result)

    # Remove all HTML/XML tags from the string
    result = re_strip_tags.sub('', result)

    # Convert numeric XML entities to the unicode character
    result = re_unicode_entities.sub(lambda x: chr(int(x.group(1))), result)

    # Convert named HTML entities to their unicode character
    result = re_html_entities.sub(lambda x: entitydefs.get(x.group(1), ''), result)

    # Convert more than two newlines to two newlines
    result = re.sub(r'([\r\n]{2})([\r\n])+', r'\1', result)

    return result.strip()
