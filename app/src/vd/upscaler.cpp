// VITA5 app — GPU upscale chain. See upscaler.hpp.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "upscaler.hpp"

#include "upscale_shaders.hpp"

#include <GL/glcorearb.h>

#include <cstddef>

#include "gfx/gl_program.hpp"

namespace vd5
{

namespace
{

void make_texture(GLuint *texture, GLint internal, int width, int height, GLenum format)
{
    glGenTextures(1, texture);
    glBindTexture(GL_TEXTURE_2D, *texture);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, width, height, 0, format, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
}

// Runs one full-target pass: attach `target`, draw the fullscreen triangle.
void run_pass(GLuint fbo, GLuint program, GLuint target, int width, int height)
{
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
    glViewport(0, 0, width, height);
    glUseProgram(program);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

} // namespace

Upscaler::~Upscaler()
{
    shutdown();
}

bool Upscaler::ensure_programs()
{
    if (prog_yuv_ != 0 && prog_blit_ != 0 && prog_easu_ != 0 && prog_rcas_ != 0 && vao_ != 0)
        return true;
    if (programs_failed_)
        return false;
    prog_yuv_ = hui::gfx::build_program("vd5-yuv", upscale_shaders::kVertex, upscale_shaders::kYuv);
    prog_blit_ =
        hui::gfx::build_program("vd5-blit", upscale_shaders::kVertex, upscale_shaders::kBlit);
    prog_easu_ =
        hui::gfx::build_program("vd5-easu", upscale_shaders::kVertex, upscale_shaders::kEasu);
    prog_rcas_ =
        hui::gfx::build_program("vd5-rcas", upscale_shaders::kVertex, upscale_shaders::kRcas);
    if (prog_yuv_ == 0 || prog_blit_ == 0 || prog_easu_ == 0 || prog_rcas_ == 0)
    {
        shutdown();
        programs_failed_ = true;
        return false;
    }
    // Sampler units are fixed per program.
    glProgramUniform1i(prog_yuv_, 0, 0); // u_y
    glProgramUniform1i(prog_yuv_, 1, 1); // u_uv
    glProgramUniform1i(prog_blit_, 0, 0);
    glProgramUniform1i(prog_easu_, 0, 0);
    glProgramUniform1i(prog_rcas_, 0, 0);
    // The passes draw gl_VertexID triangles; core profile wants a VAO.
    glGenVertexArrays(1, &vao_);
    if (vao_ == 0)
    {
        shutdown();
        programs_failed_ = true;
        return false;
    }
    return true;
}

void Upscaler::destroy_targets()
{
    const GLuint textures[] = {y_tex_, uv_tex_, work_, tmp_, out_};
    glDeleteTextures(5, textures);
    y_tex_ = uv_tex_ = work_ = tmp_ = out_ = 0;
    if (fbo_ != 0)
    {
        const GLuint fbo = fbo_;
        glDeleteFramebuffers(1, &fbo);
        fbo_ = 0;
    }
    src_w_ = src_h_ = factor_ = out_w_ = out_h_ = 0;
}

bool Upscaler::ensure_targets(int width, int height, int factor)
{
    if (out_ != 0 && src_w_ == width && src_h_ == height && factor_ == factor)
        return true;
    destroy_targets();
    if (width <= 0 || height <= 0 || width > 1280 || height > 720)
        return false;
    if (factor < 2 || factor > 4)
        return false;
    // Cap the output so a 1280x720 source at 4x stays under ~8K class.
    if (width * factor > 4096 || height * factor > 4096)
        return false;
    const int out_w = width * factor;
    const int out_h = height * factor;
    make_texture(&y_tex_, GL_R8, width, height, GL_RED);
    make_texture(&uv_tex_, GL_RG8, width / 2, height / 2, GL_RG);
    make_texture(&work_, GL_RGBA8, width, height, GL_RGBA);
    make_texture(&tmp_, GL_RGBA8, out_w, out_h, GL_RGBA);
    make_texture(&out_, GL_RGBA8, out_w, out_h, GL_RGBA);
    glGenFramebuffers(1, &fbo_);
    if (y_tex_ == 0 || uv_tex_ == 0 || work_ == 0 || tmp_ == 0 || out_ == 0 || fbo_ == 0)
    {
        destroy_targets();
        return false;
    }
    src_w_ = width;
    src_h_ = height;
    factor_ = factor;
    out_w_ = out_w;
    out_h_ = out_h;
    return true;
}

std::uint32_t Upscaler::run(const std::uint8_t *nv12, int width, int height, int mode, int factor)
{
    // mode 1 = bilinear (sharp), mode 2 = FSR EASU; factor is the integer scale.
    if (nv12 == nullptr || mode < 1 || mode > 2 || factor < 2 || factor > 4)
        return 0;
    if (!ensure_programs() || !ensure_targets(width, height, factor))
        return 0;

    const int out_w = width * factor;
    const int out_h = height * factor;

    // ---- upload the NV12 planes (R8 luma, RG8 interleaved chroma) ----
    glBindTexture(GL_TEXTURE_2D, y_tex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, nv12);
    glBindTexture(GL_TEXTURE_2D, uv_tex_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width / 2, height / 2, GL_RG, GL_UNSIGNED_BYTE,
                    nv12 + (std::size_t)width * height);
    glBindTexture(GL_TEXTURE_2D, 0);

    // ---- save the renderer's state ----
    GLint prev_fbo = 0, prev_program = 0, prev_vao = 0, prev_active = 0, prev_viewport[4] = {};
    GLint prev_tex0 = 0, prev_tex1 = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex1);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex0);
    glBindVertexArray(vao_);

    // ---- pass 1: NV12 -> RGBA at source size ----
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, y_tex_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, uv_tex_);
    run_pass(fbo_, prog_yuv_, work_, width, height);

    // ---- pass 2: stretch to factor× (bilinear) or EASU ----
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, work_);
    if (mode == 1)
    {
        glProgramUniform2f(prog_blit_, 1, (float)out_w, (float)out_h);
        run_pass(fbo_, prog_blit_, tmp_, out_w, out_h);
    }
    else
    {
        glProgramUniform2f(prog_easu_, 1, (float)width, (float)height);
        glProgramUniform2f(prog_easu_, 2, (float)out_w, (float)out_h);
        run_pass(fbo_, prog_easu_, tmp_, out_w, out_h);
    }

    // ---- pass 3: RCAS sharpening at output size ----
    glBindTexture(GL_TEXTURE_2D, tmp_);
    glProgramUniform1f(prog_rcas_, 1, 1.0f); // FsrRcasCon(0.0): full sharpness
    run_pass(fbo_, prog_rcas_, out_, out_w, out_h);

    // ---- restore ----
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    glUseProgram((GLuint)prev_program);
    glBindVertexArray((GLuint)prev_vao);
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex0);
    glActiveTexture((GLenum)prev_active);
    return out_;
}

void Upscaler::shutdown()
{
    if (prog_yuv_ != 0)
        glDeleteProgram(prog_yuv_);
    if (prog_blit_ != 0)
        glDeleteProgram(prog_blit_);
    if (prog_easu_ != 0)
        glDeleteProgram(prog_easu_);
    if (prog_rcas_ != 0)
        glDeleteProgram(prog_rcas_);
    prog_yuv_ = prog_blit_ = prog_easu_ = prog_rcas_ = 0;
    if (vao_ != 0)
    {
        const GLuint vao = vao_;
        glDeleteVertexArrays(1, &vao);
        vao_ = 0;
    }
    destroy_targets();
    programs_failed_ = false;
}

} // namespace vd5
