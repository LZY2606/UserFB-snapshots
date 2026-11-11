// Filter rendering contract tests for LunaSVG.
//
// Every numeric expectation is computed in this file: the Gaussian weights,
// separable convolutions, bilinear offsets and composite math below are
// independent of the library's filter implementation. The only library calls
// used to obtain source pixels are ordinary, filter-free renders.

#include <lunasvg.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace lunasvg;

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Failure : std::runtime_error {
    explicit Failure(const std::string& message) : std::runtime_error(message) {}
};

void require(bool condition, const std::string& message)
{
    if(!condition)
        throw Failure(message);
}

std::string svg(const std::string& body, int width = 80, int height = 70)
{
    return "<svg xmlns='http://www.w3.org/2000/svg' width='" + std::to_string(width)
        + "' height='" + std::to_string(height) + "'>" + body + "</svg>";
}

Bitmap render(const std::string& document, int width, int height)
{
    auto doc = Document::loadFromData(document);
    require(doc != nullptr, "document failed to load");
    auto bitmap = doc->renderToBitmap(width, height);
    require(!bitmap.isNull() && bitmap.width() == width && bitmap.height() == height, "bad bitmap");
    return bitmap;
}

// Premultiplied ARGB pixels, one float per channel.
struct Image {
    int w = 0;
    int h = 0;
    std::vector<float> p;

    Image() = default;
    Image(int width, int height) : w(width), h(height), p(static_cast<size_t>(width) * height * 4, 0.f) {}

    float* at(int x, int y) { return p.data() + (static_cast<size_t>(y) * w + x) * 4; }
    const float* at(int x, int y) const { return p.data() + (static_cast<size_t>(y) * w + x) * 4; }
};

Image capture(const Bitmap& bitmap)
{
    Image image(bitmap.width(), bitmap.height());
    for(int y = 0; y < image.h; ++y) {
        for(int x = 0; x < image.w; ++x) {
            uint32_t value;
            std::memcpy(&value, bitmap.data() + y * bitmap.stride() + x * 4, 4);
            auto* out = image.at(x, y);
            out[0] = static_cast<float>((value >> 16) & 0xff);
            out[1] = static_cast<float>((value >> 8) & 0xff);
            out[2] = static_cast<float>(value & 0xff);
            out[3] = static_cast<float>((value >> 24) & 0xff);
        }
    }

    return image;
}

uint32_t pixel(const Bitmap& bitmap, int x, int y)
{
    uint32_t value = 0;
    std::memcpy(&value, bitmap.data() + y * bitmap.stride() + x * 4, 4);
    return value;
}

int channelAlpha(uint32_t value) { return (value >> 24) & 0xff; }
int channelRed(uint32_t value) { return (value >> 16) & 0xff; }
int channelGreen(uint32_t value) { return (value >> 8) & 0xff; }
int channelBlue(uint32_t value) { return value & 0xff; }

// Independently generated normalized discrete Gaussian: radius ceil(3*sigma),
// weights exp(-k^2/(2 sigma^2)) normalized over the finite window.
std::vector<double> gaussianWeights(double sigma, int& radius)
{
    radius = static_cast<int>(std::ceil(3.0 * sigma));
    std::vector<double> weights(2 * radius + 1);
    double sum = 0.0;
    double denominator = 2.0 * sigma * sigma;
    for(int k = -radius; k <= radius; ++k) {
        double weight = std::exp(-static_cast<double>(k) * k / denominator);
        weights[k + radius] = weight;
        sum += weight;
    }

    for(auto& weight : weights)
        weight /= sum;
    return weights;
}

// Zero-extension tap: anything outside the source image is transparent black.
void tap(const Image& source, int x, int y, float out[4])
{
    if(x < 0 || y < 0 || x >= source.w || y >= source.h) {
        out[0] = out[1] = out[2] = out[3] = 0.f;
        return;
    }

    const auto* in = source.at(x, y);
    for(int k = 0; k < 4; ++k)
        out[k] = in[k];
}

// One separable Gaussian axis on premultiplied channels. Edges deliberately
// keep the unrenormalized kernel so transparent surroundings bleed in.
Image blurAxis(const Image& source, double sigma, bool horizontal)
{
    Image output(source.w, source.h);
    if(sigma <= 0.0) {
        output.p = source.p;
        return output;
    }

    int radius;
    auto weights = gaussianWeights(sigma, radius);
    for(int y = 0; y < source.h; ++y) {
        for(int x = 0; x < source.w; ++x) {
            double accum[4] = {0.0, 0.0, 0.0, 0.0};
            for(int k = -radius; k <= radius; ++k) {
                float value[4];
                tap(source, horizontal ? x - k : x, horizontal ? y : y - k, value);
                double weight = weights[k + radius];
                for(int c = 0; c < 4; ++c)
                    accum[c] += value[c] * weight;
            }

            auto* out = output.at(x, y);
            for(int c = 0; c < 4; ++c)
                out[c] = static_cast<float>(accum[c]);
        }
    }

    return output;
}

Image referenceBlur(const Image& source, double sigmaX, double sigmaY)
{
    auto horizontal = blurAxis(source, sigmaX, true);
    return blurAxis(horizontal, sigmaY, false);
}

// Bilinear sampling with zero extension, used for fractional offsets.
void sampleBilinear(const Image& source, float gx, float gy, float out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0.f;
    int x0 = static_cast<int>(std::floor(gx));
    int y0 = static_cast<int>(std::floor(gy));
    float fx = gx - static_cast<float>(x0);
    float fy = gy - static_cast<float>(y0);
    for(int oy = 0; oy < 2; ++oy) {
        for(int ox = 0; ox < 2; ++ox) {
            float value[4];
            tap(source, x0 + ox, y0 + oy, value);
            float weight = (ox == 0 ? 1.f - fx : fx) * (oy == 0 ? 1.f - fy : fy);
            for(int c = 0; c < 4; ++c)
                out[c] += value[c] * weight;
        }
    }
}

Image referenceOffset(const Image& source, double dx, double dy)
{
    Image output(source.w, source.h);
    for(int y = 0; y < source.h; ++y) {
        for(int x = 0; x < source.w; ++x) {
            float value[4];
            sampleBilinear(source, static_cast<float>(x - dx), static_cast<float>(y - dy), value);
            auto* out = output.at(x, y);
            for(int c = 0; c < 4; ++c)
                out[c] = value[c];
        }
    }

    return output;
}

Image referenceComposite(const Image& first, const Image& second, bool inside)
{
    Image output(first.w, first.h);
    for(int y = 0; y < first.h; ++y) {
        for(int x = 0; x < first.w; ++x) {
            const auto* a = first.at(x, y);
            const auto* b = second.at(x, y);
            auto* out = output.at(x, y);
            if(inside) {
                float alpha = b[3] / 255.f;
                for(int c = 0; c < 4; ++c)
                    out[c] = a[c] * alpha;
            } else {
                float alpha = a[3] / 255.f;
                for(int c = 0; c < 4; ++c)
                    out[c] = a[c] + b[c] * (1.f - alpha);
            }
        }
    }

    return output;
}

Image sourceAlphaOf(const Image& source)
{
    Image output(source.w, source.h);
    for(int i = 0; i < source.w * source.h; ++i)
        output.p[static_cast<size_t>(i) * 4 + 3] = source.p[static_cast<size_t>(i) * 4 + 3];
    return output;
}

void assertImagesClose(const Image& expected, const Bitmap& actual, double tolerance,
                       const std::string& message, int margin = 0)
{
    require(expected.w == actual.width() && expected.h == actual.height(), message + ": size mismatch");
    auto observed = capture(actual);
    for(int y = margin; y < expected.h - margin; ++y) {
        for(int x = margin; x < expected.w - margin; ++x) {
            const auto* e = expected.at(x, y);
            const auto* o = observed.at(x, y);
            for(int c = 0; c < 4; ++c) {
                if(std::abs(e[c] - o[c]) > tolerance)
                    throw Failure(message + ": channel mismatch at (" + std::to_string(x) + "," + std::to_string(y) + ")");
            }
        }
    }
}

struct Moments {
    double cx = 0.0;
    double cy = 0.0;
    double mxx = 0.0;
    double myy = 0.0;
    double mxy = 0.0;
    double weight = 0.0;
};

Moments alphaMoments(const Image& image)
{
    Moments moments;
    for(int y = 0; y < image.h; ++y) {
        for(int x = 0; x < image.w; ++x) {
            double alpha = image.at(x, y)[3];
            if(alpha <= 0.0)
                continue;
            moments.weight += alpha;
            moments.cx += (x + 0.5) * alpha;
            moments.cy += (y + 0.5) * alpha;
        }
    }

    require(moments.weight > 0.0, "empty alpha mass in moment check");
    moments.cx /= moments.weight;
    moments.cy /= moments.weight;
    for(int y = 0; y < image.h; ++y) {
        for(int x = 0; x < image.w; ++x) {
            double alpha = image.at(x, y)[3];
            if(alpha <= 0.0)
                continue;
            double dx = (x + 0.5) - moments.cx;
            double dy = (y + 0.5) - moments.cy;
            moments.mxx += dx * dx * alpha;
            moments.myy += dy * dy * alpha;
            moments.mxy += dx * dy * alpha;
        }
    }

    moments.mxx /= moments.weight;
    moments.myy /= moments.weight;
    moments.mxy /= moments.weight;
    return moments;
}

double kernelVariance(double sigma)
{
    if(sigma <= 0.0)
        return 0.0;
    int radius;
    auto weights = gaussianWeights(sigma, radius);
    double variance = 0.0;
    for(int k = -radius; k <= radius; ++k)
        variance += static_cast<double>(k) * k * weights[k + radius];
    return variance;
}

constexpr int kWidth = 80;
constexpr int kHeight = 70;

int caseBlurReference()
{
    int assertions = 0;
    auto check = [&](bool ok, const std::string& message) {
        require(ok, message);
        ++assertions;
    };

    const std::string region = "filterUnits='userSpaceOnUse' x='0' y='0' width='"
        + std::to_string(kWidth) + "' height='" + std::to_string(kHeight) + "'";

    // Known hard rectangle, integer aligned so the SourceGraphic has exact
    // 255/0 alpha steps.
    const std::string rect = "<rect x='20' y='18' width='24' height='20' fill='red'/>";
    auto source = capture(render(svg(rect), kWidth, kHeight));
    check(channelAlpha(pixel(render(svg(rect), kWidth, kHeight), 22, 20)) == 255, "hard source interior");
    check(channelAlpha(pixel(render(svg(rect), kWidth, kHeight), 10, 10)) == 0, "hard source exterior");

    auto expected2 = referenceBlur(source, 2.0, 2.0);
    auto actual2 = render(svg("<defs><filter id='f' " + region + ">"
                              "<feGaussianBlur stdDeviation='2'/></filter></defs>"
                              "<rect x='20' y='18' width='24' height='20' fill='red' filter='url(#f)'/>"),
                          kWidth, kHeight);
    assertImagesClose(expected2, actual2, 3.0, "isotropic blur");
    ++assertions;
    // Zero extension is not renormalized: a pixel just outside the edge must
    // match the unrenormalized reference rather than stay fully opaque/empty.
    check(std::abs(channelAlpha(pixel(actual2, 18, 28)) - static_cast<int>(std::lround(expected2.at(18, 28)[3]))) <= 3,
          "zero-extension edge alpha");
    check(channelAlpha(pixel(actual2, 2, 2)) == 0, "blur does not reach the far corner");

    auto expectedX = referenceBlur(source, 2.0, 0.0);
    auto actualX = render(svg("<defs><filter id='f' " + region + ">"
                              "<feGaussianBlur stdDeviation='2 0'/></filter></defs>"
                              "<rect x='20' y='18' width='24' height='20' fill='red' filter='url(#f)'/>"),
                          kWidth, kHeight);
    assertImagesClose(expectedX, actualX, 3.0, "horizontal-only blur (zero y axis is identity)");
    ++assertions;

    auto expectedY = referenceBlur(source, 0.0, 2.0);
    auto actualY = render(svg("<defs><filter id='f' " + region + ">"
                              "<feGaussianBlur stdDeviation='0 2'/></filter></defs>"
                              "<rect x='20' y='18' width='24' height='20' fill='red' filter='url(#f)'/>"),
                          kWidth, kHeight);
    assertImagesClose(expectedY, actualY, 3.0, "vertical-only blur (zero x axis is identity)");
    ++assertions;

    // Two colors, one of them translucent. The source pixels come from a
    // filter-free render; the convolution itself is fully independent.
    const std::string twoTone =
        "<rect x='18' y='16' width='16' height='16' fill='#ff0000'/>"
        "<rect x='34' y='16' width='16' height='16' fill='#0000ff' fill-opacity='0.5'/>";
    auto toneSource = capture(render(svg(twoTone), kWidth, kHeight));
    auto expectedTone = referenceBlur(toneSource, 1.5, 1.5);
    auto actualTone = render(svg("<defs><filter id='f' " + region + ">"
                                 "<feGaussianBlur stdDeviation='1.5'/></filter></defs>"
                                 "<g filter='url(#f)'>" + twoTone + "</g>"),
                             kWidth, kHeight);
    assertImagesClose(expectedTone, actualTone, 3.0, "two-tone translucent premultiplied blur");
    ++assertions;

    // The three presentation entry points must behave identically.
    auto attributeEntry = actual2;
    auto styleEntry = render(svg("<defs><filter id='f' " + region + ">"
                                 "<feGaussianBlur stdDeviation='2'/></filter></defs>"
                                 "<rect x='20' y='18' width='24' height='20' fill='red' style='filter:url(#f)'/>"),
                             kWidth, kHeight);
    auto sheetEntry = render(svg("<defs><filter id='f' " + region + ">"
                                 "<feGaussianBlur stdDeviation='2'/></filter></defs>"
                                 "<style>rect{filter:url(#f)}</style>"
                                 "<rect x='20' y='18' width='24' height='20' fill='red'/>"),
                             kWidth, kHeight);
    check(attributeEntry.width() == styleEntry.width() && attributeEntry.height() == styleEntry.height(), "entry sizes");
    bool identicalStyle = true;
    bool identicalSheet = true;
    for(int y = 0; y < kHeight; ++y) {
        for(int x = 0; x < kWidth; ++x) {
            identicalStyle &= pixel(attributeEntry, x, y) == pixel(styleEntry, x, y);
            identicalSheet &= pixel(attributeEntry, x, y) == pixel(sheetEntry, x, y);
        }
    }

    check(identicalStyle, "inline style filter matches attribute filter");
    check(identicalSheet, "stylesheet filter matches attribute filter");
    return assertions;
}

// Tiny independent evaluator for the supported primitive chain, used to check
// result naming, branches, overrides, defaults and SourceAlpha.
struct ReferencePipeline {
    Image source;
    std::map<std::string, Image> named;
    const Image* previous = nullptr;

    explicit ReferencePipeline(Image sourceGraphic) : source(std::move(sourceGraphic)) {}

    const Image* resolve(const std::string& name, bool first)
    {
        if(name == "SourceGraphic")
            return &source;
        if(name == "SourceAlpha") {
            auto inserted = named.emplace("__SourceAlpha__", sourceAlphaOf(source));
            return &inserted.first->second;
        }

        if(name.empty())
            return first ? &source : previous;
        auto it = named.find(name);
        if(it == named.end()) {
            missing = Image(source.w, source.h);
            return &missing;
        }

        return &it->second;
    }

    Image missing;

    void blur(const std::string& in, const std::string& result, double sx, double sy)
    {
        auto* input = resolve(in, previous == nullptr);
        auto output = referenceBlur(*input, sx, sy);
        commit(result, std::move(output));
    }

    void offset(const std::string& in, const std::string& result, double dx, double dy)
    {
        auto* input = resolve(in, previous == nullptr);
        auto output = referenceOffset(*input, dx, dy);
        commit(result, std::move(output));
    }

    void composite(const std::string& in, const std::string& in2, const std::string& result, bool inside)
    {
        auto* first = resolve(in, previous == nullptr);
        auto* second = resolve(in2, previous == nullptr);
        auto output = referenceComposite(*first, *second, inside);
        commit(result, std::move(output));
    }

    void commit(const std::string& result, Image output)
    {
        if(result.empty()) {
            storage.push_back(std::make_unique<Image>(std::move(output)));
            previous = storage.back().get();
        } else {
            named[result] = std::move(output);
            previous = &named[result];
        }
    }

    std::vector<std::unique_ptr<Image>> storage;
};

int caseResultBranches()
{
    int assertions = 0;
    auto check = [&](bool ok, const std::string& message) {
        require(ok, message);
        ++assertions;
    };

    const std::string region = "filterUnits='userSpaceOnUse' x='0' y='0' width='"
        + std::to_string(kWidth) + "' height='" + std::to_string(kHeight) + "'";
    const std::string shape = "<rect x='16' y='14' width='18' height='16' fill='#2060c0'/>";
    auto source = capture(render(svg(shape), kWidth, kHeight));

    // One result ("soft") feeds two later paths, and a later primitive reuses
    // the name "b" only after its own inputs were resolved.
    const std::string chain =
        "<feGaussianBlur in='SourceGraphic' stdDeviation='1.2' result='soft'/>"
        "<feOffset in='soft' dx='7' dy='3' result='shifted'/>"
        "<feComposite in='shifted' in2='soft' operator='over' result='merged'/>"
        "<feComposite in='merged' in2='SourceAlpha' operator='in'/>";

    ReferencePipeline pipeline(source);
    pipeline.blur("SourceGraphic", "soft", 1.2, 1.2);
    pipeline.offset("soft", "shifted", 7.0, 3.0);
    pipeline.composite("shifted", "soft", "merged", false);
    auto alpha = sourceAlphaOf(source);
    auto finalImage = referenceComposite(*pipeline.previous, alpha, true);

    auto actual = render(svg("<defs><filter id='f' " + region + ">" + chain + "</filter></defs>"
                             "<rect x='16' y='14' width='18' height='16' fill='#2060c0' filter='url(#f)'/>"),
                         kWidth, kHeight);
    assertImagesClose(finalImage, actual, 3.0, "branched result graph with SourceAlpha");
    ++assertions;

    // SourceAlpha really has black RGB: inspect the in-composite output where
    // only alpha survives and RGB stays that of the first input.
    auto alphaOnly = render(svg("<defs><filter id='f' " + region + ">"
                                "<feComposite in='SourceAlpha' in2='SourceGraphic' operator='in'/>"
                                "</filter></defs>"
                                "<rect x='30' y='30' width='10' height='10' fill='red' filter='url(#f)'/>"),
                            kWidth, kHeight);
    check(channelAlpha(pixel(alphaOnly, 35, 35)) == 255, "SourceAlpha preserves alpha");
    check(channelRed(pixel(alphaOnly, 35, 35)) == 0 && channelGreen(pixel(alphaOnly, 35, 35)) == 0
        && channelBlue(pixel(alphaOnly, 35, 35)) == 0, "SourceAlpha RGB is black");

    // Same-name override: "b" starts blurred, then becomes the plain offset.
    // The blurred pixels must not be mutated by the overwrite.
    auto overrideActual = render(svg("<defs><filter id='f' " + region + ">"
                                     "<feGaussianBlur stdDeviation='2' result='b'/>"
                                     "<feOffset in='SourceGraphic' dx='12' result='b'/>"
                                     "<feComposite in='b' in2='b' operator='over'/>"
                                     "</filter></defs>"
                                     "<rect x='10' y='10' width='8' height='8' fill='blue' filter='url(#f)'/>"),
                                 kWidth, kHeight);
    check(channelAlpha(pixel(overrideActual, 14, 14)) == 0, "overwritten name drops the blurred branch");
    check(channelAlpha(pixel(overrideActual, 26, 14)) == 255, "overwritten name keeps the offset branch");
    check(channelBlue(pixel(overrideActual, 26, 14)) == 255, "offset branch color intact");

    // Unknown and forward references read as transparent black.
    auto missingActual = render(svg("<defs><filter id='f' " + region + ">"
                                    "<feOffset in='ghost' dx='4' result='later'/>"
                                    "<feComposite in='forward' in2='later' operator='over'/>"
                                    "</filter></defs>"
                                    "<rect x='20' y='40' width='10' height='10' fill='red' filter='url(#f)'/>"),
                                kWidth, kHeight);
    bool allEmpty = true;
    for(int y = 0; y < kHeight; ++y)
        for(int x = 0; x < kWidth; ++x)
            allEmpty &= pixel(missingActual, x, y) == 0;
    check(allEmpty, "missing and forward references are transparent black");

    // Defaulting rules: first primitive defaults to SourceGraphic, later ones
    // to the previous output, including feComposite's in2.
    const std::string defaultShape = "<rect x='40' y='10' width='10' height='10' fill='green'/>";
    auto defaultSource = capture(render(svg(defaultShape), kWidth, kHeight));
    auto defaultActual = render(svg("<defs><filter id='f' " + region + ">"
                                    "<feGaussianBlur stdDeviation='1'/>"
                                    "<feOffset dx='5' dy='0'/>"
                                    "<feComposite in2='SourceGraphic' operator='over'/>"
                                    "</filter></defs>"
                                    "<rect x='40' y='10' width='10' height='10' fill='green' filter='url(#f)'/>"),
                                kWidth, kHeight);
    ReferencePipeline defaults(defaultSource);
    defaults.blur("", "", 1.0, 1.0);
    defaults.offset("", "", 5.0, 0.0);
    defaults.composite("", "SourceGraphic", "", false);
    assertImagesClose(*defaults.previous, defaultActual, 3.0, "default in and in2 resolution");
    ++assertions;
    return assertions;
}

int caseUnitsAndBounds()
{
    int assertions = 0;
    auto check = [&](bool ok, const std::string& message) {
        require(ok, message);
        ++assertions;
    };

    const std::string bigRegion = "filterUnits='userSpaceOnUse' x='-40' y='-40' width='160' height='150'";
    const std::string shape = "<rect x='30' y='26' width='14' height='10' fill='#c04080'/>";
    constexpr double kCenterX = 37.0;
    constexpr double kCenterY = 31.0;

    auto transformedMomentCase = [&](const std::string& transform, double angleDeg,
                                     double scaleX, double scaleY, double sigmaX, double sigmaY,
                                     const std::string& label) {
        auto plain = render(svg("<g transform='" + transform + "'>" + shape + "</g>"), kWidth, kHeight);
        auto filtered = render(svg("<defs><filter id='f' " + bigRegion + ">"
                                   "<feGaussianBlur stdDeviation='" + std::to_string(sigmaX) + " "
                                   + std::to_string(sigmaY) + "'/></filter></defs>"
                                   "<g transform='" + transform + "' filter='url(#f)'>" + shape + "</g>"),
                               kWidth, kHeight);
        auto sourceMoments = alphaMoments(capture(plain));
        auto outputMoments = alphaMoments(capture(filtered));

        double angle = angleDeg * kPi / 180.0;
        double columnUx = std::cos(angle) * scaleX;
        double columnUy = std::sin(angle) * scaleX;
        double columnVx = -std::sin(angle) * scaleY;
        double columnVy = std::cos(angle) * scaleY;
        double varianceU = kernelVariance(sigmaX);
        double varianceV = kernelVariance(sigmaY);
        double kxx = varianceU * columnUx * columnUx + varianceV * columnVx * columnVx;
        double kyy = varianceU * columnUy * columnUy + varianceV * columnVy * columnVy;
        double kxy = varianceU * columnUx * columnUy + varianceV * columnVx * columnVy;

        check(std::abs(outputMoments.cx - sourceMoments.cx) <= 0.25, label + ": centroid x");
        check(std::abs(outputMoments.cy - sourceMoments.cy) <= 0.25, label + ": centroid y");
        double toleranceXX = 0.5 + 0.05 * std::abs(sourceMoments.mxx + kxx);
        double toleranceYY = 0.5 + 0.05 * std::abs(sourceMoments.myy + kyy);
        double toleranceXY = 0.5 + 0.05 * std::abs(sourceMoments.mxy + kxy);
        check(std::abs(outputMoments.mxx - (sourceMoments.mxx + kxx)) <= toleranceXX, label + ": mxx");
        check(std::abs(outputMoments.myy - (sourceMoments.myy + kyy)) <= toleranceYY, label + ": myy");
        check(std::abs(outputMoments.mxy - (sourceMoments.mxy + kxy)) <= toleranceXY, label + ": mxy cross term");
    };

    transformedMomentCase("rotate(90 " + std::to_string(kCenterX) + " " + std::to_string(kCenterY) + ")",
                          90.0, 1.0, 1.0, 2.5, 1.0, "rotate-90");
    transformedMomentCase("rotate(30 " + std::to_string(kCenterX) + " " + std::to_string(kCenterY) + ")",
                          30.0, 1.0, 1.0, 2.5, 1.0, "rotate-30");
    transformedMomentCase("scale(1.5 0.8)", 0.0, 1.5, 0.8, 2.0, 1.2, "scale");
    transformedMomentCase("rotate(30 " + std::to_string(kCenterX) + " " + std::to_string(kCenterY) + ") scale(1.5 0.8)",
                          30.0, 1.5, 0.8, 2.0, 1.0, "rotate-scale");

    // Filter region clips the result: the blur tail past the region edge is
    // removed while the interior stays untouched.
    auto clippedSource = capture(render(svg(shape), kWidth, kHeight));
    auto clippedExpected = referenceBlur(clippedSource, 2.0, 2.0);
    auto clipped = render(svg("<defs><filter id='f' filterUnits='userSpaceOnUse' x='30' y='20' width='20' height='22'>"
                              "<feGaussianBlur stdDeviation='2'/></filter></defs>"
                              "<rect x='30' y='26' width='14' height='10' fill='red' filter='url(#f)'/>"),
                          kWidth, kHeight);
    check(channelAlpha(pixel(clipped, 28, 30)) == 0, "filter region clips the blur tail on its left");
    check(channelAlpha(pixel(clipped, 52, 30)) == 0, "filter region clips the blur tail on its right");
    check(std::abs(channelAlpha(pixel(clipped, 34, 31)) - static_cast<int>(std::lround(clippedExpected.at(34, 31)[3]))) <= 3,
          "filter region keeps its interior");

    // primitiveUnits objectBoundingBox: offset fractions map through the bbox.
    const std::string wideRegion = "filterUnits='userSpaceOnUse' x='4' y='4' width='64' height='56'";
    auto obbOffset = render(svg("<defs><filter id='f' " + wideRegion + " primitiveUnits='objectBoundingBox'>"
                                "<feOffset dx='0.5' dy='0.25'/></filter></defs>"
                                "<rect x='20' y='20' width='16' height='12' fill='red' filter='url(#f)'/>"),
                            kWidth, kHeight);
    auto obbSource = capture(render(svg("<rect x='20' y='20' width='16' height='12' fill='red'/>"), kWidth, kHeight));
    auto obbExpected = referenceOffset(obbSource, 8.0, 3.0);
    assertImagesClose(obbExpected, obbOffset, 2.0, "objectBoundingBox offset fractions");
    ++assertions;

    // objectBoundingBox stdDeviation scales with the bbox dimensions.
    auto obbBlur = render(svg("<defs><filter id='f' " + wideRegion + " primitiveUnits='objectBoundingBox'>"
                              "<feGaussianBlur stdDeviation='0.125 0.25'/></filter></defs>"
                              "<rect x='20' y='20' width='16' height='12' fill='red' filter='url(#f)'/>"),
                          kWidth, kHeight);
    auto obbBlurExpected = referenceBlur(obbSource, 2.0, 3.0);
    assertImagesClose(obbBlurExpected, obbBlur, 3.0, "objectBoundingBox stdDeviation scaling");
    ++assertions;

    // Invalid configurations produce an empty filter result but leave the
    // sibling untouched.
    auto invalid = render(svg("<defs><filter id='bad' filterUnits='userSpaceOnUse' x='0' y='0' width='60' height='40'>"
                              "<feGaussianBlur stdDeviation='-2'/></filter></defs>"
                              "<rect x='4' y='4' width='10' height='10' fill='red' filter='url(#bad)'/>"
                              "<rect x='30' y='4' width='10' height='10' fill='blue'/>"),
                          kWidth, kHeight);
    check(channelAlpha(pixel(invalid, 9, 9)) == 0, "negative stdDeviation yields an empty result");
    check(channelAlpha(pixel(invalid, 35, 9)) == 255, "sibling survives the empty filter");

    auto missingRegion = render(svg("<defs><filter id='bad' filterUnits='userSpaceOnUse' x='0' y='0' width='60'>"
                                    "<feOffset dx='3'/></filter></defs>"
                                    "<rect x='4' y='30' width='10' height='10' fill='red' filter='url(#bad)'/>"),
                                kWidth, kHeight);
    check(channelAlpha(pixel(missingRegion, 9, 35)) == 0, "userSpaceOnUse needs all four region values");

    auto zeroBox = render(svg("<defs><filter id='bad'><feOffset dx='3'/></filter></defs>"
                              "<rect x='4' y='50' width='10' height='0' fill='red' filter='url(#bad)'/>"
                              "<rect x='30' y='50' width='10' height='8' fill='blue'/>"),
                          kWidth, kHeight);
    check(channelAlpha(pixel(zeroBox, 35, 54)) == 255, "zero-height bbox does not disturb the sibling");
    return assertions;
}

int caseCompositeOrder()
{
    int assertions = 0;
    auto check = [&](bool ok, const std::string& message) {
        require(ok, message);
        ++assertions;
    };

    const std::string region = "filterUnits='userSpaceOnUse' x='0' y='0' width='"
        + std::to_string(kWidth) + "' height='" + std::to_string(kHeight) + "'";
    const std::string first = "<rect x='18' y='18' width='16' height='16' fill='#ff0000'/>";
    const std::string second = "<rect x='26' y='26' width='16' height='16' fill='#0000ff' fill-opacity='0.6'/>";
    auto groupSource = capture(render(svg("<g>" + first + second + "</g>"), kWidth, kHeight));
    auto shiftedGroup = referenceOffset(groupSource, 8.0, 8.0);

    // operator="over": premultiplied src-over, first input on top of in2.
    auto overExpected = referenceComposite(groupSource, shiftedGroup, false);
    auto overActual = render(svg("<defs><filter id='f' " + region + ">"
                                 "<feGaussianBlur stdDeviation='0' result='a'/>"
                                 "<feOffset in='SourceGraphic' dx='8' dy='8' result='b'/>"
                                 "<feComposite in='a' in2='b' operator='over'/>"
                                 "</filter></defs>"
                                 "<g filter='url(#f)'>" + first + second + "</g>"),
                             kWidth, kHeight);
    assertImagesClose(overExpected, overActual, 2.0, "explicit over ordering");
    ++assertions;

    // Default operator is over.
    auto defaultOver = render(svg("<defs><filter id='f' " + region + ">"
                                  "<feGaussianBlur stdDeviation='0' result='a'/>"
                                  "<feOffset in='SourceGraphic' dx='8' dy='8' result='b'/>"
                                  "<feComposite in='a' in2='b'/>"
                                  "</filter></defs>"
                                  "<g filter='url(#f)'>" + first + second + "</g>"),
                              kWidth, kHeight);
    assertImagesClose(overExpected, defaultOver, 2.0, "default composite operator is over");
    ++assertions;

    // operator="in": first input multiplied by the second input's alpha.
    auto inExpected = referenceComposite(groupSource, shiftedGroup, true);
    auto inActual = render(svg("<defs><filter id='f' " + region + ">"
                               "<feGaussianBlur stdDeviation='0' result='a'/>"
                               "<feOffset in='SourceGraphic' dx='8' dy='8' result='b'/>"
                               "<feComposite in='a' in2='b' operator='in'/>"
                               "</filter></defs>"
                               "<g filter='url(#f)'>" + first + second + "</g>"),
                           kWidth, kHeight);
    assertImagesClose(inExpected, inActual, 2.0, "in multiplies by the second alpha");
    ++assertions;
    // Targeted interior probes, well away from antialiased boundaries.
    check(channelAlpha(pixel(inActual, 30, 30)) == channelAlpha(pixel(overActual, 30, 30))
        || channelAlpha(pixel(inActual, 30, 30)) > 0, "in keeps the shared interior");
    check(channelAlpha(pixel(inActual, 20, 20)) == 0, "in removes content absent from the second alpha");

    // Pure integer offset.
    auto integerExpected = referenceOffset(groupSource, 10.0, -4.0);
    auto integerActual = render(svg("<defs><filter id='f' " + region + ">"
                                    "<feOffset dx='10' dy='-4'/></filter></defs>"
                                    "<g filter='url(#f)'>" + first + second + "</g>"),
                                kWidth, kHeight);
    assertImagesClose(integerExpected, integerActual, 2.0, "integer offset");
    ++assertions;

    // Fractional offset uses bilinear sampling at the shifted pixel centers.
    auto fractionalExpected = referenceOffset(groupSource, 3.5, 2.25);
    auto fractionalActual = render(svg("<defs><filter id='f' " + region + ">"
                                       "<feOffset dx='3.5' dy='2.25'/></filter></defs>"
                                       "<g filter='url(#f)'>" + first + second + "</g>"),
                                   kWidth, kHeight);
    assertImagesClose(fractionalExpected, fractionalActual, 2.0, "fractional offset sampling");
    ++assertions;
    return assertions;
}

int caseNestedReuse()
{
    int assertions = 0;
    auto check = [&](bool ok, const std::string& message) {
        require(ok, message);
        ++assertions;
    };

    const std::string parentRegion = "filterUnits='userSpaceOnUse' x='-10' y='-10' width='110' height='100'";
    const std::string childRegion = "filterUnits='userSpaceOnUse' x='0' y='0' width='80' height='70'";
    const std::string childShape = "<rect x='14' y='14' width='16' height='14' fill='#d04020'/>";

    // Child blur feeds the parent's SourceGraphic; the parent then offsets it.
    auto nestedActual = render(svg("<defs>"
                                   "<filter id='child' " + childRegion + "><feGaussianBlur stdDeviation='1.5'/></filter>"
                                   "<filter id='parent' " + parentRegion + "><feOffset dx='9' dy='0'/></filter>"
                                   "</defs>"
                                   "<g filter='url(#parent)'>"
                                   "<g filter='url(#child)'>" + childShape + "</g></g>"),
                               kWidth, kHeight);
    auto childOnly = capture(render(svg("<defs><filter id='child' " + childRegion + ">"
                                        "<feGaussianBlur stdDeviation='1.5'/></filter></defs>"
                                        "<g filter='url(#child)'>" + childShape + "</g>"),
                                    kWidth, kHeight));
    auto nestedExpected = referenceOffset(childOnly, 9.0, 0.0);
    assertImagesClose(nestedExpected, nestedActual, 3.0, "parent SourceGraphic contains the child filter result");
    ++assertions;
    // The blur tail extends beyond the child geometry and survives the parent.
    auto tailBefore = childOnly.at(12, 20)[3];
    auto tailAfter = pixel(nestedActual, 21, 20) >> 24;
    check(tailBefore > 0, "child blur extends past its geometry");
    check(std::abs(static_cast<int>(tailAfter) - static_cast<int>(std::lround(tailBefore))) <= 3,
          "expanded child content enters the parent filter");

    // A parent that needs its own offscreen (opacity) must allocate room for
    // the child's filter-expanded output rather than clipping the blur tail.
    auto isolatedParent = render(svg("<defs><filter id='child' " + childRegion + ">"
                                     "<feGaussianBlur stdDeviation='2'/></filter></defs>"
                                     "<g opacity='0.9'>"
                                     "<rect x='30' y='10' width='12' height='12' fill='red' filter='url(#child)'/>"
                                     "</g>"),
                                 kWidth, kHeight);
    auto isolatedReference = capture(render(svg("<defs><filter id='child' " + childRegion + ">"
                                                 "<feGaussianBlur stdDeviation='2'/></filter></defs>"
                                                 "<rect x='30' y='10' width='12' height='12' fill='red' filter='url(#child)'/>"),
                                             kWidth, kHeight));
    bool tailKept = true;
    for(int y = 8; y <= 26; ++y) {
        for(int x = 26; x <= 46; ++x) {
            double expected = std::lround(isolatedReference.at(x, y)[3] * 0.9f);
            tailKept &= std::abs(static_cast<double>(pixel(isolatedParent, x, y) >> 24) - expected) <= 2.0;
        }
    }

    check(tailKept, "opacity parent keeps the child blur tail");
    ++assertions;

    // One definition reused by elements with different bboxes: objectBounding
    // box fractions are resolved per element.
    auto reused = render(svg("<defs><filter id='shared' filterUnits='userSpaceOnUse' x='0' y='0' width='80' height='70'"
                             " primitiveUnits='objectBoundingBox'><feOffset dx='0.5' dy='0'/></filter></defs>"
                             "<rect x='6' y='8' width='10' height='8' fill='red' filter='url(#shared)'/>"
                             "<rect x='6' y='30' width='20' height='8' fill='blue' filter='url(#shared)'/>"),
                         kWidth, kHeight);
    check(channelAlpha(pixel(reused, 11, 12)) == 255 && channelAlpha(pixel(reused, 6, 12)) == 0,
          "reused filter resolves against the first bbox (dx = 5)");
    check(channelAlpha(pixel(reused, 16, 34)) == 255 && channelAlpha(pixel(reused, 6, 34)) == 0,
          "reused filter resolves against the second bbox (dx = 10)");

    // Primitive parameters changed through Element::setAttribute take effect
    // on the next render without reusing stale intermediate images.
    const std::string mutableDocument = svg("<defs><filter id='f' filterUnits='userSpaceOnUse' x='0' y='0' width='80' height='70'>"
                                            "<feGaussianBlur id='blur1' stdDeviation='1'/></filter></defs>"
                                            "<rect x='30' y='30' width='12' height='12' fill='red' filter='url(#f)'/>");
    auto doc = Document::loadFromData(mutableDocument);
    require(doc != nullptr, "mutable document load");
    auto before = doc->renderToBitmap(kWidth, kHeight);
    doc->getElementById("blur1").setAttribute("stdDeviation", "4");
    auto after = doc->renderToBitmap(kWidth, kHeight);
    check(channelAlpha(pixel(before, 27, 36)) > 0 && channelAlpha(pixel(before, 27, 36)) < 255, "small blur before mutation");
    check(channelAlpha(pixel(after, 25, 36)) > channelAlpha(pixel(before, 25, 36)),
          "increased stdDeviation spreads more alpha after setAttribute");
    doc->getElementById("blur1").setAttribute("stdDeviation", "0");
    auto identity = doc->renderToBitmap(kWidth, kHeight);
    check(channelAlpha(pixel(identity, 36, 36)) == 255 && channelAlpha(pixel(identity, 28, 36)) == 0,
          "stdDeviation reset to zero is an identity blur");

    // Re-rendering at a different resolution must not reuse cached images.
    auto small = doc->renderToBitmap(40, 35);
    auto large = doc->renderToBitmap(80, 70);
    auto smallMoments = alphaMoments(capture(small));
    auto largeMoments = alphaMoments(capture(large));
    check(std::abs(largeMoments.cx - 2.0 * smallMoments.cx) <= 0.5, "resolution change scales centroid x");
    check(std::abs(largeMoments.cy - 2.0 * smallMoments.cy) <= 0.5, "resolution change scales centroid y");
    return assertions;
}

int caseOffscreenReturn()
{
    int assertions = 0;
    auto check = [&](bool ok, const std::string& message) {
        require(ok, message);
        ++assertions;
    };

    // Source sits partly off-canvas but inside the filter region; the offset
    // brings it back into view.
    auto returned = render(svg("<defs><filter id='f' filterUnits='userSpaceOnUse' x='-12' y='0' width='70' height='50'>"
                               "<feOffset dx='10'/></filter></defs>"
                               "<rect x='-8' y='10' width='12' height='12' fill='red' filter='url(#f)'/>"),
                           kWidth, kHeight);
    check(channelAlpha(pixel(returned, 6, 16)) == 255, "off-canvas source returns through the offset");
    check(channelAlpha(pixel(returned, 0, 16)) == 0, "nothing is painted before the shifted shape");

    // Filter runs before clip-path: blur tails past the clip edge are removed.
    auto clipped = render(svg("<defs><filter id='f' filterUnits='userSpaceOnUse' x='0' y='0' width='80' height='70'>"
                              "<feGaussianBlur stdDeviation='2'/></filter>"
                              "<clipPath id='clip'><rect x='24' y='20' width='20' height='20'/></clipPath></defs>"
                              "<rect x='24' y='20' width='20' height='20' fill='red' filter='url(#f)' clip-path='url(#clip)'/>"),
                          kWidth, kHeight);
    check(channelAlpha(pixel(clipped, 22, 30)) == 0, "clip-path removes the pre-clip blur tail");
    check(channelAlpha(pixel(clipped, 30, 30)) == 255, "clip-path keeps the filtered interior");

    // Mask and opacity are applied once each after the filter.
    auto masked = render(svg("<defs><filter id='f' filterUnits='userSpaceOnUse' x='0' y='0' width='80' height='70'>"
                             "<feGaussianBlur stdDeviation='0'/></filter>"
                             "<mask id='m' maskUnits='userSpaceOnUse' x='0' y='0' width='80' height='70'>"
                             "<rect width='80' height='70' fill='white' fill-opacity='0.5'/></mask></defs>"
                             "<rect x='10' y='40' width='14' height='14' fill='red' filter='url(#f)' mask='url(#m)'/>"),
                         kWidth, kHeight);
    check(std::abs(channelAlpha(pixel(masked, 17, 47)) - 128) <= 2, "semi-transparent mask applied once");

    auto maskedOpacity = render(svg("<defs><filter id='f' filterUnits='userSpaceOnUse' x='0' y='0' width='80' height='70'>"
                                    "<feGaussianBlur stdDeviation='0'/></filter>"
                                    "<mask id='m' maskUnits='userSpaceOnUse' x='0' y='0' width='80' height='70'>"
                                    "<rect width='80' height='70' fill='white' fill-opacity='0.5'/></mask></defs>"
                                    "<rect x='30' y='40' width='14' height='14' fill='red' opacity='0.5' "
                                    "filter='url(#f)' mask='url(#m)'/>"),
                                kWidth, kHeight);
    check(std::abs(channelAlpha(pixel(maskedOpacity, 37, 47)) - 64) <= 2, "opacity and mask each multiply alpha once");

    // The filtered element blends over a colored parent background.
    auto overBackground = render(svg("<defs><filter id='f' filterUnits='userSpaceOnUse' x='0' y='0' width='80' height='70'>"
                                     "<feOffset dx='6' dy='0'/></filter></defs>"
                                     "<rect x='0' y='0' width='80' height='70' fill='#008000'/>"
                                     "<rect x='10' y='10' width='12' height='12' fill='#ff0000' fill-opacity='0.5' filter='url(#f)'/>"),
                                 kWidth, kHeight);
    auto value = pixel(overBackground, 22, 16);
    // Premultiplied src-over: source (128,0,0,128) over opaque green (0,128,0).
    int expectedRed = 128;
    int expectedGreen = 64;
    check(std::abs(channelRed(value) - expectedRed) <= 2 && std::abs(channelGreen(value) - expectedGreen) <= 2
        && channelAlpha(value) == 255, "filtered element blends over the parent background");

    // Unknown filter reference falls back to the unfiltered drawing.
    auto missing = render(svg("<rect x='40' y='10' width='12' height='12' fill='red' filter='url(#nope)'/>"),
                          kWidth, kHeight);
    check(channelAlpha(pixel(missing, 46, 16)) == 255, "missing filter reference keeps the raw drawing");

    // A filter without primitives returns SourceGraphic clipped to the region.
    auto empty = render(svg("<defs><filter id='f' filterUnits='userSpaceOnUse' x='10' y='10' width='20' height='20'></filter></defs>"
                            "<rect x='14' y='14' width='12' height='12' fill='blue' filter='url(#f)'/>"),
                        kWidth, kHeight);
    check(channelAlpha(pixel(empty, 20, 20)) == 255, "empty filter keeps the source inside its region");
    check(channelAlpha(pixel(empty, 40, 40)) == 0, "empty filter clips to its region");

    // Reflected and sheared transforms are outside the supported class: the
    // element must still appear, drawn without the filter.
    auto reflected = render(svg("<defs><filter id='f' filterUnits='userSpaceOnUse' x='0' y='0' width='80' height='70'>"
                                "<feOffset dx='6'/></filter></defs>"
                                "<g transform='scale(-1 1)'>"
                                "<rect x='-40' y='20' width='10' height='10' fill='red' filter='url(#f)'/></g>"),
                            kWidth, kHeight);
    auto reflectedControl = render(svg("<g transform='scale(-1 1)'>"
                                       "<rect x='-40' y='20' width='10' height='10' fill='red'/></g>"),
                                   kWidth, kHeight);
    bool reflectionMatches = true;
    for(int y = 0; y < kHeight; ++y)
        for(int x = 0; x < kWidth; ++x)
            reflectionMatches &= pixel(reflected, x, y) == pixel(reflectedControl, x, y);
    check(reflectionMatches, "reflected transform falls back to the unfiltered drawing");

    auto sheared = render(svg("<defs><filter id='f' filterUnits='userSpaceOnUse' x='0' y='0' width='80' height='70'>"
                              "<feOffset dx='6'/></filter></defs>"
                              "<g transform='skewX(20)'>"
                              "<rect x='20' y='20' width='10' height='10' fill='red' filter='url(#f)'/></g>"),
                          kWidth, kHeight);
    auto shearedControl = render(svg("<g transform='skewX(20)'>"
                                     "<rect x='20' y='20' width='10' height='10' fill='red'/></g>"),
                                 kWidth, kHeight);
    bool shearMatches = true;
    for(int y = 0; y < kHeight; ++y)
        for(int x = 0; x < kWidth; ++x)
            shearMatches &= pixel(sheared, x, y) == pixel(shearedControl, x, y);
    check(shearMatches, "sheared transform falls back to the unfiltered drawing");
    return assertions;
}

void runCase(const char* name, int (*function)())
{
    int assertions = function();
    require(assertions > 0, std::string(name) + " produced no assertions");
    std::cout << "FILTER_CASE_PASS " << name << " assertions=" << assertions << '\n';
}

} // namespace

int main()
{
    try {
        runCase("blur-reference", caseBlurReference);
        runCase("result-branches", caseResultBranches);
        runCase("units-and-bounds", caseUnitsAndBounds);
        runCase("composite-order", caseCompositeOrder);
        runCase("nested-reuse", caseNestedReuse);
        runCase("offscreen-return", caseOffscreenReturn);
        std::cout << "FILTER_TOTAL 6\n";
    } catch(const std::exception& exception) {
        std::cerr << "FILTER_CASE_FAIL " << exception.what() << '\n';
        return 1;
    }

    return 0;
}
