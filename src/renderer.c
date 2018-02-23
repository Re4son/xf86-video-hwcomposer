#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include "xf86.h"

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <stddef.h>

#include <malloc.h>
#include <sync/sync.h>
#include <hybris/hwcomposerwindow/hwcomposer.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "driver.h"

const char vertex_src [] =
    "attribute vec4 position;\n"
    "attribute vec4 texcoords;\n"
    "varying vec2 textureCoordinate;\n"

    "void main()\n"
    "{\n"
    "    gl_Position = position;\n"
    "    textureCoordinate = texcoords.xy;\n"
    "}\n";

const char fragment_src [] =
    "varying highp vec2 textureCoordinate;\n"
    "uniform sampler2D texture;\n"

    "void main()\n"
    "{\n"
    "    gl_FragColor = texture2D(texture, textureCoordinate);\n"
    "}\n";

const char fragment_src_bgra [] =
    "varying highp vec2 textureCoordinate;\n"
    "uniform sampler2D texture;\n"

    "void main()\n"
    "{\n"
    "    gl_FragColor = texture2D(texture, textureCoordinate).bgra;\n"
    "}\n";

GLuint load_shader(const char *shader_source, GLenum type)
{
    GLuint  shader = glCreateShader(type);

    glShaderSource(shader, 1, &shader_source, NULL);
    glCompileShader(shader);

    return shader;
}

GLint position_loc;
GLint texcoords_loc;
GLint texture_loc;

static const GLfloat squareVertices[] = {
    -1.0f, -1.0f,
    1.0f, -1.0f,
    -1.0f,  1.0f,
    1.0f,  1.0f,
};

static const GLfloat textureVertices[] = {
    1.0f, 1.0f,
    1.0f, 0.0f,
    0.0f,  1.0f,
    0.0f,  0.0f,
};

void present(void *user_data, struct ANativeWindow *window,
                                struct ANativeWindowBuffer *buffer)
{
    ScrnInfoPtr pScrn = (ScrnInfoPtr)user_data;
    HWCPtr hwc = HWCPTR(pScrn);

    hwc_display_contents_1_t **contents = hwc->hwcContents;
    hwc_layer_1_t *fblayer = hwc->fblayer;
    hwc_composer_device_1_t *hwcdevice = hwc->hwcDevicePtr;

    xf86DrvMsg(pScrn->scrnIndex, X_DEBUG, "present callback called\n");

    int oldretire = contents[0]->retireFenceFd;
    contents[0]->retireFenceFd = -1;

    fblayer->handle = buffer->handle;
    fblayer->acquireFenceFd = HWCNativeBufferGetFence(buffer);
    fblayer->releaseFenceFd = -1;
    int err = hwcdevice->prepare(hwcdevice, HWC_NUM_DISPLAY_TYPES, contents);
    assert(err == 0);

    err = hwcdevice->set(hwcdevice, HWC_NUM_DISPLAY_TYPES, contents);
    // in android surfaceflinger ignores the return value as not all display types may be supported
    HWCNativeBufferSetFence(buffer, fblayer->releaseFenceFd);

    if (oldretire != -1)
    {
        sync_wait(oldretire, -1);
        close(oldretire);
    }
}

Bool hwc_init_hybris_native_buffer(ScrnInfoPtr pScrn)
{
    HWCPtr hwc = HWCPTR(pScrn);

    if (strstr(eglQueryString(hwc->display, EGL_EXTENSIONS), "EGL_HYBRIS_native_buffer") == NULL)
    {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "EGL_HYBRIS_native_buffer is missing. Make sure libhybris EGL implementation is used\n");
        return FALSE;
    }

    hwc->eglHybrisCreateNativeBuffer = (PFNEGLHYBRISCREATENATIVEBUFFERPROC) eglGetProcAddress("eglHybrisCreateNativeBuffer");
    assert(hwc->eglHybrisCreateNativeBuffer != NULL);

    hwc->eglHybrisLockNativeBuffer = (PFNEGLHYBRISLOCKNATIVEBUFFERPROC) eglGetProcAddress("eglHybrisLockNativeBuffer");
    assert(hwc->eglHybrisLockNativeBuffer != NULL);

    hwc->eglHybrisUnlockNativeBuffer = (PFNEGLHYBRISUNLOCKNATIVEBUFFERPROC) eglGetProcAddress("eglHybrisUnlockNativeBuffer");
    assert(hwc->eglHybrisUnlockNativeBuffer != NULL);

    hwc->eglHybrisReleaseNativeBuffer = (PFNEGLHYBRISRELEASENATIVEBUFFERPROC) eglGetProcAddress("eglHybrisReleaseNativeBuffer");
    assert(hwc->eglHybrisReleaseNativeBuffer != NULL);

    hwc->eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC) eglGetProcAddress("eglCreateImageKHR");
    assert(hwc->eglCreateImageKHR != NULL);

    hwc->eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC) eglGetProcAddress("eglDestroyImageKHR");
    assert(hwc->eglDestroyImageKHR  != NULL);

    hwc->glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) eglGetProcAddress("glEGLImageTargetTexture2DOES");
    assert(hwc->glEGLImageTargetTexture2DOES != NULL);
}

Bool hwc_egl_renderer_init(ScrnInfoPtr pScrn)
{
    HWCPtr hwc = HWCPTR(pScrn);

    EGLDisplay display;
    EGLConfig ecfg;
    EGLint num_config;
    EGLint attr[] = {       // some attributes to set up our egl-interface
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_STENCIL_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_NONE
    };
    EGLSurface surface;
    EGLint ctxattr[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    EGLContext context;
    EGLBoolean rv;
    int err;

    struct ANativeWindow *win = HWCNativeWindowCreate(hwc->hwcWidth, hwc->hwcHeight, HAL_PIXEL_FORMAT_RGBA_8888, present, pScrn);

    display = eglGetDisplay(NULL);
    assert(eglGetError() == EGL_SUCCESS);
    assert(display != EGL_NO_DISPLAY);
    hwc->display = display;

    rv = eglInitialize(display, 0, 0);
    assert(eglGetError() == EGL_SUCCESS);
    assert(rv == EGL_TRUE);

    eglChooseConfig((EGLDisplay) display, attr, &ecfg, 1, &num_config);
    assert(eglGetError() == EGL_SUCCESS);
    assert(rv == EGL_TRUE);

    surface = eglCreateWindowSurface((EGLDisplay) display, ecfg, (EGLNativeWindowType)win, NULL);
    assert(eglGetError() == EGL_SUCCESS);
    assert(surface != EGL_NO_SURFACE);
    hwc->surface = surface;

    context = eglCreateContext((EGLDisplay) display, ecfg, EGL_NO_CONTEXT, ctxattr);
    assert(eglGetError() == EGL_SUCCESS);
    assert(context != EGL_NO_CONTEXT);
    hwc->context = context;

    assert(eglMakeCurrent((EGLDisplay) display, surface, surface, context) == EGL_TRUE);

    const char *version = glGetString(GL_VERSION);
    assert(version);
    printf("%s\n",version);

    glGenTextures(1, &hwc->rootTexture);
    hwc->image = EGL_NO_IMAGE_KHR;
    hwc->shaderProgram = 0;

    return TRUE;
}

void hwc_egl_renderer_screen_init(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    HWCPtr hwc = HWCPTR(pScrn);

    glBindTexture(GL_TEXTURE_2D, hwc->rootTexture);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    if (!hwc->glamor && hwc->image == EGL_NO_IMAGE_KHR) {
        hwc->image = hwc->eglCreateImageKHR(hwc->display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_HYBRIS,
                                            (EGLClientBuffer)hwc->buffer, NULL);
        hwc->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, hwc->image);
    }

    if (!hwc->shaderProgram)
    {
        GLuint vertexShader = load_shader(vertex_src, GL_VERTEX_SHADER);     // load vertex shader
        GLuint fragmentShader;

        if (hwc->glamor)
            fragmentShader = load_shader(fragment_src ,GL_FRAGMENT_SHADER);  // load fragment shader
        else
            fragmentShader = load_shader(fragment_src_bgra, GL_FRAGMENT_SHADER);  // load fragment shader

        GLuint shaderProgram  = hwc->shaderProgram = glCreateProgram();          // create program object
        glAttachShader ( shaderProgram, vertexShader );             // and attach both...
        glAttachShader ( shaderProgram, fragmentShader );           // ... shaders to it

        glLinkProgram ( shaderProgram );    // link the program

        //// now get the locations (kind of handle) of the shaders variables
        position_loc  = glGetAttribLocation  ( shaderProgram , "position" );
        texcoords_loc = glGetAttribLocation  ( shaderProgram , "texcoords" );
        texture_loc = glGetUniformLocation ( shaderProgram , "texture" );

        if ( position_loc < 0  ||  texcoords_loc < 0 || texture_loc < 0 ) {
            xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "EGLRenderer_Init: failed to get shader variables locations\n");
        }
    }

    eglSwapInterval(hwc->display, 0);
}

void hwc_egl_renderer_update(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    HWCPtr hwc = HWCPTR(pScrn);

    if (hwc->glamor) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, hwc->hwcWidth, hwc->hwcHeight);
    }

    glUseProgram(hwc->shaderProgram);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hwc->rootTexture);
    glUniform1i(texture_loc, 0);

    glVertexAttribPointer(position_loc, 2, GL_FLOAT, 0, 0, squareVertices);
    glEnableVertexAttribArray(position_loc);

    glVertexAttribPointer(texcoords_loc, 2, GL_FLOAT, 0, 0, textureVertices);
    glEnableVertexAttribArray(texcoords_loc);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisableVertexAttribArray(position_loc);
    glDisableVertexAttribArray(texcoords_loc);

    eglSwapBuffers (hwc->display, hwc->surface );  // get the rendered buffer to the screen
}

void hwc_egl_renderer_screen_close(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    HWCPtr hwc = HWCPTR(pScrn);

    if (hwc->image != EGL_NO_IMAGE_KHR) {
        hwc->eglDestroyImageKHR(hwc->display, hwc->image);
        hwc->image = EGL_NO_IMAGE_KHR;
    }
}

void hwc_egl_renderer_close(ScrnInfoPtr pScrn)
{
}
