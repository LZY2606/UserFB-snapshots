#pragma once
#include <stdexcept>
#include <iostream>
struct Check { int assertions=0; void operator()(bool ok,const char* msg) { ++assertions; if(!ok) throw std::runtime_error(msg); } };
void test_classification(Check&);
void test_visibility(Check&);
void test_render(Check&);
