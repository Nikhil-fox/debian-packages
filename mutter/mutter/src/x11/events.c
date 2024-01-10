/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/*
 * Copyright (C) 2001 Havoc Pennington
 * Copyright (C) 2002, 2003, 2004 Red Hat, Inc.
 * Copyright (C) 2003, 2004 Rob Adams
 * Copyright (C) 2004-2006 Elijah Newren
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "x11/events.h"

#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/shape.h>

#include "backends/meta-cursor-tracker-private.h"
#include "backends/x11/meta-backend-x11.h"
#include "backends/x11/meta-cursor-tracker-x11.h"
#include "compositor/meta-compositor-x11.h"
#include "cogl/cogl.h"
#include "core/bell.h"
#include "core/display-private.h"
#include "core/meta-workspace-manager-private.h"
#include "core/window-private.h"
#include "core/workspace-private.h"
#include "meta/meta-backend.h"
#include "meta/meta-context.h"
#include "meta/meta-x11-errors.h"
#include "x11/meta-startup-notification-x11.h"
#include "x11/meta-x11-display-private.h"
#include "x11/meta-x11-event-source.h"
#include "x11/meta-x11-selection-private.h"
#include "x11/meta-x11-selection-input-stream-private.h"
#include "x11/meta-x11-selection-output-stream-private.h"
#include "x11/window-x11.h"
#include "x11/window-x11-private.h"
#include "x11/xprops.h"

#ifdef HAVE_XWAYLAND
#include "wayland/meta-wayland-private.h"
#include "wayland/meta-xwayland-private.h"
#include "wayland/meta-xwayland.h"
#endif

static XIEvent *
get_input_event (MetaX11Display *x11_display,
                 XEvent         *event)
{
  if (event->type == GenericEvent &&
      event->xcookie.extension == x11_display->xinput_opcode)
    {
      XIEvent *input_event;

      input_event = (XIEvent *) event->xcookie.data;
      if (!input_event)
        return NULL;

      switch (input_event->evtype)
        {
        case XI_Motion:
        case XI_ButtonPress:
        case XI_ButtonRelease:
          if (((XIDeviceEvent *) input_event)->deviceid == META_VIRTUAL_CORE_POINTER_ID)
            return input_event;
          break;
        case XI_KeyPress:
        case XI_KeyRelease:
          if (((XIDeviceEvent *) input_event)->deviceid == META_VIRTUAL_CORE_KEYBOARD_ID)
            return input_event;
          break;
        case XI_FocusIn:
        case XI_FocusOut:
          if (((XIEnterEvent *) input_event)->deviceid == META_VIRTUAL_CORE_KEYBOARD_ID)
            return input_event;
          break;
        case XI_Enter:
        case XI_Leave:
          if (((XIEnterEvent *) input_event)->deviceid == META_VIRTUAL_CORE_POINTER_ID)
            return input_event;
          break;
        case XI_BarrierHit:
        case XI_BarrierLeave:
          if (((XIBarrierEvent *) input_event)->deviceid == META_VIRTUAL_CORE_POINTER_ID)
            return input_event;
          break;
        default:
          break;
        }
    }

  return NULL;
}

static Window
xievent_get_modified_window (MetaX11Display *x11_display,
                             XIEvent        *input_event)
{
  switch (input_event->evtype)
    {
    case XI_Motion:
    case XI_ButtonPress:
    case XI_ButtonRelease:
    case XI_KeyPress:
    case XI_KeyRelease:
      return ((XIDeviceEvent *) input_event)->event;
    case XI_FocusIn:
    case XI_FocusOut:
    case XI_Enter:
    case XI_Leave:
      return ((XIEnterEvent *) input_event)->event;
    case XI_BarrierHit:
    case XI_BarrierLeave:
      return ((XIBarrierEvent *) input_event)->event;
    }

  return None;
}

/* Return the window this has to do with, if any, rather
 * than the frame or root window that was selecting
 * for substructure
 */
static Window
event_get_modified_window (MetaX11Display *x11_display,
                           XEvent *event)
{
  XIEvent *input_event = get_input_event (x11_display, event);

  if (input_event)
    return xievent_get_modified_window (x11_display, input_event);

  switch (event->type)
    {
    case KeymapNotify:
    case Expose:
    case GraphicsExpose:
    case NoExpose:
    case VisibilityNotify:
    case ResizeRequest:
    case PropertyNotify:
    case SelectionClear:
    case SelectionRequest:
    case SelectionNotify:
    case ColormapNotify:
    case ClientMessage:
      return event->xany.window;

    case CreateNotify:
      return event->xcreatewindow.window;

    case DestroyNotify:
      return event->xdestroywindow.window;

    case UnmapNotify:
      return event->xunmap.window;

    case MapNotify:
      return event->xmap.window;

    case MapRequest:
      return event->xmaprequest.window;

    case ReparentNotify:
     return event->xreparent.window;

    case ConfigureNotify:
      return event->xconfigure.window;

    case ConfigureRequest:
      return event->xconfigurerequest.window;

    case GravityNotify:
      return event->xgravity.window;

    case CirculateNotify:
      return event->xcirculate.window;

    case CirculateRequest:
      return event->xcirculaterequest.window;

    case MappingNotify:
      return None;

    default:
      if (META_X11_DISPLAY_HAS_SHAPE (x11_display) &&
          event->type == (x11_display->shape_event_base + ShapeNotify))
        {
          XShapeEvent *sev = (XShapeEvent*) event;
          return sev->window;
        }

      return None;
    }
}

static guint32
event_get_time (MetaX11Display *x11_display,
                XEvent         *event)
{
  XIEvent *input_event = get_input_event (x11_display, event);

  if (input_event)
    return input_event->time;

  switch (event->type)
    {
    case PropertyNotify:
      return event->xproperty.time;

    case SelectionClear:
    case SelectionRequest:
    case SelectionNotify:
      return event->xselection.time;

    case KeymapNotify:
    case Expose:
    case GraphicsExpose:
    case NoExpose:
    case MapNotify:
    case UnmapNotify:
    case VisibilityNotify:
    case ResizeRequest:
    case ColormapNotify:
    case ClientMessage:
    case CreateNotify:
    case DestroyNotify:
    case MapRequest:
    case ReparentNotify:
    case ConfigureNotify:
    case ConfigureRequest:
    case GravityNotify:
    case CirculateNotify:
    case CirculateRequest:
    case MappingNotify:
    default:
      return META_CURRENT_TIME;
    }
}

const char*
meta_event_detail_to_string (int d)
{
  const char *detail = "???";
  switch (d)
    {
      /* We are an ancestor in the A<->B focus change relationship */
    case XINotifyAncestor:
      detail = "NotifyAncestor";
      break;
    case XINotifyDetailNone:
      detail = "NotifyDetailNone";
      break;
      /* We are a descendant in the A<->B focus change relationship */
    case XINotifyInferior:
      detail = "NotifyInferior";
      break;
    case XINotifyNonlinear:
      detail = "NotifyNonlinear";
      break;
    case XINotifyNonlinearVirtual:
      detail = "NotifyNonlinearVirtual";
      break;
    case XINotifyPointer:
      detail = "NotifyPointer";
      break;
    case XINotifyPointerRoot:
      detail = "NotifyPointerRoot";
      break;
    case XINotifyVirtual:
      detail = "NotifyVirtual";
      break;
    }

  return detail;
}

const char*
meta_event_mode_to_string (int m)
{
  const char *mode = "???";
  switch (m)
    {
    case XINotifyNormal:
      mode = "NotifyNormal";
      break;
    case XINotifyGrab:
      mode = "NotifyGrab";
      break;
    case XINotifyUngrab:
      mode = "NotifyUngrab";
      break;
    case XINotifyWhileGrabbed:
      mode = "NotifyWhileGrabbed";
      break;
    }

  return mode;
}

G_GNUC_UNUSED static const char*
stack_mode_to_string (int mode)
{
  switch (mode)
    {
    case Above:
      return "Above";
    case Below:
      return "Below";
    case TopIf:
      return "TopIf";
    case BottomIf:
      return "BottomIf";
    case Opposite:
      return "Opposite";
    }

  return "Unknown";
}

static gint64
sync_value_to_64 (const XSyncValue *value)
{
  gint64 v;

  v = XSyncValueLow32 (*value);
  v |= (((gint64)XSyncValueHigh32 (*value)) << 32);

  return v;
}

static const char*
alarm_state_to_string (XSyncAlarmState state)
{
  switch (state)
    {
    case XSyncAlarmActive:
      return "Active";
    case XSyncAlarmInactive:
      return "Inactive";
    case XSyncAlarmDestroyed:
      return "Destroyed";
    default:
      return "(unknown)";
    }
}

static const char *
get_xi2_event_name (XIEvent *input_event)
{
  switch (input_event->evtype)
    {
    case XI_FocusIn:
      return "XI_FocusIn";
    case XI_FocusOut:
      return "XI_FocusOut";
    case XI_Enter:
      return "XI_Enter";
    case XI_Leave:
      return "XI_Leave";
    case XI_BarrierHit:
      return "XI_BarrierHit";
    case XI_BarrierLeave:
      return "XI_BarrierLeave";
    }

  return NULL;
}

static void
meta_spew_xi2_event (MetaX11Display *x11_display,
                     XIEvent        *input_event,
                     const char    **name_p,
                     char          **extra_p)
{
  const char *name = NULL;
  char *extra = NULL;

  XIEnterEvent *enter_event = (XIEnterEvent *) input_event;

  name = get_xi2_event_name (input_event);

  switch (input_event->evtype)
    {
    case XI_FocusIn:
    case XI_FocusOut:
      extra = g_strdup_printf ("detail: %s mode: %s\n",
                               meta_event_detail_to_string (enter_event->detail),
                               meta_event_mode_to_string (enter_event->mode));
      break;
    case XI_Enter:
    case XI_Leave:
      extra = g_strdup_printf ("win: 0x%lx root: 0x%lx mode: %s detail: %s focus: %d x: %g y: %g",
                               enter_event->event,
                               enter_event->root,
                               meta_event_mode_to_string (enter_event->mode),
                               meta_event_detail_to_string (enter_event->detail),
                               enter_event->focus,
                               enter_event->root_x,
                               enter_event->root_y);
      break;
    }

  *name_p = name;
  *extra_p = extra;
}

static const char *
get_core_event_name (XEvent *event)
{
  /* GenericEvent is omitted here, as it's handled separately. */
  switch (event->type)
    {
    case KeyPress:
      return "KeyPress";
    case KeyRelease:
      return "KeyRelease";
    case ButtonPress:
      return "ButtonPress";
    case ButtonRelease:
      return "ButtonRelease";
    case MotionNotify:
      return "MotionNotify";
    case EnterNotify:
      return "EnterNotify";
    case LeaveNotify:
      return "LeaveNotify";
    case FocusIn:
      return "FocusIn";
    case FocusOut:
      return "FocusOut";
    case KeymapNotify:
      return "KeymapNotify";
    case Expose:
      return "Expose";
    case GraphicsExpose:
      return "GraphicsExpose";
    case NoExpose:
      return "NoExpose";
    case VisibilityNotify:
      return "VisibilityNotify";
    case CreateNotify:
      return "CreateNotify";
    case DestroyNotify:
      return "DestroyNotify";
    case UnmapNotify:
      return "UnmapNotify";
    case MapNotify:
      return "MapNotify";
    case MapRequest:
      return "MapRequest";
    case ReparentNotify:
      return "ReparentNotify";
    case ConfigureNotify:
      return "ConfigureNotify";
    case ConfigureRequest:
      return "ConfigureRequest";
    case GravityNotify:
      return "GravityNotify";
    case ResizeRequest:
      return "ResizeRequest";
    case CirculateNotify:
      return "CirculateNotify";
    case CirculateRequest:
      return "CirculateRequest";
    case PropertyNotify:
      return "PropertyNotify";
    case SelectionClear:
      return "SelectionClear";
    case SelectionRequest:
      return "SelectionRequest";
    case SelectionNotify:
      return "SelectionNotify";
    case ColormapNotify:
      return "ColormapNotify";
    case ClientMessage:
      return "ClientMessage";
    case MappingNotify:
      return "MappingNotify";
    }

  return NULL;
}

static const char *
get_extension_event_name (MetaX11Display *x11_display,
                          XEvent         *event)
{
  if (META_X11_DISPLAY_HAS_XSYNC (x11_display) &&
      event->type == (x11_display->xsync_event_base + XSyncAlarmNotify))
    return "XSyncAlarmNotify";

  if (META_X11_DISPLAY_HAS_SHAPE (x11_display) &&
      event->type == (x11_display->shape_event_base + ShapeNotify))
    return "ShapeNotify";

  if (META_X11_DISPLAY_HAS_DAMAGE (x11_display) &&
      event->type == (x11_display->damage_event_base + XDamageNotify))
    return "XDamageNotify";

  if (event->type == (x11_display->xfixes_event_base + XFixesSelectionNotify))
    return "XFixesSelectionNotify";

  return NULL;
}

#ifdef COGL_HAS_TRACING
static const char *
get_event_name (MetaX11Display *x11_display,
                XEvent         *event)
{
  const char *name;
  XIEvent *input_event;

  if (event->type < GenericEvent)
    return get_core_event_name (event);

  name = get_extension_event_name (x11_display, event);
  if (name)
    return name;

  input_event = get_input_event (x11_display, event);
  if (input_event)
    {
      name = get_xi2_event_name (input_event);
      if (name)
        return name;
    }

  return "Unknown event";
}
#endif

static void
meta_spew_core_event (MetaX11Display *x11_display,
                      XEvent         *event,
                      const char    **name_p,
                      char          **extra_p)
{
  const char *name = NULL;
  char *extra = NULL;

  name = get_core_event_name (event);

  if (!name)
    name = get_extension_event_name (x11_display, event);

  switch (event->type)
    {
    case KeymapNotify:
    case Expose:
    case GraphicsExpose:
    case NoExpose:
    case VisibilityNotify:
    case GravityNotify:
    case CirculateNotify:
    case CirculateRequest:
    case SelectionClear:
    case SelectionRequest:
    case SelectionNotify:
    case ColormapNotify:
      break;
    case CreateNotify:
      extra = g_strdup_printf ("parent: 0x%lx window: 0x%lx",
                               event->xcreatewindow.parent,
                               event->xcreatewindow.window);
      break;
    case DestroyNotify:
      extra = g_strdup_printf ("event: 0x%lx window: 0x%lx",
                               event->xdestroywindow.event,
                               event->xdestroywindow.window);
      break;
    case UnmapNotify:
      extra = g_strdup_printf ("event: 0x%lx window: 0x%lx from_configure: %d",
                               event->xunmap.event,
                               event->xunmap.window,
                               event->xunmap.from_configure);
      break;
    case MapNotify:
      extra = g_strdup_printf ("event: 0x%lx window: 0x%lx override_redirect: %d",
                               event->xmap.event,
                               event->xmap.window,
                               event->xmap.override_redirect);
      break;
    case MapRequest:
      extra = g_strdup_printf ("window: 0x%lx parent: 0x%lx\n",
                               event->xmaprequest.window,
                               event->xmaprequest.parent);
      break;
    case ReparentNotify:
      extra = g_strdup_printf ("window: 0x%lx parent: 0x%lx event: 0x%lx\n",
                               event->xreparent.window,
                               event->xreparent.parent,
                               event->xreparent.event);
      break;
    case ConfigureNotify:
      extra = g_strdup_printf ("x: %d y: %d w: %d h: %d above: 0x%lx override_redirect: %d",
                               event->xconfigure.x,
                               event->xconfigure.y,
                               event->xconfigure.width,
                               event->xconfigure.height,
                               event->xconfigure.above,
                               event->xconfigure.override_redirect);
      break;
    case ConfigureRequest:
      extra = g_strdup_printf ("parent: 0x%lx window: 0x%lx x: %d %sy: %d %sw: %d %sh: %d %sborder: %d %sabove: %lx %sstackmode: %s %s",
                               event->xconfigurerequest.parent,
                               event->xconfigurerequest.window,
                               event->xconfigurerequest.x,
                               event->xconfigurerequest.value_mask &
                               CWX ? "" : "(unset) ",
                               event->xconfigurerequest.y,
                               event->xconfigurerequest.value_mask &
                               CWY ? "" : "(unset) ",
                               event->xconfigurerequest.width,
                               event->xconfigurerequest.value_mask &
                               CWWidth ? "" : "(unset) ",
                               event->xconfigurerequest.height,
                               event->xconfigurerequest.value_mask &
                               CWHeight ? "" : "(unset) ",
                               event->xconfigurerequest.border_width,
                               event->xconfigurerequest.value_mask &
                               CWBorderWidth ? "" : "(unset)",
                               event->xconfigurerequest.above,
                               event->xconfigurerequest.value_mask &
                               CWSibling ? "" : "(unset)",
                               stack_mode_to_string (event->xconfigurerequest.detail),
                               event->xconfigurerequest.value_mask &
                               CWStackMode ? "" : "(unset)");
      break;
    case ResizeRequest:
      extra = g_strdup_printf ("width = %d height = %d",
                               event->xresizerequest.width,
                               event->xresizerequest.height);
      break;
    case PropertyNotify:
      {
        char *str;
        const char *state;

        meta_x11_error_trap_push (x11_display);
        str = XGetAtomName (x11_display->xdisplay,
                            event->xproperty.atom);
        meta_x11_error_trap_pop (x11_display);

        if (event->xproperty.state == PropertyNewValue)
          state = "PropertyNewValue";
        else if (event->xproperty.state == PropertyDelete)
          state = "PropertyDelete";
        else
          state = "???";

        extra = g_strdup_printf ("atom: %s state: %s",
                                 str ? str : "(unknown atom)",
                                 state);
        meta_XFree (str);
      }
      break;
    case ClientMessage:
      {
        char *str;
        meta_x11_error_trap_push (x11_display);
        str = XGetAtomName (x11_display->xdisplay,
                            event->xclient.message_type);
        meta_x11_error_trap_pop (x11_display);
        extra = g_strdup_printf ("type: %s format: %d\n",
                                 str ? str : "(unknown atom)",
                                 event->xclient.format);
        meta_XFree (str);
      }
      break;
    case MappingNotify:
      break;
    default:
      if (META_X11_DISPLAY_HAS_XSYNC (x11_display) &&
          event->type == (x11_display->xsync_event_base + XSyncAlarmNotify))
        {
          XSyncAlarmNotifyEvent *aevent = (XSyncAlarmNotifyEvent*) event;

          extra =
            g_strdup_printf ("alarm: 0x%lx"
                             " counter_value: %" G_GINT64_FORMAT
                             " alarm_value: %" G_GINT64_FORMAT
                             " time: %u alarm state: %s",
                             aevent->alarm,
                             (gint64) sync_value_to_64 (&aevent->counter_value),
                             (gint64) sync_value_to_64 (&aevent->alarm_value),
                             (unsigned int)aevent->time,
                             alarm_state_to_string (aevent->state));
        }
      else
        {
          if (META_X11_DISPLAY_HAS_SHAPE (x11_display) &&
              event->type == (x11_display->shape_event_base + ShapeNotify))
            {
              XShapeEvent *sev = (XShapeEvent*) event;

              extra =
                g_strdup_printf ("kind: %s "
                                 "x: %d y: %d w: %u h: %u "
                                 "shaped: %d",
                                 sev->kind == ShapeBounding ?
                                 "ShapeBounding" :
                                 (sev->kind == ShapeClip ?
                                  "ShapeClip" : "(unknown)"),
                                 sev->x, sev->y, sev->width, sev->height,
                                 sev->shaped);
            }
          else
            {
              extra = g_strdup_printf ("type: %d", event->xany.type);
            }
        }
      break;
    }

  *name_p = name;
  *extra_p = extra;
}

static char *
meta_spew_event (MetaX11Display *x11_display,
                 XEvent         *event)
{
  const char *name = NULL;
  char *extra = NULL;
  char *winname;
  char *ret;
  XIEvent *input_event;

  input_event = get_input_event (x11_display, event);

  if (input_event)
    meta_spew_xi2_event (x11_display, input_event, &name, &extra);
  else
    meta_spew_core_event (x11_display, event, &name, &extra);

  if (event->xany.window == x11_display->xroot)
    winname = g_strdup_printf ("root");
  else
    winname = g_strdup_printf ("0x%lx", event->xany.window);

  ret = g_strdup_printf ("%s on %s%s %s %sserial %lu", name, winname,
                         extra ? ":" : "", extra ? extra : "",
                         event->xany.send_event ? "SEND " : "",
                         event->xany.serial);

  g_free (winname);
  g_free (extra);

  return ret;
}

G_GNUC_UNUSED static void
meta_spew_event_print (MetaX11Display *x11_display,
                       XEvent         *event)
{
  char *event_str;

  /* filter overnumerous events */
  if (event->type == Expose || event->type == MotionNotify ||
      event->type == NoExpose)
    return;

  if (event->type == (x11_display->damage_event_base + XDamageNotify))
    return;

  if (event->type == (x11_display->xsync_event_base + XSyncAlarmNotify))
    return;

  if (event->type == PropertyNotify &&
      event->xproperty.atom == x11_display->atom__NET_WM_USER_TIME)
    return;

  event_str = meta_spew_event (x11_display, event);
  g_print ("%s\n", event_str);
  g_free (event_str);
}

static gboolean
handle_window_focus_event (MetaX11Display *x11_display,
                           MetaWindow     *window,
                           XIEnterEvent   *event,
                           unsigned long   serial)
{
  MetaDisplay *display = x11_display->display;
  MetaWindow *focus_window;
#ifdef WITH_VERBOSE_MODE
  const char *window_type;

  /* Note the event can be on either the window or the frame,
   * we focus the frame for output-only windows
   */
  if (window)
    {
      if (event->event == window->xwindow)
        window_type = "client window";
      else if (window->frame && event->event == window->frame->xwindow)
        window_type = "frame window";
      else
        window_type = "unknown client window";
    }
  else if (meta_x11_display_xwindow_is_a_no_focus_window (x11_display,
                                                          event->event))
    window_type = "no_focus_window";
  else if (event->event == x11_display->xroot)
    window_type = "root window";
  else
    window_type = "unknown window";

  meta_topic (META_DEBUG_FOCUS,
              "Focus %s event received on %s 0x%lx (%s) "
              "mode %s detail %s serial %lu",
              event->evtype == XI_FocusIn ? "in" :
              event->evtype == XI_FocusOut ? "out" :
              "???",
              window ? window->desc : "",
              event->event, window_type,
              meta_event_mode_to_string (event->mode),
              meta_event_detail_to_string (event->mode),
              serial);
#endif

  /* FIXME our pointer tracking is broken; see how
   * gtk+/gdk/x11/gdkevents-x11.c or XFree86/xc/programs/xterm/misc.c
   * for how to handle it the correct way.  In brief you need to track
   * pointer focus and regular focus, and handle EnterNotify in
   * PointerRoot mode with no window manager.  However as noted above,
   * accurate focus tracking will break things because we want to keep
   * windows "focused" when using keybindings on them, and also we
   * sometimes "focus" a window by focusing its frame or
   * no_focus_window; so this all needs rethinking massively.
   *
   * My suggestion is to change it so that we clearly separate
   * actual keyboard focus tracking using the xterm algorithm,
   * and mutter's "pretend" focus window, and go through all
   * the code and decide which one should be used in each place;
   * a hard bit is deciding on a policy for that.
   *
   * http://bugzilla.gnome.org/show_bug.cgi?id=90382
   */

  /* We ignore grabs, though this is questionable. It may be better to
   * increase the intelligence of the focus window tracking.
   *
   * The problem is that keybindings for windows are done with
   * XGrabKey, which means focus_window disappears and the front of
   * the MRU list gets confused from what the user expects once a
   * keybinding is used.
   */

  if (event->mode == XINotifyGrab ||
      event->mode == XINotifyUngrab ||
      /* From WindowMaker, ignore all funky pointer root events */
      event->detail > XINotifyNonlinearVirtual)
    {
      meta_topic (META_DEBUG_FOCUS,
                  "Ignoring focus event generated by a grab or other weirdness");
      return FALSE;
    }

  if (event->evtype == XI_FocusIn)
    {
      x11_display->server_focus_window = event->event;
      x11_display->server_focus_serial = serial;
      focus_window = window;
    }
  else if (event->evtype == XI_FocusOut)
    {
      if (event->detail == XINotifyInferior)
        {
          /* This event means the client moved focus to a subwindow */
          meta_topic (META_DEBUG_FOCUS,
                      "Ignoring focus out with NotifyInferior");
          return FALSE;
        }

      x11_display->server_focus_window = None;
      x11_display->server_focus_serial = serial;
      focus_window = NULL;
    }
  else
    g_assert_not_reached ();

  /* If display->focused_by_us, then the focus_serial will be used only
   * for a focus change we made and have already accounted for.
   * (See request_xserver_input_focus_change().) Otherwise, we can get
   * multiple focus events with the same serial.
   */
  if (x11_display->server_focus_serial > x11_display->focus_serial ||
      (!x11_display->focused_by_us &&
       x11_display->server_focus_serial == x11_display->focus_serial))
    {
      meta_x11_display_update_focus_window (x11_display,
                                            focus_window ?
                                            focus_window->xwindow : None,
                                            x11_display->server_focus_serial,
                                            FALSE);
      meta_display_update_focus_window (display, focus_window);
      return TRUE;
    }
  else
    {
      return FALSE;
    }
}

static gboolean
crossing_serial_is_ignored (MetaX11Display *x11_display,
                            unsigned long   serial)
{
  int i;

  i = 0;
  while (i < N_IGNORED_CROSSING_SERIALS)
    {
      if (x11_display->ignored_crossing_serials[i] == serial)
        return TRUE;
      ++i;
    }
  return FALSE;
}

static void
reset_ignored_crossing_serials (MetaX11Display *x11_display)
{
  int i;

  i = 0;
  while (i < N_IGNORED_CROSSING_SERIALS)
    {
      x11_display->ignored_crossing_serials[i] = 0;
      ++i;
    }
}

static gboolean
handle_input_xevent (MetaX11Display *x11_display,
                     XIEvent        *input_event,
                     unsigned long   serial)
{
  XIEnterEvent *enter_event = (XIEnterEvent *) input_event;
  Window modified;
  MetaWindow *window;
  MetaDisplay *display = x11_display->display;
  MetaWorkspaceManager *workspace_manager = display->workspace_manager;
  MetaContext *context = meta_display_get_context (display);
  MetaBackend *backend = meta_context_get_backend (context);
  ClutterStage *stage = CLUTTER_STAGE (meta_backend_get_stage (backend));

  if (input_event == NULL)
    return FALSE;

  switch (input_event->evtype)
    {
    case XI_Enter:
    case XI_Leave:
    case XI_FocusIn:
    case XI_FocusOut:
      break;
    default:
      return FALSE;
    }

  modified = xievent_get_modified_window (x11_display, input_event);
  window = modified != None ?
           meta_x11_display_lookup_x_window (x11_display, modified) :
           NULL;

  switch (input_event->evtype)
    {
    case XI_Enter:
      if (clutter_stage_get_grab_actor (stage) != NULL)
        break;

      /* Check if we've entered a window; do this even if window->has_focus to
       * avoid races.
       */
      if (window && !crossing_serial_is_ignored (x11_display, serial) &&
          enter_event->mode != XINotifyGrab &&
          enter_event->mode != XINotifyUngrab &&
          enter_event->detail != XINotifyInferior &&
          !meta_is_wayland_compositor () &&
          meta_x11_display_focus_sentinel_clear (x11_display))
        {
          meta_window_handle_enter (window,
                                    enter_event->time,
                                    enter_event->root_x,
                                    enter_event->root_y);

          if (window->type != META_WINDOW_DOCK)
            {
              /* stop ignoring stuff */
              reset_ignored_crossing_serials (x11_display);
            }
        }
      break;
    case XI_Leave:
      if (clutter_stage_get_grab_actor (stage) != NULL)
        break;

      if (window != NULL &&
          enter_event->mode != XINotifyGrab &&
          enter_event->mode != XINotifyUngrab)
        {
          meta_window_handle_leave (window);
        }
      break;
    case XI_FocusIn:
    case XI_FocusOut:
      if (handle_window_focus_event (x11_display, window, enter_event, serial) &&
          enter_event->event == enter_event->root)
        {
          if (enter_event->evtype == XI_FocusIn &&
              enter_event->detail == XINotifyDetailNone)
            {
              meta_topic (META_DEBUG_FOCUS,
                          "Focus got set to None, probably due to "
                          "brain-damage in the X protocol (see bug "
                          "125492).  Setting the default focus window.");
              meta_workspace_focus_default_window (workspace_manager->active_workspace,
                                                   NULL,
                                                   meta_x11_display_get_current_time_roundtrip (x11_display));
            }
          else if (enter_event->evtype == XI_FocusIn &&
                   enter_event->mode == XINotifyNormal &&
                   enter_event->detail == XINotifyInferior)
            {
              meta_topic (META_DEBUG_FOCUS,
                          "Focus got set to root window, probably due to "
                          "gnome-session logout dialog usage (see bug "
                          "153220).  Setting the default focus window.");
              meta_workspace_focus_default_window (workspace_manager->active_workspace,
                                                   NULL,
                                                   meta_x11_display_get_current_time_roundtrip (x11_display));
            }
        }
      break;
    }

  /* Don't pass these events through to Clutter / GTK+ */
  return TRUE;
}

static void
process_request_frame_extents (MetaX11Display *x11_display,
                               XEvent         *event)
{
  /* The X window whose frame extents will be set. */
  Window xwindow = event->xclient.window;
  unsigned long data[4] = { 0, 0, 0, 0 };

  meta_topic (META_DEBUG_GEOMETRY,
              "Setting _NET_FRAME_EXTENTS on unmanaged window 0x%lx",
              xwindow);

  meta_x11_error_trap_push (x11_display);
  XChangeProperty (x11_display->xdisplay, xwindow,
                   x11_display->atom__NET_FRAME_EXTENTS,
                   XA_CARDINAL,
                   32, PropModeReplace, (guchar*) data, 4);
  meta_x11_error_trap_pop (x11_display);
}

/* from fvwm2, Copyright Matthias Clasen, Dominik Vogt */
static gboolean
convert_property (MetaX11Display *x11_display,
                  Window          w,
                  Atom            target,
                  Atom            property)
{
#define N_TARGETS 4
  Atom conversion_targets[N_TARGETS];
  long icccm_version[] = { 2, 0 };

  conversion_targets[0] = x11_display->atom_TARGETS;
  conversion_targets[1] = x11_display->atom_MULTIPLE;
  conversion_targets[2] = x11_display->atom_TIMESTAMP;
  conversion_targets[3] = x11_display->atom_VERSION;

  meta_x11_error_trap_push (x11_display);
  if (target == x11_display->atom_TARGETS)
    XChangeProperty (x11_display->xdisplay, w, property,
		     XA_ATOM, 32, PropModeReplace,
		     (unsigned char *)conversion_targets, N_TARGETS);
  else if (target == x11_display->atom_TIMESTAMP)
    XChangeProperty (x11_display->xdisplay, w, property,
		     XA_INTEGER, 32, PropModeReplace,
                     (unsigned char *)&x11_display->wm_sn_timestamp, 1);
  else if (target == x11_display->atom_VERSION)
    XChangeProperty (x11_display->xdisplay, w, property,
		     XA_INTEGER, 32, PropModeReplace,
		     (unsigned char *)icccm_version, 2);
  else
    {
      meta_x11_error_trap_pop_with_return (x11_display);
      return FALSE;
    }

  if (meta_x11_error_trap_pop_with_return (x11_display) != Success)
    return FALSE;

  /* Be sure the PropertyNotify has arrived so we
   * can send SelectionNotify
   */
  /* FIXME the error trap pop synced anyway, right? */
  meta_topic (META_DEBUG_SYNC, "Syncing on %s", G_STRFUNC);
  XSync (x11_display->xdisplay, False);

  return TRUE;
}

/* from fvwm2, Copyright Matthias Clasen, Dominik Vogt */
static void
process_selection_request (MetaX11Display *x11_display,
                           XEvent         *event)
{
  XSelectionEvent reply = { 0 };

  if (x11_display->wm_sn_selection_window != event->xselectionrequest.owner ||
      x11_display->wm_sn_atom != event->xselectionrequest.selection)
    {
      char *str;

      meta_x11_error_trap_push (x11_display);
      str = XGetAtomName (x11_display->xdisplay,
                          event->xselectionrequest.selection);
      meta_x11_error_trap_pop (x11_display);

      meta_verbose ("Selection request with selection %s window 0x%lx not a WM_Sn selection we recognize",
                    str ? str : "(bad atom)", event->xselectionrequest.owner);

      meta_XFree (str);

      return;
    }

  reply.type = SelectionNotify;
  reply.display = x11_display->xdisplay;
  reply.requestor = event->xselectionrequest.requestor;
  reply.selection = event->xselectionrequest.selection;
  reply.target = event->xselectionrequest.target;
  reply.property = None;
  reply.time = event->xselectionrequest.time;

  if (event->xselectionrequest.target == x11_display->atom_MULTIPLE)
    {
      if (event->xselectionrequest.property != None)
        {
          Atom type, *adata;
          int i, format;
          unsigned long num, rest;
          unsigned char *data;

          meta_x11_error_trap_push (x11_display);
          if (XGetWindowProperty (x11_display->xdisplay,
                                  event->xselectionrequest.requestor,
                                  event->xselectionrequest.property, 0, 256, False,
                                  x11_display->atom_ATOM_PAIR,
                                  &type, &format, &num, &rest, &data) != Success)
            {
              meta_x11_error_trap_pop_with_return (x11_display);
              return;
            }

          if (meta_x11_error_trap_pop_with_return (x11_display) == Success)
            {
              /* FIXME: to be 100% correct, should deal with rest > 0,
               * but since we have 4 possible targets, we will hardly ever
               * meet multiple requests with a length > 8
               */
              adata = (Atom*)data;
              i = 0;
              while (i < (int) num)
                {
                  if (!convert_property (x11_display,
                                         event->xselectionrequest.requestor,
                                         adata[i], adata[i+1]))
                    adata[i+1] = None;
                  i += 2;
                }

              meta_x11_error_trap_push (x11_display);
              XChangeProperty (x11_display->xdisplay,
                               event->xselectionrequest.requestor,
                               event->xselectionrequest.property,
                               x11_display->atom_ATOM_PAIR,
                               32, PropModeReplace, data, num);
              meta_x11_error_trap_pop (x11_display);
              meta_XFree (data);
            }
        }
    }
  else
    {
      if (event->xselectionrequest.property == None)
        event->xselectionrequest.property = event->xselectionrequest.target;

      if (convert_property (x11_display,
                            event->xselectionrequest.requestor,
                            event->xselectionrequest.target,
                            event->xselectionrequest.property))
        reply.property = event->xselectionrequest.property;
    }

  XSendEvent (x11_display->xdisplay,
              event->xselectionrequest.requestor,
              False, 0L, (XEvent*)&reply);

  meta_verbose ("Handled selection request");
}

static gboolean
close_display_idle_cb (gpointer user_data)
{
  MetaX11Display *x11_display = META_X11_DISPLAY (user_data);
  MetaDisplay *display = x11_display->display;
  MetaContext *context = meta_display_get_context (display);

  meta_display_close (display,
                      x11_display->xselectionclear_timestamp);
  x11_display->display_close_idle = 0;
  meta_context_terminate (context);

  return G_SOURCE_REMOVE;
}

static gboolean
process_selection_clear (MetaX11Display *x11_display,
                         XEvent         *event)
{
  if (x11_display->wm_sn_selection_window != event->xselectionclear.window ||
      x11_display->wm_sn_atom != event->xselectionclear.selection)
    {
      char *str;

      meta_x11_error_trap_push (x11_display);
      str = XGetAtomName (x11_display->xdisplay,
                          event->xselectionclear.selection);
      meta_x11_error_trap_pop (x11_display);

      meta_verbose ("Selection clear with selection %s window 0x%lx not a WM_Sn selection we recognize",
                    str ? str : "(bad atom)", event->xselectionclear.window);

      meta_XFree (str);

      return FALSE;
    }

  meta_verbose ("Got selection clear for on display %s",
                x11_display->name);

  /* We can't close a Display in an event handler. */
  if (!x11_display->display_close_idle)
    {
      x11_display->xselectionclear_timestamp = event->xselectionclear.time;
      x11_display->display_close_idle = g_idle_add (close_display_idle_cb, x11_display);
    }

  return TRUE;
}

static void
notify_bell (MetaX11Display *x11_display,
             XkbAnyEvent    *xkb_ev)
{
  MetaDisplay *display = x11_display->display;
  XkbBellNotifyEvent *xkb_bell_event = (XkbBellNotifyEvent*) xkb_ev;
  MetaWindow *window;

  window = meta_x11_display_lookup_x_window (x11_display,
                                             xkb_bell_event->window);
  if (!window && display->focus_window && display->focus_window->frame)
    window = display->focus_window;

  x11_display->last_bell_time = xkb_ev->time;
  if (!meta_bell_notify (display, window) &&
      meta_prefs_bell_is_audible ())
    {
      /* Force a classic bell if the libcanberra bell failed. */
      XkbForceDeviceBell (x11_display->xdisplay,
                          xkb_bell_event->device,
                          xkb_bell_event->bell_class,
                          xkb_bell_event->bell_id,
                          xkb_bell_event->percent);
    }
}

static void
handle_other_xevent (MetaX11Display *x11_display,
                     XEvent         *event)
{
  MetaDisplay *display = x11_display->display;
  MetaWorkspaceManager *workspace_manager = display->workspace_manager;
  Window modified;
  MetaWindow *window;
  MetaWindow *property_for_window;
  gboolean frame_was_receiver;

  modified = event_get_modified_window (x11_display, event);
  window = modified != None ? meta_x11_display_lookup_x_window (x11_display, modified) : NULL;
  frame_was_receiver = (window && window->frame && modified == window->frame->xwindow);

  /* We only want to respond to _NET_WM_USER_TIME property notify
   * events on _NET_WM_USER_TIME_WINDOW windows; in particular,
   * responding to UnmapNotify events is kind of bad.
   */
  property_for_window = NULL;
  if (window && modified == window->user_time_window)
    {
      property_for_window = window;
      window = NULL;
    }

  if (META_X11_DISPLAY_HAS_XSYNC (x11_display) &&
      event->type == (x11_display->xsync_event_base + XSyncAlarmNotify))
    {
      MetaSyncCounter *sync_counter;

      sync_counter = meta_x11_display_lookup_sync_alarm (x11_display,
                                                         ((XSyncAlarmNotifyEvent*)event)->alarm);

      if (sync_counter != NULL)
        {
          XSyncValue value = ((XSyncAlarmNotifyEvent*)event)->counter_value;
          gint64 new_counter_value;
          new_counter_value = XSyncValueLow32 (value) + ((gint64)XSyncValueHigh32 (value) << 32);
          meta_sync_counter_update (sync_counter, new_counter_value);
        }
      else if (x11_display->alarm_filters)
        {
          int i;

          for (i = 0; i < x11_display->alarm_filters->len; i++)
            {
              MetaX11AlarmFilter *alarm_filter =
                x11_display->alarm_filters->pdata[i];

              if (alarm_filter->filter (x11_display,
                                        (XSyncAlarmNotifyEvent *) event,
                                        alarm_filter->user_data))
                break;
            }
        }

      return;
    }

  if (META_X11_DISPLAY_HAS_SHAPE (x11_display) &&
      event->type == (x11_display->shape_event_base + ShapeNotify))
    {
      if (window)
        {
          XShapeEvent *sev = (XShapeEvent*) event;

          if (sev->kind == ShapeBounding)
            meta_window_x11_update_shape_region (window);
          else if (sev->kind == ShapeInput)
            meta_window_x11_update_input_region (window);
        }
      else
        {
          meta_topic (META_DEBUG_SHAPES,
                      "ShapeNotify not on a client window (window 0x%lx)",
                      modified);
        }

      return;
    }

  switch (event->type)
    {
    case KeymapNotify:
      break;
    case Expose:
      break;
    case GraphicsExpose:
      break;
    case NoExpose:
      break;
    case VisibilityNotify:
      break;
    case CreateNotify:
      {
        if (event->xcreatewindow.parent == x11_display->xroot)
          meta_stack_tracker_create_event (display->stack_tracker,
                                           &event->xcreatewindow);
      }
      break;

    case DestroyNotify:
      {
        if (event->xdestroywindow.event == x11_display->xroot)
          meta_stack_tracker_destroy_event (display->stack_tracker,
                                            &event->xdestroywindow);
      }
      if (window)
        {
          /* FIXME: It sucks that DestroyNotify events don't come with
           * a timestamp; could we do something better here?  Maybe X
           * will change one day?
           */
          guint32 timestamp;
          timestamp = meta_display_get_current_time_roundtrip (display);

          if (frame_was_receiver)
            {
              meta_x11_error_trap_push (x11_display);
              meta_window_destroy_frame (window->frame->window);
              meta_x11_error_trap_pop (x11_display);
            }
          else
            {
              /* Unmanage destroyed window */
              meta_window_unmanage (window, timestamp);
              window = NULL;
            }
        }
      break;
    case UnmapNotify:
      if (window)
        {
          /* FIXME: It sucks that UnmapNotify events don't come with
           * a timestamp; could we do something better here?  Maybe X
           * will change one day?
           */
          guint32 timestamp;
          timestamp = meta_display_get_current_time_roundtrip (display);

          if (!frame_was_receiver)
            {
              if (window->unmaps_pending == 0)
                {
                  meta_topic (META_DEBUG_WINDOW_STATE,
                              "Window %s withdrawn",
                              window->desc);

                  /* Unmanage withdrawn window */
                  window->withdrawn = TRUE;
                  meta_window_unmanage (window, timestamp);
                  window = NULL;
                }
              else
                {
                  window->unmaps_pending -= 1;
                  meta_topic (META_DEBUG_WINDOW_STATE,
                              "Received pending unmap, %d now pending",
                              window->unmaps_pending);
                }
            }
        }
      break;
    case MapNotify:
      /* NB: override redirect windows won't cause a map request so we
       * watch out for map notifies against any root windows too if a
       * compositor is enabled: */
      if (window == NULL && event->xmap.event == x11_display->xroot)
        {
          window = meta_window_x11_new (display, event->xmap.window,
                                        FALSE, META_COMP_EFFECT_CREATE);
        }
      else if (window && window->restore_focus_on_map &&
               window->reparents_pending == 0)
        {
          meta_window_focus (window,
                             meta_display_get_current_time_roundtrip (display));
        }

      break;
    case MapRequest:
      if (window == NULL)
        {
          Atom type;
          int format;
          unsigned long nitems, bytes_after, *data;

          /* Check whether the new window is a frame for another window */
          meta_x11_error_trap_push (x11_display);

          if (XGetWindowProperty (x11_display->xdisplay,
                                  event->xmaprequest.window,
                                  x11_display->atom__MUTTER_FRAME_FOR,
                                  0, 32, False, XA_WINDOW,
                                  &type, &format, &nitems, &bytes_after,
                                  (guchar **) &data) != Success)
            {
              meta_x11_error_trap_pop (x11_display);
              break;
            }

          if (meta_x11_error_trap_pop_with_return (x11_display) != Success)
            break;

          if (nitems == 1)
            {
              Window client_window;

              client_window = data[0];
              XFree (data);

              window = meta_x11_display_lookup_x_window (x11_display,
                                                         client_window);

              if (window != NULL && window->decorated && !window->frame)
                {
                  meta_window_set_frame_xwindow (window,
                                                 event->xmaprequest.window);
                  meta_window_x11_initialize_state (window);
                  meta_window_update_visibility (window);
                }

              break;
            }

          window = meta_window_x11_new (display, event->xmaprequest.window,
                                        FALSE, META_COMP_EFFECT_CREATE);
        }
      else
        {
          meta_verbose ("MapRequest on %s mapped = %d minimized = %d",
                        window->desc, window->mapped, window->minimized);

          if (window->minimized && !frame_was_receiver)
            {
              meta_window_unminimize (window);
              if (window->workspace != workspace_manager->active_workspace)
                {
                  meta_verbose ("Changing workspace due to MapRequest mapped = %d minimized = %d",
                                window->mapped, window->minimized);
                  meta_window_change_workspace (window,
                                                workspace_manager->active_workspace);
                }
            }
        }
      break;
    case ReparentNotify:
      {
        if (window && window->reparents_pending > 0)
          window->reparents_pending -= 1;
        if (event->xreparent.event == x11_display->xroot)
          meta_stack_tracker_reparent_event (display->stack_tracker,
                                             &event->xreparent);
      }
      break;
    case ConfigureNotify:
      if (event->xconfigure.event != event->xconfigure.window)
        {
          if (event->xconfigure.event == x11_display->xroot &&
              event->xconfigure.window != x11_display->composite_overlay_window)
            meta_stack_tracker_configure_event (display->stack_tracker,
                                                &event->xconfigure);
        }

      if (window && window->override_redirect)
        meta_window_x11_configure_notify (window, &event->xconfigure);

      break;
    case ConfigureRequest:
      /* This comment and code is found in both twm and fvwm */
      /*
       * According to the July 27, 1988 ICCCM draft, we should ignore size and
       * position fields in the WM_NORMAL_HINTS property when we map a window.
       * Instead, we'll read the current geometry.  Therefore, we should respond
       * to configuration requests for windows which have never been mapped.
       */
      if (window == NULL)
        {
          unsigned int xwcm;
          XWindowChanges xwc;

          xwcm = event->xconfigurerequest.value_mask &
            (CWX | CWY | CWWidth | CWHeight | CWBorderWidth);

          xwc.x = event->xconfigurerequest.x;
          xwc.y = event->xconfigurerequest.y;
          xwc.width = event->xconfigurerequest.width;
          xwc.height = event->xconfigurerequest.height;
          xwc.border_width = event->xconfigurerequest.border_width;

          meta_verbose ("Configuring withdrawn window to %d,%d %dx%d border %d (some values may not be in mask)",
                        xwc.x, xwc.y, xwc.width, xwc.height, xwc.border_width);
          meta_x11_error_trap_push (x11_display);
          XConfigureWindow (x11_display->xdisplay, event->xconfigurerequest.window,
                            xwcm, &xwc);
          meta_x11_error_trap_pop (x11_display);
        }
      else if (!frame_was_receiver)
        {
          meta_window_x11_configure_request (window, event);
        }
      else if (frame_was_receiver && window->frame)
        {
          meta_frame_handle_xevent (window->frame, event);
        }
      break;
    case GravityNotify:
      break;
    case ResizeRequest:
      break;
    case CirculateNotify:
      break;
    case CirculateRequest:
      break;
    case PropertyNotify:
      {
        MetaGroup *group;

        if (window && !frame_was_receiver)
          meta_window_x11_property_notify (window, event);
        else if (property_for_window && !frame_was_receiver)
          meta_window_x11_property_notify (property_for_window, event);
        else if (frame_was_receiver)
          meta_frame_handle_xevent (window->frame, event);

        group = meta_x11_display_lookup_group (x11_display,
                                               event->xproperty.window);
        if (group != NULL)
          meta_group_property_notify (group, event);

        if (event->xproperty.window == x11_display->xroot)
          {
            if (event->xproperty.atom ==
                x11_display->atom__NET_DESKTOP_LAYOUT)
              meta_x11_display_update_workspace_layout (x11_display);
            else if (event->xproperty.atom ==
                     x11_display->atom__NET_DESKTOP_NAMES)
              meta_x11_display_update_workspace_names (x11_display);

            /* we just use this property as a sentinel to avoid
             * certain race conditions.  See the comment for the
             * sentinel_counter variable declaration in display.h
             */
            if (event->xproperty.atom ==
                x11_display->atom__MUTTER_SENTINEL)
              {
                meta_x11_display_decrement_focus_sentinel (x11_display);
              }
          }
      }
      break;
    case SelectionRequest:
      process_selection_request (x11_display, event);
      break;
    case SelectionNotify:
      break;
    case ColormapNotify:
      break;
    case ClientMessage:
      if (window)
        {
#ifdef HAVE_XWAYLAND
          if (event->xclient.message_type == x11_display->atom_WL_SURFACE_ID)
            {
              guint32 surface_id = event->xclient.data.l[0];
              meta_xwayland_handle_wl_surface_id (window, surface_id);
            }
          else if (event->xclient.message_type ==
                   x11_display->atom__XWAYLAND_MAY_GRAB_KEYBOARD)
            {
              if (meta_is_wayland_compositor ())
                g_object_set (G_OBJECT (window),
                              "xwayland-may-grab-keyboard", (event->xclient.data.l[0] != 0),
                              NULL);
            }
          else
#endif
            meta_window_x11_client_message (window, event);
        }
      else
        {
          if (event->xclient.window == x11_display->xroot)
            {
              if (event->xclient.message_type ==
                  x11_display->atom__NET_CURRENT_DESKTOP)
                {
                  int space;
                  MetaWorkspace *workspace;
                  guint32 time;

                  space = event->xclient.data.l[0];
                  time = event->xclient.data.l[1];

                  meta_verbose ("Request to change current workspace to %d with "
                                "specified timestamp of %u",
                                space, time);

                  workspace = meta_workspace_manager_get_workspace_by_index (workspace_manager, space);

                  if (workspace)
                    {
                      /* Handle clients using the older version of the spec... */
                      if (time == 0)
                        time = meta_x11_display_get_current_time_roundtrip (x11_display);

                      meta_workspace_activate (workspace, time);
                    }
                  else
                    {
                      meta_verbose ("Don't know about workspace %d", space);
                    }
                }
              else if (event->xclient.message_type ==
                       x11_display->atom__NET_NUMBER_OF_DESKTOPS)
                {
                  int num_spaces;

                  num_spaces = event->xclient.data.l[0];

                  meta_verbose ("Request to set number of workspaces to %d",
                                num_spaces);

                  meta_prefs_set_num_workspaces (num_spaces);
                }
              else if (event->xclient.message_type ==
                       x11_display->atom__NET_SHOWING_DESKTOP)
                {
                  gboolean showing_desktop;
                  guint32  timestamp;

                  showing_desktop = event->xclient.data.l[0] != 0;
                  /* FIXME: Braindead protocol doesn't have a timestamp */
                  timestamp = meta_x11_display_get_current_time_roundtrip (x11_display);
                  meta_verbose ("Request to %s desktop",
                                showing_desktop ? "show" : "hide");

                  if (showing_desktop)
                    meta_workspace_manager_show_desktop (workspace_manager, timestamp);
                  else
                    {
                      meta_workspace_manager_unshow_desktop (workspace_manager);
                      meta_workspace_focus_default_window (workspace_manager->active_workspace, NULL, timestamp);
                    }
                }
              else if (event->xclient.message_type ==
                       x11_display->atom_WM_PROTOCOLS)
                {
                  meta_verbose ("Received WM_PROTOCOLS message");

                  if ((Atom)event->xclient.data.l[0] == x11_display->atom__NET_WM_PING)
                    {
                      guint32 timestamp = event->xclient.data.l[1];

                      meta_display_pong_for_serial (display, timestamp);
                    }
                }
            }

          if (event->xclient.message_type ==
              x11_display->atom__NET_REQUEST_FRAME_EXTENTS)
            {
              meta_verbose ("Received _NET_REQUEST_FRAME_EXTENTS message");
              process_request_frame_extents (x11_display, event);
            }
        }
      break;
    case MappingNotify:
      {
        gboolean ignore_current;

        ignore_current = FALSE;

        /* Check whether the next event is an identical MappingNotify
         * event.  If it is, ignore the current event, we'll update
         * when we get the next one.
         */
        if (XPending (x11_display->xdisplay))
          {
            XEvent next_event;

            XPeekEvent (x11_display->xdisplay, &next_event);

            if (next_event.type == MappingNotify &&
                next_event.xmapping.request == event->xmapping.request)
              ignore_current = TRUE;
          }

        if (!ignore_current)
          {
            /* Let XLib know that there is a new keyboard mapping.
             */
            XRefreshKeyboardMapping (&event->xmapping);
          }
      }
      break;
    default:
      if (event->type == x11_display->xkb_base_event_type)
        {
          XkbAnyEvent *xkb_ev = (XkbAnyEvent *) event;

          switch (xkb_ev->xkb_type)
            {
            case XkbBellNotify:
              if (XSERVER_TIME_IS_BEFORE(x11_display->last_bell_time,
                                         xkb_ev->time - 100))
                {
                  notify_bell (x11_display, xkb_ev);
                }
              break;
            default:
              break;
            }
        }

      break;
    }
}

static gboolean
window_has_xwindow (MetaWindow *window,
                    Window      xwindow)
{
  if (window->xwindow == xwindow)
    return TRUE;

  if (window->frame && window->frame->xwindow == xwindow)
    return TRUE;

  return FALSE;
}

static gboolean
process_selection_event (MetaX11Display *x11_display,
                         XEvent         *event)
{
  gboolean handled = FALSE;
  GList *l;

  handled |= meta_x11_selection_handle_event (x11_display, event);

  for (l = x11_display->selection.input_streams; l && !handled;)
    {
      GList *next = l->next;

      handled |= meta_x11_selection_input_stream_xevent (l->data, event);
      l = next;
    }

  for (l = x11_display->selection.output_streams; l && !handled;)
    {
      GList *next = l->next;

      handled |= meta_x11_selection_output_stream_xevent (l->data, event);
      l = next;
    }

  return handled;
}

/**
 * meta_display_handle_xevent:
 * @display: The MetaDisplay that events are coming from
 * @event: The event that just happened
 *
 * This is the most important function in the whole program. It is the heart,
 * it is the nexus, it is the Grand Central Station of Mutter's world.
 * When we create a #MetaDisplay, we ask GDK to pass *all* events for *all*
 * windows to this function. So every time anything happens that we might
 * want to know about, this function gets called. You see why it gets a bit
 * busy around here. Most of this function is a ginormous switch statement
 * dealing with all the kinds of events that might turn up.
 */
static void
meta_x11_display_handle_xevent (MetaX11Display *x11_display,
                                XEvent         *event)
{
  MetaDisplay *display = x11_display->display;
  MetaContext *context = meta_display_get_context (display);
  MetaBackend *backend = meta_context_get_backend (context);
  Window modified;
  gboolean bypass_compositor = FALSE;
  XIEvent *input_event;
  MetaCursorTracker *cursor_tracker;
#ifdef HAVE_XWAYLAND
  MetaWaylandCompositor *wayland_compositor;
#endif

  COGL_TRACE_BEGIN (MetaX11DisplayHandleXevent,
                    "X11Display (handle X11 event)");

  if (event->type == GenericEvent)
    XGetEventData (x11_display->xdisplay, &event->xcookie);

#if 0
  meta_spew_event_print (x11_display, event);
#endif

  meta_x11_display_run_event_funcs (x11_display, event);

  if (meta_x11_startup_notification_handle_xevent (x11_display, event))
    {
      bypass_compositor = TRUE;
      goto out;
    }

#ifdef HAVE_XWAYLAND
  wayland_compositor = meta_context_get_wayland_compositor (context);

  if (meta_is_wayland_compositor () &&
      meta_xwayland_manager_handle_xevent (&wayland_compositor->xwayland_manager,
                                           event))
    {
      bypass_compositor = TRUE;
      goto out;
    }
#endif

  if (process_selection_event (x11_display, event))
    {
      bypass_compositor = TRUE;
      goto out;
    }

  display->current_time = event_get_time (x11_display, event);

  if (META_IS_BACKEND_X11 (backend))
    meta_backend_x11_reset_cached_logical_monitor (META_BACKEND_X11 (backend));

  if (x11_display->focused_by_us &&
      event->xany.serial > x11_display->focus_serial &&
      display->focus_window &&
      display->focus_window->client_type == META_WINDOW_CLIENT_TYPE_X11 &&
      !window_has_xwindow (display->focus_window, x11_display->server_focus_window))
    {
      meta_topic (META_DEBUG_FOCUS, "Earlier attempt to focus %s failed",
                  display->focus_window->desc);
      meta_x11_display_update_focus_window (x11_display,
                                            x11_display->server_focus_window,
                                            x11_display->server_focus_serial,
                                            FALSE);
      meta_display_update_focus_window (display,
                                        meta_x11_display_lookup_x_window (x11_display,
                                                                          x11_display->server_focus_window));
    }

  if (event->xany.window == x11_display->xroot)
    {
      cursor_tracker = meta_backend_get_cursor_tracker (backend);
      if (META_IS_CURSOR_TRACKER_X11 (cursor_tracker))
        {
          MetaCursorTrackerX11 *cursor_tracker_x11 =
            META_CURSOR_TRACKER_X11 (cursor_tracker);

          if (meta_cursor_tracker_x11_handle_xevent (cursor_tracker_x11, event))
            {
              bypass_compositor = TRUE;
              goto out;
            }
        }
    }

  modified = event_get_modified_window (x11_display, event);

  input_event = get_input_event (x11_display, event);

  if (handle_input_xevent (x11_display, input_event, event->xany.serial))
    {
      bypass_compositor = TRUE;
      goto out;
    }

  handle_other_xevent (x11_display, event);

  if (event->type == SelectionClear)
    {
      if (process_selection_clear (x11_display, event))
        goto out;
    }

 out:
  if (!bypass_compositor && META_IS_COMPOSITOR_X11 (display->compositor))
    {
      MetaCompositorX11 *compositor_x11 =
        META_COMPOSITOR_X11 (display->compositor);
      MetaWindow *window;

      if (modified != None)
        window = meta_x11_display_lookup_x_window (x11_display, modified);
      else
        window = NULL;

      meta_compositor_x11_process_xevent (compositor_x11, event, window);
    }

  display->current_time = META_CURRENT_TIME;

  if (event->type == GenericEvent)
    XFreeEventData (x11_display->xdisplay, &event->xcookie);

  COGL_TRACE_DESCRIBE (MetaX11DisplayHandleXevent,
                       get_event_name (x11_display, event));
  COGL_TRACE_END (MetaX11DisplayHandleXevent);
}


static gboolean
xevent_func (XEvent   *xevent,
             gpointer  data)
{
  MetaX11Display *x11_display = data;

  meta_x11_display_handle_xevent (x11_display, xevent);

  return G_SOURCE_CONTINUE;
}

void
meta_x11_display_init_events (MetaX11Display *x11_display)
{
  x11_display->event_source = meta_x11_event_source_new (x11_display->xdisplay);
  g_source_set_callback (x11_display->event_source,
                         (GSourceFunc) xevent_func,
                         x11_display, NULL);
  g_source_attach (x11_display->event_source, NULL);
}

void
meta_x11_display_free_events (MetaX11Display *x11_display)
{
  g_source_destroy (x11_display->event_source);
  g_clear_pointer (&x11_display->event_source, g_source_unref);
}
