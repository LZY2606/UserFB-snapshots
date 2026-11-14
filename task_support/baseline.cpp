#include "html.hpp"
#include <cassert>
#include <iostream>
#include <sstream>
int main() {
    html::parser parser;
    unsigned callbacks=0;
    parser.set_callback(html::selector("p[data-id='7']"),[&](html::node& n) {
        if(n.type_tag==html::tag_t::open) callbacks++;
    });
    auto normal=parser.parse("<main><p data-id='7'>entry</p><p>other</p></main>");
    auto rows=normal->select("p[data-id='7']");
    assert(rows.size()==1 && rows[0]->to_text()=="entry" && callbacks==1);
    std::istringstream stream("<main><p data-id='7'>entry</p><p>other</p></main>");
    auto streamed=parser.parse(stream);
    assert(streamed->to_raw_html()==normal->to_raw_html());
    html::node copied(*normal);
    assert(copied.select("p[data-id='7']").size()==1);
    std::cout<<"BASELINE_OK selected="<<rows.size()<<" text="<<rows[0]->to_text()<<" callbacks="<<callbacks<<"\n";
    parser.clear_callbacks();
    auto mixed=parser.parse("<svg viewBox='0 0 10 10'><title><b>caption</b></title><g><![CDATA[price<net>]]></g></svg>");
    auto title=mixed->select("b"); auto svg=mixed->select("svg"); auto g=mixed->select("g");
    assert(svg.size()==1 && g.size()==1);
    std::cout<<"FOREIGN_OBSERVATION title_b="<<title.size()<<" viewBox="<<svg[0]->get_attr("viewBox")<<" g_text="<<g[0]->to_text()<<"\n";
}
