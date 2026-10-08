// VITA5 app — video texture. See video_surface.hpp.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_surface.hpp"

#include <GL/glcorearb.h>

namespace vd5
{

VideoSurface::~VideoSurface()
{
    destroy();
}

bool VideoSurface::create(int width, int height)
{
    destroy();
    if (width <= 0 || height <= 0)
        return false;
    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (texture == 0)
        return false;
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    texture_ = texture;
    width_ = width;
    height_ = height;
    return true;
}

void VideoSurface::destroy()
{
    chain_valid_ = false;
    if (texture_ != 0)
    {
        const GLuint texture = texture_;
        glDeleteTextures(1, &texture);
    }
    texture_ = 0;
    width_ = 0;
    height_ = 0;
}

void VideoSurface::process_nv12(const std::uint8_t *nv12, int chain_mode, int factor)
{
    if (nv12 == nullptr || width_ <= 0 || height_ <= 0 || chain_mode < 1 || chain_mode > 2 ||
        factor < 2 || factor > 4)
    {
        chain_valid_ = false;
        return;
    }
    chain_valid_ = upscaler_.run(nv12, width_, height_, chain_mode, factor) != 0;
}

void VideoSurface::upload(const std::uint8_t *rgba)
{
    if (texture_ == 0 || rgba == nullptr)
        return;
    glBindTexture(GL_TEXTURE_2D, texture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindTexture(GL_TEXTURE_2D, 0);
}

} // namespace vd5
