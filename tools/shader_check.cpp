// Compares the track vertex shader with the exact C++ version of the VU1 program, on runs captured from the game.
//
// Every triangle runs through decomp/shaders/trdr_shape.vert on the GPU and, on its own, through runTrdrShape.
// Build: g++ -std=c++20 -O2 -I decomp/vu1 tools/shader_check.cpp decomp/vu1/trdr_shape.cpp -lEGL -lOpenGL
// Usage: shader_check <shader> <capture>...
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>

#include <xmmintrin.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "vu1_ref.h"

namespace {

constexpr uint32_t kRunBytes = 0x4000u + 32u * 16u + 4u;

struct Output
{
    float gs[3], stq[3], colour[4], keep, viewZ;
};

struct Stats
{
    uint64_t triangles = 0, compared = 0, culledAgree = 0, behindNear = 0, screenClipped = 0, offscreen = 0, keepDisagree = 0;
    double maxXY = 0.0, maxZ = 0.0, maxST = 0.0, maxColour = 0.0;
    uint64_t overXY = 0, overZ = 0, overST = 0, overColour = 0;
};

GLuint compileShader(const std::string &source)
{
    const GLuint shader = glCreateShader(GL_VERTEX_SHADER);
    const char *text = source.c_str();
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[4096];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "%s\n", log);
        std::exit(1);
    }
    return shader;
}

} // namespace

int main(int argc, char **argv)
{
    _mm_setcsr(_mm_getcsr() | 0x6000u); // The VU rounds toward zero.

    EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    eglInitialize(display, nullptr, nullptr);
    eglBindAPI(EGL_OPENGL_API);
    const EGLint configAttributes[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
    EGLConfig config;
    EGLint found = 0;
    eglChooseConfig(display, configAttributes, &config, 1, &found);
    const EGLint contextAttributes[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes);
    if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context))
    {
        std::fprintf(stderr, "no GL context\n");
        return 1;
    }
    std::printf("GL: %s\n", glGetString(GL_RENDERER));

    std::ifstream shaderFile(argv[1]);
    std::stringstream shaderText;
    shaderText << shaderFile.rdbuf();
    const GLuint program = glCreateProgram();
    glAttachShader(program, compileShader("#version 430 core\n#define TRDR_STANDALONE\n" + shaderText.str()));
    const char *varyings[] = {"gsPosition", "stq", "colour", "keep", "viewZ"};
    glTransformFeedbackVaryings(program, 5, varyings, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked)
    {
        char log[4096];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        std::fprintf(stderr, "%s\n", log);
        return 1;
    }
    glUseProgram(program);

    GLuint vao, input, feedback, uniforms;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &input);
    glGenBuffers(1, &feedback);
    glGenBuffers(1, &uniforms);
    GLuint framebuffer, renderbuffer; // A surfaceless context has no framebuffer of its own to draw with.
    glGenFramebuffers(1, &framebuffer);
    glGenRenderbuffers(1, &renderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 1, 1);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);
    glEnable(GL_RASTERIZER_DISCARD);

    Stats stats;
    std::vector<uint8_t> single(0x4000);
    for (int a = 2; a < argc; ++a)
    {
        std::ifstream file(argv[a], std::ios::binary);
        std::vector<uint8_t> run(kRunBytes);
        while (file.read(reinterpret_cast<char *>(run.data()), kRunBytes))
        {
            const uint8_t *memory = run.data();
            const auto quad = [&](int index) { return reinterpret_cast<const float *>(memory + (index & 0x3FF) * 16); };
            const float(*registers)[4] = reinterpret_cast<const float(*)[4]>(memory + 0x4000);
            uint32_t top;
            std::memcpy(&top, memory + 0x4000 + 32 * 16, 4);

            float draw[4 * 11] = {};
            std::memcpy(draw, quad(18), 64);
            std::memcpy(draw + 16, quad(16), 16);
            std::memcpy(draw + 20, quad(17), 16);
            std::memcpy(draw + 24, quad(15), 16);
            std::memcpy(draw + 28, quad(12), 16);
            std::memcpy(draw + 32, quad(11), 16);
            draw[36] = 2048.0f, draw[37] = 2048.0f, draw[38] = 320.0f, draw[39] = 240.0f;
            draw[40] = 16777216.0f;
            glBindBuffer(GL_UNIFORM_BUFFER, uniforms);
            glBufferData(GL_UNIFORM_BUFFER, sizeof(draw), draw, GL_STREAM_DRAW);
            glBindBufferBase(GL_UNIFORM_BUFFER, 1, uniforms);

            // Find every triangle, keeping the block each one came from.
            struct Item { int headerAt; int at; };
            std::vector<Item> items;
            int reader = static_cast<int>(top & 0x3FFu);
            for (int guard = 0; guard < 64; ++guard)
            {
                int16_t count;
                std::memcpy(&count, memory + (reader & 0x3FF) * 16 + 8, 2);
                if (count < 0)
                {
                    reader += 1 - count;
                    continue;
                }
                if (count == 0 || reader + 4 + count * 10 > 1024)
                    break;
                for (int t = 0; t < count; ++t)
                    items.push_back({reader, reader + 4 + t * 10});
                reader += 4 + count * 10;
            }
            if (items.empty())
                continue;

            std::vector<uint8_t> triangles(items.size() * 160u);
            for (size_t i = 0; i < items.size(); ++i)
                std::memcpy(triangles.data() + i * 160u, memory + items[i].at * 16, 160u);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, input);
            glBufferData(GL_SHADER_STORAGE_BUFFER, triangles.size(), triangles.data(), GL_STREAM_DRAW);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, input);
            std::vector<Output> outputs(items.size() * 3u);
            glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, feedback);
            glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, outputs.size() * sizeof(Output), nullptr, GL_STREAM_READ);
            glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, feedback);
            glBeginTransformFeedback(GL_POINTS);
            glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(outputs.size()));
            glEndTransformFeedback();
            glGetBufferSubData(GL_TRANSFORM_FEEDBACK_BUFFER, 0, outputs.size() * sizeof(Output), outputs.data());

            for (size_t i = 0; i < items.size(); ++i)
            {
                ++stats.triangles;
                // Run this triangle alone through the exact version.
                std::memcpy(single.data(), memory, 0x4000);
                const int at = static_cast<int>(top & 0x3FFu);
                std::memcpy(single.data() + at * 16, memory + items[i].headerAt * 16, 64);
                const int16_t one = 1;
                std::memcpy(single.data() + at * 16 + 8, &one, 2);
                std::memcpy(single.data() + (at + 4) * 16, memory + items[i].at * 16, 160);
                std::memset(single.data() + (at + 14) * 16, 0, 16);
                std::vector<std::vector<uint8_t>> kicks;
                rc::vu1::runTrdrShape(single.data(), registers, 0u, top, kicks);

                const Output *out = &outputs[i * 3u];
                const float nearZ = quad(12)[2];
                const bool behind = out[0].viewZ < nearZ || out[1].viewZ < nearZ || out[2].viewZ < nearZ;
                const int drawn = static_cast<int>(kicks.size()) - 2;

                if (out[0].keep == 0.0f)
                {
                    if (drawn == 0)
                        ++stats.culledAgree;
                    else
                        ++stats.keepDisagree;
                    continue;
                }
                if (behind)
                {
                    ++stats.behindNear;
                    continue;
                }
                if (drawn == 0)
                {
                    ++stats.offscreen; // Wholly past one screen edge: the GPU discards it as well.
                    continue;
                }
                // A triangle past the guard band goes through the original's clipper even if one piece comes out.
                const float guard = quad(16)[3];
                bool crosses = false;
                for (int corner = 0; corner < 3; ++corner)
                    crosses = crosses || std::fabs(out[corner].gs[0] - quad(16)[0]) > guard || std::fabs(out[corner].gs[1] - quad(16)[1]) > guard;
                if (crosses || drawn != 1 || kicks[2].size() != 160u)
                {
                    ++stats.screenClipped; // Crosses the screen edge: the GPU cuts it instead.
                    continue;
                }

                ++stats.compared;
                const uint8_t *packet = kicks[2].data() + 16;
                for (int corner = 0; corner < 3; ++corner)
                {
                    const float *st = reinterpret_cast<const float *>(packet + corner * 48);
                    const int32_t *colour = reinterpret_cast<const int32_t *>(packet + corner * 48 + 16);
                    const int32_t *xyz = reinterpret_cast<const int32_t *>(packet + corner * 48 + 32);
                    const Output &o = out[corner];
                    const double dx = std::fabs(o.gs[0] * 16.0 - xyz[0]), dy = std::fabs(o.gs[1] * 16.0 - xyz[1]);
                    const double xy = std::max(dx, dy);
                    const double z = std::fabs(std::trunc(static_cast<double>(o.gs[2])) - xyz[2]); // The game truncates depth.
                    double stError = 0.0;
                    for (int k = 0; k < 3; ++k)
                        stError = std::max(stError, std::fabs(o.stq[k] - st[k]) / std::max(1e-6, std::fabs(static_cast<double>(st[k]))));
                    double colourError = 0.0;
                    for (int k = 0; k < 4; ++k)
                        colourError = std::max(colourError, static_cast<double>(std::fabs(o.colour[k] - colour[k])));
                    stats.maxXY = std::max(stats.maxXY, xy);
                    stats.maxZ = std::max(stats.maxZ, z);
                    stats.maxST = std::max(stats.maxST, stError);
                    stats.maxColour = std::max(stats.maxColour, colourError);
                    stats.overXY += xy > 2.0;
                    stats.overZ += z > 2.0;
                    stats.overST += stError > 1e-5;
                    stats.overColour += colourError > 1.0;
                }
            }
        }
    }

    std::printf("triangles %llu: compared %llu, rejected by both %llu, cut by the near plane %llu, crossing a screen edge %llu, off screen %llu\n",
                (unsigned long long)stats.triangles, (unsigned long long)stats.compared, (unsigned long long)stats.culledAgree,
                (unsigned long long)stats.behindNear, (unsigned long long)stats.screenClipped, (unsigned long long)stats.offscreen);
    std::printf("facing test disagreements: %llu\n", (unsigned long long)stats.keepDisagree);
    std::printf("worst error: x,y %.3f (1/16 pixel), z %.1f (GS depth units), st %.2e (relative), colour %.1f\n", stats.maxXY, stats.maxZ, stats.maxST, stats.maxColour);
    std::printf("corners over tolerance: x,y %llu, z %llu, st %llu, colour %llu\n", (unsigned long long)stats.overXY, (unsigned long long)stats.overZ,
                (unsigned long long)stats.overST, (unsigned long long)stats.overColour);
    return 0;
}
