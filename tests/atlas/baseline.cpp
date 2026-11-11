#include <iostream>
#include <string>
#include <rectpack2D/finders_interface.h>
using namespace rectpack2D;
int main() {
    std::string kind;
    while (std::cin >> kind) {
        int w,h,iw,ih; std::cin >> w >> h >> iw >> ih;
        if (kind == "limited") {
            empty_spaces<false, static_empty_spaces<1>> bin({w,h});
            auto a=bin.insert({iw,ih});
            auto b=bin.insert({w,h});
            std::cout << "limited first=" << bool(a) << " retry_full=" << bool(b)
                      << " spaces=" << bin.get_spaces().get_count() << '\n';
        } else if (kind == "rotate") {
            empty_spaces<true> bin({w,h});
            auto a=bin.insert({iw,ih});
            if (!a) return 2;
            std::cout << "rotate xywh=" << a->x << ',' << a->y << ',' << a->w << ',' << a->h
                      << " flipped=" << a->flipped << '\n';
        } else if (kind == "batch") {
            empty_spaces<false> bin({w,h});
            auto a=bin.insert({iw,ih});
            auto b=bin.insert({w,h});
            auto retry=bin.insert({w,h});
            std::cout << "batch first=" << bool(a) << " second=" << bool(b)
                      << " retry_full=" << bool(retry) << '\n';
        } else return 3;
    }
}
