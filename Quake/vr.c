#ifdef OCULUS_API
extern "C" {
#include "vr_oculus.c"
}
#endif

#ifdef OPENVR_API
#include "vr_openvr.cpp"
#endif