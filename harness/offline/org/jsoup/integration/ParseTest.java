package org.jsoup.integration;
import java.io.File;
import java.net.URISyntaxException;
import java.net.URL;
/** Resource lookup used by HtmlParserTest; no integration-server implementation. */
public final class ParseTest {
    public static File getFile(String path) {
        URL url = ParseTest.class.getResource(path);
        if (url == null) throw new AssertionError("missing test resource " + path);
        try { return new File(url.toURI()); }
        catch (URISyntaxException e) { throw new AssertionError(e); }
    }
}
