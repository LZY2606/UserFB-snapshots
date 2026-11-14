#include "task_support/fixture.h"
#include "wabt/binary-reader-ir.h"
#include "wabt/binary-reader.h"
#include "wabt/apply-names.h"
#include "wabt/generate-names.h"
#include "wabt/wat-writer.h"
#include "wabt/ir.h"
#include "wabt/stream.h"
#include <iostream>
#include <stdexcept>
int main(){
 try{
  for(bool debug:{false,true}){
   auto bytes=fixture::sample(debug);wabt::ReadBinaryOptions opts;opts.read_debug_names=true;
   wabt::Module module;wabt::Errors errors;
   if(wabt::Failed(wabt::ReadBinaryIr("synthetic-labels.wasm",bytes.data(),bytes.size(),opts,&errors,&module)))throw std::runtime_error("ReadBinaryIr failed");
   if(module.funcs.size()!=1||module.funcs[0]->exprs.empty())throw std::runtime_error("missing function IR");
   if(wabt::Failed(wabt::GenerateNames(&module))||wabt::Failed(wabt::ApplyNames(&module)))throw std::runtime_error("names failed");
   wabt::MemoryStream stream;if(wabt::Failed(wabt::WriteWat(&stream,&module,wabt::WriteWatOptions{})))throw std::runtime_error("wat failed");
   const auto& data=stream.output_buffer().data;std::string wat(data.begin(),data.end());
   if(wat.find("br ")==std::string::npos)throw std::runtime_error("no real branch in output");
   std::cout<<(debug?"DEBUG_LABELS":"PLAIN_LABELS")<<" "<<(wat.find("$outer")!=std::string::npos?"preserved":"absent")<<"\n"<<wat;
  }
  std::cout<<"BASELINE_OK functions=1 structured=2 branch=1\n";
 }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
