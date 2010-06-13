/*
 * Copyright (c) 1999, 2010, Oracle and/or its affiliates. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
/*

Copyright 1987, 1998  The Open Group

Permission to use, copy, modify, distribute, and sell this software and its
documentation for any purpose is hereby granted without fee, provided that
the above copyright notice appear in all copies and that both that
copyright notice and this permission notice appear in supporting
documentation.

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
OF THIRD PARTY RIGHTS. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
HOLDERS INCLUDED IN THIS NOTICE BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL
INDIRECT OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES WHATSOEVER RESULTING
FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION
WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

Except as contained in this notice, the name of a copyright holder
shall not be used in advertising or otherwise to promote the sale, use
or other dealings in this Software without prior written authorization
of the copyright holder.

*/


/*
 * xwininfo.c	- MIT Project Athena, X Window system window
 *		  information utility.
 *
 *
 *	This program will report all relevant information
 *	about a specific window.
 *
 *  Author:	Mark Lillibridge, MIT Project Athena
 *		16-Jun-87
 */

#include "config.h"

#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include <xcb/xcb_icccm.h>
#include <xcb/shape.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>

/* Include routines to handle parsing defaults */
#include "dsimple.h"

typedef struct {
    long code;
    const char *name;
} binding;

/* Information we keep track of for each window to allow prefetching/reusing */
struct wininfo {
    xcb_window_t			window;

    /* cookies for requests we've sent */
    xcb_get_geometry_cookie_t		geometry_cookie;
    xcb_get_property_cookie_t		wm_name_cookie;
    xcb_get_property_cookie_t		wm_class_cookie;
    xcb_translate_coordinates_cookie_t	trans_coords_cookie;
    xcb_query_tree_cookie_t		tree_cookie;
    xcb_get_window_attributes_cookie_t	attr_cookie;
    xcb_get_property_cookie_t		normal_hints_cookie;
    xcb_get_property_cookie_t		hints_cookie;
    xcb_get_property_cookie_t		zoom_cookie;

    /* cached results from previous requests */
    xcb_get_geometry_reply_t *		geometry;
    xcb_get_window_attributes_reply_t *	win_attributes;
    xcb_size_hints_t *			normal_hints;
};

static void scale_init (xcb_screen_t *scrn);
static char *nscale (int, int, int, char *, size_t);
static char *xscale (int);
static char *yscale (int);
static char *bscale (int);
int main (int, char **);
static const char *LookupL (long, const binding *);
static const char *Lookup (int, const binding *);
static void Display_Window_Id (struct wininfo *, Bool);
static void Display_Stats_Info (struct wininfo *);
static void Display_Bits_Info (struct wininfo *);
static void Display_Event_Mask (long);
static void Display_Events_Info (struct wininfo *);
static void Display_Tree_Info (struct wininfo *, int);
static void display_tree_info_1 (struct wininfo *, int, int);
static void Display_Hints (xcb_size_hints_t *);
static void Display_Size_Hints (struct wininfo *);
static void Display_Window_Shape (xcb_window_t);
static void Display_WM_Info (struct wininfo *);
static void wininfo_wipe (struct wininfo *);

static const char *window_id_format = "0x%lx";

static xcb_connection_t *dpy;
static xcb_screen_t *screen;
static xcb_generic_error_t *err;

#ifndef HAVE_STRLCAT
static size_t strlcat (char *dst, const char *src, size_t dstsize)
{
    size_t sd = strlen (dst);
    size_t ss = strlen (src);
    size_t s = sd + ss;

    if (s < dstsize) {
	strcpy (dst + sd, src);
    } else {
	strncpy (dst + sd, src, dstsize-sd-1);
	dst[dstsize] = '\0';
    }
    return s;
}
#endif

/*
 * Report the syntax for calling xwininfo:
 */
void
usage (void)
{
    fprintf (stderr,
	     "usage:  %s [-options ...]\n\n"
	     "where options include:\n"
	     "    -help                print this message\n"
	     "    -display host:dpy    X server to contact\n"
	     "    -root                use the root window\n"
	     "    -id windowid         use the window with the specified id\n"
	     "    -name windowname     use the window with the specified name\n"
	     "    -int                 print window id in decimal\n"
	     "    -children            print parent and child identifiers\n"
	     "    -tree                print children identifiers recursively\n"
	     "    -stats               print window geometry [DEFAULT]\n"
	     "    -bits                print window pixel information\n"
	     "    -events              print events selected for on window\n"
	     "    -size                print size hints\n"
	     "    -wm                  print window manager hints\n"
	     "    -shape               print shape extents\n"
	     "    -frame               don't ignore window manager frames\n"
	     "    -english             print sizes in english units\n"
	     "    -metric              print sizes in metric units\n"
	     "    -all                 -tree, -stats, -bits, -events, -wm, -size, -shape\n"
	     "\n",
	     program_name);
    exit (1);
}

/*
 * pixel to inch, metric converter.
 * Hacked in by Mark W. Eichin <eichin@athena> [eichin:19880619.1509EST]
 *
 * Simply put: replace the old numbers with string print calls.
 * Returning a local string is ok, since we only ever get called to
 * print one x and one y, so as long as they don't collide, they're
 * fine. This is not meant to be a general purpose routine.
 *
 */

static int xp = 0, xmm = 0;
static int yp = 0, ymm = 0;
static int bp = 0, bmm = 0;
static int english = 0, metric = 0;

static void
scale_init (xcb_screen_t *screen)
{
    xp = screen->width_in_pixels;
    yp = screen->height_in_pixels;
    xmm = screen->width_in_millimeters;
    ymm = screen->height_in_millimeters;
    bp  = xp  + yp;
    bmm = xmm + ymm;
}

#define MILE (5280*12)
#define YARD (3*12)
#define FOOT (12)

static char *
nscale (int n, int np, int nmm, char *nbuf, size_t nbufsize)
{
    int s;
    snprintf (nbuf, nbufsize, "%d", n);

    if (metric||english) {
	s = strlcat (nbuf, " (", nbufsize);

	if (metric) {
	    snprintf (nbuf+s, nbufsize-s, "%.2f mm%s",
		      ((double) n) * nmm/np , english ? "; " : "");
	}
	if (english) {
	    double inch_frac;
	    Bool printed_anything = False;
	    int mi, yar, ft, inr;

	    inch_frac = ((double) n)*(nmm/25.4)/np;
	    inr = (int)inch_frac;
	    inch_frac -= (double)inr;
	    if (inr >= MILE) {
		mi = inr/MILE;
		inr %= MILE;
		s = strlen (nbuf);
		snprintf (nbuf+s, nbufsize-s, "%d %s(?!?)",
			  mi, (mi == 1) ? "mile" : "miles");
		printed_anything = True;
	    }
	    if (inr >= YARD) {
		yar = inr/YARD;
		inr %= YARD;
		if (printed_anything)
		    strlcat (nbuf, ", ", nbufsize);
		s = strlen (nbuf);
		snprintf (nbuf+s, nbufsize-s, "%d %s",
			 yar, (yar==1) ? "yard" : "yards");
		printed_anything = True;
	    }
	    if (inr >= FOOT) {
		ft = inr/FOOT;
		inr  %= FOOT;
		if (printed_anything)
		    strlcat (nbuf, ", ", nbufsize);
		s = strlen (nbuf);
		snprintf (nbuf+s, nbufsize-s, "%d %s",
			 ft, (ft==1) ? "foot" : "feet");
		printed_anything = True;
	    }
	    if (!printed_anything || inch_frac != 0.0 || inr != 0) {
		if (printed_anything)
		    strlcat (nbuf, ", ", nbufsize);
		s = strlen (nbuf);
		snprintf (nbuf+s, nbufsize-s, "%.2f inches", inr+inch_frac);
	    }
	}
	strlcat (nbuf, ")", nbufsize);
    }
    return (nbuf);
}

static char xbuf[BUFSIZ];
static char *
xscale (int x)
{
    return (nscale (x, xp, xmm, xbuf, sizeof(xbuf)));
}

static char ybuf[BUFSIZ];
static char *
yscale (int y)
{
    return (nscale (y, yp, ymm, ybuf, sizeof(ybuf)));
}

static char bbuf[BUFSIZ];
static char *
bscale (int b)
{
    return (nscale (b, bp, bmm, bbuf, sizeof(bbuf)));
}

/* end of pixel to inch, metric converter */

int
main (int argc, char **argv)
{
    register int i;
    int tree = 0, stats = 0, bits = 0, events = 0, wm = 0, size = 0, shape = 0;
    int frame = 0, children = 0;
    int use_root = 0;
    xcb_window_t window = 0;
    char *display_name = NULL;
    const char *window_name = NULL;
    struct wininfo wininfo;
    struct wininfo *w = &wininfo;

    program_name = argv[0];

    if (!setlocale (LC_ALL, ""))
	fprintf (stderr, "%s: can not set locale properly\n", program_name);

    memset (w, 0, sizeof(struct wininfo));

    /* Handle our command line arguments */
    for (i = 1; i < argc; i++) {
	if (!strcmp (argv[i], "-help"))
	    usage ();
	if (!strcmp (argv[i], "-display") || !strcmp (argv[i], "-d")) {
	    if (++i >= argc)
		Fatal_Error("-display requires argument");
	    display_name = argv[i];
	    continue;
	}
	if (!strcmp (argv[i], "-root")) {
	    use_root = 1;
	    continue;
	}
	if (!strcmp (argv[i], "-id")) {
	    if (++i >= argc)
		Fatal_Error("-id requires argument");
	    window = strtoul(argv[i], NULL, 0);
	    continue;
	}
	if (!strcmp (argv[i], "-name")) {
	    if (++i >= argc)
		Fatal_Error("-name requires argument");
	    window_name = argv[i];
	    continue;
	}
	if (!strcmp (argv[i], "-int")) {
	    window_id_format = "%ld";
	    continue;
	}
	if (!strcmp (argv[i], "-children")) {
	    children = 1;
	    continue;
	}
	if (!strcmp (argv[i], "-tree")) {
	    tree = 1;
	    continue;
	}
	if (!strcmp (argv[i], "-stats")) {
	    stats = 1;
	    continue;
	}
	if (!strcmp (argv[i], "-bits")) {
	    bits = 1;
	    continue;
	}
	if (!strcmp (argv[i], "-events")) {
	    events = 1;
	    continue;
	}
	if (!strcmp (argv[i], "-wm")) {
	    wm = 1;
	    continue;
	}
	if (!strcmp (argv[i], "-frame")) {
	    frame = 1;
	    continue;
	}
	if (!strcmp (argv[i], "-size")) {
	    size = 1;
	    continue;
	}
	if (!strcmp (argv[i], "-shape")) {
	    shape = 1;
	    continue;
	}
	if (!strcmp (argv[i], "-english")) {
	    english = 1;
	    continue;
	}
	if (!strcmp (argv[i], "-metric")) {
	    metric = 1;
	    continue;
	}
	if (!strcmp (argv[i], "-all")) {
	    tree = stats = bits = events = wm = size = shape = 1;
	    continue;
	}
	usage ();
    }

    Setup_Display_And_Screen (display_name, &dpy, &screen);

    /* initialize scaling data */
    scale_init(screen);

    if (use_root)
	window = screen->root;
    else if (window_name) {
	window = Window_With_Name (dpy, screen->root, window_name);
	if (!window)
	    Fatal_Error ("No window with name \"%s\" exists!", window_name);
    }

    /* If no window selected on command line, let user pick one the hard way */
    if (!window) {
	printf ("\n"
		"xwininfo: Please select the window about which you\n"
		"          would like information by clicking the\n"
		"          mouse in that window.\n");
	window = Select_Window (dpy, screen, !frame);
    }

    /*
     * Do the actual displaying as per parameters
     */
    if (!(children || tree || bits || events || wm || size))
	stats = 1;

    /*
     * make sure that the window is valid
     */
    {
	xcb_get_geometry_cookie_t gg_cookie =
	    xcb_get_geometry (dpy, window);

	w->geometry = xcb_get_geometry_reply(dpy, gg_cookie, &err);

	if (!w->geometry) {
	    char badid[20];

	    if (err)
		Print_X_Error (dpy, err);

	    snprintf (badid, sizeof(badid), window_id_format, window);
	    Fatal_Error ("No such window with id %s.", badid);
	}
    }

    /* Send requests to prefetch data we'll need */
    w->window = window;
    w->wm_name_cookie = xcb_get_wm_name (dpy, window);
    if (children || tree)
	w->tree_cookie = xcb_query_tree (dpy, window);
    if (stats) {
	w->trans_coords_cookie =
	    xcb_translate_coordinates (dpy, window, w->geometry->root,
				       -(w->geometry->border_width),
				       -(w->geometry->border_width));
    }
    if (stats || bits || events)
	w->attr_cookie = xcb_get_window_attributes (dpy, window);
    if (stats || size)
	w->normal_hints_cookie = xcb_get_wm_normal_hints (dpy, window);
    if (wm)
	w->hints_cookie = xcb_get_wm_hints(dpy, window);
    if (size)
	w->zoom_cookie = xcb_get_wm_size_hints (dpy, window,
						XCB_ATOM_WM_ZOOM_HINTS);
    xcb_flush (dpy);

    printf ("\nxwininfo: Window id: ");
    Display_Window_Id (w, True);
    if (children || tree)
	Display_Tree_Info (w, tree);
    if (stats)
	Display_Stats_Info (w);
    if (bits)
	Display_Bits_Info (w);
    if (events)
	Display_Events_Info (w);
    if (wm)
	Display_WM_Info (w);
    if (size)
	Display_Size_Hints (w);
    if (shape)
	Display_Window_Shape (window);
    printf ("\n");

    wininfo_wipe (w);
    xcb_disconnect (dpy);
    exit (0);
}

/* Ensure win_attributes field is filled in */
static xcb_get_window_attributes_reply_t *
fetch_win_attributes (struct wininfo *w)
{
    if (!w->win_attributes) {
	w->win_attributes =
	    xcb_get_window_attributes_reply (dpy, w->attr_cookie, &err);

	if (!w->win_attributes) {
	    Print_X_Error (dpy, err);
	    Fatal_Error ("Can't get window attributes.");
	}
    }
    return w->win_attributes;
}

/* Ensure normal_hints field is filled in */
static xcb_size_hints_t *
fetch_normal_hints (struct wininfo *w, xcb_size_hints_t *hints_return)
{
    xcb_size_hints_t hints;

    if (!w->normal_hints) {
	if (xcb_get_wm_normal_hints_reply (dpy, w->normal_hints_cookie,
					   &hints, NULL)) {
	    w->normal_hints = malloc (sizeof(xcb_size_hints_t));
	    if (w->normal_hints)
		memcpy(w->normal_hints, &hints, sizeof(xcb_size_hints_t));
	}
    }
    if (hints_return && w->normal_hints)
	memcpy(hints_return, w->normal_hints, sizeof(xcb_size_hints_t));
    return w->normal_hints;
}


/*
 * Lookup: lookup a code in a table.
 */
static char _lookup_buffer[100];

static const char *
LookupL (long code, const binding *table)
{
    const char *name = NULL;

    while (table->name) {
	if (table->code == code) {
	    name = table->name;
	    break;
	}
	table++;
    }

    if (name == NULL) {
	snprintf (_lookup_buffer, sizeof(_lookup_buffer),
		  "unknown (code = %ld. = 0x%lx)", code, code);
	name = _lookup_buffer;
    }

    return (name);
}

static const char *
Lookup (int code, const binding *table)
{
    return LookupL ((long)code, table);
}

/*
 * Routine to display a window id in dec/hex with name if window has one
 *
 * Requires wininfo members initialized: window, wm_name_cookie
 */

static void
Display_Window_Id (struct wininfo *w, Bool newline_wanted)
{
    xcb_get_text_property_reply_t prop;
    uint8_t got_reply;

    printf (window_id_format, w->window);      /* print id # in hex/dec */

    if (!w->window) {
	printf (" (none)");
    } else {
	if (w->window == screen->root) {
	    printf (" (the root window)");
	}
	/* Get window name if any */
	got_reply = xcb_get_wm_name_reply (dpy, w->wm_name_cookie,
					   &prop, NULL);
	if (!got_reply || prop.name_len == 0) {
	    printf (" (has no name)");
        } else {
            printf (" \"");
	    /* XXX: need to handle encoding */
	    printf ("%.*s", prop.name_len, prop.name);
            printf ("\"");
	}
	if (got_reply)
	    xcb_get_text_property_reply_wipe (&prop);
    }

    if (newline_wanted)
	printf ("\n");

    return;
}


/*
 * Display Stats on window
 */
static const binding _window_classes[] = {
	{ XCB_WINDOW_CLASS_INPUT_OUTPUT, "InputOutput" },
	{ XCB_WINDOW_CLASS_INPUT_ONLY, "InputOnly" },
        { 0, NULL } };

static const binding _map_states[] = {
	{ XCB_MAP_STATE_UNMAPPED,	"IsUnMapped" },
	{ XCB_MAP_STATE_UNVIEWABLE,	"IsUnviewable" },
	{ XCB_MAP_STATE_VIEWABLE,	"IsViewable" },
	{ 0, NULL } };

static const binding _backing_store_states[] = {
	{ XCB_BACKING_STORE_NOT_USEFUL, "NotUseful" },
	{ XCB_BACKING_STORE_WHEN_MAPPED,"WhenMapped" },
	{ XCB_BACKING_STORE_ALWAYS,	"Always" },
	{ 0, NULL } };

static const binding _bit_gravity_states[] = {
	{ XCB_GRAVITY_BIT_FORGET,	"ForgetGravity" },
	{ XCB_GRAVITY_NORTH_WEST,	"NorthWestGravity" },
	{ XCB_GRAVITY_NORTH,		"NorthGravity" },
	{ XCB_GRAVITY_NORTH_EAST,	"NorthEastGravity" },
	{ XCB_GRAVITY_WEST,		"WestGravity" },
	{ XCB_GRAVITY_CENTER,		"CenterGravity" },
	{ XCB_GRAVITY_EAST,		"EastGravity" },
	{ XCB_GRAVITY_SOUTH_WEST,	"SouthWestGravity" },
	{ XCB_GRAVITY_SOUTH,		"SouthGravity" },
	{ XCB_GRAVITY_SOUTH_EAST,	"SouthEastGravity" },
	{ XCB_GRAVITY_STATIC,		"StaticGravity" },
	{ 0, NULL }};

static const binding _window_gravity_states[] = {
	{ XCB_GRAVITY_WIN_UNMAP,	"UnmapGravity" },
	{ XCB_GRAVITY_NORTH_WEST,	"NorthWestGravity" },
	{ XCB_GRAVITY_NORTH,		"NorthGravity" },
	{ XCB_GRAVITY_NORTH_EAST,	"NorthEastGravity" },
	{ XCB_GRAVITY_WEST,		"WestGravity" },
	{ XCB_GRAVITY_CENTER,		"CenterGravity" },
	{ XCB_GRAVITY_EAST,		"EastGravity" },
	{ XCB_GRAVITY_SOUTH_WEST,	"SouthWestGravity" },
	{ XCB_GRAVITY_SOUTH,		"SouthGravity" },
	{ XCB_GRAVITY_SOUTH_EAST,	"SouthEastGravity" },
	{ XCB_GRAVITY_STATIC,		"StaticGravity" },
	{ 0, NULL }};

static const binding _visual_classes[] = {
	{ XCB_VISUAL_CLASS_STATIC_GRAY,	"StaticGray" },
	{ XCB_VISUAL_CLASS_GRAY_SCALE,	"GrayScale" },
	{ XCB_VISUAL_CLASS_STATIC_COLOR,"StaticColor" },
	{ XCB_VISUAL_CLASS_PSEUDO_COLOR,"PseudoColor" },
	{ XCB_VISUAL_CLASS_TRUE_COLOR,	"TrueColor" },
	{ XCB_VISUAL_CLASS_DIRECT_COLOR,"DirectColor" },
	{ 0, NULL }};

/*
 * Requires wininfo members initialized:
 *   window, geometry, attr_cookie, trans_coords_cookie, normal_hints_cookie
 */
static void
Display_Stats_Info (struct wininfo *w)
{
    xcb_translate_coordinates_reply_t *trans_coords;
    xcb_get_window_attributes_reply_t *win_attributes;
    xcb_size_hints_t hints;

    int dw = screen->width_in_pixels, dh = screen->height_in_pixels;
    int rx, ry, xright, ybelow;
    int showright = 0, showbelow = 0;
    xcb_window_t wmframe, parent;

    trans_coords =
	xcb_translate_coordinates_reply (dpy, w->trans_coords_cookie, NULL);
    if (!trans_coords)
	Fatal_Error ("Can't get translated coordinates.");

    rx = trans_coords->dst_x;
    ry = trans_coords->dst_y;
    free (trans_coords);

    xright = (dw - rx - w->geometry->border_width * 2 -
	      w->geometry->width);
    ybelow = (dh - ry - w->geometry->border_width * 2 -
	      w->geometry->height);


    printf ("\n");
    printf ("  Absolute upper-left X:  %s\n", xscale (rx));
    printf ("  Absolute upper-left Y:  %s\n", yscale (ry));
    printf ("  Relative upper-left X:  %s\n", xscale (w->geometry->x));
    printf ("  Relative upper-left Y:  %s\n", yscale (w->geometry->y));
    printf ("  Width: %s\n", xscale (w->geometry->width));
    printf ("  Height: %s\n", yscale (w->geometry->height));
    printf ("  Depth: %d\n", w->geometry->depth);

    win_attributes = fetch_win_attributes (w);

    printf ("  Visual: 0x%lx\n", (unsigned long) win_attributes->visual);
    if (screen)
    {
	xcb_depth_iterator_t depth_iter;
	xcb_visualtype_t  *visual_type = NULL;

	depth_iter = xcb_screen_allowed_depths_iterator (screen);
	for (; depth_iter.rem; xcb_depth_next (&depth_iter)) {
	    xcb_visualtype_iterator_t visual_iter;

	    visual_iter = xcb_depth_visuals_iterator (depth_iter.data);
	    for (; visual_iter.rem; xcb_visualtype_next (&visual_iter)) {
		if (screen->root_visual == visual_iter.data->visual_id) {
		    visual_type = visual_iter.data;
		    break;
		}
	    }
	}
	if (visual_type)
	    printf ("  Visual Class: %s\n", Lookup (visual_type->_class,
						    _visual_classes));
    }

    printf ("  Border width: %s\n", bscale (w->geometry->border_width));
    printf ("  Class: %s\n",
	    Lookup (win_attributes->_class, _window_classes));
    printf ("  Colormap: 0x%lx (%sinstalled)\n",
	    (unsigned long) win_attributes->colormap,
	    win_attributes->map_is_installed ? "" : "not ");
    printf ("  Bit Gravity State: %s\n",
	    Lookup (win_attributes->bit_gravity, _bit_gravity_states));
    printf ("  Window Gravity State: %s\n",
	    Lookup (win_attributes->win_gravity, _window_gravity_states));
    printf ("  Backing Store State: %s\n",
	    Lookup (win_attributes->backing_store, _backing_store_states));
    printf ("  Save Under State: %s\n",
	    win_attributes->save_under ? "yes" : "no");
    printf ("  Map State: %s\n",
	    Lookup (win_attributes->map_state, _map_states));
    printf ("  Override Redirect State: %s\n",
	    win_attributes->override_redirect ? "yes" : "no");
    printf ("  Corners:  +%d+%d  -%d+%d  -%d-%d  +%d-%d\n",
	    rx, ry, xright, ry, xright, ybelow, rx, ybelow);

    /*
     * compute geometry string that would recreate window
     */
    printf ("  -geometry ");

    /* compute size in appropriate units */
    if (!fetch_normal_hints (w, &hints))
	hints.flags = 0;

    if ((hints.flags & XCB_SIZE_HINT_P_RESIZE_INC)  &&
	(hints.width_inc != 0)  && (hints.height_inc != 0)) {
	if (hints.flags & (XCB_SIZE_HINT_BASE_SIZE|XCB_SIZE_HINT_P_MIN_SIZE)) {
	    if (hints.flags & XCB_SIZE_HINT_BASE_SIZE) {
		w->geometry->width -= hints.base_width;
		w->geometry->height -= hints.base_height;
	    } else {
		/* ICCCM says MinSize is default for BaseSize */
		w->geometry->width -= hints.min_width;
		w->geometry->height -= hints.min_height;
	    }
	}
	printf ("%dx%d", w->geometry->width/hints.width_inc,
		w->geometry->height/hints.height_inc);
    } else
	printf ("%dx%d", w->geometry->width, w->geometry->height);

    if (!(hints.flags & XCB_SIZE_HINT_P_WIN_GRAVITY))
	hints.win_gravity = XCB_GRAVITY_NORTH_WEST; /* per ICCCM */
    /* find our window manager frame, if any */
    for (wmframe = parent = w->window; parent != 0 ; wmframe = parent) {
	xcb_query_tree_cookie_t qt_cookie;
	xcb_query_tree_reply_t *tree;

	qt_cookie = xcb_query_tree (dpy, wmframe);
	tree = xcb_query_tree_reply (dpy, qt_cookie, &err);
	if (!tree) {
	    Print_X_Error (dpy, err);
	    Fatal_Error ("Can't query window tree.");
	}
	parent = tree->parent;
	free (tree);
	if (parent == w->geometry->root || !parent)
	    break;
    }
    if (wmframe != w->window) {
	/* WM reparented, so find edges of the frame */
	/* Only works for ICCCM-compliant WMs, and then only if the
	   window has corner gravity.  We would need to know the original width
	   of the window to correctly handle the other gravities. */
	xcb_get_geometry_cookie_t geom_cookie;
	xcb_get_geometry_reply_t *frame_geometry;

	geom_cookie = xcb_get_geometry (dpy, wmframe);
	frame_geometry = xcb_get_geometry_reply (dpy, geom_cookie, &err);

	if (!frame_geometry) {
	    Print_X_Error (dpy, err);
	    Fatal_Error ("Can't get frame geometry.");
	}
	switch (hints.win_gravity) {
	    case XCB_GRAVITY_NORTH_WEST: case XCB_GRAVITY_SOUTH_WEST:
	    case XCB_GRAVITY_NORTH_EAST: case XCB_GRAVITY_SOUTH_EAST:
	    case XCB_GRAVITY_WEST:
		rx = frame_geometry->x;
	}
	switch (hints.win_gravity) {
	    case XCB_GRAVITY_NORTH_WEST: case XCB_GRAVITY_SOUTH_WEST:
	    case XCB_GRAVITY_NORTH_EAST: case XCB_GRAVITY_SOUTH_EAST:
	    case XCB_GRAVITY_EAST:
		xright = dw - frame_geometry->x - frame_geometry->width -
		    (2 * frame_geometry->border_width);
	}
	switch (hints.win_gravity) {
	    case XCB_GRAVITY_NORTH_WEST: case XCB_GRAVITY_SOUTH_WEST:
	    case XCB_GRAVITY_NORTH_EAST: case XCB_GRAVITY_SOUTH_EAST:
	    case XCB_GRAVITY_NORTH:
		ry = frame_geometry->y;
	}
	switch (hints.win_gravity) {
	    case XCB_GRAVITY_NORTH_WEST: case XCB_GRAVITY_SOUTH_WEST:
	    case XCB_GRAVITY_NORTH_EAST: case XCB_GRAVITY_SOUTH_EAST:
	    case XCB_GRAVITY_SOUTH:
		ybelow = dh - frame_geometry->y - frame_geometry->height -
		    (2 * frame_geometry->border_width);
	}
	free (frame_geometry);
    }
    /* If edge gravity, offer a corner on that edge (because the application
       programmer cares about that edge), otherwise offer upper left unless
       some other corner is close to an edge of the screen.
       (For corner gravity, assume gravity was set by XWMGeometry.
       For CenterGravity, it doesn't matter.) */
    if (hints.win_gravity == XCB_GRAVITY_EAST  ||
	(abs (xright) <= 100  &&  abs (xright) < abs (rx)
	 &&  hints.win_gravity != XCB_GRAVITY_WEST))
	showright = 1;
    if (hints.win_gravity == XCB_GRAVITY_SOUTH  ||
	(abs (ybelow) <= 100  &&  abs (ybelow) < abs (ry)
	 &&  hints.win_gravity != XCB_GRAVITY_NORTH))
	showbelow = 1;

    if (showright)
	printf ("-%d", xright);
    else
	printf ("+%d", rx);
    if (showbelow)
	printf ("-%d", ybelow);
    else
	printf ("+%d", ry);
    printf ("\n");
}


/*
 * Display bits info:
 */
static const binding _gravities[] = {
    /* WARNING: the first two of these have the same value - see code */
	{ XCB_GRAVITY_WIN_UNMAP,	"UnMapGravity" },
	{ XCB_GRAVITY_BIT_FORGET,	"ForgetGravity" },
	{ XCB_GRAVITY_NORTH_WEST,	"NorthWestGravity" },
	{ XCB_GRAVITY_NORTH,		"NorthGravity" },
	{ XCB_GRAVITY_NORTH_EAST,	"NorthEastGravity" },
	{ XCB_GRAVITY_WEST,		"WestGravity" },
	{ XCB_GRAVITY_CENTER,		"CenterGravity" },
	{ XCB_GRAVITY_EAST,		"EastGravity" },
	{ XCB_GRAVITY_SOUTH_WEST,	"SouthWestGravity" },
	{ XCB_GRAVITY_SOUTH,		"SouthGravity" },
	{ XCB_GRAVITY_SOUTH_EAST,	"SouthEastGravity" },
	{ XCB_GRAVITY_STATIC,		"StaticGravity" },
	{ 0, NULL } };

static const binding _backing_store_hint[] = {
	{ XCB_BACKING_STORE_NOT_USEFUL, "NotUseful" },
	{ XCB_BACKING_STORE_WHEN_MAPPED,"WhenMapped" },
	{ XCB_BACKING_STORE_ALWAYS,	"Always" },
	{ 0, NULL } };

static const binding _bool[] = {
	{ 0, "No" },
	{ 1, "Yes" },
	{ 0, NULL } };

/*
 * Requires wininfo members initialized:
 *   window, attr_cookie (or win_attributes)
 */
static void
Display_Bits_Info (struct wininfo * w)
{
    xcb_get_window_attributes_reply_t *win_attributes
	= fetch_win_attributes (w);

    printf ("\n");
    printf ("  Bit gravity: %s\n",
	    Lookup (win_attributes->bit_gravity, _gravities+1));
    printf ("  Window gravity: %s\n",
	    Lookup (win_attributes->win_gravity, _gravities));
    printf ("  Backing-store hint: %s\n",
	    Lookup (win_attributes->backing_store, _backing_store_hint));
    printf ("  Backing-planes to be preserved: 0x%lx\n",
	    (unsigned long) win_attributes->backing_planes);
    printf ("  Backing pixel: %ld\n",
	    (unsigned long) win_attributes->backing_pixel);
    printf ("  Save-unders: %s\n",
	    Lookup (win_attributes->save_under, _bool));
}


/*
 * Routine to display all events in an event mask
 */
static const binding _event_mask_names[] = {
	{ XCB_EVENT_MASK_KEY_PRESS,		"KeyPress" },
	{ XCB_EVENT_MASK_KEY_RELEASE,		"KeyRelease" },
	{ XCB_EVENT_MASK_BUTTON_PRESS,		"ButtonPress" },
	{ XCB_EVENT_MASK_BUTTON_RELEASE,	"ButtonRelease" },
	{ XCB_EVENT_MASK_ENTER_WINDOW,		"EnterWindow" },
	{ XCB_EVENT_MASK_LEAVE_WINDOW,		"LeaveWindow" },
	{ XCB_EVENT_MASK_POINTER_MOTION,	"PointerMotion" },
	{ XCB_EVENT_MASK_POINTER_MOTION_HINT,	"PointerMotionHint" },
	{ XCB_EVENT_MASK_BUTTON_1_MOTION,	"Button1Motion" },
	{ XCB_EVENT_MASK_BUTTON_2_MOTION,	"Button2Motion" },
	{ XCB_EVENT_MASK_BUTTON_3_MOTION,	"Button3Motion" },
	{ XCB_EVENT_MASK_BUTTON_4_MOTION,	"Button4Motion" },
	{ XCB_EVENT_MASK_BUTTON_5_MOTION,	"Button5Motion" },
	{ XCB_EVENT_MASK_BUTTON_MOTION,		"ButtonMotion" },
	{ XCB_EVENT_MASK_KEYMAP_STATE,		"KeymapState" },
	{ XCB_EVENT_MASK_EXPOSURE,		"Exposure" },
	{ XCB_EVENT_MASK_VISIBILITY_CHANGE,	"VisibilityChange" },
	{ XCB_EVENT_MASK_STRUCTURE_NOTIFY,	"StructureNotify" },
	{ XCB_EVENT_MASK_RESIZE_REDIRECT,	"ResizeRedirect" },
	{ XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,	"SubstructureNotify" },
	{ XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT,	"SubstructureRedirect" },
	{ XCB_EVENT_MASK_FOCUS_CHANGE,		"FocusChange" },
	{ XCB_EVENT_MASK_PROPERTY_CHANGE,	"PropertyChange" },
	{ XCB_EVENT_MASK_COLOR_MAP_CHANGE,	"ColormapChange" },
	{ XCB_EVENT_MASK_OWNER_GRAB_BUTTON,	"OwnerGrabButton" },
	{ 0, NULL } };

static void
Display_Event_Mask (long mask)
{
    long bit, bit_mask;

    for (bit=0, bit_mask=1; bit < sizeof(long)*8; bit++, bit_mask <<= 1)
	if (mask & bit_mask)
	    printf ("      %s\n",
		    LookupL (bit_mask, _event_mask_names));
}


/*
 * Display info on events
 *
 * Requires wininfo members initialized:
 *   window, attr_cookie (or win_attributes)
 */
static void
Display_Events_Info (struct wininfo *w)
{
    xcb_get_window_attributes_reply_t *win_attributes
	= fetch_win_attributes (w);

    printf ("\n");
    printf ("  Someone wants these events:\n");
    Display_Event_Mask (win_attributes->all_event_masks);

    printf ("  Do not propagate these events:\n");
    Display_Event_Mask (win_attributes->do_not_propagate_mask);

    printf ("  Override redirection?: %s\n",
	    Lookup (win_attributes->override_redirect, _bool));
}


  /* left out visual stuff */
  /* left out colormap */
  /* left out map_installed */


/*
 * Display root, parent, and (recursively) children information
 * recurse - true to show children information
 *
 * Requires wininfo members initialized: window, tree_cookie
 */
static void
Display_Tree_Info (struct wininfo *w, int recurse)
{
    display_tree_info_1 (w, recurse, 0);
}

/*
 * level - recursion level
 */
static void
display_tree_info_1 (struct wininfo *w, int recurse, int level)
{
    int i, j;
    unsigned int num_children;
    xcb_query_tree_reply_t *tree;

    tree = xcb_query_tree_reply (dpy, w->tree_cookie, &err);
    if (!tree) {
	Print_X_Error (dpy, err);
	Fatal_Error ("Can't query window tree.");
    }

    if (level == 0) {
	struct wininfo rw, pw;
	rw.window = tree->root;
	rw.wm_name_cookie = xcb_get_wm_name (dpy, rw.window);
	pw.window = tree->parent;
	pw.wm_name_cookie = xcb_get_wm_name (dpy, pw.window);
	xcb_flush (dpy);

	printf ("\n");
	printf ("  Root window id: ");
	Display_Window_Id (&rw, True);
	printf ("  Parent window id: ");
	Display_Window_Id (&pw, True);
    }

    num_children = xcb_query_tree_children_length (tree);

    if (level == 0  ||  num_children > 0) {
	printf ("     ");
	for (j = 0; j < level; j++) printf ("   ");
	printf ("%d child%s%s\n", num_children, num_children == 1 ? "" : "ren",
		num_children ? ":" : ".");
    }

    if (num_children > 0) {
	xcb_window_t *child_list = xcb_query_tree_children (tree);
	struct wininfo *children
	    = calloc (num_children, sizeof(struct wininfo));

	if (children == NULL)
	    Fatal_Error ("Failed to allocate memory in display_tree_info");

	for (i = (int)num_children - 1; i >= 0; i--) {
	    struct wininfo *cw = &children[i];

	    cw->window = child_list[i];
	    cw->wm_name_cookie = xcb_get_wm_name (dpy, child_list[i]);
	    cw->wm_class_cookie = xcb_get_wm_class (dpy, child_list[i]);
	    cw->geometry_cookie = xcb_get_geometry (dpy, child_list[i]);
	    cw->trans_coords_cookie = xcb_translate_coordinates
		(dpy, child_list[i], tree->root, 0, 0);
	    if (recurse)
		cw->tree_cookie = xcb_query_tree (dpy, child_list[i]);
	}
	xcb_flush (dpy);

	for (i = (int)num_children - 1; i >= 0; i--) {
	    struct wininfo *cw = &children[i];
	    xcb_get_wm_class_reply_t classhint;
	    xcb_get_geometry_reply_t *geometry;

	    printf ("     ");
	    for (j = 0; j < level; j++) printf ("   ");
	    Display_Window_Id (cw, False);
	    printf (": (");

	    if (xcb_get_wm_class_reply (dpy, cw->wm_class_cookie,
					&classhint, NULL)) {
		if (classhint.instance_name)
		    printf ("\"%s\" ", classhint.instance_name);
		else
		    printf ("(none) ");

		if (classhint.class_name)
		    printf ("\"%s\") ", classhint.class_name);
		else
		    printf ("(none)) ");

		xcb_get_wm_class_reply_wipe (&classhint);
	    } else
		printf (") ");

	    geometry = xcb_get_geometry_reply(dpy, cw->geometry_cookie, &err);
	    if (geometry) {
		xcb_translate_coordinates_reply_t *trans_coords;

		printf (" %ux%u+%d+%d", geometry->width, geometry->height,
					geometry->x, geometry->y);

		trans_coords = xcb_translate_coordinates_reply
		    (dpy, cw->trans_coords_cookie, &err);

		if (trans_coords) {
		    int16_t abs_x = (int16_t) trans_coords->dst_x;
		    int16_t abs_y = (int16_t) trans_coords->dst_y;
		    int border = geometry->border_width;

		    printf ("  +%d+%d", abs_x - border, abs_y - border);
		    free (trans_coords);
		} else if (err) {
		    Print_X_Error (dpy, err);
		}

		free (geometry);
	    } else if (err) {
		Print_X_Error (dpy, err);
	    }
	    printf ("\n");

	    if (recurse)
		display_tree_info_1 (cw, 1, level+1);

	    wininfo_wipe (cw);
	}
	free (children);
    }

    free (tree); /* includes storage for child_list[] */
}


/*
 * Display a set of size hints
 */
static void
Display_Hints (xcb_size_hints_t *hints)
{
    long flags;

    flags = hints->flags;

    if (flags & XCB_SIZE_HINT_US_POSITION)
	printf ("      User supplied location: %s, %s\n",
		xscale (hints->x), yscale (hints->y));

    if (flags & XCB_SIZE_HINT_P_POSITION)
	printf ("      Program supplied location: %s, %s\n",
		xscale (hints->x), yscale (hints->y));

    if (flags & XCB_SIZE_HINT_US_SIZE) {
	printf ("      User supplied size: %s by %s\n",
		xscale (hints->width), yscale (hints->height));
    }

    if (flags & XCB_SIZE_HINT_P_SIZE)
	printf ("      Program supplied size: %s by %s\n",
		xscale (hints->width), yscale (hints->height));

    if (flags & XCB_SIZE_HINT_P_MIN_SIZE)
	printf ("      Program supplied minimum size: %s by %s\n",
		xscale (hints->min_width), yscale (hints->min_height));

    if (flags & XCB_SIZE_HINT_P_MAX_SIZE)
	printf ("      Program supplied maximum size: %s by %s\n",
		xscale (hints->max_width), yscale (hints->max_height));

    if (flags & XCB_SIZE_HINT_BASE_SIZE) {
	printf ("      Program supplied base size: %s by %s\n",
		xscale (hints->base_width), yscale (hints->base_height));
    }

    if (flags & XCB_SIZE_HINT_P_RESIZE_INC) {
	printf ("      Program supplied x resize increment: %s\n",
		xscale (hints->width_inc));
	printf ("      Program supplied y resize increment: %s\n",
		yscale (hints->height_inc));
	if (hints->width_inc != 0 && hints->height_inc != 0) {
	    if (flags & XCB_SIZE_HINT_US_SIZE)
		printf ("      User supplied size in resize increments:  %s by %s\n",
			(xscale (hints->width / hints->width_inc)),
			(yscale (hints->height / hints->height_inc)));
	    if (flags & XCB_SIZE_HINT_P_SIZE)
		printf ("      Program supplied size in resize increments:  %s by %s\n",
			(xscale (hints->width / hints->width_inc)),
			(yscale (hints->height / hints->height_inc)));
	    if (flags & XCB_SIZE_HINT_P_MIN_SIZE)
		printf ("      Program supplied minimum size in resize increments: %s by %s\n",
			xscale (hints->min_width / hints->width_inc), yscale (hints->min_height / hints->height_inc));
	    if (flags & XCB_SIZE_HINT_BASE_SIZE)
		printf ("      Program supplied base size in resize increments:  %s by %s\n",
			(xscale (hints->base_width / hints->width_inc)),
			(yscale (hints->base_height / hints->height_inc)));
	}
    }

    if (flags & XCB_SIZE_HINT_P_ASPECT) {
	printf ("      Program supplied min aspect ratio: %s/%s\n",
		xscale (hints->min_aspect_num), yscale (hints->min_aspect_den));
	printf ("      Program supplied max aspect ratio: %s/%s\n",
		xscale (hints->max_aspect_num), yscale (hints->max_aspect_den));
    }

    if (flags & XCB_SIZE_HINT_P_WIN_GRAVITY) {
	printf ("      Program supplied window gravity: %s\n",
		Lookup (hints->win_gravity, _gravities));
    }
}


/*
 * Display Size Hints info
 */
static void
Display_Size_Hints (struct wininfo *w)
{
    xcb_size_hints_t hints;

    printf ("\n");
    if (!fetch_normal_hints (w, &hints))
	printf ("  No normal window size hints defined\n");
    else {
	printf ("  Normal window size hints:\n");
	Display_Hints (&hints);
    }

    if (!xcb_get_wm_size_hints_reply (dpy, w->zoom_cookie, &hints, NULL))
	printf ("  No zoom window size hints defined\n");
    else {
	printf ("  Zoom window size hints:\n");
	Display_Hints (&hints);
    }
}


static void
Display_Window_Shape (xcb_window_t window)
{
    const xcb_query_extension_reply_t *shape_query;
    xcb_shape_query_extents_cookie_t extents_cookie;
    xcb_shape_query_extents_reply_t *extents;

    shape_query = xcb_get_extension_data (dpy, &xcb_shape_id);
    if (!shape_query->present)
	return;

    printf ("\n");

    extents_cookie = xcb_shape_query_extents (dpy, window);
    extents = xcb_shape_query_extents_reply (dpy, extents_cookie, &err);

    if (!extents) {
	if (err)
	    Print_X_Error (dpy, err);
	else
	{
	    printf ("  No window shape defined\n");
	    printf ("  No border shape defined\n");
	}
	return;
    }

    if (!extents->bounding_shaped)
	printf ("  No window shape defined\n");
    else {
	printf ("  Window shape extents:  %sx%s",
		xscale (extents->bounding_shape_extents_width),
		yscale (extents->bounding_shape_extents_height));
	printf ("+%s+%s\n",
		xscale (extents->bounding_shape_extents_x),
		yscale (extents->bounding_shape_extents_y));
    }
    if (!extents->clip_shaped)
	printf ("  No border shape defined\n");
    else {
	printf ("  Border shape extents:  %sx%s",
		xscale (extents->clip_shape_extents_width),
		yscale (extents->clip_shape_extents_height));
	printf ("+%s+%s\n",
		xscale (extents->clip_shape_extents_x),
		yscale (extents->clip_shape_extents_y));
    }

    free (extents);
}

/*
 * Display Window Manager Info
 *
 * Requires wininfo members initialized:
 *   window, hints_cookie
 */
static const binding _state_hints[] = {
	{ XCB_WM_STATE_WITHDRAWN, "Withdrawn State" },
	{ XCB_WM_STATE_NORMAL, "Normal State" },
	{ XCB_WM_STATE_ICONIC, "Iconic State" },
/* xwininfo previously also reported the ZoomState & InactiveState,
   but ICCCM declared those obsolete long ago */
	{ 0, NULL } };

static void
Display_WM_Info (struct wininfo *w)
{
    xcb_wm_hints_t wmhints;
    long flags;

    printf ("\n");
    if (!xcb_get_wm_hints_reply(dpy, w->hints_cookie, &wmhints, &err))
    {
	printf ("  No window manager hints defined\n");
	if (err)
	    Print_X_Error (dpy, err);
	return;
    }
    flags = wmhints.flags;

    printf ("  Window manager hints:\n");

    if (flags & XCB_WM_HINT_INPUT)
	printf ("      Client accepts input or input focus: %s\n",
		Lookup (wmhints.input, _bool));

    if (flags & XCB_WM_HINT_ICON_WINDOW) {
	struct wininfo iw;
	iw.window = wmhints.icon_window;
	iw.wm_name_cookie = xcb_get_wm_name (dpy, iw.window);

	printf ("      Icon window id: ");
	Display_Window_Id (&iw, True);
    }

    if (flags & XCB_WM_HINT_ICON_POSITION)
	printf ("      Initial icon position: %s, %s\n",
		xscale (wmhints.icon_x), yscale (wmhints.icon_y));

    if (flags & XCB_WM_HINT_STATE)
	printf ("      Initial state is %s\n",
		Lookup (wmhints.initial_state, _state_hints));
}

/* Frees all members of a wininfo struct, but not the struct itself */
static void
wininfo_wipe (struct wininfo *w)
{
    free (w->geometry);
    free (w->win_attributes);
    free (w->normal_hints);
}
