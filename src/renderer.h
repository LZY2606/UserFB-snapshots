#ifndef RENDERER_H
#define RENDERER_H

#include "vector.h"
#include "scene.h"
#include "camera.h"
#include "beamfilm.h"

class Renderer {

private:
    Scene *m_scene;
    Camera *m_camera;
    Vec *m_pixel_buffer;
    BeamTracer m_beam;
    BeamFilm m_beam_film;
    bool m_has_beam_film;

public:
    Renderer(Scene *scene, Camera *camera);
    void render(int samples=4);
    void save_image(const char * file_path);

    // Analytic beam frame. Throws std::invalid_argument on invalid input;
    // the previous beam film is retained on failure and Renderer instances
    // keep independent state.
    const BeamFilm& render_beam(int depth);
    bool has_beam_film() const { return m_has_beam_film; }
    const BeamFilm& beam_film() const;
};

#endif //RENDERER_H
