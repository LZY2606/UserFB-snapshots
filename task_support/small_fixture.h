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

// Extended, still serialization-only Small MSF builder. The original small_msf() above is kept
// byte-for-byte stable for the three analysis inputs; this variant additionally supports the
// other legal Small page sizes/FPM positions and explicit (possibly high) physical page lists.
struct SmallMSFConfig {
    unsigned pageSize = 1024;
    unsigned fpmPage = 1;
    unsigned firstDataPage = 40;
    std::vector<bool> nil;
    // When explicitPages[i] is non-empty it is used verbatim as the unsigned 16-bit page list.
    std::vector<std::vector<unsigned>> explicitPages;
    // Physical directory pages; empty keeps the legacy 17+3*i placement.
    std::vector<unsigned> directoryPages;
};

inline Bytes small_msf_ex(const std::vector<Bytes>& streams, const SmallMSFConfig& config = {}) {
    const unsigned page = config.pageSize;
    if (page != 1024 && page != 2048 && page != 4096) throw std::runtime_error("unsupported fixture page size");
    if (streams.size() > 65534) throw std::runtime_error("too many fixture streams");

    Bytes directory;
    u16(directory, static_cast<unsigned>(streams.size())); u16(directory, 0);
    for (size_t i=0;i<streams.size();++i) {
        const bool isNil = i < config.nil.size() && config.nil[i];
        u32(directory, isNil ? 0xffffffffu : static_cast<unsigned>(streams[i].size()));
        u32(directory, 0x60000000u + static_cast<unsigned>(i)*0x101u); // obsolete pointer
    }

    std::vector<std::vector<unsigned>> pages(streams.size());
    unsigned next = config.firstDataPage;
    unsigned maxUsedPage = 0;
    for (size_t i=0;i<streams.size();++i) {
        const bool isNil = i < config.nil.size() && config.nil[i];
        if (isNil) continue;
        const unsigned needed = static_cast<unsigned>((streams[i].size()+page-1)/page);
        if (i < config.explicitPages.size() && !config.explicitPages[i].empty()) {
            pages[i] = config.explicitPages[i];
            if (pages[i].size() != needed) throw std::runtime_error("explicit page list length mismatch");
        } else {
            for (unsigned k=0;k<needed;++k) { pages[i].push_back(next); next += 2; }
        }
        for (unsigned p : pages[i]) {
            if (p >= 65535u) throw std::runtime_error("fixture page number out of u16 range");
            if (p > maxUsedPage) maxUsedPage = p;
        }
        for (unsigned p : pages[i]) u16(directory, p);
    }

    const unsigned directoryPageCount = static_cast<unsigned>((directory.size()+page-1)/page);
    std::vector<unsigned> dirPages = config.directoryPages;
    if (dirPages.empty()) {
        for (unsigned i=0;i<directoryPageCount;++i) dirPages.push_back(17+3*i);
    }
    if (dirPages.size() != directoryPageCount) throw std::runtime_error("directory page count mismatch");

    unsigned blockCount = next;
    for (unsigned p : dirPages) if (p > maxUsedPage) maxUsedPage = p;
    if (!config.explicitPages.empty()) blockCount = maxUsedPage + 1u;
    if (blockCount <= maxUsedPage) blockCount = maxUsedPage + 1u;
    if (blockCount > 65535u) throw std::runtime_error("fixture block count out of u16 range");
    for (unsigned p : dirPages) if (p >= blockCount) throw std::runtime_error("directory page beyond block count");

    Bytes image(static_cast<size_t>(blockCount)*page, 0);
    const char magic[44]="Microsoft C/C++ program database 2.00\r\n\x1aJG";
    std::memcpy(image.data(),magic,44);
    Bytes header;
    u32(header,page); u16(header,config.fpmPage); u16(header,blockCount);
    u32(header,static_cast<unsigned>(directory.size())); u32(header,0x11223344u);
    for (unsigned p : dirPages) u16(header,p);
    std::memcpy(image.data()+44,header.data(),header.size());

    for (unsigned i=0;i<directoryPageCount;++i) {
        const size_t count=std::min<size_t>(page,directory.size()-static_cast<size_t>(i)*page);
        std::memcpy(image.data()+static_cast<size_t>(dirPages[i])*page,directory.data()+static_cast<size_t>(i)*page,count);
    }
    for (size_t i=0;i<streams.size();++i) {
        for (size_t j=0;j<pages[i].size();++j) {
            const size_t count=std::min<size_t>(page,streams[i].size()-j*page);
            std::memcpy(image.data()+static_cast<size_t>(pages[i][j])*page,streams[i].data()+j*page,count);
        }
    }
    return image;
}
inline Bytes small_msf_cfg(const std::vector<Bytes>& streams, const SmallMSFConfig& config = {}) {
    return small_msf_ex(streams, config);
}
}
