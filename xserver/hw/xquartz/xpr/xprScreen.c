/*
 * Xplugin rootless implementation screen functions
 *
 * Copyright (c) 2002 Apple Computer, Inc. All Rights Reserved.
 * Copyright (c) 2004 Torrey T. Lyons. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE ABOVE LISTED COPYRIGHT HOLDER(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Except as contained in this notice, the name(s) of the above copyright
 * holders shall not be used in advertising or otherwise to promote the sale,
 * use or other dealings in this Software without prior written authorization.
 */

#include "sanitizedCarbon.h"

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include "quartzCommon.h"
#include "inputstr.h"
#include "quartz.h"
#include "xpr.h"
#include "xprEvent.h"
#include "pseudoramiX.h"
#include "darwin.h"
#include "darwinEvents.h"
#include "rootless.h"
#include "dri.h"
#include "globals.h"
#include <Xplugin.h>
#include "applewmExt.h"
#include "micmap.h"

#ifdef DAMAGE
# include "damage.h"
#endif

/* 10.4's deferred update makes X slower.. have to live with the tearing
   for now.. */
#define XP_NO_DEFERRED_UPDATES 8

// Name of GLX bundle for native OpenGL
static const char *xprOpenGLBundle = "glxCGL.bundle";

/*
 * eventHandler
 *  Callback handler for Xplugin events.
 */
static void eventHandler(unsigned int type, const void *arg,
                         unsigned int arg_size, void *data) {
    
    switch (type) {
        case XP_EVENT_DISPLAY_CHANGED:
            DEBUG_LOG("XP_EVENT_DISPLAY_CHANGED\n");
            DarwinSendDDXEvent(kXquartzDisplayChanged, 0);
            break;
            
        case XP_EVENT_WINDOW_STATE_CHANGED:
            if (arg_size >= sizeof(xp_window_state_event)) {
                const xp_window_state_event *ws_arg = arg;
                
                DEBUG_LOG("XP_EVENT_WINDOW_STATE_CHANGED: id=%d, state=%d\n", ws_arg->id, ws_arg->state);
                DarwinSendDDXEvent(kXquartzWindowState, 2,
                                          ws_arg->id, ws_arg->state);
            } else {
                DEBUG_LOG("XP_EVENT_WINDOW_STATE_CHANGED: ignored\n");
            }
            break;
            
        case XP_EVENT_WINDOW_MOVED:
            DEBUG_LOG("XP_EVENT_WINDOW_MOVED\n");
            if (arg_size == sizeof(xp_window_id))  {
                xp_window_id id = * (xp_window_id *) arg;
                DarwinSendDDXEvent(kXquartzWindowMoved, 1, id);
            }
            break;
            
        case XP_EVENT_SURFACE_DESTROYED:
            DEBUG_LOG("XP_EVENT_SURFACE_DESTROYED\n");
        case XP_EVENT_SURFACE_CHANGED:
            DEBUG_LOG("XP_EVENT_SURFACE_CHANGED\n");
            if (arg_size == sizeof(xp_surface_id)) {
                int kind;
                
                if (type == XP_EVENT_SURFACE_DESTROYED)
                    kind = AppleDRISurfaceNotifyDestroyed;
                else
                    kind = AppleDRISurfaceNotifyChanged;
                
                DRISurfaceNotify(*(xp_surface_id *) arg, kind);
            }
            break;
#ifdef XP_EVENT_SPACE_CHANGED
        case  XP_EVENT_SPACE_CHANGED:
            DEBUG_LOG("XP_EVENT_SPACE_CHANGED\n");
            if(arg_size == sizeof(uint32_t)) {
                uint32_t space_id = *(uint32_t *)arg;
                DarwinSendDDXEvent(kXquartzSpaceChanged, 1, space_id);
            }
            break;
#endif
        default:
            ErrorF("Unknown XP_EVENT type (%d) in xprScreen:eventHandler\n", type);
    }
}

/*
 * displayAtIndex
 *  Return the display ID for a particular display index.
 */
static CGDirectDisplayID
displayAtIndex(int index)
{
    CGError err;
    CGDisplayCount cnt;
    CGDirectDisplayID dpy[index+1];

    err = CGGetActiveDisplayList(index + 1, dpy, &cnt);
    if (err == kCGErrorSuccess && cnt == index + 1)
        return dpy[index];
    else
        return kCGNullDirectDisplay;
}

/*
 * displayScreenBounds
 *  Return the bounds of a particular display.
 */
static CGRect
displayScreenBounds(CGDirectDisplayID id)
{
    CGRect frame;

    frame = CGDisplayBounds(id);

    DEBUG_LOG("    %dx%d @ (%d,%d).\n",
              (int)frame.size.width, (int)frame.size.height,
              (int)frame.origin.x, (int)frame.origin.y);
    
    /* Remove menubar to help standard X11 window managers. */
    if (quartzEnableRootless && 
        frame.origin.x == 0 && frame.origin.y == 0) {
        frame.origin.y += aquaMenuBarHeight;
        frame.size.height -= aquaMenuBarHeight;
    }

    DEBUG_LOG("    %dx%d @ (%d,%d).\n",
              (int)frame.size.width, (int)frame.size.height,
              (int)frame.origin.x, (int)frame.origin.y);

    return frame;
}

/*
 * xprAddPseudoramiXScreens
 *  Add a single virtual screen encompassing all the physical screens
 *  with PseudoramiX.
 */
static void
xprAddPseudoramiXScreens(int *x, int *y, int *width, int *height)
{
    CGDisplayCount i, displayCount;
    CGDirectDisplayID *displayList = NULL;
    CGRect unionRect = CGRectNull, frame;

    // Find all the CoreGraphics displays
    CGGetActiveDisplayList(0, NULL, &displayCount);
    displayList = xalloc(displayCount * sizeof(CGDirectDisplayID));
    CGGetActiveDisplayList(displayCount, displayList, &displayCount);

    /* Get the union of all screens */
    for (i = 0; i < displayCount; i++) {
        CGDirectDisplayID dpy = displayList[i];
        frame = displayScreenBounds(dpy);
        unionRect = CGRectUnion(unionRect, frame);
    }

    /* Use unionRect as the screen size for the X server. */
    *x = unionRect.origin.x;
    *y = unionRect.origin.y;
    *width = unionRect.size.width;
    *height = unionRect.size.height;

    DEBUG_LOG("  screen union origin: (%d,%d) size: (%d,%d).\n",
              *x, *y, *width, *height);

    /* Tell PseudoramiX about the real screens. */
    for (i = 0; i < displayCount; i++)
    {
        CGDirectDisplayID dpy = displayList[i];

        frame = displayScreenBounds(dpy);
        frame.origin.x -= unionRect.origin.x;
        frame.origin.y -= unionRect.origin.y;

        DEBUG_LOG("    placed at X11 coordinate (%d,%d).\n",
                  (int)frame.origin.x, (int)frame.origin.y);

        PseudoramiXAddScreen(frame.origin.x, frame.origin.y,
                             frame.size.width, frame.size.height);
    }

    xfree(displayList);
}

/*
 * xprDisplayInit
 *  Find number of CoreGraphics displays and initialize Xplugin.
 */
static void
xprDisplayInit(void)
{
    CGDisplayCount displayCount;

    DEBUG_LOG("");

    CGGetActiveDisplayList(0, NULL, &displayCount);

    /* With PseudoramiX, the X server only sees one screen; only PseudoramiX
       itself knows about all of the screens. */

    if (noPseudoramiXExtension)
        darwinScreensFound = displayCount;
    else
        darwinScreensFound =  1;

    if (xp_init(XP_BACKGROUND_EVENTS | XP_NO_DEFERRED_UPDATES) != Success)
        FatalError("Could not initialize the Xplugin library.");

    xp_select_events(XP_EVENT_DISPLAY_CHANGED
                     | XP_EVENT_WINDOW_STATE_CHANGED
                     | XP_EVENT_WINDOW_MOVED
#ifdef XP_EVENT_SPACE_CHANGED
                     | XP_EVENT_SPACE_CHANGED
#endif
                     | XP_EVENT_SURFACE_CHANGED
                     | XP_EVENT_SURFACE_DESTROYED,
                     eventHandler, NULL);

    AppleDRIExtensionInit();
    xprAppleWMInit();
}

/*
 * xprAddScreen
 *  Init the framebuffer and record pixmap parameters for the screen.
 */
static Bool
xprAddScreen(int index, ScreenPtr pScreen)
{
    DarwinFramebufferPtr dfb = SCREEN_PRIV(pScreen);
    int depth = darwinDesiredDepth;

    DEBUG_LOG("index=%d depth=%d\n", index, depth);
    
    if(depth == -1) {
        depth = CGDisplaySamplesPerPixel(kCGDirectMainDisplay) * CGDisplayBitsPerSample(kCGDirectMainDisplay);
        //dfb->depth = CGDisplaySamplesPerPixel(kCGDirectMainDisplay) * CGDisplayBitsPerSample(kCGDirectMainDisplay);
        //dfb->bitsPerRGB = CGDisplayBitsPerSample(kCGDirectMainDisplay);
        //dfb->bitsPerPixel = CGDisplayBitsPerPixel(kCGDirectMainDisplay);
    }
    
    switch(depth) {
//        case -8: // broken
//            dfb->visuals = (1 << StaticGray) | (1 << GrayScale);
//            dfb->preferredCVC = GrayScale;
//            dfb->depth = 8;
//            dfb->bitsPerRGB = 8;
//            dfb->bitsPerPixel = 8;
//            dfb->redMask = 0;
//            dfb->greenMask = 0;
//            dfb->blueMask = 0;
//            break;
        case 8: // pseudo-working
            dfb->visuals = PseudoColorMask;
            dfb->preferredCVC = PseudoColor;
            dfb->depth = 8;
            dfb->bitsPerRGB = 8;
            dfb->bitsPerPixel = 8;
            dfb->redMask = 0;
            dfb->greenMask = 0;
            dfb->blueMask = 0;
            break;
        case 15:
            dfb->visuals = LARGE_VISUALS;
            dfb->preferredCVC = TrueColor;
            dfb->depth = 15;
            dfb->bitsPerRGB = 5;
            dfb->bitsPerPixel = 16;
            dfb->redMask   = 0x7c00;
            dfb->greenMask = 0x03e0;
            dfb->blueMask  = 0x001f;
            break;
//        case 24:
        default:
            if(depth != 24)
                ErrorF("Unsupported color depth requested.  Defaulting to 24bit. (depth=%d darwinDesiredDepth=%d CGDisplaySamplesPerPixel=%d CGDisplayBitsPerSample=%d)\n",  darwinDesiredDepth, depth, (int)CGDisplaySamplesPerPixel(kCGDirectMainDisplay), (int)CGDisplayBitsPerSample(kCGDirectMainDisplay));
            dfb->visuals = LARGE_VISUALS;
            dfb->preferredCVC = TrueColor;
            dfb->depth = 24;
            dfb->bitsPerRGB = 8;
            dfb->bitsPerPixel = 32;
            dfb->redMask   = 0x00ff0000;
            dfb->greenMask = 0x0000ff00;
            dfb->blueMask  = 0x000000ff;
            break;
    }

    if (noPseudoramiXExtension)
    {
        ErrorF("Warning: noPseudoramiXExtension!\n");
        
        CGDirectDisplayID dpy;
        CGRect frame;

        dpy = displayAtIndex(index);

        frame = displayScreenBounds(dpy);

        dfb->x = frame.origin.x;
        dfb->y = frame.origin.y;
        dfb->width =  frame.size.width;
        dfb->height = frame.size.height;
    }
    else
    {
        xprAddPseudoramiXScreens(&dfb->x, &dfb->y, &dfb->width, &dfb->height);
    }

    /* Passing zero width (pitch) makes miCreateScreenResources set the
       screen pixmap to the framebuffer pointer, i.e. NULL. The generic
       rootless code takes care of making this work. */
    dfb->pitch = 0;
    dfb->framebuffer = NULL;

    DRIScreenInit(pScreen);

    return TRUE;
}

/*
 * xprSetupScreen
 *  Setup the screen for rootless access.
 */
static Bool
xprSetupScreen(int index, ScreenPtr pScreen)
{
    // Initialize accelerated rootless drawing
    // Note that this must be done before DamageSetup().

    // These are crashing ugly... better to be stable and not crash for now.
    //RootlessAccelInit(pScreen);

#ifdef DAMAGE
    // The Damage extension needs to wrap underneath the
    // generic rootless layer, so do it now.
    if (!DamageSetup(pScreen))
        return FALSE;
#endif

    // Initialize generic rootless code
    if (!xprInit(pScreen))
        return FALSE;

    return DRIFinishScreenInit(pScreen);
}

/*
 * xprUpdateScreen
 *  Update screen after configuation change.
 */
static void
xprUpdateScreen(ScreenPtr pScreen)
{
    rootlessGlobalOffsetX = darwinMainScreenX;
    rootlessGlobalOffsetY = darwinMainScreenY;

    AppleWMSetScreenOrigin(WindowTable[pScreen->myNum]);

    RootlessRepositionWindows(pScreen);
    RootlessUpdateScreenPixmap(pScreen);
}

/*
 * xprInitInput
 *  Finalize xpr specific setup.
 */
static void
xprInitInput(int argc, char **argv)
{
    int i;

    rootlessGlobalOffsetX = darwinMainScreenX;
    rootlessGlobalOffsetY = darwinMainScreenY;

    for (i = 0; i < screenInfo.numScreens; i++)
        AppleWMSetScreenOrigin(WindowTable[i]);
}

/*
 * Quartz display mode function list.
 */
static QuartzModeProcsRec xprModeProcs = {
    xprDisplayInit,
    xprAddScreen,
    xprSetupScreen,
    xprInitInput,
    QuartzInitCursor,
    QuartzSuspendXCursor,
    QuartzResumeXCursor,
    xprAddPseudoramiXScreens,
    xprUpdateScreen,
    xprIsX11Window,
    xprHideWindows,
    RootlessFrameForWindow,
    TopLevelParent,
    DRICreateSurface,
    DRIDestroySurface
};

/*
 * QuartzModeBundleInit
 *  Initialize the display mode bundle after loading.
 */
Bool
QuartzModeBundleInit(void)
{
    quartzProcs = &xprModeProcs;
    quartzOpenGLBundle = xprOpenGLBundle;
    return TRUE;
}
