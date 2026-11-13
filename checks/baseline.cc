#include "re2/re2.h"
#include <cassert>
#include <iostream>
#include <string>
int main() {
  RE2::Options o; o.set_encoding(RE2::Options::EncodingLatin1);
  RE2 a("(?:admin|user)/[a-z]+",o), b("user/[a-z]+",o);
  assert(a.ok() && b.ok());
  assert(RE2::FullMatch("admin/a",a));
  assert(!RE2::FullMatch("admin/a",b));
  assert(RE2::FullMatch("user/z",a) && RE2::FullMatch("user/z",b));
  RE2 edge("\\b[A-Z]+\\b",o), zero("a\\x00b",o);
  assert(RE2::FullMatch("ABC",edge));
  assert(RE2::FullMatch(std::string("a\0b",3),zero));
  assert(!RE2::FullMatch("xABCx",edge));
  std::cout << "RE2_BASELINE_OK fullmatch boundary nul\n";
}
