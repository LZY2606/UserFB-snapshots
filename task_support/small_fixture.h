#pragma once
#include "msf7_fixture.h"
// Serializes the published Small MSF container layout; no reader implementation.
namespace fixture {
inline Bytes small_msf(const std::vector<Bytes>& streams,
                       const std::vector<bool>& nil = {}, unsigned firstDataPage = 40) {
    const unsigned page = 1024;
    if (streams.size() > 65534) throw std::runtime_error("too many fixture streams");
    Bytes directory;
    u16(directory, static_cast<unsigned>(streams.size())); u16(directory, 0);
    for (size_t i=0;i<streams.size();++i) {
        u32(directory, i<nil.size() && nil[i] ? 0xffffffffu : static_cast<unsigned>(streams[i].size()));
        u32(directory, 0x60000000u + static_cast<unsigned>(i)*0x101u); // obsolete memory pointer, opaque on disk
    }
    unsigned next = firstDataPage;
    std::vector<std::vector<unsigned>> pages(streams.size());
    for (size_t i=0;i<streams.size();++i) {
        if (i<nil.size() && nil[i]) continue;
        for (size_t offset=0;offset<streams[i].size();offset+=page) {
            if (next>=65534) throw std::runtime_error("fixture page number too large");
            pages[i].push_back(next); u16(directory,next); next+=2;
        }
    }
    const unsigned directoryPages = static_cast<unsigned>((directory.size()+page-1)/page);
    if (directoryPages>8 || next<=17+3*directoryPages) throw std::runtime_error("fixture layout overlap");
    Bytes image(next*page,0);
    const char magic[44]="Microsoft C/C++ program database 2.00\r\n\x1aJG";
    std::memcpy(image.data(),magic,44);
    Bytes header;u32(header,page);u16(header,1);u16(header,next);
    u32(header,static_cast<unsigned>(directory.size()));u32(header,0x11223344u);
    for (unsigned i=0;i<directoryPages;++i)u16(header,17+3*i);
    std::memcpy(image.data()+44,header.data(),header.size());
    for (unsigned i=0;i<directoryPages;++i) {
        const auto count=std::min<size_t>(page,directory.size()-i*page);
        std::memcpy(image.data()+(17+3*i)*page,directory.data()+i*page,count);
    }
    for (size_t i=0;i<streams.size();++i) {
        for (size_t j=0;j<pages[i].size();++j) {
            const auto count=std::min<size_t>(page,streams[i].size()-j*page);
            std::memcpy(image.data()+pages[i][j]*page,streams[i].data()+j*page,count);
        }
    }
    return image;
}
}
