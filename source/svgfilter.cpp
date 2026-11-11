#include "svgfilter.h"
#include "svgrenderstate.h"
#include "svgparserutils.h"

#include <cmath>
#include <cstring>
#include <map>
#include <vector>

namespace lunasvg {

namespace {

constexpr float kFilterEpsilon = 1e-6f;

bool isFiniteRect(const Rect& rect)
{
    return std::isfinite(rect.x) && std::isfinite(rect.y) && std::isfinite(rect.w) && std::isfinite(rect.h);
}

// A premultiplied RGBA image in device pixel coordinates. Pixel (0, 0) of the
// buffer covers the device pixel centered at (x + 0.5, y + 0.5).
struct FilterImage {
    int x{0};
    int y{0};
    int w{0};
    int h{0};
    std::vector<float> px;

    void allocate()
    {
        px.assign(static_cast<size_t>(w) * h * 4, 0.f);
    }

    const float* pixel(int i, int j) const
    {
        return px.data() + (static_cast<size_t>(j) * w + i) * 4;
    }

    float* pixel(int i, int j)
    {
        return px.data() + (static_cast<size_t>(j) * w + i) * 4;
    }
};

bool isUnitless(const SVGLength& length)
{
    return length.value().units() == LengthUnits::None;
}

// Device-space columns of the element's user-to-device transform. Filters are
// only supported for translations, rotations and positive non-uniform scales,
// i.e. transforms with two orthogonal, non-reflected columns.
bool filterCompatibleTransform(const Transform& transform, Point& columnU, Point& columnV)
{
    const auto& m = transform.matrix();
    columnU = Point(m.a, m.b);
    columnV = Point(m.c, m.d);
    auto lengthU = std::sqrt(columnU.dot(columnU));
    auto lengthV = std::sqrt(columnV.dot(columnV));
    if(lengthU <= kFilterEpsilon || lengthV <= kFilterEpsilon)
        return false;
    if(std::abs(columnU.dot(columnV)) > kFilterEpsilon * lengthU * lengthV)
        return false;
    if(m.a * m.d - m.b * m.c <= kFilterEpsilon)
        return false;
    return true;
}

Point mapVector(const Point& columnU, const Point& columnV, float x, float y)
{
    return Point(columnU.x * x + columnV.x * y, columnU.y * x + columnV.y * y);
}

uint32_t canvasPixel(const Canvas& canvas, int deviceX, int deviceY)
{
    auto localX = deviceX - canvas.x();
    auto localY = deviceY - canvas.y();
    if(localX < 0 || localY < 0 || localX >= canvas.width() || localY >= canvas.height())
        return 0;
    uint32_t value;
    auto* surface = canvas.surface();
    auto* data = plutovg_surface_get_data(surface);
    auto stride = plutovg_surface_get_stride(surface);
    std::memcpy(&value, data + localY * stride + localX * 4, 4);
    return value;
}

std::unique_ptr<FilterImage> readCanvas(const Canvas& canvas, const Rect& region)
{
    auto image = std::make_unique<FilterImage>();
    image->x = static_cast<int>(region.x);
    image->y = static_cast<int>(region.y);
    image->w = static_cast<int>(region.w);
    image->h = static_cast<int>(region.h);
    image->allocate();
    for(int j = 0; j < image->h; ++j) {
        for(int i = 0; i < image->w; ++i) {
            auto value = canvasPixel(canvas, image->x + i, image->y + j);
            auto out = image->pixel(i, j);
            out[0] = static_cast<float>((value >> 16) & 0xff);
            out[1] = static_cast<float>((value >> 8) & 0xff);
            out[2] = static_cast<float>(value & 0xff);
            out[3] = static_cast<float>((value >> 24) & 0xff);
        }
    }

    return image;
}

std::shared_ptr<Canvas> writeCanvas(const FilterImage& image)
{
    auto canvas = Canvas::create(image.x, image.y, image.w, image.h);
    auto* surface = canvas->surface();
    auto* data = plutovg_surface_get_data(surface);
    auto stride = plutovg_surface_get_stride(surface);
    for(int j = 0; j < image.h; ++j) {
        for(int i = 0; i < image.w; ++i) {
            const auto* in = image.pixel(i, j);
            auto* out = reinterpret_cast<uint8_t*>(data) + j * stride + i * 4;
            auto a = static_cast<int>(std::lround(std::clamp(in[3], 0.f, 255.f)));
            auto r = static_cast<int>(std::lround(std::clamp(in[0], 0.f, 255.f)));
            auto g = static_cast<int>(std::lround(std::clamp(in[1], 0.f, 255.f)));
            auto b = static_cast<int>(std::lround(std::clamp(in[2], 0.f, 255.f)));
            out[0] = static_cast<uint8_t>(b);
            out[1] = static_cast<uint8_t>(g);
            out[2] = static_cast<uint8_t>(r);
            out[3] = static_cast<uint8_t>(a);
        }
    }

    return canvas;
}

// Bilinear sampling at an arbitrary device point. The area outside the image
// is treated as transparent black, so out-of-range taps contribute nothing.
void sample(const FilterImage* image, float deviceX, float deviceY, float out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0.f;
    if(image == nullptr || image->w <= 0 || image->h <= 0)
        return;

    auto gx = deviceX - (image->x + 0.5f);
    auto gy = deviceY - (image->y + 0.5f);
    auto x0 = std::floor(gx);
    auto y0 = std::floor(gy);
    auto fx = gx - x0;
    auto fy = gy - y0;

    for(int oy = 0; oy < 2; ++oy) {
        auto j = static_cast<int>(y0) + oy;
        if(j < 0 || j >= image->h)
            continue;
        auto wy = oy == 0 ? 1.f - fy : fy;
        for(int ox = 0; ox < 2; ++ox) {
            auto i = static_cast<int>(x0) + ox;
            if(i < 0 || i >= image->w)
                continue;
            auto wx = ox == 0 ? 1.f - fx : fx;
            const auto* in = image->pixel(i, j);
            auto weight = wx * wy;
            for(int k = 0; k < 4; ++k)
                out[k] += in[k] * weight;
        }
    }
}

std::vector<float> gaussianKernel(float stdDeviation, int& radius)
{
    radius = static_cast<int>(std::ceil(3.f * stdDeviation));
    std::vector<float> weights(2 * radius + 1);
    auto sum = 0.f;
    auto denominator = 2.f * stdDeviation * stdDeviation;
    for(int i = -radius; i <= radius; ++i) {
        auto weight = std::exp(-static_cast<float>(i) * i / denominator);
        weights[i + radius] = weight;
        sum += weight;
    }

    for(auto& weight : weights)
        weight /= sum;
    return weights;
}

// One pass of a directional, separable Gaussian along `direction`. The input
// is sampled with zero extension and the discrete kernel is not renormalized
// at the edges, so transparent surroundings genuinely bleed into the result.
void blurPass(const FilterImage* input, FilterImage& output, const Point& direction, float stdDeviation)
{
    if(stdDeviation <= 0.f) {
        for(int j = 0; j < output.h; ++j) {
            for(int i = 0; i < output.w; ++i) {
                float value[4];
                sample(input, output.x + i + 0.5f, output.y + j + 0.5f, value);
                auto* out = output.pixel(i, j);
                for(int k = 0; k < 4; ++k)
                    out[k] = value[k];
            }
        }
        return;
    }

    int radius;
    auto weights = gaussianKernel(stdDeviation, radius);
    for(int j = 0; j < output.h; ++j) {
        for(int i = 0; i < output.w; ++i) {
            auto centerX = output.x + i + 0.5f;
            auto centerY = output.y + j + 0.5f;
            float accum[4] = {0.f, 0.f, 0.f, 0.f};
            for(int tap = -radius; tap <= radius; ++tap) {
                float value[4];
                sample(input, centerX - tap * direction.x, centerY - tap * direction.y, value);
                auto weight = weights[tap + radius];
                for(int k = 0; k < 4; ++k)
                    accum[k] += value[k] * weight;
            }

            auto* out = output.pixel(i, j);
            for(int k = 0; k < 4; ++k)
                out[k] = accum[k];
        }
    }
}

std::unique_ptr<FilterImage> applyBlur(const FilterImage* input, const Rect& outputRegion,
                                       const Rect& filterRegion, float stdX, float stdY,
                                       const Point& columnU, const Point& columnV)
{
    auto output = std::make_unique<FilterImage>();
    output->x = static_cast<int>(outputRegion.x);
    output->y = static_cast<int>(outputRegion.y);
    output->w = static_cast<int>(outputRegion.w);
    output->h = static_cast<int>(outputRegion.h);
    output->allocate();
    if(output->w <= 0 || output->h <= 0)
        return output;

    auto lengthU = std::sqrt(columnU.dot(columnU));
    auto lengthV = std::sqrt(columnV.dot(columnV));
    auto sigmaU = stdX * lengthU;
    auto sigmaV = stdY * lengthV;
    auto directionU = Point(columnU.x / lengthU, columnU.y / lengthU);
    auto directionV = Point(columnV.x / lengthV, columnV.y / lengthV);

    // The intermediate covers the whole filter region so that the second pass
    // can read the first pass result everywhere its kernel reaches.
    auto intermediate = std::make_unique<FilterImage>();
    intermediate->x = static_cast<int>(filterRegion.x);
    intermediate->y = static_cast<int>(filterRegion.y);
    intermediate->w = static_cast<int>(filterRegion.w);
    intermediate->h = static_cast<int>(filterRegion.h);
    intermediate->allocate();
    blurPass(input, *intermediate, directionU, sigmaU);
    blurPass(intermediate.get(), *output, directionV, sigmaV);
    return output;
}

std::unique_ptr<FilterImage> applyOffset(const FilterImage* input, const Rect& region, const Point& delta)
{
    auto output = std::make_unique<FilterImage>();
    output->x = static_cast<int>(region.x);
    output->y = static_cast<int>(region.y);
    output->w = static_cast<int>(region.w);
    output->h = static_cast<int>(region.h);
    output->allocate();
    for(int j = 0; j < output->h; ++j) {
        for(int i = 0; i < output->w; ++i) {
            float value[4];
            sample(input, output->x + i + 0.5f - delta.x, output->y + j + 0.5f - delta.y, value);
            auto* out = output->pixel(i, j);
            for(int k = 0; k < 4; ++k)
                out[k] = value[k];
        }
    }

    return output;
}

std::unique_ptr<FilterImage> applyComposite(const FilterImage* first, const FilterImage* second,
                                            const Rect& region, FilterCompositeOperator op)
{
    auto output = std::make_unique<FilterImage>();
    output->x = static_cast<int>(region.x);
    output->y = static_cast<int>(region.y);
    output->w = static_cast<int>(region.w);
    output->h = static_cast<int>(region.h);
    output->allocate();
    for(int j = 0; j < output->h; ++j) {
        for(int i = 0; i < output->w; ++i) {
            auto px = output->x + i + 0.5f;
            auto py = output->y + j + 0.5f;
            float a[4];
            float b[4];
            sample(first, px, py, a);
            sample(second, px, py, b);
            auto* out = output->pixel(i, j);
            if(op == FilterCompositeOperator::In) {
                auto alpha = b[3] / 255.f;
                for(int k = 0; k < 4; ++k)
                    out[k] = a[k] * alpha;
            } else {
                auto alpha = a[3] / 255.f;
                for(int k = 0; k < 4; ++k)
                    out[k] = a[k] + b[k] * (1.f - alpha);
            }
        }
    }

    return output;
}

Rect integerRect(const Rect& rect)
{
    if(!rect.isValid() || rect.isEmpty())
        return Rect::Empty;
    auto x = std::floor(rect.x);
    auto y = std::floor(rect.y);
    auto right = std::ceil(rect.x + rect.w);
    auto bottom = std::ceil(rect.y + rect.h);
    return Rect(x, y, right - x, bottom - y);
}

} // namespace

bool filterTransformSupported(const Transform& transform)
{
    Point columnU;
    Point columnV;
    return filterCompatibleTransform(transform, columnU, columnV);
}

SVGFilterElement::SVGFilterElement(Document* document)
    : SVGElement(document, ElementID::Filter)
    , m_x(PropertyID::X, LengthDirection::Horizontal, LengthNegativeMode::Allow)
    , m_y(PropertyID::Y, LengthDirection::Vertical, LengthNegativeMode::Allow)
    , m_width(PropertyID::Width, LengthDirection::Horizontal, LengthNegativeMode::Allow)
    , m_height(PropertyID::Height, LengthDirection::Vertical, LengthNegativeMode::Allow)
{
    addProperty(m_filterUnits);
    addProperty(m_primitiveUnits);
    addProperty(m_x);
    addProperty(m_y);
    addProperty(m_width);
    addProperty(m_height);
}

Rect SVGFilterElement::localRegionRect(const SVGElement* element) const
{
    auto bbox = element->fillBoundingBox();
    Rect localRect;
    if(m_filterUnits.value() == Units::ObjectBoundingBox) {
        if(bbox.w <= 0.f || bbox.h <= 0.f)
            return Rect::Invalid;
        auto x = hasAttribute(PropertyID::X) ? m_x.value().value() : -0.1f;
        auto y = hasAttribute(PropertyID::Y) ? m_y.value().value() : -0.1f;
        auto width = hasAttribute(PropertyID::Width) ? m_width.value().value() : 1.2f;
        auto height = hasAttribute(PropertyID::Height) ? m_height.value().value() : 1.2f;
        // Explicit region values are required to be finite unitless numbers.
        if((hasAttribute(PropertyID::X) && !isUnitless(m_x))
            || (hasAttribute(PropertyID::Y) && !isUnitless(m_y))
            || (hasAttribute(PropertyID::Width) && !isUnitless(m_width))
            || (hasAttribute(PropertyID::Height) && !isUnitless(m_height))) {
            return Rect::Invalid;
        }

        localRect = Rect(bbox.x + x * bbox.w, bbox.y + y * bbox.h, width * bbox.w, height * bbox.h);
    } else {
        if(!hasAttribute(PropertyID::X) || !hasAttribute(PropertyID::Y)
            || !hasAttribute(PropertyID::Width) || !hasAttribute(PropertyID::Height)) {
            return Rect::Invalid;
        }

        if(!isUnitless(m_x) || !isUnitless(m_y) || !isUnitless(m_width) || !isUnitless(m_height))
            return Rect::Invalid;
        localRect = Rect(m_x.value().value(), m_y.value().value(), m_width.value().value(), m_height.value().value());
    }

    if(!isFiniteRect(localRect) || localRect.w <= 0.f || localRect.h <= 0.f)
        return Rect::Invalid;
    return localRect;
}

Rect SVGFilterElement::regionRect(const SVGElement* element, const Transform& elementTransform) const
{
    auto localRect = localRegionRect(element);
    if(!localRect.isValid() || localRect.isEmpty())
        return Rect::Invalid;
    auto region = elementTransform.mapRect(localRect);
    if(!isFiniteRect(region) || region.w <= 0.f || region.h <= 0.f)
        return Rect::Invalid;
    return region;
}

SVGFilterPrimitiveElement::SVGFilterPrimitiveElement(Document* document, ElementID id)
    : SVGElement(document, id)
    , m_x(PropertyID::X, LengthDirection::Horizontal, LengthNegativeMode::Allow)
    , m_y(PropertyID::Y, LengthDirection::Vertical, LengthNegativeMode::Allow)
    , m_width(PropertyID::Width, LengthDirection::Horizontal, LengthNegativeMode::Allow)
    , m_height(PropertyID::Height, LengthDirection::Vertical, LengthNegativeMode::Allow)
{
    addProperty(m_in);
    addProperty(m_result);
    addProperty(m_x);
    addProperty(m_y);
    addProperty(m_width);
    addProperty(m_height);
}

Rect SVGFilterPrimitiveElement::regionRect(const SVGFilterElement* filterElement, const SVGElement* element,
                                           const Transform& elementTransform, const Rect& filterRegion) const
{
    auto bbox = element->fillBoundingBox();
    auto localFilter = elementTransform.inverse().mapRect(filterRegion);
    Rect localRect = localFilter;
    if(hasAttribute(PropertyID::X) && !isUnitless(m_x))
        return Rect::Empty;
    if(hasAttribute(PropertyID::Y) && !isUnitless(m_y))
        return Rect::Empty;
    if(hasAttribute(PropertyID::Width) && !isUnitless(m_width))
        return Rect::Empty;
    if(hasAttribute(PropertyID::Height) && !isUnitless(m_height))
        return Rect::Empty;
    if(filterElement->primitiveUnits() == Units::ObjectBoundingBox) {
        if(bbox.w <= 0.f || bbox.h <= 0.f)
            return Rect::Empty;
        if(hasAttribute(PropertyID::X))
            localRect.x = bbox.x + m_x.value().value() * bbox.w;
        if(hasAttribute(PropertyID::Y))
            localRect.y = bbox.y + m_y.value().value() * bbox.h;
        if(hasAttribute(PropertyID::Width))
            localRect.w = m_width.value().value() * bbox.w;
        if(hasAttribute(PropertyID::Height))
            localRect.h = m_height.value().value() * bbox.h;
    } else {
        if(hasAttribute(PropertyID::X))
            localRect.x = m_x.value().value();
        if(hasAttribute(PropertyID::Y))
            localRect.y = m_y.value().value();
        if(hasAttribute(PropertyID::Width))
            localRect.w = m_width.value().value();
        if(hasAttribute(PropertyID::Height))
            localRect.h = m_height.value().value();
    }

    if(!isFiniteRect(localRect) || localRect.w <= 0.f || localRect.h <= 0.f)
        return Rect::Empty;
    auto region = elementTransform.mapRect(localRect).intersected(filterRegion);
    return integerRect(region);
}

SVGFeGaussianBlurElement::SVGFeGaussianBlurElement(Document* document)
    : SVGFilterPrimitiveElement(document, ElementID::FeGaussianBlur)
{
    addProperty(m_stdDeviation);
}

SVGFeOffsetElement::SVGFeOffsetElement(Document* document)
    : SVGFilterPrimitiveElement(document, ElementID::FeOffset)
    , m_dx(PropertyID::Dx, 0.f)
    , m_dy(PropertyID::Dy, 0.f)
{
    addProperty(m_dx);
    addProperty(m_dy);
}

SVGFeCompositeElement::SVGFeCompositeElement(Document* document)
    : SVGFilterPrimitiveElement(document, ElementID::FeComposite)
    , m_in2(PropertyID::In2)
    , m_operator(PropertyID::Composite_Operator)
{
    addProperty(m_in2);
    addProperty(m_operator);
}

FilterCompositeOperator SVGFeCompositeElement::op() const
{
    return m_operator.value() == "in" ? FilterCompositeOperator::In : FilterCompositeOperator::Over;
}

std::shared_ptr<Canvas> SVGFilterElement::apply(SVGRenderState& state, const std::shared_ptr<Canvas>& source) const
{
    const auto* element = state.element();
    const auto& elementTransform = state.currentTransform();

    Point columnU;
    Point columnV;
    if(!filterCompatibleTransform(elementTransform, columnU, columnV))
        return source;

    auto filterRegion = regionRect(element, elementTransform);
    if(!filterRegion.isValid() || filterRegion.isEmpty())
        return nullptr;
    filterRegion.intersect(state.canvas()->extents());
    filterRegion = integerRect(filterRegion);
    if(filterRegion.isEmpty() || !isFiniteRect(filterRegion))
        return nullptr;

    auto sourceGraphic = readCanvas(*source, filterRegion);
    auto sourceAlpha = std::make_unique<FilterImage>();
    sourceAlpha->x = sourceGraphic->x;
    sourceAlpha->y = sourceGraphic->y;
    sourceAlpha->w = sourceGraphic->w;
    sourceAlpha->h = sourceGraphic->h;
    sourceAlpha->px.assign(sourceGraphic->px.size(), 0.f);
    for(size_t i = 0; i < sourceAlpha->px.size(); i += 4)
        sourceAlpha->px[i + 3] = sourceGraphic->px[i + 3];

    auto bbox = element->fillBoundingBox();
    auto bboxUnits = primitiveUnits() == Units::ObjectBoundingBox;
    if(bboxUnits && (bbox.w <= 0.f || bbox.h <= 0.f))
        return nullptr;
    auto scaleX = bboxUnits ? bbox.w : 1.f;
    auto scaleY = bboxUnits ? bbox.h : 1.f;

    std::vector<std::unique_ptr<FilterImage>> images;
    std::map<std::string, FilterImage*, std::less<>> named;
    auto findImage = [&](const std::string& name) -> FilterImage* {
        if(name == "SourceGraphic")
            return sourceGraphic.get();
        if(name == "SourceAlpha")
            return sourceAlpha.get();
        auto it = named.find(name);
        return it == named.end() ? nullptr : it->second;
    };

    const FilterImage* previous = nullptr;
    const FilterImage* lastOutput = nullptr;
    bool hasPrimitive = false;
    for(const auto& child : children()) {
        const auto* primitive = toSVGElement(child);
        if(primitive == nullptr || primitive->isDisplayNone())
            continue;
        auto id = primitive->id();
        if(id != ElementID::FeGaussianBlur && id != ElementID::FeOffset && id != ElementID::FeComposite)
            continue;

        hasPrimitive = true;
        const auto* filterPrimitive = static_cast<const SVGFilterPrimitiveElement*>(primitive);
        auto region = filterPrimitive->regionRect(this, element, elementTransform, filterRegion);
        if(region.isEmpty()) {
            auto empty = std::make_unique<FilterImage>();
            empty->x = static_cast<int>(filterRegion.x);
            empty->y = static_cast<int>(filterRegion.y);
            empty->w = static_cast<int>(filterRegion.w);
            empty->h = static_cast<int>(filterRegion.h);
            empty->allocate();
            images.push_back(std::move(empty));
            previous = images.back().get();
            lastOutput = previous;
            if(!filterPrimitive->resultName().value().empty())
                named.insert_or_assign(filterPrimitive->resultName().value(), const_cast<FilterImage*>(previous));
            continue;
        }

        const auto& inName = filterPrimitive->in().value();
        const FilterImage* firstInput = previous ? previous : sourceGraphic.get();
        if(!inName.empty())
            firstInput = findImage(inName);

        std::unique_ptr<FilterImage> output;
        if(id == ElementID::FeGaussianBlur) {
            const auto* blur = static_cast<const SVGFeGaussianBlurElement*>(primitive);
            const auto& values = blur->stdDeviation();
            if(!values.empty() && (!std::isfinite(values[0]) || values[0] < 0.f
                || (values.size() > 1 && (!std::isfinite(values[1]) || values[1] < 0.f)))) {
                return nullptr;
            }

            // A missing stdDeviation is treated as zero, i.e. an identity pass.
            auto stdX = values.empty() ? 0.f : values[0] * scaleX;
            auto stdY = values.empty() ? 0.f : (values.size() > 1 ? values[1] : values[0]) * scaleY;
            output = applyBlur(firstInput, region, filterRegion, stdX, stdY, columnU, columnV);
        } else if(id == ElementID::FeOffset) {
            const auto* offset = static_cast<const SVGFeOffsetElement*>(primitive);
            auto dx = offset->dx() * scaleX;
            auto dy = offset->dy() * scaleY;
            if(!std::isfinite(dx) || !std::isfinite(dy))
                return nullptr;
            auto delta = mapVector(columnU, columnV, dx, dy);
            output = applyOffset(firstInput, region, delta);
        } else {
            const auto* composite = static_cast<const SVGFeCompositeElement*>(primitive);
            const FilterImage* secondInput = previous ? previous : sourceGraphic.get();
            const auto& in2Name = composite->in2().value();
            if(!in2Name.empty())
                secondInput = findImage(in2Name);
            output = applyComposite(firstInput, secondInput, region, composite->op());
        }

        // Inputs are fully resolved before the result name is published, so an
        // overwritten name can never mutate pixels still owned by an earlier
        // branch.
        images.push_back(std::move(output));
        previous = images.back().get();
        lastOutput = previous;
        if(!filterPrimitive->resultName().value().empty())
            named.insert_or_assign(filterPrimitive->resultName().value(), const_cast<FilterImage*>(previous));
    }

    if(!hasPrimitive) {
        auto clipped = readCanvas(*source, filterRegion);
        return writeCanvas(*clipped);
    }

    if(lastOutput == nullptr)
        return nullptr;
    return writeCanvas(*lastOutput);
}

} // namespace lunasvg
