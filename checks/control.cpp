#include <tins/ipv6.h>
#include <tins/udp.h>
#include <tins/rawpdu.h>
#include <iostream>
#include <stdexcept>
using namespace Tins;
int main(){IPv6 p("2001:db8::2","2001:db8::1");p/=UDP(80,1200)/RawPDU("odd");auto b=p.serialize();IPv6 q(b.data(),b.size());if(q.src_addr()!=p.src_addr()||q.dst_addr()!=p.dst_addr()||q.rfind_pdu<UDP>().dport()!=80||q.serialize()!=b)throw std::runtime_error("control");std::cout<<"IPV6_ROUNDTRIP_CONTROL_OK\n";}
