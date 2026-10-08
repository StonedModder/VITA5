// SPDX-License-Identifier: GPL-3.0-or-later
// Host-only fault injection into the REAL Upscaler. These GL mocks test
// fallback control flow, not PS5 shader compilation or GPU rendering.
#ifdef VD5_HOST_UPSCALER_TEST
#include "vd/upscaler.hpp"
#include <GL/glcorearb.h>
#include <cstdio>
#include <cstring>
#include <set>

namespace
{
GLuint next_id = 1;
int builds = 0;
int draws = 0;
std::set<GLuint> programs;
} // namespace

namespace hui::gfx
{
GLuint build_program(const char *label, const char *, const char *)
{
    ++builds;
    if (std::strcmp(label, "vd5-yuv") == 0)
        return 0; // simulate one failed shader while RCAS succeeds
    const GLuint id = next_id++;
    programs.insert(id);
    return id;
}
} // namespace hui::gfx

extern "C"
{
    void glGenTextures(GLsizei n, GLuint *ids)
    {
        while (n-- > 0)
            *ids++ = next_id++;
    }
    void glBindTexture(GLenum, GLuint)
    {
    }
    void glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)
    {
    }
    void glTexParameteri(GLenum, GLenum, GLint)
    {
    }
    void glBindFramebuffer(GLenum, GLuint)
    {
    }
    void glFramebufferTexture2D(GLenum, GLenum, GLenum, GLuint, GLint)
    {
    }
    void glViewport(GLint, GLint, GLsizei, GLsizei)
    {
    }
    void glUseProgram(GLuint)
    {
    }
    void glDrawArrays(GLenum, GLint, GLsizei)
    {
        ++draws;
    }
    void glProgramUniform1i(GLuint, GLint, GLint)
    {
    }
    void glProgramUniform1f(GLuint, GLint, GLfloat)
    {
    }
    void glProgramUniform2f(GLuint, GLint, GLfloat, GLfloat)
    {
    }
    void glGenVertexArrays(GLsizei n, GLuint *ids)
    {
        while (n-- > 0)
            *ids++ = next_id++;
    }
    void glDeleteTextures(GLsizei, const GLuint *)
    {
    }
    void glDeleteFramebuffers(GLsizei, const GLuint *)
    {
    }
    void glGenFramebuffers(GLsizei n, GLuint *ids)
    {
        while (n-- > 0)
            *ids++ = next_id++;
    }
    void glPixelStorei(GLenum, GLint)
    {
    }
    void glTexSubImage2D(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                         const void *)
    {
    }
    void glGetIntegerv(GLenum name, GLint *value)
    {
        *value = 0;
        if (name == GL_VIEWPORT)
            for (int i = 1; i < 4; ++i)
                value[i] = 0;
    }
    void glActiveTexture(GLenum)
    {
    }
    void glBindVertexArray(GLuint)
    {
    }
    void glDeleteProgram(GLuint id)
    {
        programs.erase(id);
    }
    void glDeleteVertexArrays(GLsizei, const GLuint *)
    {
    }
}

int main()
{
    int failures = 0;
    const unsigned char nv12[6] = {128, 128, 128, 128, 128, 128};
    {
        vd5::Upscaler chain;
        if (chain.run(nv12, 2, 2, 1, 2) != 0)
        {
            std::puts("FAIL: first shader failure must request CPU fallback");
            ++failures;
        }
        const int first_builds = builds;
        if (chain.run(nv12, 2, 2, 1, 2) != 0)
        {
            std::puts("FAIL: second frame accepted a partial shader set as usable");
            ++failures;
        }
        if (draws != 0)
        {
            std::puts("FAIL: failed chain issued GPU draws instead of CPU fallback");
            ++failures;
        }
        if (!programs.empty())
        {
            std::puts("FAIL: partial shader resources were retained after failure");
            ++failures;
        }
        if (builds != first_builds)
        {
            std::puts("FAIL: permanent shader failure retried compilation on every frame");
            ++failures;
        }
    }
    if (!programs.empty())
    {
        std::puts("FAIL: program cleanup leak");
        ++failures;
    }
    if (failures == 0)
        std::puts(
            "upscaler fallback: all checks passed (host fault injection, NOT GPU validation)");
    return failures == 0 ? 0 : 1;
}
#endif
