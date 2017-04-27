
#include "quakedef.h"
#include "vr.h"
#include "vr_menu.h"

#define UNICODE 1
#include <mmsystem.h>
#undef UNICODE

#include "openvr_c.h"

#if SDL_MAJOR_VERSION < 2
FILE *__iob_func() {
    FILE result[3] = { *stdin,*stdout,*stderr };
    return result;
}
#endif

extern void VID_Refocus();

typedef struct {
    GLuint framebuffer, depth_texture, texture;
    struct {
        float width, height;
    } size;
} fbo_t;

typedef struct {
    int index;
    fbo_t fbo;
    Hmd_Eye eye;
    HmdVector3_t position;
    HmdQuaternion_t orientation;
    float fov_x, fov_y;
} vr_eye_t;

typedef struct {
    HmdVector3_t position;
    HmdQuaternion_t orientation;
} vr_controller;

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


IVRSystem *ovrHMD;
TrackedDevicePose_t ovr_DevicePose[16]; //k_unMaxTrackedDeviceCount

static vr_eye_t eyes[2];
static vr_eye_t *current_eye = NULL;
static vr_controller controllers[2];
static vec3_t lastOrientation = { 0, 0, 0 };
static vec3_t lastAim = { 0, 0, 0 };

static qboolean vr_initialized = false;
static GLuint mirror_texture = 0;
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
cvar_t vr_viewkick = { "vr_viewkick", "0", CVAR_NONE };


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

    glGenTextures(1, &fbo.texture);
    glBindTexture(GL_TEXTURE_2D, fbo.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_INT, NULL);

    return fbo;
}

void DeleteFBO(fbo_t fbo) {
    glDeleteFramebuffersEXT(1, &fbo.framebuffer);
    glDeleteTextures(1, &fbo.depth_texture);
    glDeleteTextures(1, &fbo.texture);
}

void QuatToYawPitchRoll(HmdQuaternion_t q, vec3_t out) {
    float sqw = q.w*q.w;
    float sqx = q.x*q.x;
    float sqy = q.y*q.y;
    float sqz = q.z*q.z;
    float unit = sqx + sqy + sqz + sqw; // if normalised is one, otherwise is correction factor
    float test = q.x*q.y + q.z*q.w;
    if (test > 0.499*unit) { // singularity at north pole
        out[YAW] = 2 * atan2(q.x, q.w) / M_PI_DIV_180;
        out[ROLL] = -M_PI / 2 / M_PI_DIV_180;
        out[PITCH] = 0;
    }
    else if (test < -0.499*unit) { // singularity at south pole
        out[YAW] = -2 * atan2(q.x, q.w) / M_PI_DIV_180;
        out[ROLL] = M_PI / 2 / M_PI_DIV_180;
        out[PITCH] = 0;
    }
    else {
        out[YAW] = atan2(2 * q.y*q.w - 2 * q.x*q.z, sqx - sqy - sqz + sqw) / M_PI_DIV_180;
        out[ROLL] = -asin(2 * test / unit) / M_PI_DIV_180;
        out[PITCH] = -atan2(2 * q.x*q.w - 2 * q.y*q.z, -sqx + sqy - sqz + sqw) / M_PI_DIV_180;
    }
}

void Vec3RotateZ(vec3_t in, float angle, vec3_t out) {
    out[0] = in[0] * cos(angle) - in[1] * sin(angle);
    out[1] = in[0] * sin(angle) + in[1] * cos(angle);
    out[2] = in[2];
}

HmdMatrix44_t TransposeMatrix(HmdMatrix44_t in) {
    HmdMatrix44_t out;
    int y, x;
    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++)
            out.m[x][y] = in.m[y][x];

    return out;
}

HmdVector3_t AddVectors(HmdVector3_t a, HmdVector3_t b)
{
    HmdVector3_t out;

    out.v[0] = a.v[0] + b.v[0];
    out.v[1] = a.v[1] + b.v[1];
    out.v[2] = a.v[2] + b.v[2];

    return out;
}

// Rotates a vector by a quaternion and returns the results
// Based on math from https://gamedev.stackexchange.com/questions/28395/rotating-vector3-by-a-quaternion
HmdVector3_t RotateVectorByQuaternion(HmdVector3_t v, HmdQuaternion_t q)
{
    HmdVector3_t u, result;
    u.v[0] = q.x;
    u.v[1] = q.y;
    u.v[2] = q.z;
    float s = q.w;

    // Dot products of u,v and u,u
    float uvDot = (u.v[0] * v.v[0] + u.v[1] * v.v[1] + u.v[2] * v.v[2]);
    float uuDot = (u.v[0] * u.v[0] + u.v[1] * u.v[1] + u.v[2] * u.v[2]);

    // Calculate cross product of u, v
    HmdVector3_t uvCross;
    uvCross.v[0] = u.v[1] * v.v[2] - u.v[2] * v.v[1];
    uvCross.v[1] = u.v[2] * v.v[0] - u.v[0] * v.v[2];
    uvCross.v[2] = u.v[0] * v.v[1] - u.v[1] * v.v[0];
    
    // Calculate each vectors' result individually because there aren't arthimetic functions for HmdVector3_t dsahfkldhsaklfhklsadh
    result.v[0] = u.v[0] * 2.0f * uvDot
                + (s*s - uuDot) * v.v[0]
                + 2.0f * s * uvCross.v[0];
    result.v[1] = u.v[1] * 2.0f * uvDot
                + (s*s - uuDot) * v.v[1]
                + 2.0f * s * uvCross.v[1];
    result.v[2] = u.v[2] * 2.0f * uvDot
                + (s*s - uuDot) * v.v[2]
                + 2.0f * s * uvCross.v[2];

    return result;
}

// Multiplies quaternion a by quaternion b and returns the result
// Math borrowed from http://www.euclideanspace.com/maths/algebra/realNormedAlgebra/quaternions/code/
HmdQuaternion_t MultiplyQuaternion(HmdQuaternion_t a, HmdQuaternion_t b)
{
    HmdQuaternion_t final;

    final.x =  a.x * b.w + a.y * b.z - a.z * b.y + a.w * b.x;
    final.y = -a.x * b.z + a.y * b.w + a.z * b.x + a.w * b.y;
    final.z =  a.x * b.y - a.y * b.x + a.z * b.w + a.w * b.z;
    final.w = -a.x * b.x - a.y * b.y - a.z * b.z + a.w * b.w;

    return final;
}

// Transforms a HMD Matrix34 to a Vector3
// Math borrowed from https://github.com/Omnifinity/OpenVR-Tracking-Example
HmdVector3_t Matrix34ToVector(HmdMatrix34_t in) 
{
    HmdVector3_t vector;

    vector.v[0] = in.m[0][3];
    vector.v[1] = in.m[1][3];
    vector.v[2] = in.m[2][3];

    return vector;
}

// Transforms a HMD Matrix34 to a Quaternion
// Function logic nicked from https://github.com/Omnifinity/OpenVR-Tracking-Example
HmdQuaternion_t Matrix34ToQuaternion(HmdMatrix34_t in) 
{
    HmdQuaternion_t q;

    q.w = sqrt(fmax(0, 1 + in.m[0][0] + in.m[1][1] + in.m[2][2])) / 2;
    q.x = sqrt(fmax(0, 1 + in.m[0][0] - in.m[1][1] - in.m[2][2])) / 2;
    q.y = sqrt(fmax(0, 1 - in.m[0][0] + in.m[1][1] - in.m[2][2])) / 2;
    q.z = sqrt(fmax(0, 1 - in.m[0][0] - in.m[1][1] + in.m[2][2])) / 2;
    q.x = copysign(q.x, in.m[2][1] - in.m[1][2]);
    q.y = copysign(q.y, in.m[0][2] - in.m[2][0]);
    q.z = copysign(q.z, in.m[1][0] - in.m[0][1]);
    return q;
}


// ----------------------------------------------------------------------------
// Callbacks for cvars

static void VR_Enabled_f(cvar_t *var)
{
    VID_VR_Disable();

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

void VID_VR_Init()
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

    // Sickness stuff
    Cvar_RegisterVariable(&vr_viewkick);

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
    EVRInitError eInit = VRInitError_None;
    ovrHMD = VR_Init(&eInit, VRApplication_Scene);

    if (eInit != VRInitError_None) {
        Con_Printf("%s\nFailed to Initialize Steam VR", VR_GetVRInitErrorAsEnglishDescription(eInit));
        return false;
    }

    if (!InitOpenGLExtensions()) {
        Con_Printf("Failed to initialize OpenGL extensions");
        return false;
    }

    eyes[0].eye = Eye_Left;
    eyes[1].eye = Eye_Right;

    for (int i = 0; i < 2; i++) {
        uint32_t vrwidth, vrheight;
        float LeftTan, RightTan, UpTan, DownTan;

        IVRSystem_GetRecommendedRenderTargetSize(ovrHMD, &vrwidth, &vrheight);
        IVRSystem_GetProjectionRaw(ovrHMD, eyes[i].eye, &LeftTan, &RightTan, &UpTan, &DownTan);

        eyes[i].index = i;
        eyes[i].fbo = CreateFBO(vrwidth, vrheight);
        eyes[i].fov_x = (atan(-LeftTan) + atan(RightTan)) / M_PI_DIV_180;
        eyes[i].fov_y = (atan(-UpTan) + atan(DownTan)) / M_PI_DIV_180;
    }

    VR_SetTrackingSpace(0);    // Put us into seated tracking position
    VR_ResetOrientation();     // Recenter the HMD

    wglSwapIntervalEXT(0); // Disable V-Sync

    Cbuf_AddText ("exec vr_autoexec.cfg\n"); // Load the vr autosec config file incase the user has settings they want

    attempt_to_refocus_retry = 900; // Try to refocus our for the first 900 frames :/
    vr_initialized = true;
    return true;
}


void VID_VR_Shutdown() {
    VID_VR_Disable();
}

void VID_VR_Disable()
{
    int i;
    if (!vr_initialized)
        return;

    VR_Shutdown();
    ovrHMD = NULL;

    // TODO: Cleanup frame buffers

    vr_initialized = false;
}

static void RenderScreenForCurrentEye_OVR()
{
    // Remember the current glwidht/height; we have to modify it here for each eye
    int oldglheight = glheight;
    int oldglwidth = glwidth;

    glwidth = current_eye->fbo.size.width;
    glheight = current_eye->fbo.size.height;

    // Set up current FBO
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, current_eye->fbo.framebuffer);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, current_eye->fbo.texture, 0);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_TEXTURE_2D, current_eye->fbo.depth_texture, 0);

    glViewport(0, 0, current_eye->fbo.size.width, current_eye->fbo.size.height);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Draw everything
    srand((int)(cl.time * 1000)); //sync random stuff between eyes

    r_refdef.fov_x = current_eye->fov_x;
    r_refdef.fov_y = current_eye->fov_y;

    SCR_UpdateScreenContent();

    // Generate the eye texture and send it to the HMD
    Texture_t eyeTexture = { (void*)current_eye->fbo.texture, TextureType_OpenGL, ColorSpace_Gamma };
    IVRCompositor_Submit(VRCompositor(), current_eye->eye, &eyeTexture);
    

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
    vec3_t orientation, controller_orientation[2];
    GLint w, h;

    // Last chance to enable VR Mode - we get here when the game already start up with vr_enabled 1
    // If enabling fails, unset the cvar and return.
    if (!vr_initialized && !VR_Enable()) {
        Cvar_Set("vr_enabled", "0");
        return;
    }

    w = glwidth;
    h = glheight;

    // Update poses
    IVRCompositor_WaitGetPoses(VRCompositor(), ovr_DevicePose, k_unMaxTrackedDeviceCount, NULL, 0);

    // Get the VR devices' orientation and position
    for (int iDevice = 0; iDevice < k_unMaxTrackedDeviceCount; iDevice++)
    {
        // HMD vectors update
        if (ovr_DevicePose[iDevice].bPoseIsValid && IVRSystem_GetTrackedDeviceClass(ovrHMD, iDevice) == TrackedDeviceClass_HMD)
        {
            HmdVector3_t headPos = Matrix34ToVector(ovr_DevicePose->mDeviceToAbsoluteTracking);
            HmdQuaternion_t headQuat = Matrix34ToQuaternion(ovr_DevicePose->mDeviceToAbsoluteTracking);
            HmdVector3_t leyePos = Matrix34ToVector(IVRSystem_GetEyeToHeadTransform(ovrHMD, eyes[0].eye));
            HmdVector3_t reyePos = Matrix34ToVector(IVRSystem_GetEyeToHeadTransform(ovrHMD, eyes[1].eye));

            leyePos = RotateVectorByQuaternion(leyePos, headQuat);
            reyePos = RotateVectorByQuaternion(reyePos, headQuat);

            eyes[0].position = AddVectors(headPos, leyePos);
            eyes[1].position = AddVectors(headPos, reyePos);
            eyes[0].orientation = headQuat;
            eyes[1].orientation = headQuat;
        }
        // Controller vectors update
        else if (ovr_DevicePose[iDevice].bPoseIsValid && IVRSystem_GetTrackedDeviceClass(ovrHMD, iDevice) == TrackedDeviceClass_Controller)
        {
            // TODO: Verify controller tracking logic
            if (IVRSystem_GetControllerRoleForTrackedDeviceIndex(ovrHMD, iDevice) == TrackedControllerRole_LeftHand)
            {
                controllers[0].position = Matrix34ToVector(ovr_DevicePose->mDeviceToAbsoluteTracking);
                controllers[0].orientation = Matrix34ToQuaternion(ovr_DevicePose->mDeviceToAbsoluteTracking);
            }
            else if (IVRSystem_GetControllerRoleForTrackedDeviceIndex(ovrHMD, iDevice) == TrackedControllerRole_RightHand)
            {
                controllers[1].position = Matrix34ToVector(ovr_DevicePose->mDeviceToAbsoluteTracking);
                controllers[1].orientation = Matrix34ToQuaternion(ovr_DevicePose->mDeviceToAbsoluteTracking);
            }
        }
    }

    QuatToYawPitchRoll(eyes[1].orientation, orientation);
    switch ((int)vr_aimmode.value)
    {
        // 1: (Default) Head Aiming; View YAW is mouse+head, PITCH is head
    default:
    case VR_AIMMODE_HEAD_MYAW:
        cl.viewangles[PITCH] = cl.aimangles[PITCH] = orientation[PITCH];
        cl.aimangles[YAW] = cl.viewangles[YAW] = cl.aimangles[YAW] + orientation[YAW] - lastOrientation[YAW];
        break;

        // 2: Head Aiming; View YAW and PITCH is mouse+head (this is stupid)
    case VR_AIMMODE_HEAD_MYAW_MPITCH:
        cl.viewangles[PITCH] = cl.aimangles[PITCH] = cl.aimangles[PITCH] + orientation[PITCH] - lastOrientation[PITCH];
        cl.aimangles[YAW] = cl.viewangles[YAW] = cl.aimangles[YAW] + orientation[YAW] - lastOrientation[YAW];
        break;

        // 3: Mouse Aiming; View YAW is mouse+head, PITCH is head
    case VR_AIMMODE_MOUSE_MYAW:
        cl.viewangles[PITCH] = orientation[PITCH];
        cl.viewangles[YAW] = cl.aimangles[YAW] + orientation[YAW];
        break;

        // 4: Mouse Aiming; View YAW and PITCH is mouse+head
    case VR_AIMMODE_MOUSE_MYAW_MPITCH:
        cl.viewangles[PITCH] = cl.aimangles[PITCH] + orientation[PITCH];
        cl.viewangles[YAW] = cl.aimangles[YAW] + orientation[YAW];
        break;

    case VR_AIMMODE_BLENDED:
    case VR_AIMMODE_BLENDED_NOPITCH:
    {
        float diffHMDYaw = orientation[YAW] - lastOrientation[YAW];
        float diffHMDPitch = orientation[PITCH] - lastOrientation[PITCH];
        float diffAimYaw = cl.aimangles[YAW] - lastAim[YAW];
        float diffYaw;

        // find new view position based on orientation delta
        cl.viewangles[YAW] += diffHMDYaw;

        // find difference between view and aim yaw
        diffYaw = cl.viewangles[YAW] - cl.aimangles[YAW];

        if (abs(diffYaw) > vr_deadzone.value / 2.0f)
        {
            // apply the difference from each set of angles to the other
            cl.aimangles[YAW] += diffHMDYaw;
            cl.viewangles[YAW] += diffAimYaw;
        }
        if ((int)vr_aimmode.value == VR_AIMMODE_BLENDED) {
            cl.aimangles[PITCH] += diffHMDPitch;
        }
        cl.viewangles[PITCH] = orientation[PITCH];
    }
    break;

        // 7: Controller Aiming
    case VR_AIMMODE_CONTROLLER:

        // TODO: verify logic
        cl.viewangles[PITCH] = orientation[PITCH];
        cl.viewangles[YAW] = orientation[YAW];

        QuatToYawPitchRoll(controllers[0].orientation, controller_orientation[0]);
        QuatToYawPitchRoll(controllers[1].orientation, controller_orientation[1]);

        cl.aimangles[PITCH] = controller_orientation[1][PITCH];
        cl.aimangles[YAW] = controller_orientation[1][YAW];
        break;
    }
    cl.viewangles[ROLL] = orientation[ROLL];

    VectorCopy(orientation, lastOrientation);
    VectorCopy(cl.aimangles, lastAim);

    VectorCopy(cl.viewangles, r_refdef.viewangles);
    VectorCopy(cl.aimangles, r_refdef.aimangles);

    // Render the scene for each eye into their FBOs
    for (i = 0; i < 2; i++) {
        current_eye = &eyes[i];
        RenderScreenForCurrentEye_OVR();
    }

    // Blit mirror texture to backbuffer
    glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, eyes[0].fbo.framebuffer);
    glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, 0);
    glBlitFramebufferEXT(0, eyes[0].fbo.size.height, eyes[0].fbo.size.width, 0, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, 0);
}

void VR_SetMatrices() {
    vec3_t temp, orientation, position;
    HmdMatrix44_t projection;

    // Calculate HMD projection matrix and view offset position
    projection = TransposeMatrix(IVRSystem_GetProjectionMatrix(ovrHMD, current_eye->eye, 4.f, gl_farclip.value));

    // We need to scale the view offset position to quake units and rotate it by the current input angles (viewangle - eye orientation)
    QuatToYawPitchRoll(current_eye->orientation, orientation);
    temp[0] = -current_eye->position.v[2] * meters_to_units; // X
    temp[1] = -current_eye->position.v[0] * meters_to_units; // Y
    temp[2] = current_eye->position.v[1] * meters_to_units;  // Z
    Vec3RotateZ(temp, (r_refdef.viewangles[YAW] - orientation[YAW])*M_PI_DIV_180, position);


    // Set OpenGL projection and view matrices
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf((GLfloat*)projection.m);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glRotatef(-90, 1, 0, 0); // put Z going up
    glRotatef(90, 0, 0, 1); // put Z going up

    glRotatef(-r_refdef.viewangles[PITCH], 0, 1, 0);
    glRotatef(-r_refdef.viewangles[ROLL], 1, 0, 0);
    glRotatef(-r_refdef.viewangles[YAW], 0, 0, 1);

    glTranslatef(-r_refdef.vieworg[0] - position[0], -r_refdef.vieworg[1] - position[1], -r_refdef.vieworg[2] - position[2]);
}

void VR_AddOrientationToViewAngles(vec3_t angles)
{
    vec3_t orientation;
    QuatToYawPitchRoll(current_eye->orientation, orientation);

    angles[PITCH] = angles[PITCH] + orientation[PITCH];
    angles[YAW] = angles[YAW] + orientation[YAW];
    angles[ROLL] = orientation[ROLL];
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

    // TODO: bind to controller logic

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
        IVRSystem_ResetSeatedZeroPose(ovrHMD);
        VectorCopy(cl.aimangles, lastAim);
    }
}

void VR_SetTrackingSpace(int n)
{
    if ( n >= 0 || n < 3 )
        IVRCompositor_SetTrackingSpace(VRCompositor(), n);
}