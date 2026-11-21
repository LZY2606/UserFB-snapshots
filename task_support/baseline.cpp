#include <TimedText/Cue.h>
#include <TimedText/WebVTTParser.h>
#include <cstdio>
#include <cstring>
using namespace TimedText;
struct Text : NodeVisitor {
  String text;
  void visit(Node &n) { if(n.element()==TextNode) text=n.text(); }
};
int main() {
  Cue original(WebVTTCue,1.0,3.0,String("saved"),String("<b>old</b>"));
  if(!WebVTTParser::parseCuetext(original)) return 2;
  Text first; original.visit(first);
  if(std::strcmp(first.text.text(),"old")) return 3;
  Cue edited=original; edited.setId("draft");
  std::printf("copy_source_id=%s copy_edited_id=%s\n",original.id().text(),edited.id().text());
  edited.setText("<i>new</i>");
  Text after; edited.visit(after);
  std::printf("current_text=%s preview_leaf=%s\n",edited.text().text(),after.text.text());
  std::puts("TIMEDTEXT_BASELINE_OK parsed=old");
}
