#pragma once
#include <stdexcept>
#include <iostream>
struct Check { int assertions=0; void operator()(bool ok,const char* msg) { ++assertions; if(!ok) throw std::runtime_error(msg); } };
void test_accumulation(Check&);
void test_depth(Check&);
void test_frames(Check&);
