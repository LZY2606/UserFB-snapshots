#include "cases.hpp"

#include <lexbor/html/html.h>
#include <lexbor/html/accessible_name.h>
#include <lexbor/dom/interfaces/element.h>

#include <string>

namespace {

struct Doc {
    lxb_html_document_t *doc;

    explicit Doc(const char *html)
    {
        doc = lxb_html_document_create();
        if (doc == NULL) {
            throw std::runtime_error("document create failed");
        }

        if (lxb_html_document_parse(doc, (const lxb_char_t *) html,
                                    strlen(html)) != LXB_STATUS_OK)
        {
            throw std::runtime_error("parse failed");
        }
    }

    ~Doc() { lxb_html_document_destroy(doc); }

    lxb_dom_node_t *root()
    {
        return lxb_dom_interface_node(doc);
    }

    lxb_dom_element_t *by_id(const char *id)
    {
        lxb_dom_node_t *node = lxb_dom_node_by_id(root(),
                                    (const lxb_char_t *) id, strlen(id));
        if (node == NULL) {
            throw std::runtime_error(std::string("missing id ") + id);
        }

        return lxb_dom_interface_element(node);
    }
};

std::string aname(lxb_dom_element_t *el)
{
    size_t len = 0;
    lxb_char_t *text = lxb_html_element_accessible_name(el, &len);

    if (text == NULL) {
        throw std::runtime_error("accessible name allocation failed");
    }

    std::string out((const char *) text, len);

    lxb_dom_document_destroy_text(
        lxb_dom_interface_node(el)->owner_document, text);

    return out;
}

void set_attr(lxb_dom_element_t *el, const char *name, const char *value)
{
    if (lxb_dom_element_set_attribute(el, (const lxb_char_t *) name,
                                      strlen(name), (const lxb_char_t *) value,
                                      strlen(value)) == NULL)
    {
        throw std::runtime_error("set_attribute failed");
    }
}

void remove_attr(lxb_dom_element_t *el, const char *name)
{
    lxb_dom_element_remove_attribute(el, (const lxb_char_t *) name,
                                     strlen(name));
}

void set_text(lxb_dom_element_t *el, const char *text)
{
    if (lxb_dom_node_text_content_set(lxb_dom_interface_node(el),
            (const lxb_char_t *) text, strlen(text)) != LXB_STATUS_OK)
    {
        throw std::runtime_error("text_content_set failed");
    }
}

} /* namespace */

CASE(idref_context)
{
    static const char html[] =
        "<button id='b' aria-labelledby='x y x'>Go</button>"
        "<span id='x'>First</span>"
        "<span id='y'>Second</span>"
        "<input id='i' aria-labelledby='r1'>"
        "<span id='r1' aria-labelledby='r2' aria-label='Inner'>text</span>"
        "<span id='r2'>Nope</span>"
        "<a id='a' href='#' aria-labelledby='p1'>link</a>"
        "<span id='p1' aria-labelledby='p2'>Alpha</span>"
        "<span id='p2' aria-labelledby='p1'>Beta</span>"
        "<input id='i2' aria-labelledby='q1'>"
        "<span id='q1'>Before <b aria-label='Inner'>ignored</b> After</span>"
        "<button id='b2' aria-labelledby='gone empty' aria-label='Lbl' "
        "title='T'>x</button>"
        "<span id='empty'></span>"
        "<span role='BUTTON bogus' id='rb' title='Role'></span>"
        "<div role='link' id='rl'><i>Linked</i></div>";

    Doc d(html);

    /* Token order is kept, a repeated reference contributes only once. */
    CHECK(aname(d.by_id("b")) == "First Second");

    /* Nested aria-labelledby is not followed, aria-label still wins. */
    CHECK(aname(d.by_id("i")) == "Inner");

    /* Mutual references terminate: nested labelling is ignored. */
    CHECK(aname(d.by_id("a")) == "Alpha");

    /* aria-label inside a referenced subtree overrides its contents. */
    CHECK(aname(d.by_id("i2")) == "Before Inner After");

    /* Empty or unresolved references fall through to aria-label, then title. */
    CHECK(aname(d.by_id("b2")) == "Lbl");

    remove_attr(d.by_id("b2"), "aria-label");
    set_text(d.by_id("b2"), "");
    CHECK(aname(d.by_id("b2")) == "T");

    /* The first supported role token wins; matching is case-insensitive. */
    CHECK(aname(d.by_id("rb")) == "Role");
    CHECK(aname(d.by_id("rl")) == "Linked");
}

CASE(hidden_reference)
{
    static const char html[] =
        "<input id='h1' aria-labelledby='lbl1'>"
        "<span id='lbl1'>Visible <b hidden>Secret</b> End</span>"
        "<input id='h2' aria-labelledby='lbl2'>"
        "<span id='lbl2' hidden>Whole <b hidden>Secret</b> Text</span>"
        "<div aria-hidden='true'><span id='lbl3'>Deep</span></div>"
        "<button id='h3' aria-labelledby='lbl3'>x</button>"
        "<button id='h4'>Hi <span hidden>X</span>there</button>"
        "<input id='h5' aria-labelledby='lbl4'>"
        "<span id='lbl4' aria-hidden='false'>Shown</span>"
        "<input id='h7' aria-labelledby='lbl7'>"
        "<span id='lbl7' aria-hidden='TRUE'>Upper</span>"
        "<input id='h6' aria-labelledby='lbl5'>"
        "<span id='lbl5'>S<script>evil()</script>"
        "<style>a{}</style><template>t</template>End</span>";

    Doc d(html);

    /* Visible reference root: hidden descendants stay excluded. */
    CHECK(aname(d.by_id("h1")) == "Visible End");

    /* Hidden reference root: the whole referenced subtree contributes. */
    CHECK(aname(d.by_id("h2")) == "Whole Secret Text");

    /* The hidden root may live under an aria-hidden ancestor. */
    CHECK(aname(d.by_id("h3")) == "Deep");

    /* Ordinary native traversal skips hidden branches. */
    CHECK(aname(d.by_id("h4")) == "Hi there");

    /* aria-hidden="false" does not hide. */
    CHECK(aname(d.by_id("h5")) == "Shown");

    /* A hidden reference root is still exposed; token match is case-insensitive. */
    CHECK(aname(d.by_id("h7")) == "Upper");

    /* script, style and template never contribute text; adjacency is kept. */
    CHECK(aname(d.by_id("h6")) == "SEnd");
}

CASE(native_labels)
{
    static const char html[] =
        "<label for='c1'>User name</label>"
        "<input id='c1' type='text'>"
        "<label>Wrap <input id='c2' value='V'></label>"
        "<label id='lab' for='c3'>Name <input id='c3'></label>"
        "<label for='c4'>A</label><label for='c4'>B</label>"
        "<label for='c4b'>Orphan</label><input id='c4b' type='range'>"
        "<input id='c4'>"
        "<label for='c5'><span>Box <b>bold</b></span></label>"
        "<input id='c5'>"
        "<label for='c6'>Default</label><input id='c6'>"
        "<img id='im' alt='Picture'>"
        "<img id='im2' title='Photograph'>"
        "<button id='btn'>Fallback<span hidden>SECRET</span></button>"
        "<a id='lnk' href='/p'>L<i>ink</i></a>"
        "<a id='nolink'>NoHref</a>"
        "<div id='plain'>Container text</div>"
        "<label for='chk'>Agree</label>"
        "<input id='chk' type='checkbox'>"
        "<label for='rad'>Pick</label>"
        "<input id='rad' type='radio'>"
        "<label for='srch'>Query</label>"
        "<input id='srch' type='search'>"
        "<span role='bogus switch' id='sw' title='Toggle'></span>"
        "<span role='checkbox' id='rchk' aria-label='Check'></span>"
        "<div role='textbox' id='rtb'><input value='TV'></div>"
        "<div role='combobox' id='rcb'>"
        "<select><option selected>A</option><option>B</option></select></div>";

    Doc d(html);

    /* Explicit for/id association. */
    CHECK(aname(d.by_id("c1")) == "User name");

    /* Wrapping label: the named control itself is skipped in label text. */
    CHECK(aname(d.by_id("c2")) == "Wrap");

    /* Self-referencing label does not re-expand the control being named. */
    CHECK(aname(d.by_id("c3")) == "Name");

    /* Several labels join in document order with a separating space. */
    CHECK(aname(d.by_id("c4")) == "A B");

    /* A label targeting a non-supported (non-labelable) control is dropped. */
    CHECK(aname(d.by_id("c4b")) == "");

    /* Ordinary containers still contribute text when used as label content. */
    CHECK(aname(d.by_id("c5")) == "Box bold");

    /* Missing type defaults to text and is labelable. */
    CHECK(aname(d.by_id("c6")) == "Default");

    /* img uses alt, then title as the last resort. */
    CHECK(aname(d.by_id("im")) == "Picture");
    CHECK(aname(d.by_id("im2")) == "Photograph");

    /* Button contents hide the hidden tooltip. */
    CHECK(aname(d.by_id("btn")) == "Fallback");

    /* Links take their contents; adjacent text keeps its concatenation. */
    CHECK(aname(d.by_id("lnk")) == "Link");
    CHECK(aname(d.by_id("nolink")) == "");

    /* Unsupported roots produce an empty but releaseable name. */
    size_t len = 1;
    lxb_char_t *text = lxb_html_element_accessible_name(d.by_id("plain"),
                                                         &len);
    CHECK(text != NULL);
    CHECK(len == 0);
    lxb_dom_document_destroy_text(
        lxb_dom_interface_node(d.by_id("plain"))->owner_document, text);

    CHECK(aname(d.by_id("chk")) == "Agree");
    CHECK(aname(d.by_id("rad")) == "Pick");
    CHECK(aname(d.by_id("srch")) == "Query");

    /* Explicit roles: the first supported token, with the usual fallbacks. */
    CHECK(aname(d.by_id("sw")) == "Toggle");
    CHECK(aname(d.by_id("rchk")) == "Check");

    /* Role containers take embedded values from their contents. */
    CHECK(aname(d.by_id("rtb")) == "TV");
    CHECK(aname(d.by_id("rcb")) == "A");
}

CASE(embedded_values)
{
    static const char html[] =
        "<button id='e1'>Go <input value='Val'></button>"
        "<button id='e2'>Msg <textarea>hello</textarea></button>"
        "<button id='e3'>Pick "
        "<select><option>A</option><option selected>B</option></select>"
        "</button>"
        "<button id='e4'>Pick "
        "<select><option>One</option><option>Two</option></select>"
        "</button>"
        "<div role='button' id='e5'>Pick "
        "<select multiple><option selected>A</option>"
        "<option selected>B</option></select></div>"
        "<button id='e6'>Line1<br>Line2</button>"
        "<button id='e7'>Go <input type='checkbox'> <input value=''></button>"
        "<button id='e8'>  A   B\n\t</button>"
        "<button id='e9'>名前</button>"
        "<button id='e10'>T <input value='X'><textarea>Y</textarea></button>"
        "<button id='e11'>Group "
        "<select><optgroup label='G'>"
        "<option>O1</option><option selected>O2</option>"
        "</optgroup></select></button>";

    Doc d(html);

    /* Embedded text inputs contribute the current value attribute. */
    CHECK(aname(d.by_id("e1")) == "Go Val");

    /* textarea contributes its DOM text. */
    CHECK(aname(d.by_id("e2")) == "Msg hello");

    /* The actually selected option is used, not the selected attribute alone. */
    CHECK(aname(d.by_id("e3")) == "Pick B");

    /* First option gains selectedness by default. */
    CHECK(aname(d.by_id("e4")) == "Pick One");

    /* Multiple selected options are joined with spaces. */
    CHECK(aname(d.by_id("e5")) == "Pick A B");

    /* br inserts a separator even without surrounding whitespace. */
    CHECK(aname(d.by_id("e6")) == "Line1 Line2");

    /* Checkboxes and empty values contribute nothing. */
    CHECK(aname(d.by_id("e7")) == "Go");

    /* ASCII whitespace is collapsed and trimmed; non-ASCII is preserved. */
    CHECK(aname(d.by_id("e8")) == "A B");
    CHECK(aname(d.by_id("e9")) == "名前");

    /* Each embedded value is separated from its neighbours. */
    CHECK(aname(d.by_id("e10")) == "T X Y");

    /* Selected options nested in optgroups are still found; optgroup label
     * attribute does not contribute at this stage. */
    CHECK(aname(d.by_id("e11")) == "Group O2");
}

CASE(dom_refresh)
{
    static const char html[] =
        "<span id='n1'>One</span><span id='n2'>Two</span>"
        "<button id='r1' aria-labelledby='n1'>x</button>"
        "<label id='lab' for='f1'>L1</label>"
        "<input id='f1'>"
        "<input id='f2'>"
        "<button id='r2'>Go <input id='vi' value='Old'></button>"
        "<button id='r3'>Msg <textarea id='ta'>old text</textarea></button>"
        "<div role='button' id='r4'>Pick "
        "<select id='sel'><option id='o1'>O1</option>"
        "<option id='o2'>O2</option></select></div>";

    Doc d(html);

    /* Initial aria-labelledby resolution. */
    CHECK(aname(d.by_id("r1")) == "One");

    /* Re-pointing the reference list is seen on the next call. */
    set_attr(d.by_id("r1"), "aria-labelledby", "n2");
    CHECK(aname(d.by_id("r1")) == "Two");

    /* Label text edits are picked up immediately. */
    set_text(d.by_id("n2"), "Three");
    CHECK(aname(d.by_id("r1")) == "Three");

    /* Removing the attribute falls back to native contents. */
    remove_attr(d.by_id("r1"), "aria-labelledby");
    CHECK(aname(d.by_id("r1")) == "x");

    /* label[for] retargeting changes association instantly. */
    CHECK(aname(d.by_id("f1")) == "L1");
    CHECK(aname(d.by_id("f2")) == "");
    set_attr(d.by_id("lab"), "for", "f2");
    CHECK(aname(d.by_id("f1")) == "");
    CHECK(aname(d.by_id("f2")) == "L1");

    /* value attribute edits flow into embedded values. */
    CHECK(aname(d.by_id("r2")) == "Go Old");
    set_attr(d.by_id("vi"), "value", "New");
    CHECK(aname(d.by_id("r2")) == "Go New");

    /* textarea text edits flow into embedded values. */
    CHECK(aname(d.by_id("r3")) == "Msg old text");
    set_text(d.by_id("ta"), "new text");
    CHECK(aname(d.by_id("r3")) == "Msg new text");

    /* Select default selectedness: the first option starts selected. */
    CHECK(aname(d.by_id("r4")) == "Pick O1");

    /* Changing selectedness is reflected without any cached result. */
    set_attr(d.by_id("o2"), "selected", "selected");
    CHECK(aname(d.by_id("r4")) == "Pick O1 O2");
    remove_attr(d.by_id("o2"), "selected");
    CHECK(aname(d.by_id("r4")) == "Pick O1");

    /* id edits change IDREF resolution immediately. */
    set_attr(d.by_id("n1"), "id", "renamed");
    set_attr(d.by_id("r1"), "aria-labelledby", "renamed");
    CHECK(aname(d.by_id("r1")) == "One");
}
