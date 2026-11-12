#include "id3v2tag.h"
#include "textidentificationframe.h"
#include <cassert>
#include <cstdio>
int main(){TagLib::ID3v2::Tag t;auto*f=new TagLib::ID3v2::TextIdentificationFrame("TIT2",TagLib::String::UTF8);f->setText("chapter title");t.addFrame(f);auto a=t.render(TagLib::ID3v2::v3);auto b=t.render(TagLib::ID3v2::v4);assert(a.mid(0,3)=="ID3"&&a[3]==3&&b[3]==4);assert(t.title()=="chapter title");std::printf("TAGLIB_RENDER_OK v3=%u v4=%u\n",a.size(),b.size());}
