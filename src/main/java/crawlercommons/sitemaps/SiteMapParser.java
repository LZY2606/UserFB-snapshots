/**
 * Copyright 2016 Crawler-Commons
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package crawlercommons.sitemaps;

import static java.nio.charset.StandardCharsets.UTF_8;

import java.io.BufferedInputStream;
import java.io.BufferedReader;
import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.StringReader;
import java.io.UncheckedIOException;
import java.net.HttpURLConnection;
import java.net.MalformedURLException;
import java.net.URI;
import java.net.URISyntaxException;
import java.net.URL;
import java.net.URLConnection;
import java.util.ArrayList;
import java.util.Collection;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.BooleanSupplier;
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.zip.GZIPInputStream;

import javax.xml.parsers.ParserConfigurationException;
import javax.xml.parsers.SAXParser;
import javax.xml.parsers.SAXParserFactory;

import org.apache.commons.io.IOUtils;
import org.apache.commons.io.input.BOMInputStream;
import org.apache.commons.io.input.BoundedInputStream;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.xml.sax.EntityResolver;
import org.xml.sax.InputSource;
import org.xml.sax.SAXException;

import crawlercommons.filters.URLFilter;
import crawlercommons.mimetypes.MimeTypeDetector;
import crawlercommons.sitemaps.AbstractSiteMap.SitemapType;
import crawlercommons.sitemaps.extension.Extension;
import crawlercommons.sitemaps.sax.DelegatorHandler;
import crawlercommons.sitemaps.sax.UrlLimitExceededException;

public class SiteMapParser {
    public static final Logger LOG = LoggerFactory.getLogger(SiteMapParser.class);

    /**
     * According to the specs, 50K URLs per Sitemap is the max
     */
    public static final int MAX_URLS = 50000;

    /**
     * Sitemaps (including sitemap index files) &quot;must be no larger than
     * 50MB (52,428,800 bytes)&quot; as specified in the
     * <a href="https://www.sitemaps.org/protocol.html#index">Sitemaps XML
     * format</a> (before Nov. 2016 the limit has been 10MB).
     */
    public static final int MAX_BYTES_ALLOWED = 52428800;

    /**
     * True (by default) meaning that invalid URLs should be rejected, as the
     * official docs allow the siteMapURLs to be only under the base URL:
     * https://www.sitemaps.org/protocol.html#location
     */
    protected boolean strict = true;

    /**
     * If true: allow URLs from sitemaps only partially parsed because of format
     * errors or truncated (incompletely fetched) content. If false any parser
     * error will cause an {@link UnknownFormatException}.
     */
    private boolean allowPartial = false;

    /**
     * Indicates whether the parser should work with the namespace from the
     * specifications or any namespace. Defaults to false.
     **/
    protected boolean strictNamespace = false;

    /** Set of namespaces (if {@link #strictNamespace}) accepted by the parser. URLs from other namespaces are ignored. */
    protected Set<String> acceptedNamespaces = new HashSet<>();

    /**
     * Map of sitemap extension namespaces required to find the right extension
     * handler.
     */
    protected Map<String, Extension> extensionNamespaces = new HashMap<>();

    private MimeTypeDetector mimeTypeDetector;

    /**
     * Option to allow DTDs in sitemaps.
     */
    private boolean allowDocTypeDefinitions = false;

    /* Function to normalize or filter URLs. Does nothing by default. */
    private Function<String, String> urlFilter = (String url) -> url;

    /** Maximum number of URLs accepted from a single sitemap. */
    private int maxUrls = MAX_URLS;

    /** Maximum size of a sitemap (uncompressed) in bytes. */
    private long maxBytes = MAX_BYTES_ALLOWED;

    /** Maximum number of sitemap documents downloaded concurrently by one walk. */
    private int walkParallelism = 1;

    /**
     * SiteMapParser with strict location validation ({@link #isStrict()}) and not
     * allowing partially parsed content.
     */
    public SiteMapParser() {
        this(true, false);
    }

    /**
     * SiteMapParser with configurable location validation, not allowing
     * partially parsed content.
     * 
     * @param strict
     *            see {@link #isStrict()}
     */
    public SiteMapParser(boolean strict) {
        this(strict, false);
    }

    /**
     * @param strict
     *            see {@link #isStrict()}
     * @param allowPartial
     *            if true: allow URLs from sitemaps only partially parsed
     *            because of format errors or truncated (incompletely fetched)
     *            content. If false any parser error will cause an
     *            {@link UnknownFormatException}.
     */
    public SiteMapParser(boolean strict, boolean allowPartial) {
        this.strict = strict;
        this.allowPartial = allowPartial;

        this.mimeTypeDetector = new MimeTypeDetector();
    }

    /**
     * Sets if the parser allows a DTD in sitemaps or feeds.
     *
     * @param allowDocTypeDefinitions
     *            true if allowed. Default is false.
     */
    public void setAllowDocTypeDefinitions(boolean allowDocTypeDefinitions) {
        this.allowDocTypeDefinitions = allowDocTypeDefinitions;
    }

    /**
     * @return whether invalid URLs will be rejected (where invalid means that
     *         the URL is not under the base URL, see <a href=
     *         "https://www.sitemaps.org/protocol.html#location">sitemap file
     *         location</a>). See also {@link #urlIsValid(String, String)}.
     */
    public boolean isStrict() {
        return strict;
    }

    /**
     * @return whether the parser allows any namespace or just the one from the
     *         specification (or any namespace accepted,
     *         {@link #addAcceptedNamespace(String)})
     */
    public boolean isStrictNamespace() {
        return strictNamespace;
    }

    /**
     * Sets the parser to allow any XML namespace or just the one from the
     * specification, or any accepted namespace (see
     * {@link #addAcceptedNamespace(String)}). Note enabling strict namespace
     * checking always adds the namespace defined by the current sitemap
     * specification ({@link Namespace#SITEMAP}) to the list of accepted
     * namespaces.
     * 
     * @param s
     *            if true enable strict namespace-checking, disable if false
     */
    public void setStrictNamespace(boolean s) {
        strictNamespace = s;
        if (strictNamespace) {
            addAcceptedNamespace(Namespace.SITEMAP);
        }
    }

    /**
     * Add namespace URI to set of accepted namespaces.
     * 
     * @param namespaceUri
     *            URI of the accepted XML namespace
     */
    public void addAcceptedNamespace(String namespaceUri) {
        acceptedNamespaces.add(namespaceUri);
    }

    /**
     * Add namespace URIs to set of accepted namespaces.
     * 
     * @param namespaceUris
     *            array of accepted XML namespace URIs
     */
    public void addAcceptedNamespace(String[] namespaceUris) {
        for (String namespaceUri : namespaceUris) {
            acceptedNamespaces.add(namespaceUri);
        }
    }

    /**
     * Enable a support for a sitemap extension in the parser.
     * 
     * @param extension
     *            sitemap extension (news, images, videos, etc.)
     */
    public void enableExtension(Extension extension) {
        for (String namespaceUri : Namespace.SITEMAP_EXTENSION_NAMESPACES.get(extension)) {
            extensionNamespaces.put(namespaceUri, extension);
        }
    }

    /**
     * Enable all supported sitemap extensions in the parser.
     */
    public void enableExtensions() {
        for (Extension extension : Extension.values()) {
            for (String namespaceUri : Namespace.SITEMAP_EXTENSION_NAMESPACES.get(extension)) {
                extensionNamespaces.put(namespaceUri, extension);
            }
        }
    }

    /**
     * Set URL filter function to normalize URLs found in sitemaps or filter
     * URLs away if the function returns null.
     */
    public void setURLFilter(Function<String, String> filter) {
        urlFilter = filter;
    }

    /**
     * Use {@link URLFilter} to filter URLs, eg. to configure that URLs found in
     * sitemaps are normalized by
     * {@link crawlercommons.filters.basic.BasicURLNormalizer}:
     * 
     * <pre>
     * sitemapParser.setURLFilter(new BasicURLNormalizer());
     * </pre>
     */
    public void setURLFilter(URLFilter filter) {
        urlFilter = filter::filter;
    }

    /**
     * @return the maximum number of URLs accepted from a single sitemap, see
     *         {@link #setMaxUrls(int)}
     */
    public int getMaxUrls() {
        return maxUrls;
    }

    /**
     * Set the maximum number of URLs accepted from a single sitemap, sitemap
     * index or feed. If a sitemap contains more URLs, parsing is stopped and
     * the sitemap is returned holding only the first <code>maxUrls</code>
     * URLs. URLs skipped because they are invalid or rejected by the URL
     * filter are not counted.
     * 
     * <p>
     * Note that the limit is applied per sitemap: a sitemap index may list up
     * to <code>maxUrls</code> sitemaps each containing up to
     * <code>maxUrls</code> URLs. With the default limit, a sitemap index
     * referencing only sitemaps (no further indexes) may result in up to 2.5
     * billion URLs. Nested sitemap indexes may result in even more URLs.
     * </p>
     * 
     * @param maxUrls
     *            maximum number of URLs, default is {@link #MAX_URLS}
     * @throws IllegalArgumentException
     *             if maxUrls is not a positive number
     */
    public void setMaxUrls(int maxUrls) {
        if (maxUrls <= 0) {
            throw new IllegalArgumentException("Max. number of URLs must be positive: " + maxUrls);
        }
        this.maxUrls = maxUrls;
    }

    /**
     * @return the maximum size of a sitemap in bytes, see
     *         {@link #setMaxBytes(long)}
     */
    public long getMaxBytes() {
        return maxBytes;
    }

    /**
     * Set the maximum size of a sitemap in bytes. The limit is applied to the
     * content passed to the parser and also to the uncompressed content of
     * gzipped sitemaps. If a sitemap is larger, an
     * {@link UnknownFormatException} is thrown unless partially parsed
     * sitemaps are allowed. In the latter case, the content is truncated and
     * the URLs found within the size limit are returned.
     * 
     * @param maxBytes
     *            maximum size in bytes, default is {@link #MAX_BYTES_ALLOWED}
     * @throws IllegalArgumentException
     *             if maxBytes is not a positive number
     */
    public void setMaxBytes(long maxBytes) {
        if (maxBytes <= 0) {
            throw new IllegalArgumentException("Max. number of bytes must be positive: " + maxBytes);
        }
        this.maxBytes = maxBytes;
    }

    /**
     * @return the maximum number of sitemap documents downloaded concurrently by
     *         a single {@link #walkSiteMap(URL, Consumer)} or
     *         {@link #walkSiteMap(AbstractSiteMap, Consumer)} call.
     */
    public int getWalkParallelism() {
        return walkParallelism;
    }

    /**
     * Set the maximum number of sitemap documents which may be downloaded
     * concurrently during one walk. The value is read once when a walk starts
     * and does not change while it is running. Redirects followed while fetching
     * a sitemap count against this limit as well.
     * <p>
     * The action passed to the walk is always executed sequentially in the
     * thread which started the walk, in depth-first order, no matter how many
     * downloads are performed in parallel.
     *
     * @param walkParallelism
     *            maximum number of concurrent downloads, default is 1
     * @throws IllegalArgumentException
     *             if walkParallelism is not a positive number
     */
    public void setWalkParallelism(int walkParallelism) {
        if (walkParallelism <= 0) {
            throw new IllegalArgumentException("Walk parallelism must be positive: " + walkParallelism);
        }
        this.walkParallelism = walkParallelism;
    }

    /**
     * Returns a SiteMap or SiteMapIndex given an online sitemap URL
     *
     * Please note that this method is a static method which goes online and
     * fetches the sitemap then parses it
     *
     * This method is a convenience method for a user who has a sitemap URL and
     * wants a "Keep it simple" way to parse it.
     * 
     * @param onlineSitemapUrl
     *            URL of the online sitemap
     * @return Extracted SiteMap/SiteMapIndex or null if the onlineSitemapUrl is
     *         null
     * @throws UnknownFormatException
     *             if there is an error parsing the sitemap
     * @throws IOException
     *             if there is an error reading in the site map
     *             {@link java.net.URL}
     */
    public AbstractSiteMap parseSiteMap(URL onlineSitemapUrl) throws UnknownFormatException, IOException {
        if (onlineSitemapUrl == null) {
            return null;
        }
        Download download = downloadDocument(onlineSitemapUrl, null, null);
        return parseSiteMap(download.body, download.finalUrl);
    }

    /** A downloaded sitemap document: response body and final response URL. */
    private static final class Download {
        final byte[] body;
        final URL finalUrl;

        Download(byte[] body, URL finalUrl) {
            this.body = body;
            this.finalUrl = finalUrl;
        }
    }

    private static final int MAX_REDIRECTS = 20;

    /**
     * Download the document identified by {@code requestedUrl}, following HTTP
     * redirects. Redirects are followed manually so that every hop respects the
     * size limit and an optional abort signal, and so that the final response
     * URL is known. Connections opened for redirect hops are closed before the
     * next hop is requested.
     *
     * @param requestedUrl
     *            URL to fetch
     * @param abort
     *            optional abort signal, checked before each request
     * @param openConnections
     *            optional set tracking open connections, so that another thread
     *            may disconnect them to abort the download
     * @return downloaded document
     */
    private Download downloadDocument(URL requestedUrl, BooleanSupplier abort, Set<HttpURLConnection> openConnections) throws IOException {
        URL current = requestedUrl;
        int redirects = 0;
        while (true) {
            if (abort != null && abort.getAsBoolean()) {
                throw new IOException("Walk aborted before requesting " + current);
            }
            URLConnection connection = current.openConnection();
            if (connection instanceof HttpURLConnection) {
                HttpURLConnection http = (HttpURLConnection) connection;
                http.setInstanceFollowRedirects(false);
                if (openConnections != null) {
                    synchronized (openConnections) {
                        if (abort != null && abort.getAsBoolean()) {
                            throw new IOException("Walk aborted before requesting " + current);
                        }
                        openConnections.add(http);
                    }
                }
                try {
                    int code = http.getResponseCode();
                    String location = http.getHeaderField("Location");
                    if (code >= 300 && code < 400 && location != null) {
                        http.disconnect();
                        if (++redirects > MAX_REDIRECTS) {
                            throw new IOException("Too many redirects for " + requestedUrl);
                        }
                        current = resolveRedirect(current, location);
                        continue;
                    }
                    if (code < 200 || code >= 300) {
                        throw new IOException("HTTP " + code + " fetching sitemap " + current);
                    }
                    try (InputStream in = limitSize(http.getInputStream())) {
                        byte[] bytes = IOUtils.toByteArray(in);
                        return new Download(bytes, current);
                    }
                } finally {
                    http.disconnect();
                    if (openConnections != null) {
                        synchronized (openConnections) {
                            openConnections.remove(http);
                        }
                    }
                }
            } else {
                try (InputStream in = limitSize(connection.getInputStream())) {
                    byte[] bytes = IOUtils.toByteArray(in);
                    return new Download(bytes, current);
                }
            }
        }
    }

    private static URL resolveRedirect(URL base, String location) throws IOException {
        try {
            return base.toURI().resolve(location).toURL();
        } catch (URISyntaxException | IllegalArgumentException | MalformedURLException e) {
            throw new IOException("Invalid redirect location '" + location + "' from " + base, e);
        }
    }

    /**
     * Identity of a sitemap document for the purpose of deduplication during
     * <em>one</em> walk: lower-cased scheme and host, effective port, path with
     * dot segments removed, and the raw query. The fragment is ignored.
     * Documents with different queries are different documents.
     */
    private static String documentIdentity(URL url) {
        String scheme = url.getProtocol();
        scheme = (scheme == null) ? "" : scheme.toLowerCase(Locale.ROOT);
        String host = url.getHost();
        host = (host == null) ? "" : host.toLowerCase(Locale.ROOT);
        int port = url.getPort();
        if (port == -1) {
            if ("http".equals(scheme)) {
                port = 80;
            } else if ("https".equals(scheme)) {
                port = 443;
            }
        }
        String path = url.getPath();
        path = (path == null) ? "" : removeDotSegments(path);
        StringBuilder sb = new StringBuilder();
        sb.append(scheme).append("://").append(host);
        if (port != -1) {
            sb.append(':').append(port);
        }
        sb.append(path);
        if (url.getQuery() != null) {
            sb.append('?').append(url.getQuery());
        }
        // the fragment deliberately does not take part in the identity
        return sb.toString();
    }

    /**
     * Remove &quot;.&quot; and &quot;..&quot; path segments as described by
     * <a href="https://www.rfc-editor.org/rfc/rfc3986#section-5.2.4">RFC 3986,
     * section 5.2.4</a>, preserving a trailing slash.
     */
    static String removeDotSegments(String path) {
        if (path.indexOf('.') < 0) {
            return path;
        }
        String[] segments = path.split("/", -1);
        List<String> out = new ArrayList<>(segments.length);
        for (int i = 0; i < segments.length; i++) {
            String segment = segments[i];
            boolean last = (i == segments.length - 1);
            if (i == 0) {
                // leading empty segment of an absolute path
                out.add(segment);
            } else if (segment.equals("..")) {
                if (out.size() > 1) {
                    out.remove(out.size() - 1);
                }
                if (last) {
                    out.add("");
                }
            } else if (segment.equals(".")) {
                if (last) {
                    out.add("");
                }
            } else {
                out.add(segment);
            }
        }
        return String.join("/", out);
    }

    /**
     * Returns a processed copy of an unprocessed sitemap object, i.e. transfer
     * the value of getLastModified(). Please note that the sitemap input stays
     * unchanged. Note that contentType is assumed to be correct; in general it
     * is more robust to use the method that doesn't take a contentType, but
     * instead detects this using Tika.
     * 
     * @param contentType
     *            MIME type of content
     * @param content
     *            raw bytes of sitemap file
     * @param sitemap
     *            an {@link crawlercommons.sitemaps.AbstractSiteMap}
     *            implementation
     * @return Extracted SiteMap/SiteMapIndex
     * @throws UnknownFormatException
     *             if there is an error parsing the sitemap
     * @throws IOException
     *             if there is an error reading in the site map
     *             {@link java.net.URL}
     */
    public AbstractSiteMap parseSiteMap(String contentType, byte[] content, final AbstractSiteMap sitemap) throws UnknownFormatException, IOException {
        AbstractSiteMap asmCopy = parseSiteMap(contentType, content, sitemap.getUrl());
        asmCopy.setLastModified(sitemap.getLastModified());
        return asmCopy;
    }

    /**
     * Parse a sitemap, given the content bytes and the URL.
     * 
     * @param content
     *            raw bytes of sitemap file
     * @param url
     *            URL to sitemap file
     * @return Extracted SiteMap/SiteMapIndex
     * @throws UnknownFormatException
     *             if there is an error parsing the sitemap
     * @throws IOException
     *             if there is an error reading in the site map
     *             {@link java.net.URL}
     */
    public AbstractSiteMap parseSiteMap(byte[] content, URL url) throws UnknownFormatException, IOException {
        if (url == null) {
            return null;
        }

        String contentType = mimeTypeDetector.detect(content);
        if (contentType == null) {
            String msg = String.format(Locale.ROOT, "Failed to detect MediaType of sitemap '%s'", url);
            throw new UnknownFormatException(msg);
        }
        return parseSiteMap(contentType, content, url);
    }

    /**
     * Parse a sitemap, given the MIME type, the content bytes, and the URL.
     * Note that contentType is assumed to be correct; in general it is more
     * robust to use the method that doesn't take a contentType, but instead
     * detects this using Tika.
     * 
     * @param contentType
     *            MIME type of content
     * @param content
     *            raw bytes of sitemap file
     * @param url
     *            URL to sitemap file
     * @return Extracted SiteMap/SiteMapIndex
     * @throws UnknownFormatException
     *             if there is an error parsing the sitemap
     * @throws IOException
     *             if there is an error reading in the site map
     *             {@link java.net.URL}
     */
    public AbstractSiteMap parseSiteMap(String contentType, byte[] content, URL url) throws UnknownFormatException, IOException {
        String mimeType = mimeTypeDetector.normalize(contentType, content);

        checkSize(url, content.length);

        String msg;
        if (mimeTypeDetector.isXml(mimeType)) {
            return processXml(url, content);
        } else if (mimeTypeDetector.isText(mimeType)) {
            return processText(url, content);
        } else if (mimeTypeDetector.isGzip(mimeType)) {
            try (BoundedInputStream bounded = limitSize(new GZIPInputStream(new ByteArrayInputStream(content))); InputStream decompressed = new BufferedInputStream(bounded)) {
                String compressedType = mimeTypeDetector.detect(decompressed);
                if (mimeTypeDetector.isXml(compressedType)) {
                    return processGzippedXML(url, content);
                } else if (mimeTypeDetector.isText(compressedType)) {
                    SiteMap sitemap = processText(url, decompressed, () -> bounded.getCount() > maxBytes);
                    checkSize(url, bounded.getCount());
                    return sitemap;
                } else if (compressedType == null) {
                    msg = String.format(Locale.ROOT, "Failed to detect embedded MediaType of gzipped sitemap '%s'", url);
                } else {
                    msg = String.format(Locale.ROOT, "Can't parse a sitemap with MediaType '%s' (embedded in %s) from '%s'", compressedType, contentType, url);
                }
            } catch (UnknownFormatException e) {
                throw e;
            } catch (Exception e) {
                msg = String.format(Locale.ROOT, "Failed to detect embedded MediaType of gzipped sitemap '%s'", url);
                throw new UnknownFormatException(msg, e);
            }
        } else {
            msg = String.format(Locale.ROOT, "Can't parse a sitemap with MediaType '%s' from '%s'", contentType, url);
        }

        throw new UnknownFormatException(msg);
    }

    /**
     * Fetch a sitemap from the specified URL, recursively fetching and
     * traversing the content of any enclosed sitemap index, and performing the
     * specified action for each sitemap URL until all URLs have been processed
     * or the action throws an exception.
     * <p>
     * This method is a convenience method for a user who has a sitemap URL and
     * wants a simple way to traverse it.
     * <p>
     * Exceptions thrown by the action are relayed to the caller.
     *
     * @param onlineSitemapUrl
     *            URL of the online sitemap
     * @param action
     *            The action to be performed for each element
     * @throws UnknownFormatException
     *             if there is an error parsing the sitemap
     * @throws IOException
     *             if there is an error fetching the content of any
     *             {@link java.net.URL}
     */
    public void walkSiteMap(URL onlineSitemapUrl, Consumer<SiteMapURL> action) throws UnknownFormatException, IOException {
        if (onlineSitemapUrl == null || action == null) {
            LOG.info("Got null sitemap URL and/or action, stopping traversal");
            return;
        }
        runWalk(walkParallelism, action, null, onlineSitemapUrl);
    }

    /**
     * Traverse a sitemap, recursively fetching and traversing the content of
     * any enclosed sitemap index, and performing the specified action for each
     * sitemap URL until all URLs have been processed or the action throws an
     * exception.
     * <p>
     * This method is a convenience method for a user who has a sitemap and
     * wants a simple way to traverse it.
     * <p>
     * Note that the limits on the number of URLs ({@link #setMaxUrls(int)}) and
     * the size ({@link #setMaxBytes(long)}) are applied to every single
     * sitemap but not to the traversal as a whole: with the default limits a
     * sitemap index referencing only sitemaps may hold up to 2.5 billion URLs
     * (50,000 sitemaps with 50,000 URLs each). The traversal follows nested
     * sitemap indexes without any limit on the depth, so the number of URLs
     * may be even higher.
     * <p>
     * Exceptions thrown by the action are relayed to the caller.
     *
     * @param sitemap
     *            The sitemap to traverse
     * @param action
     *            The action to be performed for each element
     * @throws UnknownFormatException
     *             if there is an error parsing the sitemap
     * @throws IOException
     *             if there is an error fetching the content of any
     *             {@link java.net.URL}
     */
    public void walkSiteMap(AbstractSiteMap sitemap, Consumer<SiteMapURL> action) throws UnknownFormatException, IOException {
        if (sitemap == null || action == null) {
            LOG.info("Got null sitemap and/or action, stopping traversal");
            return;
        }
        runWalk(walkParallelism, action, sitemap, null);
    }

    private void runWalk(int parallelism, Consumer<SiteMapURL> action, AbstractSiteMap root, URL rootUrl) throws UnknownFormatException, IOException {
        Walk walk = new Walk(parallelism);
        try {
            if (root != null) {
                walk.walkRoot(root, action);
            } else {
                walk.walkUrl(rootUrl, action);
            }
        } finally {
            walk.close();
        }
    }

    /**
     * State of one walk invocation: per-walk document deduplication, bounded
     * parallel downloads, and depth-first delivery of leaf URLs in the thread
     * which called the walk.
     * <p>
     * Downloads may start for the direct children of a sitemap index before
     * the depth-first descent reaches them, but the action is only ever
     * executed by the caller's thread. A destination is expanded at most once
     * per walk, at the position of its first depth-first reference.
     */
    private final class Walk {
        private static final int DRAIN_SECONDS = 10;

        private final int parallelism;
        private final boolean parallel;
        private final ExecutorService executor;
        private final AtomicInteger threadCounter = new AtomicInteger();

        /** Guards all mutable state below. */
        private final Object lock = new Object();
        /** In-flight or completed download per requested document identity. */
        private final Map<String, CompletableFuture<Download>> downloads = new HashMap<>();
        /** Completed downloads, keyed by canonical document identity. */
        private final Map<String, Download> completed = new HashMap<>();
        /** Aliases mapping document identities to their canonical identity. */
        private final Map<String, String> aliases = new HashMap<>();
        /** Canonical identities of documents already expanded. */
        private final Set<String> expanded = new HashSet<>();
        /** Connections currently open, disconnected to abort downloads. */
        private final Set<HttpURLConnection> openConnections = new HashSet<>();
        private boolean failed;
        private boolean closed;

        Walk(int parallelism) {
            this.parallelism = parallelism;
            this.parallel = parallelism > 1;
            if (parallel) {
                ThreadFactory factory = runnable -> {
                    Thread thread = new Thread(runnable, "sitemap-walk-download-" + threadCounter.incrementAndGet());
                    thread.setDaemon(true);
                    return thread;
                };
                this.executor = Executors.newFixedThreadPool(parallelism, factory);
            } else {
                this.executor = null;
            }
        }

        /** Stop download activity and release the worker threads. */
        void close() {
            synchronized (lock) {
                closed = true;
                for (CompletableFuture<Download> future : new ArrayList<>(downloads.values())) {
                    future.cancel(true);
                }
                downloads.clear();
            }
            disconnectOpenConnections();
            if (executor != null) {
                executor.shutdownNow();
                try {
                    if (!executor.awaitTermination(DRAIN_SECONDS, TimeUnit.SECONDS)) {
                        LOG.warn("Sitemap walk download threads did not terminate");
                    }
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                }
            }
        }

        /** Traverse an already parsed sitemap without fetching it again. */
        void walkRoot(AbstractSiteMap root, Consumer<SiteMapURL> action) throws UnknownFormatException, IOException {
            try {
                processNode(root, action);
            } catch (UnknownFormatException | IOException | RuntimeException | Error e) {
                fail();
                throw e;
            }
        }

        void walkUrl(URL rootUrl, Consumer<SiteMapURL> action) throws UnknownFormatException, IOException {
            try {
                processNode(parseDownloaded(downloadForWalk(rootUrl)), action);
            } catch (UnknownFormatException | IOException | RuntimeException | Error e) {
                fail();
                throw e;
            }
        }

        /* ------------------------------------------------------------------ */
        /* Document identity and aliasing                                      */
        /* ------------------------------------------------------------------ */

        /** Canonical identity of a document (union-find, called with lock held). */
        private String find(String identity) {
            String parent = aliases.get(identity);
            if (parent == null || parent.equals(identity)) {
                return identity;
            }
            String root = find(parent);
            aliases.put(identity, root);
            return root;
        }

        /** Record that two identities denote the same document (lock held). */
        private void union(String a, String b) {
            String rootA = find(a);
            String rootB = find(b);
            if (rootA.equals(rootB)) {
                return;
            }
            aliases.put(rootA, rootB);
            // migrate canonical state so that a merged document is not seen as
            // a new one (and thus fetched or expanded a second time)
            Download migrated = completed.remove(rootA);
            if (migrated != null) {
                completed.putIfAbsent(rootB, migrated);
            }
            if (expanded.remove(rootA)) {
                expanded.add(rootB);
            }
        }

        /** Whether the document behind an identity was expanded already. */
        private boolean isExpanded(String identity) {
            synchronized (lock) {
                return expanded.contains(find(identity));
            }
        }

        /**
         * Mark a node's document as expanded. Returns false if it had already
         * been expanded, in which case the node must not be expanded again.
         */
        private boolean markExpanded(AbstractSiteMap node) {
            URL url = node.getUrl();
            if (url == null) {
                return true;
            }
            synchronized (lock) {
                String identity = documentIdentity(url);
                // a previously parsed child object may itself be a redirect
                // target reached by another reference; the alias map merges them
                return expanded.add(find(identity));
            }
        }

        /* ------------------------------------------------------------------ */
        /* Download scheduling                                                 */
        /* ------------------------------------------------------------------ */

        private Download downloadForWalk(URL url) throws UnknownFormatException, IOException {
            return await(requestDownload(url), url);
        }

        /** Request (or join) the single download for a document identity. */
        private CompletableFuture<Download> requestDownload(URL url) {
            String identity = documentIdentity(url);
            synchronized (lock) {
                if (closed || failed) {
                    return failedFuture(new IOException("Walk aborted before requesting " + url));
                }
                String root = find(identity);
                Download known = completed.get(root);
                if (known != null) {
                    return CompletableFuture.completedFuture(known);
                }
                CompletableFuture<Download> future = downloads.get(identity);
                if (future != null) {
                    return future;
                }
                future = new CompletableFuture<>();
                downloads.put(identity, future);
                final CompletableFuture<Download> task = future;
                if (parallel) {
                    try {
                        executor.execute(() -> runDownload(url, identity, task));
                        prefetchDescendants(task);
                    } catch (RejectedExecutionException e) {
                        downloads.remove(identity);
                        future.completeExceptionally(new IOException("Walk executor rejected download for " + url, e));
                    }
                } else {
                    // no nesting an async task on itself: fetch synchronously
                    try {
                        Download download = fetchAndRegister(url, identity);
                        future.complete(download);
                    } catch (Throwable t) {
                        future.completeExceptionally(t);
                    }
                }
                return future;
            }
        }

        private <T> CompletableFuture<T> failedFuture(Throwable failure) {
            CompletableFuture<T> future = new CompletableFuture<>();
            future.completeExceptionally(failure);
            return future;
        }

        private void runDownload(URL url, String identity, CompletableFuture<Download> future) {
            try {
                future.complete(fetchAndRegister(url, identity));
            } catch (Throwable t) {
                future.completeExceptionally(t);
            }
        }

        /**
         * Once an index has been downloaded, request its direct children as
         * well, so that a fast right branch downloads ahead while the
         * depth-first descent is still busy on the left. Delivery order is not
         * affected: results are only consumed by the depth-first walk. Nodes
         * already being downloaded or expanded are not requested again.
         */
        private void prefetchDescendants(CompletableFuture<Download> parent) {
            parent.thenAccept(download -> {
                if (!parallel) {
                    return;
                }
                AbstractSiteMap parsed;
                try {
                    parsed = parseSiteMap(download.body, download.finalUrl);
                } catch (UnknownFormatException | IOException | RuntimeException e) {
                    // the depth-first walk will report the failure at its position
                    return;
                }
                if (!(parsed instanceof SiteMapIndex)) {
                    return;
                }
                for (AbstractSiteMap child : new ArrayList<>(((SiteMapIndex) parsed).getSitemaps())) {
                    if (child == null || child.getUrl() == null || child.isProcessed()) {
                        continue;
                    }
                    CompletableFuture<Download> future = requestDownload(child.getUrl());
                    // a speculative branch may fail without aborting the walk
                    future.exceptionally(t -> null);
                }
            });
        }

        /**
         * Fetch a document and register the result under the identity of the
         * requested URL and under the identity of the final response URL. A
         * document already downloaded concurrently (because its alias became
         * known while both requests were in flight) is not fetched twice by
         * the lookup, but the caller gets the previously registered body.
         */
        private Download fetchAndRegister(URL url, String identity) throws IOException {
            synchronized (lock) {
                if (closed || failed) {
                    throw new IOException("Walk aborted before requesting " + url);
                }
                Download known = completed.get(find(identity));
                if (known != null) {
                    return known;
                }
            }
            Download download = downloadDocument(url, this::isAborted, openConnections);
            String finalIdentity = documentIdentity(download.finalUrl);
            synchronized (lock) {
                union(identity, finalIdentity);
                String root = find(identity);
                Download existing = completed.get(root);
                if (existing == null) {
                    completed.put(root, download);
                    existing = download;
                }
                // make the final identity directly reachable as well
                completed.putIfAbsent(find(finalIdentity), existing);
                if (closed) {
                    throw new IOException("Walk aborted while downloading " + url);
                }
                return existing;
            }
        }

        private boolean isAborted() {
            synchronized (lock) {
                return failed || closed;
            }
        }

        private Download await(CompletableFuture<Download> future, URL url) throws UnknownFormatException, IOException {
            try {
                return future.get();
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                throw new IOException("Interrupted while downloading sitemap " + url, e);
            } catch (CancellationException e) {
                throw new IOException("Download cancelled for sitemap " + url, e);
            } catch (ExecutionException e) {
                Throwable cause = e.getCause();
                if (cause instanceof UnknownFormatException) {
                    throw (UnknownFormatException) cause;
                }
                if (cause instanceof IOException) {
                    throw (IOException) cause;
                }
                if (cause instanceof RuntimeException) {
                    throw (RuntimeException) cause;
                }
                if (cause instanceof Error) {
                    throw (Error) cause;
                }
                throw new IOException("Failed to download sitemap " + url, cause);
            }
        }

        private void fail() {
            synchronized (lock) {
                failed = true;
                for (CompletableFuture<Download> future : new ArrayList<>(downloads.values())) {
                    future.cancel(true);
                }
                downloads.clear();
            }
            disconnectOpenConnections();
        }

        private void disconnectOpenConnections() {
            List<HttpURLConnection> connections;
            synchronized (openConnections) {
                connections = new ArrayList<>(openConnections);
                openConnections.clear();
            }
            for (HttpURLConnection connection : connections) {
                try {
                    connection.disconnect();
                } catch (RuntimeException e) {
                    LOG.debug("Failed to disconnect sitemap connection", e);
                }
            }
        }

        /* ------------------------------------------------------------------ */
        /* Depth-first traversal (always in the calling thread)                */
        /* ------------------------------------------------------------------ */

        private void processNode(AbstractSiteMap node, Consumer<SiteMapURL> action) throws UnknownFormatException, IOException {
            if (node == null || !markExpanded(node)) {
                return;
            }
            if (node.isIndex()) {
                processIndex((SiteMapIndex) node, action);
            } else if (node instanceof SiteMap) {
                processLeaf((SiteMap) node, action);
            }
        }

        private void processIndex(SiteMapIndex index, Consumer<SiteMapURL> action) throws UnknownFormatException, IOException {
            List<AbstractSiteMap> children = new ArrayList<>(index.getSitemaps());
            int size = children.size();
            CompletableFuture<Download>[] prefetched = newFutureArray(size);
            if (parallel && !failed) {
                prefetch(children, prefetched);
            }
            for (int i = 0; i < size; i++) {
                if (failed) {
                    throw new IOException("Walk aborted after a previous failure");
                }
                AbstractSiteMap child = children.get(i);
                if (child == null) {
                    continue;
                }
                URL childUrl = child.getUrl();
                if (childUrl == null) {
                    processNode(child, action);
                    continue;
                }
                if (child.isProcessed()) {
                    // already parsed object: use it as is, never fetch it again
                    processNode(child, action);
                    continue;
                }
                if (isExpanded(documentIdentity(childUrl))) {
                    continue;
                }
                Download download = (prefetched[i] != null) ? await(prefetched[i], childUrl) : downloadForWalk(childUrl);
                processNode(parseDownloaded(download), action);
            }
        }

        @SuppressWarnings("unchecked")
        private CompletableFuture<Download>[] newFutureArray(int size) {
            return (CompletableFuture<Download>[]) new CompletableFuture<?>[size];
        }

        /**
         * Start downloads for the direct children of an index before descending
         * into them. Requests are started up to the parallelism limit, but the
         * results are consumed in declaration order, so a slow left branch keeps
         * the delivery position of its shared descendants.
         */
        private void prefetch(List<AbstractSiteMap> children, CompletableFuture<Download>[] prefetched) {
            for (int i = 0; i < children.size(); i++) {
                AbstractSiteMap child = children.get(i);
                if (child == null || child.getUrl() == null || child.isProcessed()) {
                    continue;
                }
                String identity = documentIdentity(child.getUrl());
                synchronized (lock) {
                    if (failed || closed) {
                        return;
                    }
                    Download known = completed.get(find(identity));
                    if (known != null) {
                        prefetched[i] = CompletableFuture.completedFuture(known);
                        continue;
                    }
                    if (isExpanded(identity)) {
                        continue;
                    }
                    CompletableFuture<Download> future = downloads.get(identity);
                    if (future == null) {
                        future = new CompletableFuture<>();
                        downloads.put(identity, future);
                        final CompletableFuture<Download> task = future;
                        try {
                            executor.execute(() -> runDownload(child.getUrl(), identity, task));
                            prefetchDescendants(task);
                        } catch (RejectedExecutionException e) {
                            downloads.remove(identity);
                            future.completeExceptionally(new IOException("Walk executor rejected download for " + child.getUrl(), e));
                        }
                    }
                    prefetched[i] = future;
                }
            }
        }

        private AbstractSiteMap parseDownloaded(Download download) throws UnknownFormatException, IOException {
            // parsing is based on the final response URL, so that strict
            // location checks use the address the content was served from
            return parseSiteMap(download.body, download.finalUrl);
        }

        /**
         * Deliver the URLs of a leaf. The map document itself is expanded at
         * most once per walk, but all pages it lists are delivered as they are:
         * repeated pages within a leaf, and the same page listed by distinct
         * leaves, are passed to the action each time.
         */
        private void processLeaf(SiteMap leaf, Consumer<SiteMapURL> action) {
            Collection<SiteMapURL> links = leaf.getSiteMapUrls();
            if (links == null) {
                return;
            }
            for (SiteMapURL url : new ArrayList<>(links)) {
                if (url == null) {
                    continue;
                }
                action.accept(url);
            }
        }
    }

    /**
     * Parse the given XML content.
     * 
     * @param sitemapUrl
     *            URL to sitemap file
     * @param xmlContent
     *            the byte[] backing the sitemapUrl
     * @return The site map
     * @throws UnknownFormatException
     *             if there is an error parsing the sitemap
     */
    protected AbstractSiteMap processXml(URL sitemapUrl, byte[] xmlContent) throws UnknownFormatException {

        InputStream in;
        try {
            in = new SkipLeadingWhiteSpaceInputStream(new BOMInputStream(new ByteArrayInputStream(xmlContent, 0, limitLength(xmlContent))));
        } catch (IOException e) {
            /*
             * Since commons-io 2.22.0 the BOMInputStream constructor reads the
             * byte-order mark eagerly and declares IOException. Reading from
             * the in-memory byte[] cannot fail, so this is unreachable - the
             * signature of this method is kept unchanged.
             */
            throw new UncheckedIOException("Failed to read byte-order mark of sitemap " + sitemapUrl, e);
        }
        InputSource is = new InputSource();
        is.setCharacterStream(new BufferedReader(new InputStreamReader(in, UTF_8)));

        return processXml(sitemapUrl, is);
    }

    /**
     * Process a text-based Sitemap. Text sitemaps only list URLs but no
     * priorities, last mods, etc.
     * 
     * @param sitemapUrl
     *            URL to sitemap file
     * @param content
     *            the byte[] backing the sitemapUrl
     * @return The site map
     * @throws IOException
     *             if there is an error reading in the site map content
     */
    protected SiteMap processText(URL sitemapUrl, byte[] content) throws IOException {
        return processText(sitemapUrl, new ByteArrayInputStream(content, 0, limitLength(content)), () -> content.length > maxBytes);
    }

    /**
     * Process a text-based Sitemap. Text sitemaps only list URLs but no
     * priorities, last mods, etc.
     *
     * @param sitemapUrl
     *            URL to sitemap file
     * @param stream
     *            content stream
     * @return The site map
     * @throws IOException
     *             if there is an error reading in the site map content
     */
    protected SiteMap processText(URL sitemapUrl, InputStream stream) throws IOException {
        return processText(sitemapUrl, stream, () -> false);
    }

    /**
     * Process a text-based Sitemap.
     *
     * @param sitemapUrl
     *            URL to sitemap file
     * @param stream
     *            content stream
     * @param truncated
     *            tells whether the content was truncated at the size limit, if
     *            true the last line is skipped because it may be incomplete
     * @return The site map
     * @throws IOException
     *             if there is an error reading in the site map content
     */
    private SiteMap processText(URL sitemapUrl, InputStream stream, BooleanSupplier truncated) throws IOException {
        LOG.debug("Processing textual Sitemap");

        SiteMap textSiteMap = new SiteMap(sitemapUrl);
        textSiteMap.setType(SitemapType.TEXT);

        BOMInputStream bomIs = new BOMInputStream(stream);
        @SuppressWarnings("resource")
        BufferedReader reader = new BufferedReader(new InputStreamReader(bomIs, UTF_8));

        String line;
        String nextLine = reader.readLine();
        int i = 0;
        while ((line = nextLine) != null) {
            nextLine = reader.readLine();
            if (nextLine == null && truncated.getAsBoolean()) {
                LOG.warn("Truncated sitemap {}: skipped last line, size limit of {} bytes exceeded", sitemapUrl, maxBytes);
                break;
            }
            line = line.trim();
            if (line.isEmpty()) {
                continue;
            }
            String urlFiltered = urlFilter.apply(line);
            if (urlFiltered == null) {
                LOG.info("Filtered url: [{}]", line.substring(0, Math.min(1024, line.length())));
                continue;
            }
            try {
                URL url = new URI(urlFiltered).toURL();
                boolean valid = urlIsValid(textSiteMap.getBaseUrl(), url.toString());
                if (valid || !strict) {
                    if (i >= maxUrls) {
                        LOG.warn("Truncated sitemap {}: more than {} URLs", sitemapUrl, maxUrls);
                        break;
                    }
                    SiteMapURL sUrl = new SiteMapURL(url, valid);
                    textSiteMap.addSiteMapUrl(sUrl);
                    i++;
                    LOG.debug("  {}. {}", i, sUrl);
                } else {
                    LOG.info("URL: {} is excluded from the sitemap as it is not a valid url = not under the base url: {}", url.toExternalForm(), textSiteMap.getBaseUrl());
                }
            } catch (IllegalArgumentException | MalformedURLException | URISyntaxException e) {
                LOG.warn("Bad url: [{}]", line.substring(0, Math.min(1024, line.length())));
            }
        }
        textSiteMap.setProcessed(true);

        return textSiteMap;
    }

    /**
     * Decompress the gzipped content and process the resulting XML Sitemap.
     * 
     * @param url
     *            - URL of the gzipped content
     * @param response
     *            - Gzipped content
     * @return the site map
     * @throws UnknownFormatException
     *             if there is an error parsing the gzip
     * @throws IOException
     *             if there is an error reading in the gzip {@link java.net.URL}
     */
    protected AbstractSiteMap processGzippedXML(URL url, byte[] response) throws IOException, UnknownFormatException {

        LOG.debug("Processing gzipped XML");

        InputStream is = new ByteArrayInputStream(response);

        // Remove .gz ending
        String xmlUrl = url.toString().replaceFirst("\\.gz$", "");
        LOG.debug("XML url = {}", xmlUrl);

        BoundedInputStream bounded = limitSize(new GZIPInputStream(is));
        InputStream decompressed = new SkipLeadingWhiteSpaceInputStream(new BOMInputStream(bounded));
        InputSource in = new InputSource(decompressed);
        in.setSystemId(xmlUrl);
        AbstractSiteMap sitemap;
        try {
            sitemap = processXml(url, in);
        } catch (UnknownFormatException e) {
            if (bounded.getCount() > maxBytes) {
                // parsing failed because the content was truncated
                String msg = String.format(Locale.ROOT, "Sitemap '%s' exceeds the size limit of %d bytes", url, maxBytes);
                throw new UnknownFormatException(msg, e);
            }
            throw e;
        }
        checkSize(url, bounded.getCount());
        return sitemap;
    }

    /**
     * Verify that the size of a sitemap does not exceed the limit, see
     * {@link #setMaxBytes(long)}.
     * 
     * @param sitemapUrl
     *            URL of the sitemap
     * @param size
     *            size of the sitemap in bytes
     * @throws UnknownFormatException
     *             if the limit is exceeded and partially parsed sitemaps are
     *             not allowed
     */
    private void checkSize(URL sitemapUrl, long size) throws UnknownFormatException {
        if (size <= maxBytes) {
            return;
        }
        if (!allowPartial) {
            String msg = String.format(Locale.ROOT, "Sitemap '%s' exceeds the size limit of %d bytes", sitemapUrl, maxBytes);
            throw new UnknownFormatException(msg);
        }
        LOG.warn("Truncated sitemap {}: size limit of {} bytes exceeded", sitemapUrl, maxBytes);
    }

    /**
     * Wrap the stream so that one byte more than the size limit can be read.
     * The extra byte allows to distinguish content which exceeds the limit
     * from content with a size equal to the limit.
     */
    private BoundedInputStream limitSize(InputStream stream) throws IOException {
        long max = (maxBytes == Long.MAX_VALUE) ? maxBytes : maxBytes + 1;
        return BoundedInputStream.builder().setInputStream(stream).setMaxCount(max).get();
    }

    /** @return the number of bytes of the content within the size limit */
    private int limitLength(byte[] content) {
        return (int) Math.min(content.length, maxBytes);
    }

    /**
     * Parse the given XML content.
     * 
     * @param sitemapUrl
     *            a sitemap {@link java.net.URL}
     * @param is
     *            an {@link org.xml.sax.InputSource} backing the sitemap
     * @return the site map
     * @throws UnknownFormatException
     *             if there is an error parsing the
     *             {@link org.xml.sax.InputSource}
     */
    protected AbstractSiteMap processXml(URL sitemapUrl, InputSource is) throws UnknownFormatException {

        SAXParserFactory factory = SAXParserFactory.newInstance();

        // disable validation and avoid that DTDs, schemas, XML snippets, etc.
        // are fetched from remote servers or the local file system
        factory.setValidating(false);
        factory.setXIncludeAware(false);

        // support the use of an explicit namespace.
        factory.setNamespaceAware(true);

        // Configure underlying parser features to reduce the risk of XXE attacks
        // See https://cheatsheetseries.owasp.org/cheatsheets/XML_External_Entity_Prevention_Cheat_Sheet.html#java
        try {
            factory.setFeature("http://xml.org/sax/features/external-general-entities", false);
            factory.setFeature("http://xml.org/sax/features/external-parameter-entities", false);
            factory.setFeature("http://apache.org/xml/features/nonvalidating/load-external-dtd", false);
            if (!this.allowDocTypeDefinitions) {
                factory.setFeature("http://apache.org/xml/features/disallow-doctype-decl", true);
            }
        } catch (Exception e) {
            throw new RuntimeException("Failed to configure XML parser: " + e.toString());
        }

        DelegatorHandler handler = new DelegatorHandler(sitemapUrl, strict);
        handler.setStrictNamespace(isStrictNamespace());
        if (isStrictNamespace()) {
            handler.setAcceptedNamespaces(acceptedNamespaces);
        }
        handler.setExtensionNamespaces(extensionNamespaces);
        handler.setURLFilter(urlFilter);
        handler.setMaxUrls(maxUrls);

        try {
            SAXParser saxParser = factory.newSAXParser();
            saxParser.getXMLReader().setEntityResolver(new EntityResolver() {
                // noop entity resolver, does not fetch remote content
                @Override
                public InputSource resolveEntity(String publicId, String systemId) {
                    return new InputSource(new StringReader(""));
                }
            });
            saxParser.parse(is, handler);
            AbstractSiteMap sitemap = handler.getSiteMap();
            if (sitemap == null) {
                UnknownFormatException ex = handler.getException();
                if (ex != null) {
                    throw ex;
                }
                throw new UnknownFormatException("Unknown XML format for: " + sitemapUrl);
            }
            return sitemap;
        } catch (IOException e) {
            LOG.warn("Error parsing sitemap {}: {}", sitemapUrl, e.getMessage());
            UnknownFormatException ufe = new UnknownFormatException("Failed to parse " + sitemapUrl);
            ufe.initCause(e);
            throw ufe;
        } catch (UrlLimitExceededException e) {
            LOG.warn("Truncated sitemap {}: more than {} URLs", sitemapUrl, maxUrls);
            AbstractSiteMap sitemap = handler.getSiteMap();
            if (sitemap == null) {
                throw new UnknownFormatException("Failed to parse " + sitemapUrl, e);
            }
            sitemap.setProcessed(true);
            return sitemap;
        } catch (SAXException e) {
            LOG.warn("Error parsing sitemap {}: {}", sitemapUrl, e.getMessage());
            AbstractSiteMap sitemap = handler.getSiteMap();
            if (allowPartial && sitemap != null) {
                LOG.warn("Processed broken/partial sitemap for '{}'", sitemapUrl);
                sitemap.setProcessed(true);
                return sitemap;
            } else {
                UnknownFormatException ufe = new UnknownFormatException("Failed to parse " + sitemapUrl);
                ufe.initCause(e);
                throw ufe;
            }
        } catch (ParserConfigurationException e) {
            throw new IllegalStateException(e);
        }
    }

    /**
     * Verify whether the <code>testUrl</code> is under the
     * <code>sitemapBaseUrl</code>. Only URLs under sitemapBaseUrl are valid,
     * see <a href="https://www.sitemaps.org/protocol.html#location">sitemap
     * file location</a>. This is method is used when the parser is configured
     * with <b>strict</b> checking, see {@link #isStrict()}.
     * 
     * Note that this method does not allow to check for <a href=
     * "https://www.sitemaps.org/protocol.html#sitemaps_cross_submits">cross
     * submits</a>: if a sitemap, located on host A, is referenced in the
     * robots.txt file of host B, then this is considered as
     * <q>prove of ownership</q> and the sitemap is allowed to list URLs on host
     * B. For cross-submit validation, see {@link SiteMapCrossSubmitValidator}.
     * 
     * @param sitemapBaseUrl
     *            the base URL of the sitemap
     * @param testUrl
     *            the URL to be tested
     * @return true if testUrl is under sitemapBaseUrl, false otherwise
     */
    public static boolean urlIsValid(String sitemapBaseUrl, String testUrl) {
        return testUrl.startsWith(sitemapBaseUrl);
    }
}
