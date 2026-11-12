#include <tins/ipv6.h>
#include <tins/udp.h>
#include <tins/rawpdu.h>
#include <iostream>
#include <vector>
#include <stdexcept>
using namespace Tins;
static unsigned sum(const uint8_t*p,size_t n){unsigned s=0;for(size_t i=0;i<n;i+=2)s+=(unsigned(p[i])<<8)|(i+1<n?p[i+1]:0);return s;}
static unsigned residual(const std::vector<uint8_t>&b,size_t start,IPv6Address src,IPv6Address dst){
 uint8_t pseudo[40]={0};src.copy(pseudo);dst.copy(pseudo+16);size_t n=b.size()-start;
 pseudo[32]=(n>>24)&255;pseudo[33]=(n>>16)&255;pseudo[34]=(n>>8)&255;pseudo[35]=n&255;pseudo[39]=17;
 unsigned s=sum(pseudo,40)+sum(b.data()+start,n);while(s>>16)s=(s&65535)+(s>>16);return s;
}
int main(){
 IPv6Address src("2001:db8::1"),next("2001:db8::10"),final("2001:db8::20");
 IPv6 plain(next,src);plain/=UDP(2,1)/RawPDU("odd");auto p=plain.serialize();
 if(residual(p,40,src,next)!=65535)throw std::runtime_error("control checksum");
 std::cout<<"IPV6_PLAIN_CONTROL_OK\n";
 std::vector<uint8_t> rh(22,0);rh[0]=2;rh[1]=1;final.copy(rh.data()+6);
 IPv6 routed(next,src);routed.add_header(IPv6::ext_header(IPv6::ROUTING,rh.size(),rh.data()));
 routed/=UDP(2,1)/RawPDU("odd");auto b=routed.serialize();
 unsigned wrong=residual(b,64,src,final),wire=residual(b,64,src,next);
 std::cout<<"ROUTED_CHECKSUM final_residual="<<wrong<<" wire_residual="<<wire<<"\n";
 if(wrong==65535||wire!=65535)throw std::runtime_error("startpoint changed");
 IPv6 reparsed(b.data(),b.size());if(!reparsed.find_pdu<UDP>())throw std::runtime_error("parse");
 std::cout<<"IPV6_ROUTING_BASELINE_REPRODUCED\n";
}
