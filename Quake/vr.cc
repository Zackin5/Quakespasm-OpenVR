
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
    //vr::TextureID_t eye_texture;
    struct {
        float width, height;
    } size;
} fbo_t;

typedef struct {
    int index;
    fbo_t fbo;
    vr::Hmd_Eye eye;
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

vr::IVRSystem *ovrHMD;
vr::TrackedDevicePose_t ovr_DevicePose[vr::k_unMaxTrackedDeviceCount];

static vr_eye_t eyes[2];
static vr_eye_t *current_eye = NULL;
static vec3_t lastOrientation = { 0, 0, 0 };
static vec3_t lastAim = { 0, 0, 0 };

static qboolean vr_initialized = false;
static GLuint mirror_fbo = 0;
static int attempt_to_refocus_retry = 0;


// Wolfenstein 3D, DOOM and QUAKE use the same coordinate/unit system:
// 8 foot (96 inch) height wall == 64 units, 1.5 inches per pixel unit
// 1.0 pixel unit / 1.5 inch == 0.666666 pixel units per inch
static const float meters_to_units = 1.0f / (1.5f * 0.0254f);

extern cvar_t gl_farclip;
extern int glwidth, glheight;
extern void SCR_UpdateScreenContent();
extern refdef_t r_refdef;

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

fbo_t CreateFBO(int width, int height) {
    int i;
    fbo_t fbo;
    int swap_chain_length = 0;

    fbo.size.width = width;
    fbo.size.height = height;

    glGenFramebuffersEXT(1, &fbo.framebuffer);

    glGenTextures(1, &fbo.depth_texture);
    glBindTexture(GL_TEXTURE_2D, fbo.depth_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);

    return fbo;
}

void DeleteFBO(fbo_t fbo) {
    glDeleteFramebuffersEXT(1, &fbo.framebuffer);
    glDeleteTextures(1, &fbo.depth_texture);
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
    ovrHMD = vr::VR_Init(&eInit, vr::VRApplication_Scene);

    if (eInit != vr::VRInitError_None) {
        Con_Printf("%s\nFailed to Initialize Steam VR", vr::VR_GetVRInitErrorAsEnglishDescription(eInit));
        return false;
    }

    if (!InitOpenGLExtensions()) {
        Con_Printf("Failed to initialize OpenGL extensions");
        return false;
    }

    eyes[0].eye = vr::Eye_Left;
    eyes[1].eye = vr::Eye_Right;

    for (i = 0; i < 2; i++) {
        uint32_t vrwidth, vrheight;
        float LeftTan, RightTan, UpTan, DownTan;

        ovrHMD->GetRecommendedRenderTargetSize(&vrwidth, &vrheight);
        ovrHMD->GetProjectionRaw(eyes[0].eye, &LeftTan, &RightTan, &UpTan, &DownTan); // TODO: Not 100% sure these are actually tangent values

        eyes[i].index = i;
        eyes[i].fbo = CreateFBO(vrwidth, vrheight);
        eyes[i].fov_x = (atan(LeftTan) + atan(RightTan)) / M_PI_DIV_180;
        eyes[i].fov_y = (atan(UpTan) + atan(DownTan)) / M_PI_DIV_180;
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
    ovrHMD = NULL;

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

    ovrHMD->GetRecommendedRenderTargetSize(&vrwidth, &vrheight);

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

    vr::Texture_t eyeTexture = { reinterpret_cast<void*>(intptr_t(current_eye->fbo.framebuffer)), vr::TextureType_OpenGL, vr::ColorSpace_Gamma };

    vr::VRCompositor()->Submit(current_eye->eye, &eyeTexture);


    // Reset
    glwidth = oldglwidth;
    glheight = oldglheight;

    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_TEXTURE_2D, 0, 0);
}

void VR_UpdateScreenContent()
{
    int i;
    vec3_t orientation;

    GLint w, h;

    // Last chance to enable VR Mode - we get here when the game already start up with vr_enabled 1
    // If enabling fails, unset the cvar and return.
    if (!vr_initialized && !VR_Enable()) {
        Cvar_Set("vr_enabled", "0");
        return;
    }

    vr::VRCompositor()->WaitGetPoses(ovr_DevicePose, vr::k_unMaxTrackedDeviceCount, NULL, 0);

    // Render the scene for each eye into their FBOs
    for (i = 0; i < 2; i++) {
        current_eye = &eyes[i];
        RenderScreenForCurrentEye_OVR();
    }

}

void VR_ShowCrosshair()
{
    vec3_t forward, up, right;
    vec3_t start, end, impact;
    float size, alpha;

    if ((sv_player && (int)(sv_player->v.weapon) == IT_AXE))
        return;

    size = CLAMP(0.0, vr_crosshair_size.value, 32.0);
    alpha = CLAMP(0.0, vr_crosshair_alpha.value, 1.0);

    if (size <= 0 || alpha <= 0)
        return;

    // setup gl
    glDisable(GL_DEPTH_TEST);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    GL_PolygonOffset(OFFSET_SHOWTRIS);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_CULL_FACE);

    // calc the line and draw
    VectorCopy(cl.viewent.origin, start);
    start[2] -= cl.viewheight - 10;
    AngleVectors(cl.aimangles, forward, right, up);

    switch ((int)vr_crosshair.value)
    {
    default:
    case VR_CROSSHAIR_POINT:
        if (vr_crosshair_depth.value <= 0) {
            // trace to first wall
            VectorMA(start, 4096, forward, end);
            TraceLine(start, end, impact);
        }
        else {
            // fix crosshair to specific depth
            VectorMA(start, vr_crosshair_depth.value * meters_to_units, forward, impact);
        }

        glEnable(GL_POINT_SMOOTH);
        glColor4f(1, 0, 0, alpha);
        glPointSize(size * glwidth / vid.width);

        glBegin(GL_POINTS);
        glVertex3f(impact[0], impact[1], impact[2]);
        glEnd();
        glDisable(GL_POINT_SMOOTH);
        break;

    case VR_CROSSHAIR_LINE:
        // trace to first entity
        VectorMA(start, 4096, forward, end);
        TraceLineToEntity(start, end, impact, sv_player);

        glColor4f(1, 0, 0, alpha);
        glLineWidth(size * glwidth / vid.width);
        glBegin(GL_LINES);
        glVertex3f(start[0], start[1], start[2]);
        glVertex3f(impact[0], impact[1], impact[2]);
        glEnd();
        break;
    }

    // cleanup gl
    glColor3f(1, 1, 1);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    GL_PolygonOffset(OFFSET_NONE);
    glEnable(GL_DEPTH_TEST);
}

void VR_Draw2D()
{
    qboolean draw_sbar = false;
    vec3_t menu_angles, forward, right, up, target;
    float scale_hud = 0.13;

    int oldglwidth = glwidth,
        oldglheight = glheight,
        oldconwidth = vid.conwidth,
        oldconheight = vid.conheight;

    glwidth = 320;
    glheight = 200;

    vid.conwidth = 320;
    vid.conheight = 200;

    // draw 2d elements 1m from the users face, centered
    glPushMatrix();
    glDisable(GL_DEPTH_TEST); // prevents drawing sprites on sprites from interferring with one another
    glEnable(GL_BLEND);

    VectorCopy(r_refdef.aimangles, menu_angles)

        if (vr_aimmode.value == VR_AIMMODE_HEAD_MYAW || vr_aimmode.value == VR_AIMMODE_HEAD_MYAW_MPITCH)
            menu_angles[PITCH] = 0;

    AngleVectors(menu_angles, forward, right, up);

    VectorMA(r_refdef.vieworg, 48, forward, target);

    glTranslatef(target[0], target[1], target[2]);
    glRotatef(menu_angles[YAW] - 90, 0, 0, 1); // rotate around z
    glRotatef(90 + menu_angles[PITCH], -1, 0, 0); // keep bar at constant angled pitch towards user
    glTranslatef(-(320.0 * scale_hud / 2), -(200.0 * scale_hud / 2), 0); // center the status bar
    glScalef(scale_hud, scale_hud, scale_hud);


    if (scr_drawdialog) //new game confirm
    {
        if (con_forcedup)
            Draw_ConsoleBackground();
        else
            draw_sbar = true; //Sbar_Draw ();
        Draw_FadeScreen();
        SCR_DrawNotifyString();
    }
    else if (scr_drawloading) //loading
    {
        SCR_DrawLoading();
        draw_sbar = true; //Sbar_Draw ();
    }
    else if (cl.intermission == 1 && key_dest == key_game) //end of level
    {
        Sbar_IntermissionOverlay();
    }
    else if (cl.intermission == 2 && key_dest == key_game) //end of episode
    {
        Sbar_FinaleOverlay();
        SCR_CheckDrawCenterString();
    }
    else
    {
        //SCR_DrawCrosshair (); //johnfitz
        SCR_DrawRam();
        SCR_DrawNet();
        SCR_DrawTurtle();
        SCR_DrawPause();
        SCR_CheckDrawCenterString();
        draw_sbar = true; //Sbar_Draw ();
        SCR_DrawDevStats(); //johnfitz
        SCR_DrawFPS(); //johnfitz
        SCR_DrawClock(); //johnfitz
        SCR_DrawConsole();
        M_Draw();
    }

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glPopMatrix();

    if (draw_sbar)
        VR_DrawSbar();

    glwidth = oldglwidth;
    glheight = oldglheight;
    vid.conwidth = oldconwidth;
    vid.conheight = oldconheight;
}

void VR_DrawSbar()
{
    vec3_t sbar_angles, forward, right, up, target;
    float scale_hud = 0.025;

    glPushMatrix();
    glDisable(GL_DEPTH_TEST); // prevents drawing sprites on sprites from interferring with one another

    VectorCopy(cl.aimangles, sbar_angles)

        if (vr_aimmode.value == VR_AIMMODE_HEAD_MYAW || vr_aimmode.value == VR_AIMMODE_HEAD_MYAW_MPITCH)
            sbar_angles[PITCH] = 0;

    AngleVectors(sbar_angles, forward, right, up);

    VectorMA(cl.viewent.origin, 1.0, forward, target);

    glTranslatef(target[0], target[1], target[2]);
    glRotatef(sbar_angles[YAW] - 90, 0, 0, 1); // rotate around z
    glRotatef(90 + 45 + sbar_angles[PITCH], -1, 0, 0); // keep bar at constant angled pitch towards user
    glTranslatef(-(320.0 * scale_hud / 2), 0, 0); // center the status bar
    glTranslatef(0, 0, 10); // move hud down a bit
    glScalef(scale_hud, scale_hud, scale_hud);

    Sbar_Draw();

    glEnable(GL_DEPTH_TEST);
    glPopMatrix();
}

void VR_SetAngles(vec3_t angles)
{
    VectorCopy(angles, cl.aimangles);
    VectorCopy(angles, cl.viewangles);
    VectorCopy(angles, lastAim);
}

void VR_ResetOrientation()
{
    cl.aimangles[YAW] = cl.viewangles[YAW];
    cl.aimangles[PITCH] = cl.viewangles[PITCH];
    if (vr_enabled.value) {
        ovrHMD->ResetSeatedZeroPose();
        VectorCopy(cl.aimangles, lastAim);
    }
}