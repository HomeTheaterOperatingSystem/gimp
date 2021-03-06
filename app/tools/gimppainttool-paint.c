/* GIMP - The GNU Image Manipulation Program
 * Copyright (C) 1995 Spencer Kimball and Peter Mattis
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <gegl.h>
#include <gtk/gtk.h>

#include "libgimpmath/gimpmath.h"

#include "tools-types.h"

#include "core/gimpdrawable.h"
#include "core/gimpimage.h"
#include "core/gimpprojection.h"

#include "paint/gimppaintcore.h"
#include "paint/gimppaintoptions.h"

#include "display/gimpdisplay.h"
#include "display/gimpdisplayshell-utils.h"

#include "gimppainttool.h"
#include "gimppainttool-paint.h"


#define DISPLAY_UPDATE_INTERVAL 10000 /* microseconds */


typedef enum
{
  PAINT_ITEM_TYPE_INTERPOLATE,
  PAINT_ITEM_TYPE_FINISH
} PaintItemType;


typedef struct
{
  PaintItemType      type;

  union
  {
    struct
    {
      GimpPaintTool *paint_tool;
      GimpCoords     coords;
      guint32        time;
    };

    gboolean        *finished;
  };
} PaintItem;


/*  local function prototypes  */

static gboolean   gimp_paint_tool_paint_use_thread     (GimpPaintTool *paint_tool);
static gpointer   gimp_paint_tool_paint_thread         (gpointer       data);

static gboolean   gimp_paint_tool_paint_timeout        (GimpPaintTool *paint_tool);

static void       gimp_paint_tool_paint_update_outline (GimpPaintTool *paint_tool);


/*  static variables  */

static GThread           *paint_thread;

static GMutex             paint_mutex;
static GCond              paint_cond;

static GQueue             paint_queue = G_QUEUE_INIT;
static GMutex             paint_queue_mutex;
static GCond              paint_queue_cond;

static guint              paint_timeout_id;
static volatile gboolean  paint_timeout_pending;


/*  private functions  */


static gboolean
gimp_paint_tool_paint_use_thread (GimpPaintTool *paint_tool)
{
  if (! paint_tool->draw_line)
    {
      if (! paint_thread)
        {
          static gint use_paint_thread = -1;

          if (use_paint_thread < 0)
            use_paint_thread = g_getenv ("GIMP_NO_PAINT_THREAD") == NULL;

          if (use_paint_thread)
            {
              paint_thread = g_thread_new ("paint",
                                           gimp_paint_tool_paint_thread, NULL);
            }
        }

      return paint_thread != NULL;
    }

  return FALSE;
}

static gpointer
gimp_paint_tool_paint_thread (gpointer data)
{
  g_mutex_lock (&paint_queue_mutex);

  while (TRUE)
    {
      PaintItem *item;

      while (! (item = g_queue_pop_head (&paint_queue)))
        g_cond_wait (&paint_queue_cond, &paint_queue_mutex);

      switch (item->type)
        {
        case PAINT_ITEM_TYPE_INTERPOLATE:
          {
            GimpPaintTool    *paint_tool    = item->paint_tool;
            GimpPaintOptions *paint_options = GIMP_PAINT_TOOL_GET_OPTIONS (paint_tool);
            GimpPaintCore    *core          = paint_tool->core;
            GimpDrawable     *drawable      = paint_tool->drawable;

            g_mutex_unlock (&paint_queue_mutex);
            g_mutex_lock (&paint_mutex);

            while (paint_timeout_pending)
              g_cond_wait (&paint_cond, &paint_mutex);

            gimp_paint_core_interpolate (core, drawable, paint_options,
                                         &item->coords, item->time);

            g_mutex_unlock (&paint_mutex);
            g_mutex_lock (&paint_queue_mutex);
          }
          break;

        case PAINT_ITEM_TYPE_FINISH:
          {
            *item->finished = TRUE;
            g_cond_signal (&paint_queue_cond);
          }
          break;
        }

      g_slice_free (PaintItem, item);
    }

  g_mutex_unlock (&paint_queue_mutex);

  return NULL;
}

static gboolean
gimp_paint_tool_paint_timeout (GimpPaintTool *paint_tool)
{
  GimpDrawable *drawable = paint_tool->drawable;
  gboolean      update;

  paint_timeout_pending = TRUE;

  g_mutex_lock (&paint_mutex);

  update = gimp_drawable_flush_paint (drawable);

  if (update)
    gimp_paint_tool_paint_update_outline (paint_tool);

  paint_timeout_pending = FALSE;
  g_cond_signal (&paint_cond);

  g_mutex_unlock (&paint_mutex);

  if (update)
    {
      GimpDrawTool *draw_tool = GIMP_DRAW_TOOL (paint_tool);
      GimpDisplay  *display   = paint_tool->display;
      GimpImage    *image     = gimp_display_get_image (display);

      gimp_draw_tool_pause (draw_tool);

      gimp_projection_flush_now (gimp_image_get_projection (image));
      gimp_display_flush_now (display);

      gimp_draw_tool_resume (draw_tool);
    }

  return G_SOURCE_CONTINUE;
}

static void
gimp_paint_tool_paint_update_outline (GimpPaintTool *paint_tool)
{
  if (gimp_paint_tool_paint_use_thread (paint_tool))
    {
      gimp_paint_tool_set_draw_fallback (paint_tool, FALSE, 0.0);

      if (paint_tool->draw_brush)
        {
          GimpPaintCore *core     = paint_tool->core;
          GimpDisplay   *display  = paint_tool->display;
          GimpDrawable  *drawable = paint_tool->drawable;
          gint           off_x, off_y;
          gdouble        x, y;

          gimp_item_get_offset (GIMP_ITEM (drawable), &off_x, &off_y);

          x = core->cur_coords.x + off_x;
          y = core->cur_coords.y + off_y;

          if (paint_tool->outline)
            g_object_unref (paint_tool->outline);

          paint_tool->outline =
            GIMP_PAINT_TOOL_GET_CLASS (paint_tool)->get_outline (paint_tool,
                                                                 display, x, y);
        }
    }
}


/*  public functions  */


gboolean
gimp_paint_tool_paint_start (GimpPaintTool     *paint_tool,
                             GimpDisplay       *display,
                             const GimpCoords  *coords,
                             guint32            time,
                             gboolean           constrain,
                             GError           **error)
{
  GimpTool         *tool;
  GimpPaintOptions *paint_options;
  GimpPaintCore    *core;
  GimpDisplayShell *shell;
  GimpImage        *image;
  GimpDrawable     *drawable;
  GimpCoords        curr_coords;
  gint              off_x, off_y;

  g_return_val_if_fail (GIMP_IS_PAINT_TOOL (paint_tool), FALSE);
  g_return_val_if_fail (GIMP_IS_DISPLAY (display), FALSE);
  g_return_val_if_fail (coords != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail (paint_tool->display == NULL, FALSE);

  tool          = GIMP_TOOL (paint_tool);
  paint_tool    = GIMP_PAINT_TOOL (paint_tool);
  paint_options = GIMP_PAINT_TOOL_GET_OPTIONS (paint_tool);
  core          = paint_tool->core;
  shell         = gimp_display_get_shell (display);
  image         = gimp_display_get_image (display);
  drawable      = gimp_image_get_active_drawable (image);

  curr_coords = *coords;

  gimp_item_get_offset (GIMP_ITEM (drawable), &off_x, &off_y);

  curr_coords.x -= off_x;
  curr_coords.y -= off_y;

  /*  If we use a separate paint thread, enter paint mode before starting the
   *  paint core
   */
  if (gimp_paint_tool_paint_use_thread (paint_tool))
    gimp_drawable_start_paint (drawable);

  /*  Start the paint core  */
  if (! gimp_paint_core_start (core,
                               drawable, paint_options, &curr_coords,
                               error))
    {
      gimp_drawable_end_paint (drawable);

      return FALSE;
    }

  paint_tool->display  = display;
  paint_tool->drawable = drawable;

  if ((display != tool->display) || ! paint_tool->draw_line)
    {
      /*  If this is a new display, resest the "last stroke's endpoint"
       *  because there is none
       */
      if (display != tool->display)
        core->start_coords = core->cur_coords;

      core->last_coords = core->cur_coords;

      core->distance   = 0.0;
      core->pixel_dist = 0.0;
    }
  else if (paint_tool->draw_line)
    {
      /*  If shift is down and this is not the first paint
       *  stroke, then draw a line from the last coords to the pointer
       */
      gimp_paint_core_round_line (
        core, paint_options,
        constrain,
        gimp_display_shell_get_constrained_line_offset_angle (shell));
    }

  /*  Let the specific painting function initialize itself  */
  gimp_paint_core_paint (core, drawable, paint_options,
                         GIMP_PAINT_STATE_INIT, time);

  /*  Paint to the image  */
  if (paint_tool->draw_line)
    {
      gimp_paint_core_interpolate (core, drawable, paint_options,
                                   &core->cur_coords, time);
    }
  else
    {
      gimp_paint_core_paint (core, drawable, paint_options,
                             GIMP_PAINT_STATE_MOTION, time);
    }

  /*  Update the brush outline  */
  gimp_paint_tool_paint_update_outline (paint_tool);

  gimp_projection_flush_now (gimp_image_get_projection (image));
  gimp_display_flush_now (display);

  /*  Start the display update timeout  */
  if (gimp_paint_tool_paint_use_thread (paint_tool))
    {
      paint_timeout_id = g_timeout_add_full (
        G_PRIORITY_HIGH_IDLE,
        DISPLAY_UPDATE_INTERVAL / 1000,
        (GSourceFunc) gimp_paint_tool_paint_timeout,
        paint_tool, NULL);
    }

  return TRUE;
}

void
gimp_paint_tool_paint_end (GimpPaintTool *paint_tool,
                           guint32        time,
                           gboolean       cancel)
{
  GimpPaintOptions *paint_options;
  GimpPaintCore    *core;
  GimpDrawable     *drawable;

  g_return_if_fail (GIMP_IS_PAINT_TOOL (paint_tool));
  g_return_if_fail (paint_tool->display != NULL);

  paint_options = GIMP_PAINT_TOOL_GET_OPTIONS (paint_tool);
  core          = paint_tool->core;
  drawable      = paint_tool->drawable;

  /*  Process remaining paint items  */
  if (gimp_paint_tool_paint_use_thread (paint_tool))
    {
      PaintItem *item;
      gboolean   finished = FALSE;
      guint64    end_time;

      g_source_remove (paint_timeout_id);
      paint_timeout_id = 0;

      item = g_slice_new (PaintItem);

      item->type     = PAINT_ITEM_TYPE_FINISH;
      item->finished = &finished;

      g_mutex_lock (&paint_queue_mutex);

      g_queue_push_tail (&paint_queue, item);
      g_cond_signal (&paint_queue_cond);

      end_time = g_get_monotonic_time () + DISPLAY_UPDATE_INTERVAL;

      while (! finished)
        {
          if (! g_cond_wait_until (&paint_queue_cond, &paint_queue_mutex,
                                   end_time))
            {
              g_mutex_unlock (&paint_queue_mutex);

              gimp_paint_tool_paint_timeout (paint_tool);

              g_mutex_lock (&paint_queue_mutex);

              end_time = g_get_monotonic_time () + DISPLAY_UPDATE_INTERVAL;
            }
        }

      g_mutex_unlock (&paint_queue_mutex);
    }

  /*  Let the specific painting function finish up  */
  gimp_paint_core_paint (core, drawable, paint_options,
                         GIMP_PAINT_STATE_FINISH, time);

  if (cancel)
    gimp_paint_core_cancel (core, drawable);
  else
    gimp_paint_core_finish (core, drawable, TRUE);

  /*  Clear the brush outline  */
  g_clear_object (&paint_tool->outline);

  /*  Exit paint mode  */
  if (gimp_paint_tool_paint_use_thread (paint_tool))
    gimp_drawable_end_paint (drawable);

  paint_tool->display  = NULL;
  paint_tool->drawable = NULL;
}

void
gimp_paint_tool_paint_motion (GimpPaintTool    *paint_tool,
                              const GimpCoords *coords,
                              guint32           time)
{
  GimpPaintOptions *paint_options;
  GimpPaintCore    *core;
  GimpDrawable     *drawable;
  GimpCoords        curr_coords;
  gint              off_x, off_y;

  g_return_if_fail (GIMP_IS_PAINT_TOOL (paint_tool));
  g_return_if_fail (coords != NULL);
  g_return_if_fail (paint_tool->display != NULL);

  paint_options = GIMP_PAINT_TOOL_GET_OPTIONS (paint_tool);
  core          = paint_tool->core;
  drawable      = paint_tool->drawable;

  curr_coords = *coords;

  gimp_item_get_offset (GIMP_ITEM (drawable), &off_x, &off_y);

  curr_coords.x -= off_x;
  curr_coords.y -= off_y;

  gimp_paint_core_smooth_coords (core, paint_options, &curr_coords);

  /*  Don't paint while the Shift key is pressed for line drawing  */
  if (paint_tool->draw_line)
    {
      gimp_paint_core_set_current_coords (core, &curr_coords);

      return;
    }

  if (gimp_paint_tool_paint_use_thread (paint_tool))
    {
      PaintItem *item;

      /*  Push an item to the queue, to be processed by the paint thread  */

      item = g_slice_new (PaintItem);

      item->type       = PAINT_ITEM_TYPE_INTERPOLATE;
      item->paint_tool = paint_tool;
      item->coords     = curr_coords;
      item->time       = time;

      g_mutex_lock (&paint_queue_mutex);

      g_queue_push_tail (&paint_queue, item);
      g_cond_signal (&paint_queue_cond);

      g_mutex_unlock (&paint_queue_mutex);
    }
  else
    {
      GimpDrawTool *draw_tool = GIMP_DRAW_TOOL (paint_tool);
      GimpDisplay  *display   = paint_tool->display;
      GimpImage    *image     = gimp_display_get_image (display);

      /*  Paint directly  */

      gimp_draw_tool_pause (draw_tool);

      gimp_paint_core_interpolate (core,
                                   drawable, paint_options, &curr_coords, time);

      gimp_projection_flush_now (gimp_image_get_projection (image));
      gimp_display_flush_now (display);

      gimp_draw_tool_resume (draw_tool);
    }
}
