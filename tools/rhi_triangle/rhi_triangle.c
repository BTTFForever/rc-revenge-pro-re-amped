/* rhi_triangle: the vendored ico-pc RHI (third_party/ico_rhi, Vulkan) drawing one textured triangle in an SDL3
 * window. Renderer plan P0 step 2: proves the RHI builds, picks the device, presents, samples a texture and reads
 * the backbuffer back.
 *
 *   rhi_triangle [--frames N] [--screenshot out.png|out.ppm] [--no-validation]
 *
 * Draws N frames (default 120) of a triangle textured with a procedural 64x64 checkerboard (8x8 texel cells,
 * nearest sampling) over a dark grey clear, then exits. On the last frame the backbuffer is read back, checked
 * pixel by pixel against the expected checkerboard and, with --screenshot, written as PNG (uncompressed) or PPM.
 * The validation layer is asked for unless --no-validation (the RHI's debugLayers; ICO_VK_VALIDATION=0/1
 * overrides). Exit code 0 on success, 1 on any failure (including a validation error), 77 if there is no
 * Vulkan window. */
#include "rhi.h"
#include "vk/rhi_vk.h"
#include "triangle_frag.h"
#include "triangle_vert.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WIN_W = 640, WIN_H = 480, TEX = 64, CELL = 8 };

static const uint8_t COLOR_A[4] = {240, 160, 32, 255}; /* orange */
static const uint8_t COLOR_B[4] = {24, 64, 200, 255};  /* blue */
static const float CLEAR[4] = {32 / 255.0f, 32 / 255.0f, 32 / 255.0f, 1.0f};

typedef struct Vtx {
    float x, y, u, v;
} Vtx;

/* The RHI's clip space is y-up as in D3D (vk_cmd.c rhi_cmd_set_viewport flips the Vulkan viewport), so +y is the
 * top of the window. The texture coordinate is the window position mapped to 0..1 (v down), so a pixel's texel is
 * a function of its screen position alone (the check below). */
static const Vtx TRI[3] = {
    {0.0f, 0.8f, 0.5f, 0.1f},
    {0.8f, -0.8f, 0.9f, 0.9f},
    {-0.8f, -0.8f, 0.1f, 0.9f},
};

/* --------------------------------------------------------------------------------------------- image output */
static uint32_t crcTable[256];

static void crcInit(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        crcTable[n] = c;
    }
}

static uint32_t crc(uint32_t c, const uint8_t *p, size_t n)
{
    c ^= 0xFFFFFFFFu;
    while (n--) {
        c = crcTable[(c ^ *p++) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static bool writeChunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t hdr[8];
    put32(hdr, len);
    memcpy(hdr + 4, type, 4);
    uint32_t c = crc(0, hdr + 4, 4);
    c = crc(c, data, len);
    uint8_t tail[4];
    put32(tail, c);
    return fwrite(hdr, 1, 8, f) == 8 && (len == 0 || fwrite(data, 1, len, f) == len) &&
           fwrite(tail, 1, 4, f) == 4;
}

/* rgb: w*h*3 bytes. PNG with stored (uncompressed) deflate blocks: no zlib needed. */
static bool writePng(const char *path, const uint8_t *rgb, uint32_t w, uint32_t h)
{
    const size_t row = (size_t)w * 3 + 1;
    const size_t raw = row * h;
    const size_t blocks = (raw + 65534) / 65535;
    const size_t zlen = 2 + raw + blocks * 5 + 4;
    uint8_t *z = malloc(zlen);
    uint8_t *img = malloc(raw);
    if (!z || !img) {
        free(z);
        free(img);
        return false;
    }
    for (uint32_t y = 0; y < h; y++) {
        img[y * row] = 0; /* filter: none */
        memcpy(img + y * row + 1, rgb + (size_t)y * w * 3, (size_t)w * 3);
    }
    size_t o = 0;
    z[o++] = 0x78;
    z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw; i++) {
        a = (a + img[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    for (size_t off = 0; off < raw; off += 65535) {
        const size_t n = raw - off < 65535 ? raw - off : 65535;
        z[o++] = off + n == raw ? 1 : 0;
        z[o++] = (uint8_t)n;
        z[o++] = (uint8_t)(n >> 8);
        z[o++] = (uint8_t)~n;
        z[o++] = (uint8_t)(~n >> 8);
        memcpy(z + o, img + off, n);
        o += n;
    }
    put32(z + o, (b << 16) | a);
    o += 4;
    uint8_t ihdr[13];
    put32(ihdr, w);
    put32(ihdr + 4, h);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 2;  /* RGB */
    ihdr[10] = 0; /* deflate */
    ihdr[11] = 0;
    ihdr[12] = 0;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(sig, 1, 8, f) == 8 && writeChunk(f, "IHDR", ihdr, 13) &&
              writeChunk(f, "IDAT", z, (uint32_t)o) && writeChunk(f, "IEND", NULL, 0);
    if (f && fclose(f) != 0) {
        ok = false;
    }
    free(z);
    free(img);
    return ok;
}

static bool writePpm(const char *path, const uint8_t *rgb, uint32_t w, uint32_t h)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    bool ok = fprintf(f, "P6\n%u %u\n255\n", w, h) > 0 && fwrite(rgb, 3, (size_t)w * h, f) == (size_t)w * h;
    return fclose(f) == 0 && ok;
}

/* -------------------------------------------------------------------------------------------- the check */
static float edge(float ax, float ay, float bx, float by, float px, float py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* Every pixel well inside the triangle and away from a checker cell edge must be the cell's colour exactly; every
 * pixel well outside must be the clear colour. Returns the number of wrong pixels; *checked the number tested. */
static uint32_t checkImage(const uint8_t *rgb, uint32_t w, uint32_t h, uint32_t *checked)
{
    float vx[3], vy[3];
    for (int i = 0; i < 3; i++) {
        vx[i] = (TRI[i].x * 0.5f + 0.5f) * (float)w;
        vy[i] = (0.5f - TRI[i].y * 0.5f) * (float)h;
    }
    const float area = edge(vx[0], vy[0], vx[1], vy[1], vx[2], vy[2]);
    const uint8_t clear8[3] = {32, 32, 32};
    uint32_t bad = 0, n = 0;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            const float px = (float)x + 0.5f, py = (float)y + 0.5f;
            /* signed distances to the edges, in pixels, positive inside */
            float d = 1e9f;
            for (int i = 0; i < 3; i++) {
                const int j = (i + 1) % 3;
                const float len = SDL_sqrtf((vx[j] - vx[i]) * (vx[j] - vx[i]) + (vy[j] - vy[i]) * (vy[j] - vy[i]));
                const float e = edge(vx[i], vy[i], vx[j], vy[j], px, py) / len * (area > 0 ? 1.0f : -1.0f);
                d = e < d ? e : d;
            }
            const uint8_t *got = rgb + ((size_t)y * w + x) * 3;
            const uint8_t *want;
            if (d > 2.0f) {
                /* the texel this pixel samples: u = px/w, v = py/h over the 64x64 texture */
                const float tu = px / (float)w * TEX, tv = py / (float)h * TEX;
                const float fu = tu / CELL - SDL_floorf(tu / CELL), fv = tv / CELL - SDL_floorf(tv / CELL);
                if (fu < 0.15f || fu > 0.85f || fv < 0.15f || fv > 0.85f) {
                    continue; /* near a cell edge: one texel either way is fine */
                }
                const int cx = (int)(tu / CELL), cy = (int)(tv / CELL);
                want = ((cx + cy) & 1) ? COLOR_B : COLOR_A;
            } else if (d < -2.0f) {
                want = clear8;
            } else {
                continue;
            }
            n++;
            if (got[0] != want[0] || got[1] != want[1] || got[2] != want[2]) {
                if (bad < 5) {
                    printf("rhi_triangle: pixel (%u,%u) is %u %u %u, expected %u %u %u\n", x, y, got[0], got[1],
                           got[2], want[0], want[1], want[2]);
                }
                bad++;
            }
        }
    }
    *checked = n;
    return bad;
}

/* --------------------------------------------------------------------------------------------- main */
static RhiShader makeShader(RhiShaderStage stage, const uint32_t *code, size_t size, const char *name)
{
    RhiShaderDesc d = {0};
    d.stage = stage;
    d.bytecode = code;
    d.bytecodeSize = size;
    d.entryPoint = "main";
    d.debugName = name;
    return rhi_create_shader(&d);
}

int main(int argc, char **argv)
{
    int frames = 120;
    const char *shot = NULL;
    bool validate = true;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            shot = argv[++i];
        } else if (strcmp(argv[i], "--no-validation") == 0) {
            validate = false;
        } else {
            fprintf(stderr, "usage: %s [--frames N] [--screenshot out.png|out.ppm] [--no-validation]\n", argv[0]);
            return 2;
        }
    }
    if (frames < 1) {
        frames = 1;
    }
    crcInit();

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        printf("rhi_triangle: SKIP: SDL_Init: %s\n", SDL_GetError());
        return 77;
    }
    SDL_Window *win = SDL_CreateWindow("rhi_triangle", WIN_W, WIN_H, SDL_WINDOW_VULKAN);
    if (!win) {
        printf("rhi_triangle: SKIP: no Vulkan window: %s\n", SDL_GetError());
        SDL_Quit();
        return 77;
    }
    RhiDeviceDesc dd = {0};
    dd.sdlWindow = win;
    dd.vsync = true;
    dd.debugLayers = validate;
    dd.appName = "rhi_triangle";
    if (!rhi_init(&dd)) {
        printf("rhi_triangle: FAIL: rhi_init\n");
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    printf("rhi_triangle: backend %s, adapter \"%s\", swapchain format %d, present mode %s\n",
           rhi_backend_kind_name(rhi_backend()), rhi_adapter_name(), (int)rhi_swapchain_format(),
           rhi_present_mode_name());
    int failures = 0;

    /* the checkerboard, uploaded once through an upload buffer */
    RhiTexture tex = rhi_create_texture(&(RhiTextureDesc){
        TEX, TEX, 1, RHI_FMT_RGBA8_UNORM, RHI_TEX_SAMPLED | RHI_TEX_COPY_DST, "checkerboard", 0});
    RhiBuffer staging = rhi_create_buffer(
        &(RhiBufferDesc){TEX * TEX * 4, RHI_BUF_COPY_SRC, RHI_MEM_UPLOAD, "checkerboard upload"});
    RhiBuffer vb = rhi_create_buffer(&(RhiBufferDesc){sizeof(TRI), RHI_BUF_VERTEX, RHI_MEM_UPLOAD, "triangle"});
    RhiSampler smp = rhi_create_sampler(&(RhiSamplerDesc){RHI_FILTER_NEAREST, RHI_FILTER_NEAREST,
                                                          RHI_FILTER_NEAREST, RHI_WRAP_REPEAT, RHI_WRAP_REPEAT,
                                                          1.0f, 0.0f, 0.0f, 0.0f});
    uint8_t *texels = rhi_map_buffer(staging);
    Vtx *verts = rhi_map_buffer(vb);
    if (!tex.id || !staging.id || !vb.id || !smp.id || !texels || !verts) {
        printf("rhi_triangle: FAIL: resource creation\n");
        failures++;
        goto done;
    }
    for (int y = 0; y < TEX; y++) {
        for (int x = 0; x < TEX; x++) {
            memcpy(texels + (y * TEX + x) * 4, (((x / CELL) + (y / CELL)) & 1) ? COLOR_B : COLOR_A, 4);
        }
    }
    rhi_unmap_buffer(staging);
    memcpy(verts, TRI, sizeof(TRI));
    rhi_unmap_buffer(vb);

    /* layouts: group 0 and 1 empty, group 2 texture t1 and sampler s1 (the fixed slot model of rhi.h) */
    static const RhiBindSlot slots2[2] = {{1, RHI_BIND_SAMPLED_TEXTURE, 1u << RHI_STAGE_FRAGMENT},
                                          {1, RHI_BIND_SAMPLER, 1u << RHI_STAGE_FRAGMENT}};
    RhiBindGroupLayout layouts[3] = {
        rhi_create_bind_group_layout(&(RhiBindGroupLayoutDesc){NULL, 0, "group0"}),
        rhi_create_bind_group_layout(&(RhiBindGroupLayoutDesc){NULL, 0, "group1"}),
        rhi_create_bind_group_layout(&(RhiBindGroupLayoutDesc){slots2, 2, "group2"}),
    };
    RhiShader vs = makeShader(RHI_STAGE_VERTEX, spv_triangle_vert, sizeof(spv_triangle_vert), "triangle.vert");
    RhiShader fs = makeShader(RHI_STAGE_FRAGMENT, spv_triangle_frag, sizeof(spv_triangle_frag), "triangle.frag");
    static const RhiVertexBinding vbind = {0, sizeof(Vtx), false};
    static const RhiVertexAttr vattr[2] = {{0, 0, RHI_VTX_F32x2, offsetof(Vtx, x)},
                                           {1, 0, RHI_VTX_F32x2, offsetof(Vtx, u)}};
    RhiPipelineDesc pd = {0};
    pd.vertex = vs;
    pd.fragment = fs;
    pd.layouts = layouts;
    pd.layoutCount = 3;
    pd.vertexBindings = &vbind;
    pd.vertexBindingCount = 1;
    pd.vertexAttrs = vattr;
    pd.vertexAttrCount = 2;
    pd.topology = RHI_TOPO_TRIANGLE_LIST;
    pd.cullNone = true;
    pd.blend[0] = (RhiBlendState){.enable = false, .writeMask = 0xF};
    pd.colorFormats[0] = rhi_swapchain_format();
    pd.colorCount = 1;
    pd.depthFormat = RHI_FMT_UNKNOWN;
    pd.debugName = "triangle";
    RhiPipeline pipe = rhi_create_pipeline(&pd);
    if (!layouts[0].id || !layouts[1].id || !layouts[2].id || !vs.id || !fs.id || !pipe.id) {
        printf("rhi_triangle: FAIL: layouts, shaders or pipeline\n");
        failures++;
        goto done;
    }

    /* the upload */
    {
        RhiCommandList cl = rhi_begin_commands();
        RhiTextureBarrier b0 = {tex, RHI_STATE_UNDEFINED, RHI_STATE_COPY_DST};
        rhi_cmd_barrier(cl, &b0, 1);
        rhi_cmd_copy_buffer_to_texture(cl, staging, 0, TEX * 4, tex, 0, (RhiRect){0, 0, TEX, TEX});
        RhiTextureBarrier b1 = {tex, RHI_STATE_COPY_DST, RHI_STATE_SHADER_READ};
        rhi_cmd_barrier(cl, &b1, 1);
        rhi_submit(cl);
    }

    uint8_t *px = NULL;
    uint32_t shotW = 0, shotH = 0;
    bool quit = false;
    int drawn = 0;
    for (int n = 0; n < frames && !quit; n++) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                quit = true;
            }
        }
        const bool last = n == frames - 1 || quit;
        rhi_wait_frame();
        RhiTexture bb = rhi_acquire_backbuffer();
        uint32_t w = 0, h = 0;
        if (!bb.id || !rhi_swapchain_size(&w, &h)) {
            printf("rhi_triangle: FAIL: frame %d: no backbuffer\n", n);
            failures++;
            break;
        }
        RhiBinding binds[2] = {0};
        binds[0].slot = 1;
        binds[0].type = RHI_BIND_SAMPLED_TEXTURE;
        binds[0].texture = tex;
        binds[0].aspect = RHI_ASPECT_COLOR;
        binds[1].slot = 1;
        binds[1].type = RHI_BIND_SAMPLER;
        binds[1].sampler = smp;
        RhiBindGroup g0 = rhi_create_bind_group(&(RhiBindGroupDesc){layouts[0], NULL, 0});
        RhiBindGroup g1 = rhi_create_bind_group(&(RhiBindGroupDesc){layouts[1], NULL, 0});
        RhiBindGroup g2 = rhi_create_bind_group(&(RhiBindGroupDesc){layouts[2], binds, 2});

        RhiCommandList cl = rhi_begin_commands();
        RhiTextureBarrier b0 = {bb, RHI_STATE_UNDEFINED, RHI_STATE_RENDER_TARGET};
        rhi_cmd_barrier(cl, &b0, 1);
        RhiRenderPassDesc rp = {0};
        rp.color[0] = (RhiColorAttachment){bb, RHI_LOAD_CLEAR, {CLEAR[0], CLEAR[1], CLEAR[2], CLEAR[3]},
                                           RHI_STORE_STORE, false};
        rp.colorCount = 1;
        rp.width = w;
        rp.height = h;
        rhi_cmd_begin_render_pass(cl, &rp);
        rhi_cmd_set_viewport(cl, &(RhiViewport){0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f});
        rhi_cmd_set_scissor(cl, &(RhiRect){0, 0, w, h});
        rhi_cmd_set_pipeline(cl, pipe);
        rhi_cmd_set_bind_group(cl, 0, g0);
        rhi_cmd_set_bind_group(cl, 1, g1);
        rhi_cmd_set_bind_group(cl, 2, g2);
        rhi_cmd_set_vertex_buffer(cl, 0, vb, 0);
        rhi_cmd_draw(cl, 3, 0, 1);
        rhi_cmd_end_render_pass(cl);
        if (last) {
            RhiTextureBarrier b1 = {bb, RHI_STATE_RENDER_TARGET, RHI_STATE_COPY_SRC};
            rhi_cmd_barrier(cl, &b1, 1);
            rhi_submit(cl);
            /* the readback: BGRA or RGBA bytes as the swapchain format says */
            const size_t size = (size_t)w * h * 4;
            uint8_t *raw = malloc(size);
            uint32_t pitch = 0;
            if (!raw || !rhi_readback_texture(bb, RHI_ASPECT_COLOR, raw, size, &pitch) || pitch != w * 4) {
                printf("rhi_triangle: FAIL: readback\n");
                failures++;
            } else {
                const bool bgra = rhi_swapchain_format() == RHI_FMT_BGRA8_UNORM;
                px = malloc((size_t)w * h * 3);
                if (px) {
                    for (size_t i = 0; i < (size_t)w * h; i++) {
                        px[i * 3 + 0] = raw[i * 4 + (bgra ? 2 : 0)];
                        px[i * 3 + 1] = raw[i * 4 + 1];
                        px[i * 3 + 2] = raw[i * 4 + (bgra ? 0 : 2)];
                    }
                    shotW = w;
                    shotH = h;
                }
            }
            free(raw);
            cl = rhi_begin_commands();
            RhiTextureBarrier b2 = {bb, RHI_STATE_COPY_SRC, RHI_STATE_PRESENT};
            rhi_cmd_barrier(cl, &b2, 1);
            rhi_submit(cl);
        } else {
            RhiTextureBarrier b1 = {bb, RHI_STATE_RENDER_TARGET, RHI_STATE_PRESENT};
            rhi_cmd_barrier(cl, &b1, 1);
            rhi_submit(cl);
        }
        rhi_present();
        drawn++;
    }
    printf("rhi_triangle: %d frame(s) drawn\n", drawn);

    if (px) {
        uint32_t checked = 0;
        const uint32_t bad = checkImage(px, shotW, shotH, &checked);
        printf("rhi_triangle: image %ux%u, %u pixels checked, %u wrong\n", shotW, shotH, checked, bad);
        if (bad || checked < shotW * shotH / 4) {
            printf("rhi_triangle: FAIL: image check\n");
            failures++;
        }
        if (shot) {
            const size_t len = strlen(shot);
            const bool ppm = len > 4 && SDL_strcasecmp(shot + len - 4, ".ppm") == 0;
            if (!(ppm ? writePpm : writePng)(shot, px, shotW, shotH)) {
                printf("rhi_triangle: FAIL: could not write %s\n", shot);
                failures++;
            } else {
                printf("rhi_triangle: screenshot %s\n", shot);
            }
        }
        free(px);
    } else if (!failures) {
        printf("rhi_triangle: FAIL: no image read back\n");
        failures++;
    }

    rhi_wait_idle();
    rhi_destroy_pipeline(pipe);
    rhi_destroy_shader(vs);
    rhi_destroy_shader(fs);
    for (int i = 0; i < 3; i++) {
        rhi_destroy_bind_group_layout(layouts[i]);
    }
done:
    rhi_wait_idle();
    if (smp.id) {
        rhi_destroy_sampler(smp);
    }
    if (vb.id) {
        rhi_destroy_buffer(vb);
    }
    if (staging.id) {
        rhi_destroy_buffer(staging);
    }
    if (tex.id) {
        rhi_destroy_texture(tex);
    }
    const uint32_t verrs = rhi_vk_validation_error_count();
    rhi_shutdown();
    SDL_DestroyWindow(win);
    SDL_Quit();
    if (verrs) {
        printf("rhi_triangle: FAIL: %u validation error(s)\n", verrs);
        failures++;
    }
    printf("rhi_triangle: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
