#include "lib/check.h"
#include "checknullpointer.h"
#include "errorlogger.h"
#include "settings.h"
#include "token.h"
#include "tokenize.h"
#include "vfvalue.h"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>
struct Logger : ErrorLogger {
 std::vector<ErrorMessage> errors;
 void reportOut(const std::string&,Color) override {}
 void reportErr(const ErrorMessage& e) override {errors.push_back(e);}
};
static void require(bool ok,const char* why){if(!ok) throw std::runtime_error(why);}
static bool analyse(const std::string& code) {
 Settings settings;Logger logger;
 require(settings.library.load(nullptr,"task_support/benign.cfg").errorcode==Library::ErrorCode::OK,"old cfg load");
 Tokenizer tokenizer(settings,logger);std::istringstream input(code);
 require(tokenizer.list.createTokens(input,"benign.cpp"),"create tokens");
 require(tokenizer.simplifyTokens1(""),"actual ValueFlow run");
 bool found=false,nonNull=false;
 for(const Token* t=tokenizer.tokens();t;t=t->next()) {
  if(t->str()!="observe" || !Token::simpleMatch(t->next(),"( p )")) continue;
  if(!t->next()->astOperand1()) continue;
  const Token* pointer=t->tokAt(2);
  if(!pointer->variable() || !pointer->scope() || !pointer->exprId()) continue;
  for(const auto& value:pointer->values()) {
   if(value.isIntValue() && value.intvalue==0 && value.isImpossible()) nonNull=true;
  }
  found=true;
 }
 require(found,"observation argument located");
 bool ranChecker=false; for(Check* checker:Check::instances()) { if(dynamic_cast<CheckNullPointer*>(checker)) { checker->runChecks(tokenizer,&logger);ranChecker=true; } }
 require(ranChecker,"registered NullPointer checker reached");
 for(const auto& error:logger.errors) require(error.severity!=Severity::error,"benign memory checker diagnostic");
 return nonNull;
}
int main() {
 require(analyse("void observe(const int*); void f(const int* p) { if(p) { observe(p); } }"),"native condition nonnull proof");
 require(!analyse("void observe(const int*); bool ready(const int*); void f(const int* p) { if(ready(p)) { observe(p); } }"),"opaque guard unexpectedly has a pointer contract");
 std::cout<<"NATIVE_POINTER_BRANCH_FACT_OK\nOPAQUE_GUARD_BASELINE_OK\nCHECK_NULLPOINTER_BENIGN_OK\n";
}
