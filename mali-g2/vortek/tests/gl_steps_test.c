/* [mali-g2] Minimal GLES2 steps over EGL surfaceless (zink): clear, draw, read back. Prints
 * which step first fails, to tell a GPU fault in zink's command streams from a present problem. */
#include <stdio.h>
#include <string.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
static int check(const char* step) {
    unsigned char px[4] = {0};
    glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    GLenum e = glGetError();
    printf("%-6s -> pixel %3u %3u %3u %3u  glError 0x%x  reset %s\n", step, px[0], px[1], px[2], px[3], e, "-");
    fflush(stdout);
    return e;
}
int main(int argc, char** argv) {
    int steps = argc > 1 ? atoi(argv[1]) : 3;
    PFNEGLGETPLATFORMDISPLAYEXTPROC getDisplay = (void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay dpy = getDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (!eglInitialize(dpy, NULL, NULL)) { printf("eglInitialize failed\n"); return 1; }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ca);
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) { printf("makecurrent failed\n"); return 1; }
    printf("renderer: %s\n", glGetString(GL_RENDERER));
    GLuint tex, fbo;
    glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glViewport(0, 0, 64, 64);
    glClearColor(1, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    check("clear");
    if (steps < 2) return 0;
    const char* vs = "attribute vec2 p; void main(){ gl_Position = vec4(p, 0.0, 1.0); }";
    const char* fs = "precision mediump float; void main(){ gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }";
    GLuint v = glCreateShader(GL_VERTEX_SHADER); glShaderSource(v, 1, &vs, NULL); glCompileShader(v);
    GLuint f = glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(f, 1, &fs, NULL); glCompileShader(f);
    GLuint prog = glCreateProgram(); glAttachShader(prog, v); glAttachShader(prog, f);
    glBindAttribLocation(prog, 0, "p"); glLinkProgram(prog); glUseProgram(prog);
    float tri[] = {-1, -1, 3, -1, -1, 3};
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri); glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    check("draw");
    if (steps < 3) return 0;
    for (int i = 0; i < 30; i++) { glClearColor(0, 0, (float)i / 30, 1); glClear(GL_COLOR_BUFFER_BIT); glDrawArrays(GL_TRIANGLES, 0, 3); glFlush(); }
    check("loop");
    return 0;
}
