#include <cstdlib>

#include <renderer.h>
#include <math/matrix4.h>

#include <gl_window.h>

#include <timing.h>
#include <sampler.h>
#include <model.h>
#include <objload.h>

#include <random>
#include <vector>

#include "pbr_material.h"


struct Material
{
    Texture<RGBA8> map_albedo;
    Texture<RGBA8> map_metallic_roughness;
    Texture<RGBA8> map_normal;
};

/* Mesh definition (pbr::Vertex and Material --> see model.h for members) */
using Mesh = asset::Mesh<pbr::Vertex, Material>;

int main(int argc, char** argv)
{
    /* model */
    auto model = asset::loadObj<Mesh>("assets/camera/camera.obj");
    //auto model = asset::loadObj<Mesh>("assets/drakefire/drakefire.obj");
    std::vector< BufferIndexed<pbr::Vertex, unsigned int> > buffers;
    std::vector< Material > materials;

    for(auto& mesh : model.meshes())
    {
        auto& buffer = buffers.emplace_back();
        buffer.vertices = mesh.vertices();
        buffer.indices = mesh.materialGroups().front().indices;

        auto& material = materials.emplace_back();
        material = mesh.material(0);
    }

    /* load precomputed irradiance, radiance and brdf maps (see pbr_precompute.cpp) */
    Texture<RGBAF> texture_irradiance;
    Texture<RGBAF> texture_radiance;
    Texture<RGBAF> texture_brdf;
    load_texture(texture_irradiance, "assets/pbr/precomputed_irradiance.hdr");
    load_mipmaps(texture_radiance, "assets/pbr/", "precomputed_radiance.hdr");
    load_texture(texture_brdf, "assets/pbr/precomputed_brdf.hdr");


    /*========== Setup Shader Program ========*/
    Program<pbr::Vertex, pbr::Varying, pbr::Uniforms> program;
    program.onVertex(pbr::vertexShader);

    program.onFragment([](const pbr::Uniforms& uniform, const pbr::Varying& in, Vec4& out)
    {
        out = Vec4(pbr::shadeFragment(uniform, in), 1.0f);
    });

    /* set uniforms */
    auto& uniforms = program.uniforms();
    uniforms.proj = Mat4::perspective(radians(45.0f), 1280.0f/720.0f, 1.0, 7.0);
    uniforms.view = Mat4::translation(-Vec3{0, 0.0, 3.5});
    uniforms.viewPos = Vec3{0, 0.0, 3.5};

    uniforms.material.albedo.filter = eFilter::NEAREST_MIPMAP_LINEAR;
    uniforms.material.metallic_roughness.filter = eFilter::NEAREST_MIPMAP_LINEAR;
    uniforms.material.normal.filter = eFilter::NEAREST_MIPMAP_LINEAR;

    uniforms.irradiance = texture_irradiance;
    uniforms.irradiance.wrap = eWrap::CLAMP_EDGE;
    uniforms.prefilter_radiance = texture_radiance;
    uniforms.prefilter_radiance.wrap = eWrap::CLAMP_EDGE;
    uniforms.brdf = texture_brdf;
    uniforms.brdf.wrap = eWrap::CLAMP_EDGE;

    /* A soft rectangular emitter in front of the asset.  Replace this list
       from application code to move/animate area lights without GL code. */
    uniforms.lights.push_back(pbr::AreaLight{
        {
            pbr::Vec3d{-1.8, 1.6, 2.2},
            pbr::Vec3d{-1.8, 0.2, 2.2},
            pbr::Vec3d{ 1.8, 0.2, 2.2},
            pbr::Vec3d{ 1.8, 1.6, 2.2}
        },
        pbr::Vec3d{2.8, 2.55, 2.25},
        true
    });


    /* rasterizer */
    unsigned int fbwidth = 1280; unsigned fbheight = 720;
    Renderer rasterizer(fbwidth, fbheight);

    /*========== OpenGL/GLFW Viewer ========*/
    Window window("Physically-Based-Rendering", 1280, 720, fbwidth, fbheight);

    window.onDraw([&](Window& window, float dt)
    {
        rasterizer.framebuffer().clear(Vec4(0, 0, 0, 1));

        static float time = 0.0;
        time += dt;

        uniforms.model = Mat4::translation(Vec3{0, -0.25, 0}) * Mat4::scale(1.4, 1.4, 1.4) * Mat4::rotationY(-radians(160.0f + time*20.0f)) * Mat4::rotationX(radians(10.0f));

        for(unsigned int i = 0; i < buffers.size(); i++)
        {
            uniforms.material.albedo = materials[i].map_albedo;
            uniforms.material.metallic_roughness = materials[i].map_metallic_roughness;
            uniforms.material.normal = materials[i].map_normal;

            TIME_MS(rasterizer.draw(program, buffers[i]));
        }

        window.swap(rasterizer.framebuffer());
    });

    window.run();

    return EXIT_SUCCESS;
}
