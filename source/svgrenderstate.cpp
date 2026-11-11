#include "svgrenderstate.h"
#include "svgfilter.h"

namespace lunasvg {

SVGBlendInfo::SVGBlendInfo(const SVGElement* element)
    : m_clipper(element->clipper())
    , m_masker(element->masker())
    , m_filter(element->filterer())
    , m_opacity(element->opacity())
{
}

bool SVGBlendInfo::requiresCompositing(SVGRenderMode mode) const
{
    return (m_clipper && m_clipper->requiresMasking())
        || (mode == SVGRenderMode::Painting && (m_masker || m_filter || m_opacity < 1.f));
}

bool SVGRenderState::hasCycleReference(const SVGElement* element) const
{
    auto current = this;
    do {
        if(element == current->element())
            return true;
        current = current->parent();
    } while(current);
    return false;
}

void SVGRenderState::beginGroup(const SVGBlendInfo& blendInfo)
{
    auto requiresCompositing = blendInfo.requiresCompositing(m_mode);
    if(requiresCompositing) {
        Rect boundingBox;
        if(m_mode == SVGRenderMode::Painting && blendInfo.filterer()
            && filterTransformSupported(m_currentTransform)) {
            // The offscreen must cover the whole filter region, including the
            // part outside the parent canvas: SourceGraphic is collected
            // without the parent clip so that an offset can move it back in.
            boundingBox = blendInfo.filterer()->regionRect(m_element, m_currentTransform);
            if(!boundingBox.isValid() || boundingBox.isEmpty())
                boundingBox = m_currentTransform.mapRect(m_element->paintBoundingBox());
        } else {
            // Unsupported transforms fall back to the unfiltered drawing, so
            // size the offscreen from the painted geometry instead of a region
            // that the transform may place somewhere else entirely.
            boundingBox = m_currentTransform.mapRect(m_element->paintBoundingBox());
            boundingBox.intersect(m_canvas->extents());
        }

        m_canvas = Canvas::create(boundingBox);
    } else {
        m_canvas->save();
    }

    if(!requiresCompositing && blendInfo.clipper()) {
        blendInfo.clipper()->applyClipPath(*this);
    }
}

void SVGRenderState::endGroup(const SVGBlendInfo& blendInfo)
{
    if(m_canvas == m_parent->canvas()) {
        m_canvas->restore();
        return;
    }

    if(m_mode == SVGRenderMode::Painting && blendInfo.filterer()) {
        auto filtered = blendInfo.filterer()->apply(*this, m_canvas);
        if(!filtered)
            return;
        m_canvas = std::move(filtered);
    }

    auto opacity = m_mode == SVGRenderMode::Clipping ? 1.f : blendInfo.opacity();
    if(blendInfo.clipper())
        blendInfo.clipper()->applyClipMask(*this);
    if(m_mode == SVGRenderMode::Painting && blendInfo.masker()) {
        blendInfo.masker()->applyMask(*this);
    }

    m_parent->m_canvas->blendCanvas(*m_canvas, BlendMode::Src_Over, opacity);
}

} // namespace lunasvg
