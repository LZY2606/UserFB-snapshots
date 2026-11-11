#ifndef LUNASVG_SVGFILTER_H
#define LUNASVG_SVGFILTER_H

#include "svgelement.h"

namespace lunasvg {

enum class FilterCompositeOperator {
    Over,
    In
};

class SVGFilterElement;

// Whether the element's user-to-device transform is expressible as two
// orthogonal, non-reflected columns: translations, rotations and positive
// non-uniform scales. Shear, singular and reflected transforms are rejected.
bool filterTransformSupported(const Transform& transform);

class SVGFilterElement final : public SVGElement {
public:
    SVGFilterElement(Document* document);

    Units filterUnits() const { return m_filterUnits.value(); }
    Units primitiveUnits() const { return m_primitiveUnits.value(); }

    Rect localRegionRect(const SVGElement* element) const;
    Rect regionRect(const SVGElement* element, const Transform& elementTransform) const;

    // Runs the filter pipeline on `source`, the already painted SourceGraphic.
    // Returns nullptr when the filter result is empty.
    std::shared_ptr<Canvas> apply(SVGRenderState& state, const std::shared_ptr<Canvas>& source) const;

private:
    SVGEnumeration<Units> m_filterUnits{PropertyID::FilterUnits, Units::ObjectBoundingBox};
    SVGEnumeration<Units> m_primitiveUnits{PropertyID::PrimitiveUnits, Units::UserSpaceOnUse};
    SVGLength m_x{PropertyID::X, LengthDirection::Horizontal, LengthNegativeMode::Allow};
    SVGLength m_y{PropertyID::Y, LengthDirection::Vertical, LengthNegativeMode::Allow};
    SVGLength m_width{PropertyID::Width, LengthDirection::Horizontal, LengthNegativeMode::Allow};
    SVGLength m_height{PropertyID::Height, LengthDirection::Vertical, LengthNegativeMode::Allow};
};

class SVGFilterPrimitiveElement : public SVGElement {
public:
    SVGFilterPrimitiveElement(Document* document, ElementID id);

    const SVGString& in() const { return m_in; }
    const SVGString& resultName() const { return m_result; }

    Rect regionRect(const SVGFilterElement* filterElement, const SVGElement* element,
                    const Transform& elementTransform, const Rect& filterRegion) const;

private:
    SVGString m_in{PropertyID::In};
    SVGString m_result{PropertyID::Result};
    SVGLength m_x{PropertyID::X, LengthDirection::Horizontal, LengthNegativeMode::Allow};
    SVGLength m_y{PropertyID::Y, LengthDirection::Vertical, LengthNegativeMode::Allow};
    SVGLength m_width{PropertyID::Width, LengthDirection::Horizontal, LengthNegativeMode::Allow};
    SVGLength m_height{PropertyID::Height, LengthDirection::Vertical, LengthNegativeMode::Allow};
};

class SVGFeGaussianBlurElement final : public SVGFilterPrimitiveElement {
public:
    SVGFeGaussianBlurElement(Document* document);

    const NumberList& stdDeviation() const { return m_stdDeviation.values(); }

private:
    SVGNumberList m_stdDeviation{PropertyID::Std_Deviation};
};

class SVGFeOffsetElement final : public SVGFilterPrimitiveElement {
public:
    SVGFeOffsetElement(Document* document);

    float dx() const { return m_dx.value(); }
    float dy() const { return m_dy.value(); }

private:
    SVGNumber m_dx{PropertyID::Dx, 0.f};
    SVGNumber m_dy{PropertyID::Dy, 0.f};
};

class SVGFeCompositeElement final : public SVGFilterPrimitiveElement {
public:
    SVGFeCompositeElement(Document* document);

    const SVGString& in2() const { return m_in2; }
    FilterCompositeOperator op() const;

private:
    SVGString m_in2{PropertyID::In2};
    SVGString m_operator{PropertyID::Composite_Operator};
};

} // namespace lunasvg

#endif // LUNASVG_SVGFILTER_H
