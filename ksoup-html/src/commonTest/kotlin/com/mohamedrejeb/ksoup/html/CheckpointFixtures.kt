package com.mohamedrejeb.ksoup.html

/**
 * HTML fixtures for checkpoint/resume tests.
 *
 * Each fixture deliberately contains constructs that must not be split by a
 * checkpoint (entities, comments, raw-text elements, foreign content) and
 * multibyte UTF-8 at stream boundaries.
 */
internal object CheckpointFixtures {

    /** Attributes and entities that land across chunk boundaries. */
    val ATTRS_AND_ENTITIES = """
        <div class="main &amp; co" data-x='a&#39;b'>
          <p>Tom &amp; Jerry &copy; 2026 &#8211; done</p>
          <a href="/x?a=1&amp;b=2" title="&lt;escaped&gt;">link</a>
          <input type="text" disabled value="&quot;hi&quot;">
        </div>
    """.trimIndent()

    /** Script raw-text with partial end sequences and escaped-looking content. */
    val SCRIPT_ESCAPED = """
        <html>
        <head>
        <script>
          var a = 1;
          var s = "</script";        // partial end sequence, must stay text
          var t = "</script" + ">";  // completed only via concatenation
          if (a < b && c > d) { /* &not;an entity */ }
          var u = "<\/script>";
        </script>
        <style>.x { content: "</style"; }</style>
        </head>
        <body><p>after script &amp; text</p></body>
        </html>
    """.trimIndent()

    /** Foreign content: SVG and MathML with self-closing and integration points. */
    val FOREIGN_CONTENT = """
        <div>
        <svg width="100" viewBox="0 0 10 10">
          <circle cx="5" cy="5" r="4" />
          <foreignObject><div xmlns="http://www.w3.org/1999/xhtml">x</div></foreignObject>
          <title>svg &amp; title</title>
          <desc>a &#8211; b</desc>
        </svg>
        <math>
          <mi>x</mi>
          <mtext>text &amp; more</mtext>
          <annotation-xml encoding="text/html"><span>hi</span></annotation-xml>
        </math>
        </div>
    """.trimIndent()

    /** Parse-error recovery: misnested tags, implied closes, stray markup. */
    val ERROR_RECOVERY = """
        <p>one<p>two
        <ul><li>a<li>b</ul>
        <table><tr><td>x<tr><td>y</table>
        <a>open only<a/>
        <b>bold </i>italic
        </p>
        <br/>
        <p>three<br>four
    """.trimIndent()

    /** Comments, CDATA, declarations and processing instructions. */
    val COMMENTS_AND_DECL = """
        <!DOCTYPE html>
        <!-- a normal comment -->
        <?pi target data?>
        <![CDATA[ some cdata & not an entity ]]>
        <div><!-- nested -- dash -- ></div>
        <!decl thing>
    """.trimIndent()

    /**
     * Multibyte UTF-8 at the very end of the stream, including a 4-byte
     * emoji and characters split so a chunk boundary falls inside a sequence.
     */
    val TRAILING_UTF8 = "<p>café 中文 😀😁</p>"

    /** All fixtures, exercised at every safe point. */
    val ALL: List<Pair<String, String>> = listOf(
        "attrsAndEntities" to ATTRS_AND_ENTITIES,
        "scriptEscaped" to SCRIPT_ESCAPED,
        "foreignContent" to FOREIGN_CONTENT,
        "errorRecovery" to ERROR_RECOVERY,
        "commentsAndDecl" to COMMENTS_AND_DECL,
        "trailingUtf8" to TRAILING_UTF8,
    )
}
