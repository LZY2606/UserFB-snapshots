#pragma once
#include <stdexcept>
#include <iostream>
struct Check { int assertions=0; void operator()(bool ok,const char* msg) { ++assertions; if(!ok) throw std::runtime_error(msg); } };
void test_gradients(Check&);
void test_clipping(Check&);
void test_sampling(Check&);
