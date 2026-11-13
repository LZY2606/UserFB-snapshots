#include <workflow/WFTaskFactory.h>
#include <workflow/WFHttpServer.h>
#include <workflow/WFFacilities.h>
#include <arpa/inet.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <mutex>
#include <vector>
static void check(bool b, const char *s) { if(!b){std::fprintf(stderr,"FAIL %s\n",s);std::exit(1);} }
int main() {
 std::atomic<int> hits{0}, complete{0}, joined{0};
 std::mutex mu; std::vector<std::string> bodies;
 WFHttpServer server([&](WFHttpTask *t) {
  ++hits; std::string path=t->get_req()->get_request_uri();
  t->get_resp()->set_http_version("HTTP/1.1"); t->get_resp()->set_status_code("200");
  t->get_resp()->set_reason_phrase("OK");
  t->get_resp()->append_output_body(path=="/mode"?"delta":path);
 });
 check(server.start("127.0.0.1",0)==0,"server start");
 sockaddr_in address{};socklen_t len=sizeof(address);
 check(server.get_listen_addr((sockaddr*)&address,&len)==0,"listen address");
 std::string root="http://127.0.0.1:"+std::to_string(ntohs(address.sin_port));
 auto http=[&](const char *path) {
  return WFTaskFactory::create_http_task((root+path).c_str(),0,0,[&](WFHttpTask*t){
   check(t->get_state()==WFT_STATE_SUCCESS,"HTTP task state");
   check(std::string(t->get_resp()->get_status_code())=="200","HTTP status");
   const void* data;size_t n;t->get_resp()->get_parsed_body(&data,&n);
   {std::lock_guard<std::mutex> lock(mu); bodies.emplace_back((const char*)data,n);}
   ++complete;
  });
 };
 WFFacilities::WaitGroup wait(1);
 auto g=WFTaskFactory::create_graph_task([&](WFGraphTask*){wait.done();});
 auto &mode=g->create_graph_node(http("/mode"));
 auto &full=g->create_graph_node(http("/full"));
 auto &delta=g->create_graph_node(http("/delta"));
 auto &join=g->create_graph_node(WFTaskFactory::create_timer_task(0,[&](WFTimerTask*){
  check(complete==3,"fan-in observes all HTTP callbacks");++joined;
 }));
 mode.precede(full);mode.precede(delta);full.precede(join);delta.precede(join);
 g->start(); wait.wait(); server.stop();
 check(hits==3 && joined==1 && bodies.size()==3,"graph counts");
 bool mode_ok=false,full_ok=false,delta_ok=false;
 for(auto &s:bodies){mode_ok|=s=="delta";full_ok|=s=="/full";delta_ok|=s=="/delta";}
 check(mode_ok && full_ok && delta_ok,"actual response bodies");
 std::puts("BASELINE HTTP graph hits=3 full=1 delta=1 join=1");
 std::puts("BASELINE existing graph has unconditional edges; target conditional selection is not implemented");
}
