// filter_demo.cpp
//
// Renders a small PNG that demonstrates the supported filter pipeline:
//   panel 1 - the plain SourceGraphic,
//   panel 2 - two named branches (a blurred glow and an offset copy) joined
//             with feComposite operator="over",
//   panel 3 - a nested filter where the child's blur becomes part of the
//             parent's SourceGraphic and is then offset.
//
// Usage: filter_demo <output.png>

#include <lunasvg.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace lunasvg;

namespace {

constexpr int kPanelWidth = 80;
constexpr int kPanelHeight = 70;
constexpr int kPanelGap = 8;
constexpr int kWidth = kPanelWidth * 3 + kPanelGap * 4;
constexpr int kHeight = kPanelHeight + kPanelGap * 2;

std::string panelSvg(const std::string& defs, const std::string& body)
{
    return "<svg xmlns='http://www.w3.org/2000/svg' width='" + std::to_string(kPanelWidth)
        + "' height='" + std::to_string(kPanelHeight) + "'>" + defs + body + "</svg>";
}

uint32_t pixelAt(const Bitmap& bitmap, int x, int y)
{
    uint32_t value = 0;
    std::memcpy(&value, bitmap.data() + y * bitmap.stride() + x * 4, 4);
    return value;
}

int alphaAt(const Bitmap& bitmap, int x, int y)
{
    return static_cast<int>(pixelAt(bitmap, x, y) >> 24);
}

void renderPanel(Document& document, Bitmap& target, int originX, int originY)
{
    Matrix matrix(1.f, 0.f, 0.f, 1.f, static_cast<float>(originX), static_cast<float>(originY));
    document.render(target, matrix);
}

void require(bool condition, const std::string& message)
{
    if(!condition)
        throw std::runtime_error(message);
}

} // namespace

int main(int argc, char** argv)
{
    if(argc != 2) {
        std::cerr << "usage: " << (argc > 0 ? argv[0] : "filter_demo") << " <output.png>\n";
        return 2;
    }

    const std::string region =
        "filterUnits='userSpaceOnUse' x='0' y='0' width='" + std::to_string(kPanelWidth)
        + "' height='" + std::to_string(kPanelHeight) + "'";
    const std::string shape = "<rect x='24' y='24' width='22' height='18' rx='3' fill='#d8432a'/>";

    // Panel 1: unfiltered SourceGraphic.
    auto sourceDocument = Document::loadFromData(panelSvg("", shape));
    require(sourceDocument != nullptr, "failed to load source panel");

    // Panel 2: a blurred branch named "glow" and an offset branch named
    // "shift", composited over the original SourceGraphic.
    const std::string branchDefs =
        "<defs><filter id='f' " + region + ">"
        "<feGaussianBlur in='SourceGraphic' stdDeviation='2.2' result='glow'/>"
        "<feOffset in='SourceGraphic' dx='6' dy='5' result='shift'/>"
        "<feComposite in='shift' in2='glow' operator='over' result='merged'/>"
        "<feComposite in='SourceGraphic' in2='merged' operator='over'/>"
        "</filter></defs>";
    auto branchDocument = Document::loadFromData(panelSvg(branchDefs,
        "<rect x='24' y='24' width='22' height='18' rx='3' fill='#d8432a' filter='url(#f)'/>"));
    require(branchDocument != nullptr, "failed to load branch panel");

    // Panel 3: nested filters - the child blurs its shape, the parent then
    // offsets the already blurred result.
    const std::string nestedDefs =
        "<defs>"
        "<filter id='child' " + region + "><feGaussianBlur stdDeviation='1.6'/></filter>"
        "<filter id='parent' " + region + "><feOffset dx='7' dy='-4'/></filter>"
        "</defs>";
    auto nestedDocument = Document::loadFromData(panelSvg(nestedDefs,
        "<g filter='url(#parent)'><g filter='url(#child)'>" + shape + "</g></g>"));
    require(nestedDocument != nullptr, "failed to load nested panel");

    Bitmap output(kWidth, kHeight);
    require(!output.isNull(), "failed to allocate output bitmap");
    renderPanel(*sourceDocument, output, kPanelGap, kPanelGap);
    renderPanel(*branchDocument, output, kPanelGap * 2 + kPanelWidth, kPanelGap);
    renderPanel(*nestedDocument, output, kPanelGap * 3 + kPanelWidth * 2, kPanelGap);

    // Representative pixel checks before writing the file.
    const int sourceOriginX = kPanelGap;
    const int branchOriginX = kPanelGap * 2 + kPanelWidth;
    const int nestedOriginX = kPanelGap * 3 + kPanelWidth * 2;

    // Source panel: hard interior and empty surroundings.
    require(alphaAt(output, sourceOriginX + 30, kPanelGap + 32) == 255, "source interior is opaque");
    require(alphaAt(output, sourceOriginX + 4, kPanelGap + 4) == 0, "source surroundings are empty");

    // Branch panel: glow tail around the original position and the shifted
    // copy at (24+6, 24+5).
    require(alphaAt(output, branchOriginX + 20, kPanelGap + 21) > 0
        && alphaAt(output, branchOriginX + 20, kPanelGap + 21) < 255, "blurred glow branch has soft edges");
    require(alphaAt(output, branchOriginX + 36, kPanelGap + 36) == 255, "offset branch is present");

    // Nested panel: the blurred child is displaced by the parent filter.
    require(alphaAt(output, nestedOriginX + 38, kPanelGap + 26) == 255, "nested filter offsets the blurred child");
    require(alphaAt(output, nestedOriginX + 28, kPanelGap + 30) > 0
        && alphaAt(output, nestedOriginX + 28, kPanelGap + 30) < 255, "nested filter keeps the blur tail");

    if(!output.writeToPng(argv[1])) {
        std::cerr << "failed to write " << argv[1] << '\n';
        return 1;
    }

    std::cout << "FILTER_DEMO_OK\n";
    return 0;
}
