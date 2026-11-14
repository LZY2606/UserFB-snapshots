package org.jsoup.nodes;

import org.jsoup.Connection;
import org.jsoup.Jsoup;
import org.jsoup.helper.HttpConnection;
import org.jsoup.helper.Validate;
import org.jsoup.parser.Tag;
import org.jsoup.select.Elements;
import org.jspecify.annotations.Nullable;

import java.util.ArrayList;
import java.util.Collection;
import java.util.IdentityHashMap;
import java.util.Iterator;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.function.Predicate;
import java.util.function.UnaryOperator;

/**
 * A HTML Form Element provides ready access to the form fields/controls that are associated with it. It also allows a
 * form to easily be submitted.
 */
public class FormElement extends Element {
    // Controls linked to this form at parse time (or via addElement). A linked control may be outside this form's
    // subtree (e.g. foster-parented out of a table), so the links are tracked explicitly. A link is only live while
    // neither the control nor the form has been detached since it was made; see Element#formOwner().
    private LinkedHashSet<Element> linkedEls = new LinkedHashSet<>();

    /**
     * Create a new, standalone form element.
     *
     * @param tag        tag of this element
     * @param baseUri    the base URI
     * @param attributes initial attributes
     */
    public FormElement(Tag tag, String baseUri, Attributes attributes) {
        super(tag, baseUri, attributes);
    }

    /**
     * Get the list of form control elements associated with this form.
     * <p>The association is evaluated against the current DOM. This includes controls that the parser associated with
     * this form even though they are not descendants (e.g. inputs foster-parented out of a table), as long as neither
     * the control nor the form has since been moved or removed from the tree.</p>
     * <p>The returned list is a copy in document order, with each node only once. Changing the returned list does not
     * change the form's associations.</p>
     * @return form controls associated with this element.
     */
    public Elements elements() {
        return new ControlList(collectControls());
    }

    /** Computes the current controls, in tree order, de-duplicated by node identity. */
    private Elements collectControls() {
        Elements found = new Elements();
        IdentityHashMap<Element, Boolean> seen = new IdentityHashMap<>();

        // Walk this form's tree in document order, and keep the form-listed controls that currently resolve to this
        // form. This covers ancestor associations, live parse links, and form="#id" references in the same Document.
        Node root = root();
        if (root instanceof Element) {
            for (Element el : ((Element) root).getAllElements()) {
                if (el.tag().isFormListed() && el.formOwner() == this) {
                    found.add(el);
                    seen.put(el, Boolean.TRUE);
                }
            }
        }

        // Include live parse-time links that aren't in this tree (e.g. via addElement()), in link order. A detached
        // control resolves to no form, so it drops out here. Only prune links that are no longer valid; an explicit
        // form attribute supersedes a (still valid) parse link, but doesn't invalidate it.
        Iterator<Element> it = linkedEls.iterator();
        while (it.hasNext()) {
            Element el = it.next();
            if (!el.formLinkIs(this)) {
                it.remove();
                continue;
            }
            if (!seen.containsKey(el) && el.formOwner() == this) {
                found.add(el);
                seen.put(el, Boolean.TRUE);
            }
        }
        return found;
    }

    /**
     * Add a form control element to this form. The control will be associated with this form if it is not otherwise
     * associated to another form, and will be included in the results of {@link #elements()} and {@link #formData()}.
     * Adding the same control more than once has no further effect.
     * @param element form control to add
     * @return this form element, for chaining
     */
    public FormElement addElement(Element element) {
        element.setFormLink(this);
        linkedEls.add(element);
        return this;
    }

    /** Adds a control link to this form without setting the back-link; used when remapping links during a clone. */
    void addLinkedControl(Element element) {
        linkedEls.add(element);
    }

    @Override
    protected Element doClone(@Nullable Node parent) {
        FormElement clone = (FormElement) super.doClone(parent);
        clone.linkedEls = new LinkedHashSet<>(); // links are remapped by Node.clone if their other end was copied
        return clone;
    }

    @Override
    public FormElement shallowClone() {
        // A standalone form, with no children and no associated controls
        String baseUri = baseUri();
        if (baseUri.isEmpty()) baseUri = null;
        return new FormElement(tag(), baseUri, attributes == null ? null : attributes.clone());
    }

    /**
     Prepare to submit this form. A Connection object is created with the request set up from the form values. This
     Connection will inherit the settings and the cookies (etc) of the connection/session used to request this Document
     (if any), as available in {@link Document#connection()}
     <p>You can then set up other options (like user-agent, timeout, cookies), then execute it.</p>

     @return a connection prepared from the values of this form, in the same session as the one used to request it
     @throws IllegalArgumentException if the form's absolute action URL cannot be determined. Make sure you pass the
     document's base URI when parsing.
     */
    public Connection submit() {
        String action = hasAttr("action") ? absUrl("action") : baseUri();
        Validate.notEmpty(action, "Could not determine a form action URL for submit. Ensure you set a base URI when parsing.");
        Connection.Method method = attr("method").equalsIgnoreCase("POST") ?
                Connection.Method.POST : Connection.Method.GET;

        Document owner = ownerDocument();
        Connection connection = owner != null? owner.connection().newRequest() : Jsoup.newSession();
        return connection.url(action)
                .data(formData())
                .method(method);
    }

    /**
     * Get the data that this form submits. The returned list is a copy of the data, and changes to the contents of the
     * list will not be reflected in the DOM.
     * @return a list of key vals
     */
    public List<Connection.KeyVal> formData() {
        ArrayList<Connection.KeyVal> data = new ArrayList<>();

        // iterate the form control elements and accumulate their values
        for (Element el: elements()) {
            if (!el.tag().isFormSubmittable()) continue; // contents are form listable, superset of submitable
            if (el.hasAttr("disabled")) continue; // skip disabled form inputs
            String name = el.attr("name");
            if (name.length() == 0) continue;
            String type = el.attr("type");

            if (type.equalsIgnoreCase("button") || type.equalsIgnoreCase("image")) continue; // browsers don't submit these

            if (el.nameIs("select")) {
                Elements options = el.select("option[selected]");
                boolean set = false;
                for (Element option: options) {
                    data.add(HttpConnection.KeyVal.create(name, option.val()));
                    set = true;
                }
                if (!set) {
                    Element option = el.selectFirst("option");
                    if (option != null)
                        data.add(HttpConnection.KeyVal.create(name, option.val()));
                }
            } else if ("checkbox".equalsIgnoreCase(type) || "radio".equalsIgnoreCase(type)) {
                // only add checkbox or radio if they have the checked attribute
                if (el.hasAttr("checked")) {
                    final String val = el.val().length() >  0 ? el.val() : "on";
                    data.add(HttpConnection.KeyVal.create(name, val));
                }
            } else {
                data.add(HttpConnection.KeyVal.create(name, el.val()));
            }
        }
        return data;
    }

    @Override
    public FormElement clone() {
        return (FormElement) super.clone();
    }

    /**
     An {@link Elements} list of control elements that is independent of the form. Unlike the general {@link Elements}
     list, its mutators only change the list itself, and never the DOM — so they cannot change the form's associations.
     (The DOM-mutating overrides in {@link Elements} are bypassed by using {@link ArrayList}'s list operations directly.)
     */
    private static final class ControlList extends Elements {
        ControlList(Elements controls) {
            super(controls);
        }

        @Override public Element set(int index, Element element) {
            Validate.notNull(element);
            Element old = get(index);
            super.removeRange(index, index + 1);   // ArrayList.removeRange (Elements does not override it)
            super.add(index, element);
            return old;
        }

        @Override public Element remove(int index) {
            Element old = get(index);
            super.removeRange(index, index + 1);
            return old;
        }

        @Override public boolean remove(Object o) {
            int index = indexOf(o);
            if (index < 0) return false;
            remove(index);
            return true;
        }

        @Override public void clear() {
            super.removeRange(0, size());
        }

        @Override public boolean removeAll(Collection<?> c) {
            boolean changed = false;
            for (Object o : c) {
                int index = indexOf(o);
                if (index >= 0) {
                    remove(index);
                    changed = true;
                }
            }
            return changed;
        }

        @Override public boolean retainAll(Collection<?> c) {
            boolean changed = false;
            for (int i = size() - 1; i >= 0; i--) {
                if (!c.contains(get(i))) {
                    super.removeRange(i, i + 1);
                    changed = true;
                }
            }
            return changed;
        }

        @Override public boolean removeIf(Predicate<? super Element> filter) {
            boolean changed = false;
            for (int i = size() - 1; i >= 0; i--) {
                if (filter.test(get(i))) {
                    super.removeRange(i, i + 1);
                    changed = true;
                }
            }
            return changed;
        }

        @Override public void replaceAll(UnaryOperator<Element> operator) {
            for (int i = 0; i < size(); i++) {
                set(i, operator.apply(get(i)));
            }
        }

        /** Elements' no-arg remove() detaches all elements from the DOM; here it only clears the snapshot list. */
        @Override public Elements remove() {
            clear();
            return this;
        }
    }
}
