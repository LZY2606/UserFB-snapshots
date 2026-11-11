cheerio = require("cheerio")
extractor = require("./extractor")
cleaner = require("./cleaner")

module.exports = unfluff = (html, language) ->
  doc = cheerio.load(html)
  lng = language || extractor.lang(doc)

  pageData =
    title: extractor.title(doc)
    softTitle: extractor.softTitle(doc)
    date: extractor.date(doc)
    author: extractor.author(doc)
    publisher: extractor.publisher(doc)
    copyright: extractor.copyright(doc)
    favicon: extractor.favicon(doc)
    description: extractor.description(doc)
    keywords: extractor.keywords(doc)
    lang: lng
    canonicalLink: extractor.canonicalLink(doc)
    tags: extractor.tags(doc)
    image: extractor.image(doc)

  # Step 1: Clean the doc
  cleaner(doc)

  # Step 2: Find the doc node with the best text
  topNode = extractor.calculateBestNode(doc, lng)

  # Step 3: Extract text, videos, images, links
  pageData.videos = extractor.videos(doc, topNode)
  pageData.links = extractor.links(doc, topNode, lng)
  pageData.text = extractor.text(doc, topNode, lng)

  pageData

# Allow access to document properties with lazy evaluation
#
# The lazy interface keeps two independent parsed documents:
#   * a metadata document that is never cleaned, so header-level fields
#     (author, tags, title, ...) keep the same values no matter which
#     content getters run first or whether the cleaning pass removes
#     nodes such as bylines;
#   * a content document on which the cleaner and best-node selection run
#     at most once, shared by the text/links/videos getters.
# Each getter memoizes its own result, so repeated calls are stable and
# unrequested fields are never computed.
unfluff.lazy = (html, language) ->
  title: () ->
    doc = getMetadataDoc.call(this, html)
    @title_ ?= extractor.title(doc)

  softTitle: () ->
    doc = getMetadataDoc.call(this, html)
    @softTitle_ ?= extractor.softTitle(doc)

  date: () ->
    doc = getMetadataDoc.call(this, html)
    @date_ ?= extractor.date(doc)

  copyright: () ->
    doc = getMetadataDoc.call(this, html)
    @copyright_ ?= extractor.copyright(doc)

  author: () ->
    doc = getMetadataDoc.call(this, html)
    @author_ ?= extractor.author(doc)

  publisher: () ->
    doc = getMetadataDoc.call(this, html)
    @publisher_ ?= extractor.publisher(doc)

  favicon: () ->
    doc = getMetadataDoc.call(this, html)
    @favicon_ ?= extractor.favicon(doc)

  description: () ->
    doc = getMetadataDoc.call(this, html)
    @description_ ?= extractor.description(doc)

  keywords: () ->
    doc = getMetadataDoc.call(this, html)
    @keywords_ ?= extractor.keywords(doc)

  lang: () ->
    doc = getMetadataDoc.call(this, html)
    @language_ ?= language or extractor.lang(doc)

  canonicalLink: () ->
    doc = getMetadataDoc.call(this, html)
    @canonicalLink_ ?= extractor.canonicalLink(doc)

  tags: () ->
    doc = getMetadataDoc.call(this, html)
    @tags_ ?= extractor.tags(doc)

  image: () ->
    doc = getMetadataDoc.call(this, html)
    @image_ ?= extractor.image(doc)

  videos: () ->
    return @videos_ if @videos_?
    {doc, nodeSnapshot} = getContentState.call(this, html)
    @videos_ = extractor.videos(doc, nodeSnapshot)

  text: () ->
    return @text_ if @text_?
    {doc, topNode} = getContentState.call(this, html)
    @text_ = extractor.text(doc, topNode, this.lang())

  links: () ->
    return @links_ if @links_?
    {doc, topNode} = getContentState.call(this, html)
    @links_ = extractor.links(doc, topNode, this.lang())

# Load an untouched doc for metadata extraction and cache it.
# It is never cleaned, so content getters cannot erase bylines or tags.
getMetadataDoc = (html) ->
  @metadataDoc_ ?= cheerio.load(html)

# Cached version of calculateBestNode
getTopNode = (doc, lng) ->
  @topNode_ ?= extractor.calculateBestNode(doc, lng)

# Cached version of the cleaned content document.
# Independent of the metadata document, so cleaning never mutates it.
getCleanedDoc = (html) ->
  return @cleanedDoc_ if @cleanedDoc_?
  doc = cheerio.load(html)
  @cleanedDoc_ = cleaner(doc)
  @cleanedDoc_

# Prepare the shared content pipeline once: cleaning and best-node
# selection each run a single time per lazy instance. The snapshot is
# taken before post-cleanup mutates the best node, mirroring the eager
# pipeline where videos are collected first; text formats its own copy
# of the node and links only read it, so the three getters never destroy
# each other's data regardless of call order.
getContentState = (html) ->
  return @contentState_ if @contentState_?
  doc = getCleanedDoc.call(this, html)
  topNode = getTopNode.call(this, doc, this.lang())
  @contentState_ =
    doc: doc
    topNode: topNode
    nodeSnapshot: topNode.clone()
