#pragma once

#include "SDL.h"
#include <vector>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <algorithm>

namespace swr {

class Texture {
public:
    static constexpr int MAX_ANISOTROPY = 16;

    /// Description of the texel-space filtering footprint used for one sample.
    struct Footprint {
        float majorLength;   ///< Long axis of the texel-space ellipse, in texels.
        float minorLength;   ///< Short axis of the texel-space ellipse, in texels.
        float dirMajorU;     ///< Unit long axis direction, u component (texel space).
        float dirMajorV;     ///< Unit long axis direction, v component (texel space).
        float effectiveMinor;///< Short axis width actually filtered, in texels.
        float lod;           ///< log2(effectiveMinor), clamped to the mip range.
        int   tapCount;      ///< Number of taps placed along the long axis.
    };

    Texture(SDL_Surface* baseSurface, int maxAnisotropy = 8)
        : m_maxAnisotropy(std::clamp(maxAnisotropy, 1, MAX_ANISOTROPY))
    {
        if (!baseSurface) {
            throw std::runtime_error("Null base surface provided to Texture constructor");
        }
        if (!baseSurface->pixels) {
            throw std::runtime_error("Base surface has no pixel data");
        }
        generateMipmaps(baseSurface);
    }

    ~Texture() {
        cleanup();
    }

    // Prevent copying since we manage SDL resources
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    int maxAnisotropy() const { return m_maxAnisotropy; }

    /// Compute the texel-space footprint for the given UV derivatives.
    /** The footprint is built from the ellipse spanned by the two screen-space
      * derivative vectors, so it depends only on the texel-space ellipse and
      * not on the choice of screen basis. All lengths are in texels. */
    Footprint queryFootprint(float u, float v,
                             float dudx, float dvdx,
                             float dudy, float dvdy) const
    {
        validateInputs(u, v, dudx, dvdx, dudy, dvdy);

        SDL_Surface* baseLevel = m_mipmaps[0];
        const float texW = static_cast<float>(baseLevel->w);
        const float texH = static_cast<float>(baseLevel->h);

        // Convert the UV derivatives to texel space. Width and height are
        // applied separately so non-square textures keep the correct ellipse.
        double dux = double(dudx) * double(texW);
        double dvx = double(dvdx) * double(texH);
        double duy = double(dudy) * double(texW);
        double dvy = double(dvdy) * double(texH);

        // J * J^T for J = [dux duy] (columns are the screen axis vectors).
        //                      [dvx dvy]
        // Its eigenvectors are the ellipse axes in texel space and the square
        // roots of its eigenvalues are the axis lengths. The Gram matrix is
        // formed in double so finite inputs cannot overflow into NaN/inf.
        double A = dux * dux + duy * duy;
        double B = dvx * dvx + dvy * dvy;
        double C = dux * dvx + duy * dvy;

        double discriminant = std::sqrt(std::max(0.0, (A - B) * (A - B) + 4.0 * C * C));
        double lambdaMajor = std::max(0.0, (A + B + discriminant) * 0.5);
        double lambdaMinor = std::max(0.0, (A + B - discriminant) * 0.5);

        // Texel-space lengths can exceed the float range even when every
        // input is finite (a derivative of FLT_MAX times the texture width).
        // Saturate instead of letting them turn into infinity.
        double majorD = std::sqrt(lambdaMajor);
        double minorD = std::sqrt(lambdaMinor);
        const double floatMax = static_cast<double>(std::numeric_limits<float>::max());
        float majorLength = static_cast<float>(std::min(majorD, floatMax));
        float minorLength = static_cast<float>(std::min(minorD, floatMax));

        // Long axis unit vector. (lambdaMajor - B, C) is always an
        // eigenvector of the Gram matrix; it only vanishes for a diagonal
        // matrix, where the axes align with the texel coordinate axes.
        float dirU = 1.0f;
        float dirV = 0.0f;
        const double eps = 1e-20;
        double evx = lambdaMajor - B;
        double evy = C;
        double evLen = std::sqrt(evx * evx + evy * evy);
        if (evLen > eps) {
            dirU = static_cast<float>(evx / evLen);
            dirV = static_cast<float>(evy / evLen);
        } else if (A < B) {
            dirU = 0.0f;
            dirV = 1.0f;
        }

        // A zero-length axis still spans one texel for filtering purposes.
        float majorAxis = std::max(majorLength, 1.0f);
        float minorAxis = std::max(minorLength, 1.0f);

        // At the anisotropy limit the short axis has to cover enough texels
        // for the available taps; widening the mip filter is the correct
        // response, simply sampling fewer times from an overly fine mip is not.
        float cap = majorAxis / static_cast<float>(m_maxAnisotropy);
        float effectiveMinor = std::max({minorAxis, 1.0f, cap});

        float lod = std::log2(effectiveMinor);
        lod = std::clamp(lod, 0.0f, static_cast<float>(m_mipmaps.size() - 1));

        int tapCount = static_cast<int>(std::ceil(majorAxis / effectiveMinor));
        tapCount = std::clamp(tapCount, 1, m_maxAnisotropy);

        Footprint f;
        f.majorLength = majorLength;
        f.minorLength = minorLength;
        f.dirMajorU = dirU;
        f.dirMajorV = dirV;
        f.effectiveMinor = effectiveMinor;
        f.lod = lod;
        f.tapCount = tapCount;
        return f;
    }

    void sample(float u, float v, float dudx, float dvdx, float dudy, float dvdy, Uint32& outColor) const {
        validateInputs(u, v, dudx, dvdx, dudy, dvdy);

        if (m_mipmaps.empty()) {
            outColor = 0;
            return;
        }

        // Repeat wrap of the texture coordinate.
        u = std::fmod(u, 1.0f);
        v = std::fmod(v, 1.0f);
        if (u < 0) u += 1.0f;
        if (v < 0) v += 1.0f;

        SDL_Surface* baseLevel = m_mipmaps[0];
        if (!baseLevel || !baseLevel->pixels) {
            outColor = 0;
            return;
        }

        Footprint fp = queryFootprint(u, v, dudx, dvdx, dudy, dvdy);

        if (fp.tapCount <= 1) {
            // Isotropic footprint: regular trilinear filtering.
            sampleTrilinear(u, v, fp.effectiveMinor, outColor);
            return;
        }

        // Anisotropic footprint: taps are placed symmetrically and at equal
        // spacing along the texel-space long axis, inside the footprint.
        float r = 0.0f, g = 0.0f, b = 0.0f;
        float step = 1.0f / static_cast<float>(fp.tapCount);

        for (int i = 0; i < fp.tapCount; ++i) {
            float t = (static_cast<float>(i) + 0.5f) * step - 0.5f;
            float offset = t * fp.majorLength;
            float sample_u = u + fp.dirMajorU * offset / static_cast<float>(baseLevel->w);
            float sample_v = v + fp.dirMajorV * offset / static_cast<float>(baseLevel->h);

            sample_u = std::fmod(sample_u, 1.0f);
            sample_v = std::fmod(sample_v, 1.0f);
            if (sample_u < 0) sample_u += 1.0f;
            if (sample_v < 0) sample_v += 1.0f;

            Uint32 sample_color;
            sampleTrilinear(sample_u, sample_v, fp.effectiveMinor, sample_color);

            r += static_cast<float>(getR(sample_color));
            g += static_cast<float>(getG(sample_color));
            b += static_cast<float>(getB(sample_color));
        }

        float invTaps = 1.0f / static_cast<float>(fp.tapCount);
        r = std::min(255.0f, r * invTaps);
        g = std::min(255.0f, g * invTaps);
        b = std::min(255.0f, b * invTaps);

        outColor = packRGB(static_cast<Uint8>(r), static_cast<Uint8>(g), static_cast<Uint8>(b));
    }

private:
    static void validateInputs(float u, float v,
                               float dudx, float dvdx,
                               float dudy, float dvdy)
    {
        if (!std::isfinite(u) || !std::isfinite(v) ||
            !std::isfinite(dudx) || !std::isfinite(dvdx) ||
            !std::isfinite(dudy) || !std::isfinite(dvdy)) {
            throw std::invalid_argument("Texture::sample received non-finite UV coordinate or derivative");
        }
    }

    void sampleTrilinear(float u, float v, float rho, Uint32& outColor) const {
        if (m_mipmaps.empty()) {
            outColor = 0;
            return;
        }

        // Calculate mipmap level based on rho (texel-space filter width)
        float lod = std::log2(std::max(rho, 1e-6f));
        lod = std::clamp(lod, 0.0f, static_cast<float>(m_mipmaps.size() - 1));

        int lodBase = static_cast<int>(std::floor(lod));
        int lodNext = std::min(lodBase + 1, static_cast<int>(m_mipmaps.size()) - 1);
        float lodFrac = std::clamp(lod - lodBase, 0.0f, 1.0f);

        Uint32 colorBase, colorNext;
        sampleBilinear(lodBase, u, v, colorBase);
        if (lodBase != lodNext) {
            sampleBilinear(lodNext, u, v, colorNext);
            outColor = lerpColors(colorBase, colorNext, lodFrac);
        } else {
            outColor = colorBase;
        }
    }

    void sampleBilinear(int mipLevel, float u, float v, Uint32& outColor) const {
        if (mipLevel < 0 || mipLevel >= static_cast<int>(m_mipmaps.size())) {
            outColor = 0;
            return;
        }

        SDL_Surface* mip = m_mipmaps[mipLevel];
        if (!mip || !mip->pixels) {
            outColor = 0;
            return;
        }

        float px = u * (mip->w - 1);
        float py = v * (mip->h - 1);

        int x0 = std::max(0, int(std::floor(px)));
        int y0 = std::max(0, int(std::floor(py)));
        int x1 = std::min(mip->w - 1, x0 + 1);
        int y1 = std::min(mip->h - 1, y0 + 1);

        float fx = px - x0;
        float fy = py - y0;

        Uint32* pixels = static_cast<Uint32*>(mip->pixels);
        if (!pixels) {
            outColor = 0;
            return;
        }

        Uint32 c00 = pixels[y0 * mip->w + x0];
        Uint32 c10 = pixels[y0 * mip->w + x1];
        Uint32 c01 = pixels[y1 * mip->w + x0];
        Uint32 c11 = pixels[y1 * mip->w + x1];

        outColor = bilinearLerpColors(c00, c10, c01, c11, fx, fy);
    }

    static inline Uint8 getR(Uint32 color) { return (color >> 16) & 0xFF; }
    static inline Uint8 getG(Uint32 color) { return (color >> 8) & 0xFF; }
    static inline Uint8 getB(Uint32 color) { return color & 0xFF; }

    static inline Uint32 packRGB(Uint8 r, Uint8 g, Uint8 b) {
        return (r << 16) | (g << 8) | b;
    }

    static inline Uint32 lerpColors(Uint32 c1, Uint32 c2, float t) {
        Uint8 r = getR(c1) + (Uint8)((getR(c2) - getR(c1)) * t);
        Uint8 g = getG(c1) + (Uint8)((getG(c2) - getG(c1)) * t);
        Uint8 b = getB(c1) + (Uint8)((getB(c2) - getB(c1)) * t);
        return packRGB(r, g, b);
    }

    static inline Uint32 bilinearLerpColors(Uint32 c00, Uint32 c10, Uint32 c01, Uint32 c11, float fx, float fy) {
        Uint8 r = (Uint8)(
            getR(c00) * (1 - fx) * (1 - fy) +
            getR(c10) * fx * (1 - fy) +
            getR(c01) * (1 - fx) * fy +
            getR(c11) * fx * fy
        );
        Uint8 g = (Uint8)(
            getG(c00) * (1 - fx) * (1 - fy) +
            getG(c10) * fx * (1 - fy) +
            getG(c01) * (1 - fx) * fy +
            getG(c11) * fx * fy
        );
        Uint8 b = (Uint8)(
            getB(c00) * (1 - fx) * (1 - fy) +
            getB(c10) * fx * (1 - fy) +
            getB(c01) * (1 - fx) * fy +
            getB(c11) * fx * fy
        );
        return packRGB(r, g, b);
    }

    void generateMipmaps(SDL_Surface* baseTexture) {
        cleanup();

        if (!baseTexture || !baseTexture->pixels) {
            throw std::runtime_error("Invalid base texture in generateMipmaps");
        }

        // Copy base texture
        SDL_Surface* baseCopy = SDL_CreateRGBSurface(0, baseTexture->w, baseTexture->h, 32,
            0x00FF0000,  // R mask
            0x0000FF00,  // G mask
            0x000000FF,  // B mask
            0x00000000); // A mask

        if (!baseCopy) {
            throw std::runtime_error(std::string("Failed to create base texture copy: ") + SDL_GetError());
        }

        SDL_BlitSurface(baseTexture, NULL, baseCopy, NULL);
        m_mipmaps.push_back(baseCopy);

        SDL_Surface* prevLevel = baseCopy;
        int width = baseTexture->w;
        int height = baseTexture->h;

        // Generate mip chain until 1x1
        while (width > 1 || height > 1) {
            int newWidth = std::max(1, width / 2);
            int newHeight = std::max(1, height / 2);

            SDL_Surface* newMip = SDL_CreateRGBSurface(0, newWidth, newHeight, 32,
                0x00FF0000,  // R mask
                0x0000FF00,  // G mask
                0x000000FF,  // B mask
                0x00000000); // A mask

            if (!newMip) {
                break;
            }

            if (SDL_LockSurface(prevLevel) < 0 || SDL_LockSurface(newMip) < 0) {
                SDL_FreeSurface(newMip);
                throw std::runtime_error("Failed to lock surfaces for mipmap generation");
            }

            Uint32* srcPixels = static_cast<Uint32*>(prevLevel->pixels);
            Uint32* dstPixels = static_cast<Uint32*>(newMip->pixels);

            for (int y = 0; y < newHeight; y++) {
                for (int x = 0; x < newWidth; x++) {
                    Uint32 p00 = srcPixels[(y*2) * prevLevel->w + (x*2)];
                    Uint32 p10 = x*2+1 < prevLevel->w ? srcPixels[(y*2) * prevLevel->w + (x*2+1)] : p00;
                    Uint32 p01 = y*2+1 < prevLevel->h ? srcPixels[(y*2+1) * prevLevel->w + (x*2)] : p00;
                    Uint32 p11 = (x*2+1 < prevLevel->w && y*2+1 < prevLevel->h) ?
                                srcPixels[(y*2+1) * prevLevel->w + (x*2+1)] : p00;

                    Uint8 r = (getR(p00) + getR(p10) + getR(p01) + getR(p11)) >> 2;
                    Uint8 g = (getG(p00) + getG(p10) + getG(p01) + getG(p11)) >> 2;
                    Uint8 b = (getB(p00) + getB(p10) + getB(p01) + getB(p11)) >> 2;

                    dstPixels[y * newMip->w + x] = packRGB(r, g, b);
                }
            }

            SDL_UnlockSurface(prevLevel);
            SDL_UnlockSurface(newMip);

            m_mipmaps.push_back(newMip);

            prevLevel = newMip;
            width = newWidth;
            height = newHeight;
        }
    }

    void cleanup() {
        for (SDL_Surface* mip : m_mipmaps) {
            if (mip) {
                SDL_FreeSurface(mip);
            }
        }
        m_mipmaps.clear();
    }

    std::vector<SDL_Surface*> m_mipmaps;
    int m_maxAnisotropy;
};

} // namespace swr
