/*
 *
 * Compiz application swap plugin
 *
 * swap.c
 *
 * Copyright : (C) 2008 by Eduardo Gurgel
 * E-mail    : edgurgel@gmail.com
 *
 * Copyright : (C) 2008 by Marco Diego Aurelio Mesquita
 * E-mail    : marcodiegomesquita@gmail.com
 *
 * Based on staticswitcher.c
 * Copyright : (C) 2008 by Danny Baumann
 * E-mail    : dannybaumann@web.de
 *
 * Based on switcher.c:
 * Copyright : (C) 2007 David Reveman
 * E-mail    : davidr@novell.com
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/types.h>
#include <unistd.h>
#include <X11/Xatom.h>

#include <compiz-core.h>
#include <decoration.h>
#include "swap_options.h"

static int SwapDisplayPrivateIndex;

typedef struct _SwapDisplay {
    int		    screenPrivateIndex;
    HandleEventProc handleEvent;

    Atom selectWinAtom;
    Atom selectFgColorAtom;
} SwapDisplay;

typedef enum {
    CurrentViewport = 0,
    AllViewports,
    Group,
    Panels
} SwapWindowSelection;

typedef struct _SwapScreen {
    PreparePaintScreenProc preparePaintScreen;
    DonePaintScreenProc    donePaintScreen;
    PaintOutputProc	   paintOutput;
    PaintWindowProc        paintWindow;
    DamageWindowRectProc   damageWindowRect;

    Window            popupWindow;
    CompTimeoutHandle popupDelayHandle;

    Window selectedWindow;
    Window clientLeader;

    unsigned int previewWidth;
    unsigned int previewHeight;
    unsigned int previewBorder;
    unsigned int xCount;

    int  grabIndex;
    Bool swaping;

    int     moreAdjust;
    GLfloat mVelocity;

    CompWindow **windows;
    int        windowsSize;
    int        nWindows;

    float pos;
    float move;

    SwapWindowSelection selection;

    unsigned int fgColor[4];
} SwapScreen;

#define ICON_SIZE 64

#define PREVIEWSIZE 150
#define BORDER 10

#define SWAP_DISPLAY(d) PLUGIN_DISPLAY(d, Swap, s)
#define SWAP_SCREEN(s) PLUGIN_SCREEN(s, Swap, s)

static void
swapSetSelectedWindowHint (CompScreen *s)
{
    SWAP_DISPLAY (s->display);
    SWAP_SCREEN (s);

    XChangeProperty (s->display->display, ss->popupWindow, sd->selectWinAtom,
		     XA_WINDOW, 32, PropModeReplace,
		     (unsigned char *) &ss->selectedWindow, 1);
}

static Bool
isSwapWin (CompWindow *w)
{
    CompScreen *s = w->screen;

    SWAP_SCREEN (s);

    if (!w->mapNum || w->attrib.map_state != IsViewable)
    {
	if (swapGetMinimized (s))
	{
	    if (!w->minimized && !w->inShowDesktopMode && !w->shaded)
		return FALSE;
	}
	else
	{
	    return FALSE;
	}
    }

    if (!(w->inputHint || (w->protocols & CompWindowProtocolTakeFocusMask)))
	return FALSE;

    if (w->attrib.override_redirect)
	return FALSE;

    if (ss->selection == Panels)
    {
	if (!(w->type & (CompWindowTypeDockMask | CompWindowTypeDesktopMask)))
	    return FALSE;
    }
    else
    {
	if (w->wmType & (CompWindowTypeDockMask | CompWindowTypeDesktopMask))
	    return FALSE;

	if (w->state & CompWindowStateSkipTaskbarMask)
	    return FALSE;

	if (!matchEval (swapGetWindowMatch (s), w))
	    return FALSE;
    }

    if (ss->selection == CurrentViewport)
    {
	if (!w->mapNum || w->attrib.map_state != IsViewable)
	{
	    if (w->serverX + w->width  <= 0    ||
		w->serverY + w->height <= 0    ||
		w->serverX >= w->screen->width ||
		w->serverY >= w->screen->height)
		return FALSE;
	}
	else
	{
	    if (!(*w->screen->focusWindow) (w))
		return FALSE;
	}
    }
    else if (ss->selection == Group)
    {
	if (ss->clientLeader != w->clientLeader &&
	    ss->clientLeader != w->id)
	    return FALSE;
    }

    return TRUE;
}

static void
swapActivateEvent (CompScreen *s,
		   Bool	activating)
{
    CompOption o[2];

    o[0].type = CompOptionTypeInt;
    o[0].name = "root";
    o[0].value.i = s->root;

    o[1].type = CompOptionTypeBool;
    o[1].name = "active";
    o[1].value.b = activating;

    (*s->display->handleCompizEvent) (s->display, "swap", "activate", o, 2);
}

static int
swapCompareWindows (const void *elem1,
		    const void *elem2)
{
    CompWindow *w1 = *((CompWindow **) elem1);
    CompWindow *w2 = *((CompWindow **) elem2);

    if (w1->mapNum && !w2->mapNum)
	return -1;

    if (w2->mapNum && !w1->mapNum)
	return 1;

    return w2->activeNum - w1->activeNum;
}

static void
swapAddWindowToList (CompScreen *s,
		     CompWindow *w)
{
    SWAP_SCREEN (s);

    if (ss->windowsSize <= ss->nWindows)
    {
	ss->windows = realloc (ss->windows,
			       sizeof (CompWindow *) * (ss->nWindows + 32));
	if (!ss->windows)
	    return;

	ss->windowsSize = ss->nWindows + 32;
    }

    ss->windows[ss->nWindows++] = w;
}

static void
swapUpdatePopupWindow (CompScreen *s,
		       int        count)
{
    unsigned int winWidth, winHeight;
    unsigned int xCount, yCount;
    float        aspect;
    double       dCount = count;
    unsigned int w = PREVIEWSIZE, h = PREVIEWSIZE, b = BORDER;
    XSizeHints xsh;
    int x, y;

    SWAP_SCREEN (s);

    /* maximum window size is 2/3 of the current output */
    winWidth  = s->outputDev[s->currentOutputDev].width * 2 / 3;
    winHeight = s->outputDev[s->currentOutputDev].height * 2 / 3;

    if (count <= 4)
    {
	/* don't put 4 or less windows in multiple rows */
	xCount = count;
	yCount = 1;
    }
    else
    {
	aspect = (float) winWidth / winHeight;
	/* round is available in C99 only, so use a replacement for that */
	yCount = floor (sqrt (dCount / aspect) + 0.5f);
	xCount = ceil (dCount / yCount);
    }

    while ((w + b) * xCount > winWidth ||
	   (h + b) * yCount > winHeight)
    {
	/* shrink by 10% until all windows fit */
	w = w * 9 / 10;
	h = h * 9 / 10;
	b = b * 9 / 10;
    }

    winWidth = MIN (count, xCount);
    winHeight = (count + xCount - 1) / xCount;

    winWidth = winWidth * w + (winWidth + 1) * b;
    winHeight = winHeight * h + (winHeight + 1) * b;
    ss->xCount = MIN (xCount, count);

    ss->previewWidth = w;
    ss->previewHeight = h;
    ss->previewBorder = b;

    xsh.flags       = PSize | PPosition | PWinGravity;
    xsh.width       = winWidth;
    xsh.height      = winHeight;
    xsh.win_gravity = StaticGravity;

    XSetWMNormalHints (s->display->display, ss->popupWindow, &xsh);

    x = s->outputDev[s->currentOutputDev].region.extents.x1 +
	s->outputDev[s->currentOutputDev].width / 2;
    y = s->outputDev[s->currentOutputDev].region.extents.y1 +
	s->outputDev[s->currentOutputDev].height / 2;

    XMoveResizeWindow (s->display->display, ss->popupWindow,
		       x - winWidth / 2, y - winHeight / 2,
		       winWidth, winHeight);
}

static void
swapUpdateWindowList (CompScreen *s,
		      int        count)
{
    SWAP_SCREEN (s);

    ss->pos  = 0.0;
    ss->move = 0.0;

    ss->selectedWindow = ss->windows[0]->id;

    if (ss->popupWindow)
	swapUpdatePopupWindow (s, count);
}

static void
swapCreateWindowList (CompScreen *s,
		      int	 count)
{
    CompWindow *w;

    SWAP_SCREEN (s);

    ss->nWindows = 0;

    for (w = s->windows; w; w = w->next)
    {
	if (isSwapWin (w))
	    swapAddWindowToList (s, w);
    }

    qsort (ss->windows, ss->nWindows, sizeof (CompWindow *), swapCompareWindows);

    swapUpdateWindowList (s, count);
}

static Bool
swapGetPaintRectangle (CompWindow *w,
		       BoxPtr     rect,
		       int        *opacity)
{
    SwapHighlightRectHiddenEnum mode;

    mode = swapGetHighlightRectHidden (w->screen);

    if (w->attrib.map_state == IsViewable || w->shaded)
    {
	rect->x1 = w->attrib.x - w->input.left;
	rect->y1 = w->attrib.y - w->input.top;
	rect->x2 = w->attrib.x + w->width + w->input.right;
	rect->y2 = w->attrib.y + w->height + w->input.bottom;
	return TRUE;
    }
    else if (mode == HighlightRectHiddenTaskbarEntry && w->iconGeometrySet)
    {
	rect->x1 = w->iconGeometry.x;
	rect->y1 = w->iconGeometry.y;
	rect->x2 = rect->x1 + w->iconGeometry.width;
	rect->y2 = rect->y1 + w->iconGeometry.height;
	return TRUE;
    }
    else if (mode == HighlightRectHiddenOriginalWindowPosition)
    {
	rect->x1 = w->serverX - w->input.left;
	rect->y1 = w->serverY - w->input.top;
	rect->x2 = w->serverX + w->serverWidth + w->input.right;
	rect->y2 = w->serverY + w->serverHeight + w->input.bottom;

	if (opacity)
	    *opacity /= 4;

	return TRUE;
    }

    return FALSE;
}

static void
swapDoWindowDamage (CompWindow *w)
{
    if (w->attrib.map_state == IsViewable || w->shaded)
	addWindowDamage (w);
    else
    {
	BoxRec box;
	if (swapGetPaintRectangle (w, &box, NULL))
	{
	    REGION reg;

	    reg.rects    = &reg.extents;
	    reg.numRects = 1;

	    reg.extents.x1 = box.x1 - 2;
	    reg.extents.y1 = box.y1 - 2;
	    reg.extents.x2 = box.x2 + 2;
	    reg.extents.y2 = box.y2 + 2;

	    damageScreenRegion (w->screen, &reg);
	}
    }
}

static void
swapToWindow (CompScreen *s,
	      Bool       toNext)
{
    CompWindow *w;
    int	       cur, nextIdx;

    SWAP_SCREEN (s);

    if (!ss->grabIndex)
	return;

    for (cur = 0; cur < ss->nWindows; cur++)
    {
	if (ss->windows[cur]->id == ss->selectedWindow)
	    break;
    }

    if (cur == ss->nWindows)
	return;

    if (toNext)
	nextIdx = (cur + 1) % ss->nWindows;
    else
	nextIdx = (cur + ss->nWindows - 1) % ss->nWindows;

    w = ss->windows[nextIdx];

    if (w)
    {
	Window old = ss->selectedWindow;

	if (ss->selection == AllViewports && swapGetAutoChangeVp (s))
	{
	    XEvent xev;
	    int	   x, y;

	    defaultViewportForWindow (w, &x, &y);

	    xev.xclient.type = ClientMessage;
	    xev.xclient.display = s->display->display;
	    xev.xclient.format = 32;

	    xev.xclient.message_type = s->display->desktopViewportAtom;
	    xev.xclient.window = s->root;

	    xev.xclient.data.l[0] = x * s->width;
	    xev.xclient.data.l[1] = y * s->height;
	    xev.xclient.data.l[2] = 0;
	    xev.xclient.data.l[3] = 0;
	    xev.xclient.data.l[4] = 0;

	    XSendEvent (s->display->display, s->root, FALSE,
			SubstructureRedirectMask | SubstructureNotifyMask,
			&xev);
	}

	ss->selectedWindow = w->id;

	if (old != w->id)
	{
	    ss->move = nextIdx;

	    ss->moreAdjust = 1;
	}

	if (ss->popupWindow)
	{
	    CompWindow *popup;

	    popup = findWindowAtScreen (s, ss->popupWindow);
	    if (popup)
		addWindowDamage (popup);

	    swapSetSelectedWindowHint (s);
	}

	swapDoWindowDamage (w);

	if (old)
	{
	    w = findWindowAtScreen (s, old);
	    if (w)
		swapDoWindowDamage (w);
	}
    }
}

static int
swapCountWindows (CompScreen *s)
{
    CompWindow *w;
    int	       count = 0;

    for (w = s->windows; w; w = w->next)
	if (isSwapWin (w))
	    count++;

    return count;
}

static Visual *
swapFindArgbVisual (Display *dpy, int scr)
{
    XVisualInfo		*xvi;
    XVisualInfo		template;
    int			nvi;
    int			i;
    XRenderPictFormat	*format;
    Visual		*visual;

    template.screen = scr;
    template.depth  = 32;
    template.class  = TrueColor;

    xvi = XGetVisualInfo (dpy,
			  VisualScreenMask |
			  VisualDepthMask  |
			  VisualClassMask,
			  &template,
			  &nvi);
    if (!xvi)
	return 0;

    visual = 0;
    for (i = 0; i < nvi; i++)
    {
	format = XRenderFindVisualFormat (dpy, xvi[i].visual);
	if (format->type == PictTypeDirect && format->direct.alphaMask)
	{
	    visual = xvi[i].visual;
	    break;
	}
    }

    XFree (xvi);

    return visual;
}

static Bool
swapShowPopup (void *closure)
{
    CompScreen *s = (CompScreen *) closure;
    CompWindow *w;

    SWAP_SCREEN (s);

    w = findWindowAtScreen (s, ss->popupWindow);
    if (w && (w->state & CompWindowStateHiddenMask))
    {
	w->hidden = FALSE;
	showWindow (w);
    }
    else
    {
	XMapWindow (s->display->display, ss->popupWindow);
    }

    damageScreen (s);

    ss->popupDelayHandle = 0;

    return FALSE;
}

static void
swapInitiate (CompScreen          *s,
	      SwapWindowSelection selection,
	      Bool	          showPopup)
{
    int count;

    SWAP_SCREEN (s);

    if (otherScreenGrabExist (s, "swap", "scale", "cube", 0))
	return;

    ss->selection      = selection;
    ss->selectedWindow = None;

    count = swapCountWindows (s);
    if (count < 1)
	return;

    if (!ss->popupWindow && showPopup)
    {
	Display		     *dpy = s->display->display;
	XWMHints	     xwmh;
	Atom		     state[4];
	int		     nState = 0;
	XSetWindowAttributes attr;
	Visual		     *visual;

	visual = swapFindArgbVisual (dpy, s->screenNum);
	if (!visual)
	    return;

	xwmh.flags = InputHint;
	xwmh.input = 0;

	attr.background_pixel = 0;
	attr.border_pixel     = 0;
	attr.colormap	      = XCreateColormap (dpy, s->root, visual,
						 AllocNone);

	ss->popupWindow =
	    XCreateWindow (dpy, s->root, -1, -1, 1, 1, 0,
			   32, InputOutput, visual,
			   CWBackPixel | CWBorderPixel | CWColormap, &attr);

	XSetWMProperties (dpy, ss->popupWindow, NULL, NULL,
			  programArgv, programArgc,
			  NULL, &xwmh, NULL);

	state[nState++] = s->display->winStateAboveAtom;
	state[nState++] = s->display->winStateStickyAtom;
	state[nState++] = s->display->winStateSkipTaskbarAtom;
	state[nState++] = s->display->winStateSkipPagerAtom;

	XChangeProperty (dpy, ss->popupWindow,
			 s->display->winStateAtom,
			 XA_ATOM, 32, PropModeReplace,
			 (unsigned char *) state, nState);

	XChangeProperty (dpy, ss->popupWindow,
			 s->display->winTypeAtom,
			 XA_ATOM, 32, PropModeReplace,
			 (unsigned char *) &s->display->winTypeUtilAtom, 1);

	setWindowProp (s->display, ss->popupWindow,
		       s->display->winDesktopAtom,
		       0xffffffff);

	swapSetSelectedWindowHint (s);
    }

    if (!ss->grabIndex)
	ss->grabIndex = pushScreenGrab (s, s->invisibleCursor, "switch");

    if (ss->grabIndex)
    {
	if (!ss->swaping)
	{
	    swapCreateWindowList (s, count);

	    if (ss->popupWindow && showPopup)
	    {
		unsigned int delay;

		delay = swapGetPopupDelay (s) * 1000;
		if (delay)
		{
		    if (ss->popupDelayHandle)
			compRemoveTimeout (ss->popupDelayHandle);

		    ss->popupDelayHandle = compAddTimeout (delay,
							   (float) delay * 1.2,
							   swapShowPopup, s);
		}
		else
		{
		    swapShowPopup (s);
		}
	    }

	    swapActivateEvent (s, TRUE);
	}

	damageScreen (s);

	ss->swaping  = TRUE;
	ss->moreAdjust = 1;
    }
}
static void
swapUpdateWindowGeometry(CompWindow   *w,
			 Bool         minimized,
			 unsigned int state,
			 int	      x,
			 int	      y,
			 int	      width,
			 int	      height)
{
    changeWindowState (w,state);
    if(minimized)
	minimizeWindow(w);

    XWindowChanges xwc;
    unsigned int mask = 0;
    int auxW = width;
    int auxH = height;
    xwc.width = width;
    xwc.height = height;
    if(constrainNewWindowSize (w,width,height,&auxW,&auxH))
    {
	mask |= CWWidth | CWHeight;
        xwc.width = auxW;
        xwc.height = auxH;
    }

    xwc.width = auxW;
    xwc.height = auxH;
    mask |= CWWidth | CWHeight;

    xwc.x = x;
    xwc.y = y;
    mask |= CWX | CWY;

    if(w->mapNum && (mask & (CWWidth | CWHeight)))
        sendSyncRequest (w);

    configureXWindow(w,mask,&xwc);
}

static void
swapWindows (CompWindow  *w1,
	     CompWindow  *w2)
{
    unsigned int auxState = w1->state;
    int auxX = w1->serverX;
    int auxY = w1->serverY;
    int auxWidth = w1->serverWidth;
    int auxHeight = w1->serverHeight;
    swapUpdateWindowGeometry (w1,w2->minimized,w2->state,w2->serverX,w2->serverY,w2->serverWidth,w2->serverHeight);
    swapUpdateWindowGeometry (w2,w1->minimized,auxState,auxX,auxY,auxWidth,auxHeight);
}

static Bool
swapTerminate (CompDisplay     *d,
	       CompAction      *action,
	       CompActionState state,
	       CompOption      *option,
	       int	       nOption)
{
    CompScreen *s;
    Window     xid;

    xid = getIntOptionNamed (option, nOption, "root", 0);

    for (s = d->screens; s; s = s->next)
    {
	SWAP_SCREEN (s);

	if (xid && s->root != xid)
	    continue;

	if (ss->grabIndex)
	{
	    CompWindow *w;

	    if (ss->popupDelayHandle)
	    {
		compRemoveTimeout (ss->popupDelayHandle);
		ss->popupDelayHandle = 0;
	    }

	    if (ss->popupWindow)
	    {
		w = findWindowAtScreen (s, ss->popupWindow);
		if (w && w->managed && w->mapNum)
		{
		    w->hidden = TRUE;
		    hideWindow (w);
		}
		else
		{
		    XUnmapWindow (s->display->display, ss->popupWindow);
		}
	    }

	    ss->swaping = FALSE;

	    if (state & CompActionStateCancel)
		ss->selectedWindow = None;

	    if (state && ss->selectedWindow)
	    {
		w = findWindowAtScreen (s, ss->selectedWindow);

		if (w)
		{
		    Window currentXid;
		    CompWindow *currentW;

		    currentXid = getIntOptionNamed (option, nOption, "window", 0);
		    currentW = findWindowAtDisplay (d,currentXid);

		    swapWindows(currentW,w);
		    sendWindowActivationRequest (w->screen, w->id);
		}
	    }

	    removeScreenGrab (s, ss->grabIndex, 0);
	    ss->grabIndex = 0;

	    ss->selectedWindow = None;

	    swapActivateEvent (s, FALSE);
	    swapSetSelectedWindowHint (s);

	    damageScreen (s);
	}
    }

    if (action)
	action->state &= ~(CompActionStateTermKey | CompActionStateTermButton);

    return FALSE;
}

static Bool
swapInitiateCommon (CompDisplay           *d,
		    CompAction            *action,
		    CompActionState       state,
		    CompOption            *option,
		    int                   nOption,
		    SwapWindowSelection   selection,
		    Bool                  showPopup,
		    Bool                  nextWindow)
{
    CompScreen *s;
    Window     xid;

    xid = getIntOptionNamed (option, nOption, "root", 0);

    s = findScreenAtDisplay (d, xid);
    if (s)
    {
	SWAP_SCREEN (s);

	if (!ss->swaping)
	{
	    if (selection == Group)
	    {
		CompWindow *w;
		Window     xid;

		xid = getIntOptionNamed (option, nOption, "window", 0);
		w   = findWindowAtDisplay (d, xid);
		if (w)
		    ss->clientLeader = (w->clientLeader) ?
			               w->clientLeader : w->id;
		else
		    ss->clientLeader = None;
	    }

	    swapInitiate (s, selection, showPopup);

	    if (state & CompActionStateInitKey)
		action->state |= CompActionStateTermKey;

	    if (state & CompActionStateInitEdge)
		action->state |= CompActionStateTermEdge;
	    else if (state & CompActionStateInitButton)
		action->state |= CompActionStateTermButton;
	}

	swapToWindow (s, nextWindow);
    }

    return FALSE;
}

static Bool
swapNext (CompDisplay     *d,
	  CompAction      *action,
	  CompActionState state,
	  CompOption      *option,
	  int	          nOption)
{
    return swapInitiateCommon (d, action, state, option, nOption,
			       CurrentViewport, TRUE, TRUE);
}

static Bool
swapPrev (CompDisplay     *d,
	  CompAction      *action,
	  CompActionState state,
	  CompOption      *option,
	  int	          nOption)
{
    return swapInitiateCommon (d, action, state, option, nOption,
			       CurrentViewport, TRUE, FALSE);
}

static Bool
swapNextAll (CompDisplay     *d,
	     CompAction      *action,
	     CompActionState state,
	     CompOption      *option,
	     int	     nOption)
{
    return swapInitiateCommon (d, action, state, option, nOption,
			       AllViewports, TRUE, TRUE);
}

static Bool
swapPrevAll (CompDisplay     *d,
	     CompAction      *action,
	     CompActionState state,
	     CompOption      *option,
	     int	     nOption)
{
    return swapInitiateCommon (d, action, state, option, nOption,
			       AllViewports, TRUE, FALSE);
}

static Bool
swapNextGroup (CompDisplay     *d,
	       CompAction      *action,
	       CompActionState state,
	       CompOption      *option,
	       int	       nOption)
{
    return swapInitiateCommon (d, action, state, option, nOption,
			       Group, TRUE, TRUE);
}

static Bool
swapPrevGroup (CompDisplay     *d,
	       CompAction      *action,
	       CompActionState state,
	       CompOption      *option,
	       int	       nOption)
{
    return swapInitiateCommon (d, action, state, option, nOption,
			       Group, TRUE, FALSE);
}

static Bool
swapNextNoPopup (CompDisplay       *d,
		   CompAction      *action,
		   CompActionState state,
		   CompOption      *option,
		   int	           nOption)
{
    return swapInitiateCommon (d, action, state, option, nOption,
			       CurrentViewport, FALSE, TRUE);
}

static Bool
swapPrevNoPopup (CompDisplay     *d,
		 CompAction      *action,
		 CompActionState state,
		 CompOption      *option,
		 int	         nOption)
{
    return swapInitiateCommon (d, action, state, option, nOption,
			       CurrentViewport, FALSE, FALSE);
}

static Bool
swapNextPanel (CompDisplay       *d,
		 CompAction      *action,
		 CompActionState state,
		 CompOption      *option,
		 int	         nOption)
{
    return swapInitiateCommon (d, action, state, option, nOption,
			       Panels, FALSE, TRUE);
}

static Bool
swapPrevPanel (CompDisplay     *d,
	       CompAction      *action,
	       CompActionState state,
	       CompOption      *option,
	       int	       nOption)
{
    return swapInitiateCommon (d, action, state, option, nOption,
			       Panels, FALSE, FALSE);
}

static void
swapWindowRemove (CompDisplay *d,
		  Window      id)
{
    CompWindow *w;

    w = findWindowAtDisplay (d, id);
    if (w)
    {
	Bool   inList = FALSE;
	int    count, j, i = 0;
	Window selected, old;
	CompScreen *s = w->screen;

	SWAP_SCREEN (s);

	if (isSwapWin (w))
	    return;

	old = selected = ss->selectedWindow;

	while (i < ss->nWindows)
	{
	    if (ss->windows[i] == w)
	    {
		inList = TRUE;

		if (w->id == selected)
		{
		    if (i < ss->nWindows)
			selected = ss->windows[i + 1]->id;
		    else
			selected = ss->windows[0]->id;
		}

		ss->nWindows--;
		for (j = i; j < ss->nWindows; j++)
		    ss->windows[j] = ss->windows[j + 1];
	    }
	    else
	    {
		i++;
	    }
	}

	if (!inList)
	    return;

	count = ss->nWindows;

	if (ss->nWindows == 0)
	{
	    CompOption o;

	    o.type    = CompOptionTypeInt;
	    o.name    = "root";
	    o.value.i = w->screen->root;

	    swapTerminate (d, NULL, 0, &o, 1);
	    return;
	}

	if (!ss->grabIndex)
	    return;

	swapUpdateWindowList (w->screen, count);

	for (i = 0; i < ss->nWindows; i++)
	{
	    ss->selectedWindow = ss->windows[i]->id;
	    ss->move = ss->pos = i;

	    if (ss->selectedWindow == selected)
		break;
	}

	if (ss->popupWindow)
	{
	    CompWindow *popup;

	    popup = findWindowAtScreen (w->screen, ss->popupWindow);
	    if (popup)
		addWindowDamage (popup);

	    swapSetSelectedWindowHint (w->screen);
	}

	if (old != ss->selectedWindow)
	{
	    swapDoWindowDamage (w);

	    w = findWindowAtScreen (w->screen, old);
	    if (w)
		swapDoWindowDamage (w);

	    ss->moreAdjust = 1;
	}
    }
}

static void
swapUpdateForegroundColor (CompScreen *s)
{
    Atom	  actual;
    int		  result, format;
    unsigned long n, left;
    unsigned char *propData;

    SWAP_SCREEN (s);
    SWAP_DISPLAY (s->display);

    if (!ss->popupWindow)
	return;


    result = XGetWindowProperty (s->display->display, ss->popupWindow,
				 sd->selectFgColorAtom, 0L, 4L, FALSE,
				 XA_INTEGER, &actual, &format,
				 &n, &left, &propData);

    if (result == Success && n && propData)
    {
	if (n == 3 || n == 4)
	{
	    long *data = (long *) propData;

	    ss->fgColor[0] = MIN (0xffff, data[0]);
	    ss->fgColor[1] = MIN (0xffff, data[1]);
	    ss->fgColor[2] = MIN (0xffff, data[2]);

	    if (n == 4)
		ss->fgColor[3] = MIN (0xffff, data[3]);
	}

	XFree (propData);
    }
    else
    {
	ss->fgColor[0] = 0;
	ss->fgColor[1] = 0;
	ss->fgColor[2] = 0;
	ss->fgColor[3] = 0xffff;
    }
}

static void
swapHandleEvent (CompDisplay *d,
		 XEvent      *event)
{
    CompWindow *w;
    SWAP_DISPLAY (d);

    UNWRAP (sd, d, handleEvent);
    (*d->handleEvent) (d, event);
    WRAP (sd, d, handleEvent, swapHandleEvent);

    switch (event->type) {
    case UnmapNotify:
	swapWindowRemove (d, event->xunmap.window);
	break;
    case DestroyNotify:
	swapWindowRemove (d, event->xdestroywindow.window);
	break;
    case PropertyNotify:
	if (event->xproperty.atom == sd->selectFgColorAtom)
        {
            w = findWindowAtDisplay (d, event->xproperty.window);
            if (w)
            {
		CompScreen *s = w->screen;

		SWAP_SCREEN (s);

		if (event->xproperty.window == ss->popupWindow)
		    swapUpdateForegroundColor (s);
            }
        }

    default:
	break;
    }
}

static int
adjustSwapVelocity (CompScreen *s)
{
    float dx, adjust, amount;

    SWAP_SCREEN (s);

    dx = ss->move - ss->pos;
    if (abs (dx) > abs (dx + ss->nWindows))
	dx += ss->nWindows;
    if (abs (dx) > abs (dx - ss->nWindows))
	dx -= ss->nWindows;

    adjust = dx * 0.15f;
    amount = fabs (dx) * 1.5f;
    if (amount < 0.2f)
	amount = 0.2f;
    else if (amount > 2.0f)
	amount = 2.0f;

    ss->mVelocity = (amount * ss->mVelocity + adjust) / (amount + 1.0f);

    if (fabs (dx) < 0.001f && fabs (ss->mVelocity) < 0.001f)
    {
	ss->mVelocity = 0.0f;
	return 0;
    }

    return 1;
}

static void
swapPreparePaintScreen (CompScreen *s,
			int	   msSinceLastPaint)
{
    SWAP_SCREEN (s);

    if (ss->moreAdjust)
    {
	int   steps;
	float amount, chunk;

	amount = msSinceLastPaint * 0.05f * swapGetSpeed (s);
	steps  = amount / (0.5f * swapGetTimestep (s));
	if (!steps) steps = 1;
	chunk  = amount / (float) steps;

	while (steps--)
	{
	    ss->moreAdjust = adjustSwapVelocity (s);
	    if (!ss->moreAdjust)
	    {
		ss->pos = ss->move;
		break;
	    }

	    ss->pos += ss->mVelocity * chunk;
	    ss->pos = fmod (ss->pos, ss->nWindows);
	    if (ss->pos < 0.0)
		ss->pos += ss->nWindows;
	}
    }

    UNWRAP (ss, s, preparePaintScreen);
    (*s->preparePaintScreen) (s, msSinceLastPaint);
    WRAP (ss, s, preparePaintScreen, swapPreparePaintScreen);
}

static inline void
swapPaintRect (BoxRec         *box,
	       unsigned int   offset,
	       unsigned short *color,
	       int            opacity)
{
    glColor4us (color[0], color[1], color[2], color[3] * opacity / 100);
    glBegin (GL_LINE_LOOP);
    glVertex2i (box->x1 + offset, box->y1 + offset);
    glVertex2i (box->x2 - offset, box->y1 + offset);
    glVertex2i (box->x2 - offset, box->y2 - offset);
    glVertex2i (box->x1 + offset, box->y2 - offset);
    glEnd ();
}

static Bool
swapPaintOutput (CompScreen		   *s,
		 const ScreenPaintAttrib   *sAttrib,
		 const CompTransform	   *transform,
		 Region		           region,
		 CompOutput		   *output,
		 unsigned int		   mask)
{
    Bool status;

    SWAP_SCREEN (s);

    if (ss->grabIndex)
    {
	SwapHighlightModeEnum mode;
	CompWindow                      *swaper, *zoomed;
	Window	                        zoomedAbove = None;
	Bool	                        saveDestroyed = FALSE;

	swaper = findWindowAtScreen (s, ss->popupWindow);
	if (swaper)
	{
	    saveDestroyed = swaper->destroyed;
	    swaper->destroyed = TRUE;
	}

	if (!ss->popupDelayHandle)
	    mode = swapGetHighlightMode (s);
	else
	    mode = HighlightModeNone;

	if (mode == HighlightModeBringSelectedToFront)
	{
	    zoomed = findWindowAtScreen (s, ss->selectedWindow);
	    if (zoomed)
	    {
		CompWindow *w;

		for (w = zoomed->prev; w && w->id <= 1; w = w->prev);
		zoomedAbove = (w) ? w->id : None;

		unhookWindowFromScreen (s, zoomed);
		insertWindowIntoScreen (s, zoomed, s->reverseWindows->id);
	    }
	}
	else
	{
	    zoomed = NULL;
	}

	UNWRAP (ss, s, paintOutput);
	status = (*s->paintOutput) (s, sAttrib, transform,
				    region, output, mask);
	WRAP (ss, s, paintOutput, swapPaintOutput);

	if (zoomed)
	{
	    unhookWindowFromScreen (s, zoomed);
	    insertWindowIntoScreen (s, zoomed, zoomedAbove);
	}

	if (swaper || mode == HighlightModeShowRectangle)
	{
	    CompTransform sTransform = *transform;

	    transformToScreenSpace (s, output, -DEFAULT_Z_CAMERA, &sTransform);

	    glPushMatrix ();
	    glLoadMatrixf (sTransform.m);

	    if (mode == HighlightModeShowRectangle)
	    {
		CompWindow *w;

		if (zoomed)
		    w = zoomed;
		else
		    w = findWindowAtScreen (s, ss->selectedWindow);

		if (w)
		{
		    BoxRec box;
		    int    opacity = 100;

		    if (swapGetPaintRectangle (w, &box, &opacity))
		    {
			unsigned short *color;
			GLushort       r, g, b, a;

			glEnable (GL_BLEND);

			/* fill rectangle */
			r = swapGetHighlightColorRed (s);
			g = swapGetHighlightColorGreen (s);
			b = swapGetHighlightColorBlue (s);
			a = swapGetHighlightColorAlpha (s);
			a = a * opacity / 100;

			glColor4us (r, g, b, a);
			glRecti (box.x1, box.y2, box.x2, box.y1);

			/* draw outline */
			glLineWidth (1.0);
			glDisable (GL_LINE_SMOOTH);

			color = swapGetHighlightBorderColor (s);
			swapPaintRect (&box, 0, color, opacity);
			swapPaintRect (&box, 2, color, opacity);
			color = swapGetHighlightBorderInlayColor (s);
			swapPaintRect (&box, 1, color, opacity);

			/* clean up */
			glColor4usv (defaultColor);
			glDisable (GL_BLEND);
		    }
		}
	    }

	    if (swaper)
	    {
		swaper->destroyed = saveDestroyed;

		if (!swaper->destroyed		     &&
		    swaper->attrib.map_state == IsViewable &&
		    swaper->damaged)
		{
		    (*s->paintWindow) (swaper, &swaper->paint, &sTransform,
				       &infiniteRegion, 0);
		}
	    }

	    glPopMatrix ();
	}
    }
    else
    {
	UNWRAP (ss, s, paintOutput);
	status = (*s->paintOutput) (s, sAttrib, transform, region, output,
				    mask);
	WRAP (ss, s, paintOutput, swapPaintOutput);
    }

    return status;
}

static void
swapDonePaintScreen (CompScreen *s)
{
    SWAP_SCREEN (s);

    if (ss->grabIndex && ss->moreAdjust)
    {
	CompWindow *w;

	w = findWindowAtScreen (s, ss->popupWindow);
	if (w)
	    addWindowDamage (w);
    }

    UNWRAP (ss, s, donePaintScreen);
    (*s->donePaintScreen) (s);
    WRAP (ss, s, donePaintScreen, swapDonePaintScreen);
}

static void
swapPaintThumb (CompWindow		  *w,
		const WindowPaintAttrib   *attrib,
		const CompTransform	  *transform,
		unsigned int		  mask,
		int			  x,
		int			  y)
{
    CompScreen *s = w->screen;
    WindowPaintAttrib sAttrib = *attrib;
    int		      wx, wy;
    float	      width, height;
    CompIcon	      *icon = NULL;

    SWAP_SCREEN (s);

    mask |= PAINT_WINDOW_TRANSFORMED_MASK;

    if (w->mapNum)
    {
	if (!w->texture->pixmap && !w->bindFailed)
	    bindWindow (w);
    }

    if (w->texture->pixmap)
    {
	AddWindowGeometryProc oldAddWindowGeometry;
	FragmentAttrib	      fragment;
	CompTransform	      wTransform = *transform;
	int		      ww, wh;

	width  = ss->previewWidth;
	height = ss->previewHeight;

	ww = w->width  + w->input.left + w->input.right;
	wh = w->height + w->input.top  + w->input.bottom;

	if (ww > width)
	    sAttrib.xScale = width / ww;
	else
	    sAttrib.xScale = 1.0f;

	if (wh > height)
	    sAttrib.yScale = height / wh;
	else
	    sAttrib.yScale = 1.0f;

	if (sAttrib.xScale < sAttrib.yScale)
	    sAttrib.yScale = sAttrib.xScale;
	else
	    sAttrib.xScale = sAttrib.yScale;

	width  = ww * sAttrib.xScale;
	height = wh * sAttrib.yScale;

	wx = x + (ss->previewWidth / 2) - (width / 2);
	wy = y + (ss->previewHeight / 2) - (height / 2);

#if 0
	if (w->id != ss->selectedWindow)
	{
	    sAttrib.brightness /= 2;
	    sAttrib.saturation /= 2;
	}
#endif

	sAttrib.xTranslate = wx - w->attrib.x + w->input.left * sAttrib.xScale;
	sAttrib.yTranslate = wy - w->attrib.y + w->input.top  * sAttrib.yScale;

	initFragmentAttrib (&fragment, &sAttrib);

	if (w->alpha || fragment.opacity != OPAQUE)
	    mask |= PAINT_WINDOW_TRANSLUCENT_MASK;

	matrixTranslate (&wTransform, w->attrib.x, w->attrib.y, 0.0f);
	matrixScale (&wTransform, sAttrib.xScale, sAttrib.yScale, 1.0f);
	matrixTranslate (&wTransform,
			 sAttrib.xTranslate / sAttrib.xScale - w->attrib.x,
			 sAttrib.yTranslate / sAttrib.yScale - w->attrib.y,
			 0.0f);

	glPushMatrix ();
	glLoadMatrixf (wTransform.m);

	/* XXX: replacing the addWindowGeometry function like this is
	   very ugly but necessary until the vertex stage has been made
	   fully pluggable. */
	oldAddWindowGeometry = w->screen->addWindowGeometry;
	w->screen->addWindowGeometry = addWindowGeometry;
	(w->screen->drawWindow) (w, &wTransform, &fragment, &infiniteRegion,
				 mask);
	w->screen->addWindowGeometry = oldAddWindowGeometry;

	glPopMatrix ();

	if (swapGetIcon (s))
	{
	    icon = getWindowIcon (w, ICON_SIZE, ICON_SIZE);
	    if (icon)
	    {
		sAttrib.xScale = (float) ss->previewWidth / PREVIEWSIZE;
		sAttrib.yScale = sAttrib.xScale;

		wx = x + ss->previewWidth - (sAttrib.xScale * icon->width);
		wy = y + ss->previewHeight - (sAttrib.yScale * icon->height);
	    }
	}
    }
    else
    {
	width  = ss->previewWidth * 3 / 4;
	height = ss->previewHeight * 3 / 4;

	icon = getWindowIcon (w, width, height);
	if (!icon)
	    icon = w->screen->defaultIcon;

	if (icon)
	{
	    int iw, ih;

	    iw = width;
	    ih = height;

	    if (icon->width < (iw >> 1))
		sAttrib.xScale = (iw / icon->width);
	    else
		sAttrib.xScale = 1.0f;

	    if (icon->height < (ih >> 1))
		sAttrib.yScale = (ih / icon->height);
	    else
		sAttrib.yScale = 1.0f;

	    if (sAttrib.xScale < sAttrib.yScale)
		sAttrib.yScale = sAttrib.xScale;
	    else
		sAttrib.xScale = sAttrib.yScale;

	    width  = icon->width  * sAttrib.xScale;
	    height = icon->height * sAttrib.yScale;

	    wx = x + (ss->previewWidth / 2) - (width / 2);
	    wy = y + (ss->previewHeight / 2) - (height / 2);
	}
    }

    if (icon && (icon->texture.name || iconToTexture (w->screen, icon)))
    {
	REGION     iconReg;
	CompMatrix matrix;

	mask |= PAINT_WINDOW_BLEND_MASK;

	iconReg.rects    = &iconReg.extents;
	iconReg.numRects = 1;

	iconReg.extents.x1 = w->attrib.x;
	iconReg.extents.y1 = w->attrib.y;
	iconReg.extents.x2 = w->attrib.x + icon->width;
	iconReg.extents.y2 = w->attrib.y + icon->height;

	matrix = icon->texture.matrix;
	matrix.x0 -= (w->attrib.x * icon->texture.matrix.xx);
	matrix.y0 -= (w->attrib.y * icon->texture.matrix.yy);

	sAttrib.xTranslate = wx - w->attrib.x;
	sAttrib.yTranslate = wy - w->attrib.y;

	w->vCount = w->indexCount = 0;
	addWindowGeometry (w, &matrix, 1, &iconReg, &infiniteRegion);
	if (w->vCount)
	{
	    FragmentAttrib fragment;
	    CompTransform  wTransform = *transform;

	    initFragmentAttrib (&fragment, &sAttrib);

	    matrixTranslate (&wTransform, w->attrib.x, w->attrib.y, 0.0f);
	    matrixScale (&wTransform, sAttrib.xScale, sAttrib.yScale, 1.0f);
	    matrixTranslate (&wTransform,
			     sAttrib.xTranslate / sAttrib.xScale - w->attrib.x,
			     sAttrib.yTranslate / sAttrib.yScale - w->attrib.y,
			     0.0f);

	    glPushMatrix ();
	    glLoadMatrixf (wTransform.m);

	    (*w->screen->drawWindowTexture) (w,
					     &icon->texture, &fragment,
					     mask);

	    glPopMatrix ();
	}
    }
}

static void
swapPaintSelectionRect (SwapScreen   *ss,
			int          x,
			int          y,
			float        dx,
			float        dy,
			unsigned int opacity)
{
    int            i;
    float          color[4], op;
    unsigned int   w, h;

    w = ss->previewWidth + ss->previewBorder;
    h = ss->previewHeight + ss->previewBorder;

    glEnable (GL_BLEND);

    if (dx > ss->xCount - 1)
	op = 1.0 - MIN (1.0, dx - (ss->xCount - 1));
    else if (dx + (dy * ss->xCount) > ss->nWindows - 1)
	op = 1.0 - MIN (1.0, dx - (ss->nWindows - 1 - (dy * ss->xCount)));
    else if (dx < 0.0)
	op = 1.0 + MAX (-1.0, dx);
    else
	op = 1.0;

    for (i = 0; i < 4; i++)
	color[i] = (float)ss->fgColor[i] * opacity * op / 0xffffffff;

    glColor4fv (color);
    glPushMatrix ();
    glTranslatef (x + ss->previewBorder / 2 + (dx * w),
		  y + ss->previewBorder / 2 + (dy * h), 0.0f);

    glBegin (GL_QUADS);
    glVertex2i (-1, -1);
    glVertex2i (-1, 1);
    glVertex2i (w + 1, 1);
    glVertex2i (w + 1, -1);
    glVertex2i (-1, h - 1);
    glVertex2i (-1, h + 1);
    glVertex2i (w + 1, h + 1);
    glVertex2i (w + 1, h - 1);
    glVertex2i (-1, 1);
    glVertex2i (-1, h - 1);
    glVertex2i (1, h - 1);
    glVertex2i (1, 1);
    glVertex2i (w - 1, 1);
    glVertex2i (w - 1, h - 1);
    glVertex2i (w + 1, h - 1);
    glVertex2i (w + 1, 1);
    glEnd ();

    glPopMatrix ();
    glColor4usv (defaultColor);
    glDisable (GL_BLEND);
}

static inline int
swapGetRowXOffset (CompScreen   *s,
		   SwapScreen   *ss,
		   int          y)
{
    int retval = 0;

    if (ss->nWindows - (y * ss->xCount) >= ss->xCount)
	return 0;

    switch (swapGetRowAlign (s)) {
    case RowAlignLeft:
	break;
    case RowAlignCentered:
	retval = (ss->xCount - ss->nWindows + (y * ss->xCount)) *
	         (ss->previewWidth + ss->previewBorder) / 2;
	break;
    case RowAlignRight:
	retval = (ss->xCount - ss->nWindows + (y * ss->xCount)) *
	         (ss->previewWidth + ss->previewBorder);
	break;
    }

    return retval;
}

static Bool
swapPaintWindow (CompWindow		   *w,
		   const WindowPaintAttrib *attrib,
		   const CompTransform	   *transform,
		   Region		   region,
		   unsigned int		   mask)
{
    CompScreen *s = w->screen;
    Bool       status;

    SWAP_SCREEN (s);

    if (w->id == ss->popupWindow)
    {
	GLenum         filter;
	int            x, y, offX, i;
	float          px, py, pos;

	if (mask & PAINT_WINDOW_OCCLUSION_DETECTION_MASK)
	    return FALSE;

	UNWRAP (ss, s, paintWindow);
	status = (*s->paintWindow) (w, attrib, transform, region, mask);
	WRAP (ss, s, paintWindow, swapPaintWindow);

	if (!(mask & PAINT_WINDOW_TRANSFORMED_MASK) && region->numRects == 0)
	    return TRUE;

	filter = s->display->textureFilter;

	if (swapGetMipmap (s))
	    s->display->textureFilter = GL_LINEAR_MIPMAP_LINEAR;

	glPushAttrib (GL_SCISSOR_BIT);

	glEnable (GL_SCISSOR_TEST);
	glScissor (w->attrib.x, 0, w->width, w->screen->height);

	offX = 0;
	for (i = 0; i < ss->nWindows; i++)
	{
	    x = i % ss->xCount;
	    y = i / ss->xCount;

	    if (x == 0)
		offX = swapGetRowXOffset (s, ss, y);

	    x = x * ss->previewWidth + (x + 1) * ss->previewBorder;
	    y = y * ss->previewHeight + (y + 1) * ss->previewBorder;

	    swapPaintThumb (ss->windows[i], &w->lastPaint, transform,
			    mask, offX + x + w->attrib.x, y + w->attrib.y);
	}

	s->display->textureFilter = filter;

	pos = fmod (ss->pos, ss->nWindows);
	px  = fmod (pos, ss->xCount);
	py  = floor (pos / ss->xCount);

	offX = swapGetRowXOffset (s, ss, py);

	if (pos > ss->nWindows - 1)
	{
	    px = fmod (pos - ss->nWindows, ss->xCount);
	    swapPaintSelectionRect (ss, w->attrib.x, w->attrib.y, px, 0.0,
				    w->lastPaint.opacity);

	    px = fmod (pos, ss->xCount);
	    swapPaintSelectionRect (ss, w->attrib.x + offX, w->attrib.y,
				    px, py, w->lastPaint.opacity);
	}
	if (px > ss->xCount - 1)
	{
	    swapPaintSelectionRect (ss, w->attrib.x, w->attrib.y, px, py,
				    w->lastPaint.opacity);

	    py = fmod (py + 1, ceil ((double) ss->nWindows / ss->xCount));
	    offX = swapGetRowXOffset (s, ss, py);

	    swapPaintSelectionRect (ss, w->attrib.x + offX, w->attrib.y,
				    px - ss->xCount, py,
				    w->lastPaint.opacity);
	}
	else
	{
	    swapPaintSelectionRect (ss, w->attrib.x + offX, w->attrib.y,
				    px, py, w->lastPaint.opacity);
	}
	glDisable (GL_SCISSOR_TEST);
	glPopAttrib ();
    }
    else if (ss->swaping && !ss->popupDelayHandle &&
	     (w->id != ss->selectedWindow))
    {
	WindowPaintAttrib sAttrib = *attrib;
	GLuint            value;

	value = swapGetSaturation (s);
	if (value != 100)
	    sAttrib.saturation = sAttrib.saturation * value / 100;

	value = swapGetBrightness (s);
	if (value != 100)
	    sAttrib.brightness = sAttrib.brightness * value / 100;

	if (w->wmType & ~(CompWindowTypeDockMask | CompWindowTypeDesktopMask))
	{
	    value = swapGetOpacity (s);
	    if (value != 100)
		sAttrib.opacity = sAttrib.opacity * value / 100;
	}

	UNWRAP (ss, s, paintWindow);
	status = (*s->paintWindow) (w, &sAttrib, transform, region, mask);
	WRAP (ss, s, paintWindow, swapPaintWindow);
    }
    else
    {
	UNWRAP (ss, s, paintWindow);
	status = (*s->paintWindow) (w, attrib, transform, region, mask);
	WRAP (ss, s, paintWindow, swapPaintWindow);
    }

    return status;
}

static Bool
swapDamageWindowRect (CompWindow *w,
		      Bool	 initial,
		      BoxPtr     rect)
{
    CompScreen *s = w->screen;
    Bool status;

    SWAP_SCREEN (s);

    if (ss->grabIndex)
    {
	CompWindow *popup;
	int	   i;

	for (i = 0; i < ss->nWindows; i++)
	{
	    if (ss->windows[i] == w)
	    {
		popup = findWindowAtScreen (s, ss->popupWindow);
		if (popup)
		    addWindowDamage (popup);

		break;
	    }
	}
    }

    UNWRAP (ss, s, damageWindowRect);
    status = (*s->damageWindowRect) (w, initial, rect);
    WRAP (ss, s, damageWindowRect, swapDamageWindowRect);

    return status;
}

static Bool
swapInitDisplay (CompPlugin  *p,
		 CompDisplay *d)
{
    SwapDisplay *sd;

    if (!checkPluginABI ("core", CORE_ABIVERSION))
	return FALSE;

    sd = malloc (sizeof (SwapDisplay));
    if (!sd)
	return FALSE;

    sd->screenPrivateIndex = allocateScreenPrivateIndex (d);
    if (sd->screenPrivateIndex < 0)
    {
	free (sd);
	return FALSE;
    }

    swapSetNextButtonInitiate (d, swapNext);
    swapSetNextButtonTerminate (d, swapTerminate);
    swapSetNextKeyInitiate (d, swapNext);
    swapSetNextKeyTerminate (d, swapTerminate);
    swapSetPrevButtonInitiate (d, swapPrev);
    swapSetPrevButtonTerminate (d, swapTerminate);
    swapSetPrevKeyInitiate (d, swapPrev);
    swapSetPrevKeyTerminate (d, swapTerminate);
    swapSetNextAllButtonInitiate (d, swapNextAll);
    swapSetNextAllButtonTerminate (d, swapTerminate);
    swapSetNextAllKeyInitiate (d, swapNextAll);
    swapSetNextAllKeyTerminate (d, swapTerminate);
    swapSetPrevAllButtonInitiate (d, swapPrevAll);
    swapSetPrevAllButtonTerminate (d, swapTerminate);
    swapSetPrevAllKeyInitiate (d, swapPrevAll);
    swapSetPrevAllKeyTerminate (d, swapTerminate);
    swapSetNextGroupButtonInitiate (d, swapNextGroup);
    swapSetNextGroupButtonTerminate (d, swapTerminate);
    swapSetNextGroupKeyInitiate (d, swapNextGroup);
    swapSetNextGroupKeyTerminate (d, swapTerminate);
    swapSetPrevGroupButtonInitiate (d, swapPrevGroup);
    swapSetPrevGroupButtonTerminate (d, swapTerminate);
    swapSetPrevGroupKeyInitiate (d, swapPrevGroup);
    swapSetPrevGroupKeyTerminate (d, swapTerminate);
    swapSetNextNoPopupButtonInitiate (d, swapNextNoPopup);
    swapSetNextNoPopupButtonTerminate (d, swapTerminate);
    swapSetNextNoPopupKeyInitiate (d, swapNextNoPopup);
    swapSetNextNoPopupKeyTerminate (d, swapTerminate);
    swapSetPrevNoPopupButtonInitiate (d, swapPrevNoPopup);
    swapSetPrevNoPopupButtonTerminate (d, swapTerminate);
    swapSetPrevNoPopupKeyInitiate (d, swapPrevNoPopup);
    swapSetPrevNoPopupKeyTerminate (d, swapTerminate);
    swapSetNextPanelButtonInitiate (d, swapNextPanel);
    swapSetNextPanelButtonTerminate (d, swapTerminate);
    swapSetNextPanelKeyInitiate (d, swapNextPanel);
    swapSetNextPanelKeyTerminate (d, swapTerminate);
    swapSetPrevPanelButtonInitiate (d, swapPrevPanel);
    swapSetPrevPanelButtonTerminate (d, swapTerminate);
    swapSetPrevPanelKeyInitiate (d, swapPrevPanel);
    swapSetPrevPanelKeyTerminate (d, swapTerminate);

    sd->selectWinAtom     = XInternAtom (d->display,
					 DECOR_SWITCH_WINDOW_ATOM_NAME, 0);
    sd->selectFgColorAtom =
	XInternAtom (d->display, DECOR_SWITCH_FOREGROUND_COLOR_ATOM_NAME, 0);

    WRAP (sd, d, handleEvent, swapHandleEvent);

    d->base.privates[SwapDisplayPrivateIndex].ptr = sd;

    return TRUE;
}

static void
swapFiniDisplay (CompPlugin  *p,
		 CompDisplay *d)
{
    SWAP_DISPLAY (d);

    freeScreenPrivateIndex (d, sd->screenPrivateIndex);

    UNWRAP (sd, d, handleEvent);

    free (sd);
}

static Bool
swapInitScreen (CompPlugin *p,
		CompScreen *s)
{
    SwapScreen *ss;

    SWAP_DISPLAY (s->display);

    ss = malloc (sizeof (SwapScreen));
    if (!ss)
	return FALSE;

    ss->popupWindow      = None;
    ss->popupDelayHandle = 0;

    ss->selectedWindow = None;
    ss->clientLeader   = None;

    ss->windows     = 0;
    ss->nWindows    = 0;
    ss->windowsSize = 0;

    ss->pos  = 0;
    ss->move = 0;

    ss->swaping = FALSE;
    ss->grabIndex = 0;

    ss->moreAdjust = 0;
    ss->mVelocity  = 0.0f;

    ss->selection = CurrentViewport;

    ss->fgColor[0] = 0;
    ss->fgColor[1] = 0;
    ss->fgColor[2] = 0;
    ss->fgColor[3] = 0xffff;

    WRAP (ss, s, preparePaintScreen, swapPreparePaintScreen);
    WRAP (ss, s, donePaintScreen, swapDonePaintScreen);
    WRAP (ss, s, paintOutput, swapPaintOutput);
    WRAP (ss, s, paintWindow, swapPaintWindow);
    WRAP (ss, s, damageWindowRect, swapDamageWindowRect);

    s->base.privates[sd->screenPrivateIndex].ptr = ss;

    return TRUE;
}

static void
swapFiniScreen (CompPlugin *p,
		CompScreen *s)
{
    SWAP_SCREEN (s);

    UNWRAP (ss, s, preparePaintScreen);
    UNWRAP (ss, s, donePaintScreen);
    UNWRAP (ss, s, paintOutput);
    UNWRAP (ss, s, paintWindow);
    UNWRAP (ss, s, damageWindowRect);

    if (ss->popupDelayHandle)
	compRemoveTimeout (ss->popupDelayHandle);

    if (ss->popupWindow)
	XDestroyWindow (s->display->display, ss->popupWindow);

    if (ss->windows)
	free (ss->windows);

    free (ss);
}

static CompBool
swapInitObject (CompPlugin *p,
		CompObject *o)
{
    static InitPluginObjectProc dispTab[] = {
	(InitPluginObjectProc) 0, /* InitCore */
	(InitPluginObjectProc) swapInitDisplay,
	(InitPluginObjectProc) swapInitScreen
    };

    RETURN_DISPATCH (o, dispTab, ARRAY_SIZE (dispTab), TRUE, (p, o));
}

static void
swapFiniObject (CompPlugin *p,
		CompObject *o)
{
    static FiniPluginObjectProc dispTab[] = {
	(FiniPluginObjectProc) 0, /* FiniCore */
	(FiniPluginObjectProc) swapFiniDisplay,
	(FiniPluginObjectProc) swapFiniScreen
    };

    DISPATCH (o, dispTab, ARRAY_SIZE (dispTab), (p, o));
}

static Bool
swapInit (CompPlugin *p)
{
    SwapDisplayPrivateIndex = allocateDisplayPrivateIndex ();
    if (SwapDisplayPrivateIndex < 0)
	return FALSE;

    return TRUE;
}

static void
swapFini (CompPlugin *p)
{
    freeDisplayPrivateIndex (SwapDisplayPrivateIndex);
}

CompPluginVTable swapVTable = {
    "swap",
    0,
    swapInit,
    swapFini,
    swapInitObject,
    swapFiniObject,
    0,
    0
};

CompPluginVTable *
getCompPluginInfo (void)
{
    return &swapVTable;
}
