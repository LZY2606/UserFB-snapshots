#include "html/ParserDom.h"
#include <iostream>
#include <stdexcept>
using namespace htmlcxx::HTML;
class Events : public ParserSax {
public: unsigned starts=0, ends=0, tags=0, texts=0;
protected:
 void beginParsing() override {++starts;}
 void endParsing() override {++ends;}
 void foundTag(Node, bool) override {++tags;}
 void foundText(Node) override {++texts;}
};
static void check(bool b) {if(!b) throw std::runtime_error("baseline mismatch");}
int main() {
 const std::string html="<article data-k='a>b'>one<!--x--></article><article>two</article>";
 ParserDom p; const auto& tree=p.parseTree(html);
 unsigned records=0,comments=0; std::string text;
 for(auto i=tree.begin();i!=tree.end();++i) {
   if(i->isTag() && i->tagName()=="article") {++records; check(!i->closingText().empty());}
   if(i->isComment()) ++comments;
   if(!i->isTag() && !i->isComment()) text+=i->text();
 }
 check(records==2 && comments==1 && text=="onetwo");
 check(tree.begin()->length()==html.size());
 std::cout<<"ONE_SHOT records="<<records<<" text="<<text<<" bytes="<<tree.begin()->length()<<"\n";
 Events e; e.parse(std::string("<article>one</article>")); e.parse(std::string("<article>two</article>"));
 check(e.starts==2 && e.ends==2 && e.tags==4 && e.texts==2);
 std::cout<<"SEPARATE_PARSE starts="<<e.starts<<" ends="<<e.ends<<" tags="<<e.tags<<"\n";
 p.parseTree("<article>one</article>"); p.parseTree("<article>two</article>");
 unsigned count=0; std::string last;
 for(auto i=p.getTree().begin();i!=p.getTree().end();++i) {
  if(i->isTag()&&i->tagName()=="article") ++count;
  if(!i->isTag()&&!i->isComment()) last+=i->text();
 }
 check(count==1 && last=="two");
 std::cout<<"REPARSE records="<<count<<" text="<<last<<" first_record_replaced=yes\n";
}
