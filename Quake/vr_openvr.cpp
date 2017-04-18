
#include "quakedef.h"
#include "vr.h"
#include "vr_menu.h"

#define UNICODE 1
#include <mmsystem.h>
#undef UNICODE

#include "openvr.h"

#if SDL_MAJOR_VERSION < 2
FILE *__iob_func() {
    FILE result[3] = { *stdin,*stdout,*stderr };
    return result;
}
#endif

extern void VID_Refocus();

typedef struct {
    GLuint framebuffer, depth_texture;
    //ovrTextureSwapChain swap_chain;
    struct {
        float width, height;
    } size;
} fbo_t;

typedef struct {
    int index;
    fbo_t fbo;
    //ovrEyeRenderDesc render_desc;
    //ovrPosef pose;
    float fov_x, fov_y;
} vr_eye_t;

// OpenGL Extensions
#define GL_READ_FRAMEBUFFER_EXT 0x8CA8
#define GL_DRAW_FRAMEBUFFER_EXT 0x8CA9
#define GL_FRAMEBUFFER_SRGB_EXT 0x8DB9

typedef void (APIENTRYP PFNGLBLITFRAMEBUFFEREXTPROC) (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
typedef BOOL(APIENTRYP PFNWGLSWAPINTERVALEXTPROC) (int);

static PFNGLBINDFRAMEBUFFEREXTPROC glBindFramebufferEXT;
static PFNGLBLITFRAMEBUFFEREXTPROC glBlitFramebufferEXT;
static PFNGLDELETEFRAMEBUFFERSEXTPROC glDeleteFramebuffersEXT;
static PFNGLGENFRAMEBUFFERSEXTPROC glGenFramebuffersEXT;
static PFNGLFRAMEBUFFERTEXTURE2DEXTPROC glFramebufferTexture2DEXT;
static PFNGLFRAMEBUFFERRENDERBUFFEREXTPROC glFramebufferRenderbufferEXT;
static PFNWGLSWAPINTERVALEXTPROC wglSwapIntervalEXT;

struct {
    void *func; char *name;
} gl_extensions[] = {
    { &glBindFramebufferEXT, "glBindFramebufferEXT" },
    { &glBlitFramebufferEXT, "glBlitFramebufferEXT" },
    { &glDeleteFramebuffersEXT, "glDeleteFramebuffersEXT" },
    { &glGenFramebuffersEXT, "glGenFramebuffersEXT" },
    { &glFramebufferTexture2DEXT, "glFramebufferTexture2DEXT" },
    { &glFramebufferRenderbufferEXT, "glFramebufferRenderbufferEXT" },
    { &wglSwapIntervalEXT, "wglSwapIntervalEXT" },
    { NULL, NULL },
};

// main screen & 2D drawing
extern void SCR_SetUpToDrawConsole(void);
extern void SCR_UpdateScreenContent();
extern qboolean	scr_drawdialog;
extern void SCR_DrawNotifyString(void);
extern qboolean	scr_drawloading;
extern void SCR_DrawLoading(void);
extern void SCR_CheckDrawCenterString(void);
extern void SCR_DrawRam(void);
extern void SCR_DrawNet(void);
extern void SCR_DrawTurtle(void);
extern void SCR_DrawPause(void);
extern void SCR_DrawDevStats(void);
extern void SCR_DrawFPS(void);
extern void SCR_DrawClock(void);
extern void SCR_DrawConsole(void);

// rendering
extern void R_SetupView(void);
extern void R_RenderScene(void);
extern int glx, gly, glwidth, glheight;
extern refdef_t r_refdef;
extern vec3_t vright;

vr::IVRSystem *ovr_SYSTEM;

static vr::Hmd_Eye eyes[2];
static vr::Hmd_Eye *current_eye = NULL;

static qboolean vr_initialized = false;
static GLuint mirror_fbo = 0;
static int attempt_to_refocus_retry = 0;

cvar_t vr_enabled = { "vr_enabled", "0", CVAR_NONE };
cvar_t vr_crosshair = { "vr_crosshair","1", CVAR_ARCHIVE };
cvar_t vr_crosshair_depth = { "vr_crosshair_depth","0", CVAR_ARCHIVE };
cvar_t vr_crosshair_size = { "vr_crosshair_size","3.0", CVAR_ARCHIVE };
cvar_t vr_crosshair_alpha = { "vr_crosshair_alpha","0.25", CVAR_ARCHIVE };
cvar_t vr_aimmode = { "vr_aimmode","1", CVAR_ARCHIVE };
cvar_t vr_deadzone = { "vr_deadzone","30",CVAR_ARCHIVE };
cvar_t vr_perfhud = { "vr_perfhud", "0", CVAR_ARCHIVE };


static qboolean InitOpenGLExtensions()
{
    int i;
    static qboolean extensions_initialized;

    if (extensions_initialized)
        return true;

    for (i = 0; gl_extensions[i].func; i++) {
        void *func = SDL_GL_GetProcAddress(gl_extensions[i].name);
        if (!func)
            return false;

        *((void **)gl_extensions[i].func) = func;
    }

    extensions_initialized = true;
    return extensions_initialized;
}


// ----------------------------------------------------------------------------
// Callbacks for cvars

static void VR_Enabled_f(cvar_t *var)
{
    VR_Disable();

    if (!vr_enabled.value)
        return;

    if (!VR_Enable())
        Cvar_SetValueQuick(&vr_enabled, 0);
}



static void VR_Deadzone_f(cvar_t *var)
{
    // clamp the mouse to a max of 0 - 70 degrees
    float deadzone = CLAMP(0.0f, vr_deadzone.value, 70.0f);
    if (deadzone != vr_deadzone.value)
        Cvar_SetValueQuick(&vr_deadzone, deadzone);
}



// ----------------------------------------------------------------------------
// Public vars and functions

void VR_Init()
{
    // This is only called once at game start
    Cvar_RegisterVariable(&vr_enabled);
    Cvar_SetCallback(&vr_enabled, VR_Enabled_f);
    Cvar_RegisterVariable(&vr_crosshair);
    Cvar_RegisterVariable(&vr_crosshair_depth);
    Cvar_RegisterVariable(&vr_crosshair_size);
    Cvar_RegisterVariable(&vr_crosshair_alpha);
    Cvar_RegisterVariable(&vr_aimmode);
    Cvar_RegisterVariable(&vr_deadzone);
    Cvar_SetCallback(&vr_deadzone, VR_Deadzone_f);
    Cvar_RegisterVariable(&vr_perfhud);

    VR_Menu_Init();

    // Set the cvar if invoked from a command line parameter
    {
        int i = COM_CheckParm("-vr");
        if (i && i < com_argc - 1) {
            Cvar_SetQuick(&vr_enabled, "1");
        }
    }
}



qboolean VR_Enable()
{
    int i;
    int mirror_texture_id = 0;
    UINT ovr_audio_id;

    vr::EVRInitError eInit = vr::VRInitError_None;
    ovr_SYSTEM = vr::VR_Init(&eInit, vr::VRApplication_Scene);

    if (eInit != vr::VRInitError_None) {
        Con_Printf("%s\nFailed to Initialize Steam VR", vr::VR_GetVRInitErrorAsEnglishDescription(eInit));
        return false;
    }

    if (!InitOpenGLExtensions()) {
        Con_Printf("Failed to initialize OpenGL extensions");
        return false;
    }

    wglSwapIntervalEXT(0); // Disable V-Sync

    attempt_to_refocus_retry = 900; // Try to refocus our for the first 900 frames :/
    vr_initialized = true;
    return true;
}


void VR_Shutdown() {
    VR_Disable();
}

void VR_Disable()
{
    int i;
    if (!vr_initialized)
        return;

    vr::VR_Shutdown();
    ovr_SYSTEM = NULL;

    // TODO: Cleanup frame buffers

    vr_initialized = false;
}

static void RenderScreenForCurrentEye_OVR()
{
    int swap_index = 0;
    int swap_texture_id = 0;

    // Remember the current glwidht/height; we have to modify it here for each eye
    int oldglheight = glheight;
    int oldglwidth = glwidth;

    uint32_t vrwidth, vrheight;
    
    ovr_SYSTEM->GetRecommendedRenderTargetSize(&vrwidth, &vrheight);

    glwidth = vrwidth;
    glheight = vrheight;

    // Set up current FBO
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, current_eye->fbo.framebuffer);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, swap_texture_id, 0);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_TEXTURE_2D, current_eye->fbo.depth_texture, 0);

    glViewport(0, 0, current_eye->fbo.size.width, current_eye->fbo.size.height);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);


    // Draw everything
    srand((int)(cl.time * 1000)); //sync random stuff between eyes

    r_refdef.fov_x = current_eye->fov_x;
    r_refdef.fov_y = current_eye->fov_y;

    SCR_UpdateScreenContent();
    vr::VRCompositor()->Submit(current_eye, );


    // Reset
    glwidth = oldglwidth;
    glheight = oldglheight;

    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_TEXTURE_2D, 0, 0);
}

void VR_UpdateScreenContent()
{


    // Last chance to enable VR Mode - we get here when the game already start up with vr_enabled 1
    // If enabling fails, unset the cvar and return.
    if (!vr_initialized && !VR_Enable()) {
        Cvar_Set("vr_enabled", "0");
        return;
    }
}