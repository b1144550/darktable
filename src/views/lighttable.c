/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.
    copyright (c) 2011--2012 Henrik Andersson.
    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
/** this is the view for the lighttable module.  */
#include "bauhaus/bauhaus.h"
#include "common/collection.h"
#include "common/colorlabels.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/focus.h"
#include "common/grouping.h"
#include "common/history.h"
#include "common/image_cache.h"
#include "common/ratings.h"
#include "common/selection.h"
#include "common/undo.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "control/settings.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/drag_and_drop.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "views/view.h"
#include "views/view_api.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <gdk/gdkkeysyms.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

DT_MODULE(1)

#define FULL_PREVIEW_IN_MEMORY_LIMIT 9

typedef enum dt_lighttable_direction_t
{
  DIRECTION_NONE = -1,
  DIRECTION_UP = 0,
  DIRECTION_DOWN = 1,
  DIRECTION_LEFT = 2,
  DIRECTION_RIGHT = 3,
  DIRECTION_ZOOM_IN = 4,
  DIRECTION_ZOOM_OUT = 5,
  DIRECTION_TOP = 6,
  DIRECTION_BOTTOM = 7,
  DIRECTION_PGUP = 8,
  DIRECTION_PGDOWN = 9,
  DIRECTION_CENTER = 10,
} dt_lighttable_direction_t;

static gboolean rating_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                          GdkModifierType modifier, gpointer data);
static gboolean colorlabels_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                               GdkModifierType modifier, gpointer data);
static gboolean go_up_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                         GdkModifierType modifier, gpointer data);
static gboolean go_down_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                           GdkModifierType modifier, gpointer data);
static gboolean go_pgup_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                           GdkModifierType modifier, gpointer data);
static gboolean go_pgdown_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                             GdkModifierType modifier, gpointer data);

static void _update_collected_images(dt_view_t *self);

/* returns TRUE if lighttable is using the custom order filter */
static gboolean _is_custom_image_order_actif(dt_view_t *self);
/* returns TRUE if lighttable is using the custom order filter */
static gboolean _is_rating_order_actif(dt_view_t *self);
/* returns TRUE if lighttable is using the custom order filter */
static gboolean _is_colorlabels_order_actif(dt_view_t *self);
/* register for redraw only the selected images */
static void _redraw_selected_images(dt_view_t *self);

static gboolean _expose_again_full(gpointer user_data);

typedef struct dt_preview_surface_t
{
  int mip;
  int32_t imgid;
  int32_t width;
  int32_t height;
  cairo_surface_t *surface;
  uint8_t *rgbbuf;
  int w_lock;

  float w_fit;
  float h_fit;
  float zoom_100;

  float max_dx;
  float max_dy;
} dt_preview_surface_t;

/**
 * this organises the whole library:
 * previously imported film rolls..
 */
typedef struct dt_library_t
{
  // tmp mouse vars:
  float select_offset_x, select_offset_y;
  float pan_x, pan_y;
  int32_t last_selected_idx, selection_origin_idx;
  int button;
  int key_jump_offset;
  int using_arrows;
  int key_select;
  dt_lighttable_direction_t key_select_direction;
  uint32_t modifiers;
  uint32_t center, pan;
  dt_view_image_over_t activate_on_release;
  int32_t track, offset, first_visible_zoomable, first_visible_filemanager;
  float zoom_x, zoom_y;
  dt_view_image_over_t image_over;
  int full_preview_sticky;
  int32_t full_preview_id;
  int32_t full_preview_rowid;
  gboolean full_preview_follow_sel;
  int display_focus;
  gboolean offset_changed;
  int images_in_row;
  int max_rows;
  int visible_rows;
  int32_t single_img_id;
  dt_lighttable_layout_t current_layout;

  float pointed_img_x, pointed_img_y, pointed_img_wd, pointed_img_ht;
  dt_view_image_over_t pointed_img_over;

  float thumb_size;
  int32_t last_exposed_id;
  float offset_x, offset_y;
  gboolean force_expose_all;
  GHashTable *thumbs_table;

  uint8_t *full_res_thumb;
  int32_t full_res_thumb_id, full_res_thumb_wd, full_res_thumb_ht;
  dt_image_orientation_t full_res_thumb_orientation;
  dt_focus_cluster_t full_res_focus[49];

  int32_t last_mouse_over_id;

  int32_t collection_count;

  int last_first_selected; // offset of the last selected image inside the current selection

  // stuff for the audio player
  GPid audio_player_pid;   // the pid of the child process
  int32_t audio_player_id; // the imgid of the image the audio is played for
  guint audio_player_event_source;

  // zoom in image preview (full)
  int missing_thumbnails;
  float full_zoom;
  float full_x;
  float full_y;
  dt_preview_surface_t fp_surf[FULL_PREVIEW_IN_MEMORY_LIMIT];

  /* prepared and reusable statements */
  struct
  {
    /* main query statement, should be update on listener signal of collection */
    sqlite3_stmt *main_query;
    /* select imgid from selected_images */
    sqlite3_stmt *select_imgid_in_selection;
    /* delete from selected_images where imgid != ?1 */
    sqlite3_stmt *delete_except_arg;
    /* check if the group of the image under the mouse has others, too, ?1: group_id, ?2: imgid */
    sqlite3_stmt *is_grouped;
  } statements;

  GtkWidget *profile_floating_window;

} dt_library_t;

typedef struct dt_layout_image_t
{
    gint imgid;
    gint width, height, x, y;
} dt_layout_image_t;

static inline float absmul(float a, float b) {
  return a > b ? a/b : b/a;
}


/* drag and drop callbacks to reorder picture sequence (dnd)*/

static void _dnd_get_picture_reorder(GtkWidget *widget, GdkDragContext *context, gint x, gint y,
                                    GtkSelectionData *selection_data, guint target_type, guint time,
                                    gpointer data);
static void _dnd_begin_picture_reorder(GtkWidget *widget, GdkDragContext *context, gpointer user_data);

static gboolean _dnd_drag_picture_motion(GtkWidget *dest_button, GdkDragContext *dc, gint x, gint y, guint time, gpointer user_data);

static void _register_custom_image_order_drag_n_drop(dt_view_t *self);
static void _unregister_custom_image_order_drag_n_drop(dt_view_t *self);

static void _stop_audio(dt_library_t *lib);

const char *name(dt_view_t *self)
{
  return _("lighttable");
}


uint32_t view(const dt_view_t *self)
{
  return DT_VIEW_LIGHTTABLE;
}

static inline dt_lighttable_layout_t get_layout(void)
{
  return dt_view_lighttable_get_layout(darktable.view_manager);
}

static inline gint get_zoom(void)
{
  return dt_view_lighttable_get_zoom(darktable.view_manager);
}

static inline int get_display_num_images(void)
{
  return dt_view_lighttable_get_display_num_images(darktable.view_manager);
}

static inline void filmstrip_set_active_image(dt_library_t *lib, const int imgid)
{
  int offset = -1;
  GList *collected = dt_collection_get_all(darktable.collection, -1);
  if(collected)
  {
    int id = -1;
    int i = 0;
    GList *l = collected;
    while(l)
    {
      id = GPOINTER_TO_INT(l->data);
      if(imgid == id) break;
      i++;

      l = g_list_next(l);
    }
    if(imgid == id && id >= 0) offset = i;

    g_list_free(collected);
  }

  lib->last_first_selected = offset;
  dt_selection_select_single(darktable.selection, imgid);
  dt_view_filmstrip_set_active_image(darktable.view_manager, imgid);
  if(lib->full_preview_id > -1) lib->full_preview_id = imgid;
}

static void check_layout(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  const dt_lighttable_layout_t layout = get_layout();
  const dt_lighttable_layout_t layout_old = lib->current_layout;

  if(lib->current_layout == layout) return;
  lib->current_layout = layout;

  // layout has changed, let restore panels
  dt_ui_restore_panels(darktable.gui->ui);

  if(layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER)
  {
    if(lib->first_visible_zoomable >= 0 && layout_old == DT_LIGHTTABLE_LAYOUT_ZOOMABLE)
    {
      lib->first_visible_filemanager = lib->offset = lib->first_visible_zoomable;
    }
    lib->first_visible_zoomable = 0;

    if(lib->center) lib->offset = 0;
    lib->center = 0;

    lib->offset_changed = TRUE;
    lib->offset_x = 0;
    lib->offset_y = 0;
  }

  dt_lib_module_t *m = darktable.view_manager->proxy.filmstrip.module;
  dt_lib_module_t *timeline = darktable.view_manager->proxy.timeline.module;
  gboolean vs = dt_lib_is_visible(timeline);

  if(layout == DT_LIGHTTABLE_LAYOUT_EXPOSE || layout == DT_LIGHTTABLE_LAYOUT_CULLING || lib->full_preview_id != -1)
  {
    gtk_widget_hide(GTK_WIDGET(timeline->widget));
    gtk_widget_show(GTK_WIDGET(m->widget));
  }
  else
  {
    gtk_widget_hide(GTK_WIDGET(m->widget));
    if(vs) gtk_widget_show(GTK_WIDGET(timeline->widget));
    g_timeout_add(200, _expose_again_full, self);
  }
}

static inline void _destroy_preview_surface(dt_preview_surface_t *fp_surf)
{
  if(fp_surf->surface) cairo_surface_destroy(fp_surf->surface);
  fp_surf->surface = NULL;
  if(fp_surf->rgbbuf) free(fp_surf->rgbbuf);
  fp_surf->rgbbuf = NULL;
  fp_surf->mip = 0;
  fp_surf->width = 0;
  fp_surf->height = 0;
  fp_surf->imgid = -1;
  fp_surf->w_lock = 0;

  fp_surf->zoom_100 = 40.0f;
  fp_surf->w_fit = 0.0f;
  fp_surf->h_fit = 0.0f;

  fp_surf->max_dx = 0.0f;
  fp_surf->max_dy = 0.0f;
}

static void _full_preview_destroy(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;

  for(int i = 0; i < FULL_PREVIEW_IN_MEMORY_LIMIT; i++)
  {
    _destroy_preview_surface(lib->fp_surf + i);
  }
}

static void move_view(dt_library_t *lib, dt_lighttable_direction_t dir)
{
  const int iir = get_zoom();
  const int current_offset = lib->offset;

  switch(dir)
  {
    case DIRECTION_UP:
    {
      if(lib->offset >= 1) lib->offset = lib->offset - iir;
    }
    break;
    case DIRECTION_DOWN:
    {
      lib->offset = lib->offset + iir;
      while(lib->offset >= lib->collection_count) lib->offset -= iir;
    }
    break;
    case DIRECTION_PGUP:
    {
      lib->offset -= (lib->max_rows - 1) * iir;
      while(lib->offset <= -iir) lib->offset += iir;
    }
    break;
    case DIRECTION_PGDOWN:
    {
      lib->offset += (lib->max_rows - 1) * iir;
      while(lib->offset >= lib->collection_count) lib->offset -= iir;
    }
    break;
    case DIRECTION_TOP:
    {
      lib->offset = 0;
    }
    break;
    case DIRECTION_BOTTOM:
    {
      lib->offset = lib->collection_count - iir;
    }
    break;
    case DIRECTION_CENTER:
    {
      lib->offset -= lib->offset % iir;
    }
    break;
    default:
      break;
  }

  lib->first_visible_filemanager = lib->offset;
  lib->offset_changed = (current_offset != lib->offset);
}

/* This function allows the file manager view to zoom "around" the image
 * currently under the mouse cursor, instead of around the top left image */
static void zoom_around_image(dt_library_t *lib, double pointerx, double pointery, int width, int height,
                              int old_images_in_row, int new_images_in_row)
{
  /* calculate which image number (relative to total collection)
   * is currently under the cursor, i.e. which image is the zoom anchor */
  float wd = width / (float)old_images_in_row;
  float ht = width / (float)old_images_in_row;
  int pi = pointerx / (float)wd;
  int pj = pointery / (float)ht;

  int zoom_anchor_image = lib->offset + pi + (pj * old_images_in_row);

  // make sure that we don't try to zoom around an image that doesn't exist
  if(zoom_anchor_image > lib->collection_count) zoom_anchor_image = lib->collection_count;

  // make sure that we don't try to zoom around an image that doesn't exist
  if(zoom_anchor_image < 0) zoom_anchor_image = 0;

  /* calculate which image number (relative to offset) will be
   * under the cursor after zooming. Then subtract that value
   * from the zoom anchor image number to see what the new offset should be */
  wd = width / (float)new_images_in_row;
  ht = width / (float)new_images_in_row;
  pi = pointerx / (float)wd;
  pj = pointery / (float)ht;

  lib->offset = zoom_anchor_image - pi - (pj * new_images_in_row);
  lib->first_visible_filemanager = lib->offset;
  lib->offset_changed = TRUE;
}

static void _view_lighttable_collection_listener_internal(dt_view_t *self, dt_library_t *lib)
{
  lib->force_expose_all = TRUE;
  _unregister_custom_image_order_drag_n_drop(self);
  _register_custom_image_order_drag_n_drop(self);

  _full_preview_destroy(self);
  lib->full_zoom = 1.0f;
  lib->full_x = 0.0f;
  lib->full_y = 0.0f;

  _update_collected_images(self);
}

static void _view_lighttable_selection_listener_internal_culling(dt_view_t *self, dt_library_t *lib)
{
  if(lib->current_layout == DT_LIGHTTABLE_LAYOUT_CULLING)
  {
    // save the offset of the first selected image
    GList *first_selected = dt_collection_get_selected(darktable.collection, 1);
    // we have a selected image
    if(first_selected)
    {
      const int imgid = GPOINTER_TO_INT(first_selected->data);
      lib->last_first_selected = dt_collection_image_offset(imgid);
      if(imgid >= 0 && dt_view_filmstrip_get_activated_imgid(darktable.view_manager) != imgid)
        dt_view_filmstrip_set_active_image(darktable.view_manager, imgid);
      g_list_free(first_selected);
    }
    // we have the previous selected image
    else if(lib->last_first_selected >= 0)
    {
      GList *collected = dt_collection_get_all(darktable.collection, -1);
      if(collected)
      {
        GList *l = NULL;
        if(g_list_length(collected) > lib->last_first_selected)
          l = g_list_nth(collected, lib->last_first_selected);
        else
          l = g_list_last(collected);
        const int imgid = (l) ? GPOINTER_TO_INT(l->data) : -1;
        if(imgid >= 0) filmstrip_set_active_image(lib, imgid);
        g_list_free(collected);
      }
    }
    // this is a new collection, select the first image
    else
    {
      GList *collected = dt_collection_get_all(darktable.collection, 1);
      if(collected)
      {
        const int imgid = GPOINTER_TO_INT(collected->data);
        if(imgid >= 0) filmstrip_set_active_image(lib, imgid);
        g_list_free(collected);
      }
    }
  }
}

static void _view_lighttable_selection_listener_internal_preview(dt_view_t *self, dt_library_t *lib)
{
  if(lib->full_preview_id != -1)
  {
    GList *first_selected = dt_collection_get_selected(darktable.collection, 1);
    // we have a selected image
    if(first_selected)
    {
      const int imgid = GPOINTER_TO_INT(first_selected->data);
      if(lib->full_preview_id != imgid)
      {
        lib->full_preview_id = imgid;
        dt_control_queue_redraw_center();
      }
      g_list_free(first_selected);
    }
  }
}

static void _view_lighttable_query_listener_callback(gpointer instance, gpointer user_data)
{
  // this will always happen in conjonction with the _view_lighttable_collection_listener_callback
  // so we only need to reset the offset
  dt_view_t *self = (dt_view_t *)user_data;
  dt_library_t *lib = (dt_library_t *)self->data;

  // in filemanager, we want to reset the offset to the beggining
  const dt_lighttable_layout_t layout = get_layout();
  if(layout == lib->current_layout && layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER && lib->offset > 0
     && lib->first_visible_filemanager > 0)
  {
    lib->offset = lib->first_visible_filemanager = 0;
    lib->offset_changed = TRUE;
  }
  // also in culling
  if(layout == lib->current_layout && layout == DT_LIGHTTABLE_LAYOUT_CULLING)
  {
    lib->last_first_selected = -1;
    GList *collected = dt_collection_get_all(darktable.collection, 1);
    if(collected)
    {
      const int imgid = GPOINTER_TO_INT(collected->data);
      if(imgid >= 0) filmstrip_set_active_image(lib, imgid);
      g_list_free(collected);
    }
  }
}

static void _view_lighttable_collection_listener_callback(gpointer instance, gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_library_t *lib = (dt_library_t *)self->data;

  if(lib->current_layout != DT_LIGHTTABLE_LAYOUT_CULLING)
    _view_lighttable_collection_listener_internal(self, lib);
  _view_lighttable_selection_listener_internal_culling(self, lib);
}

static void _view_lighttable_selection_listener_callback(gpointer instance, gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_library_t *lib = (dt_library_t *)self->data;

  // we need to redraw all thumbs to display the selected ones, record full redraw here
  lib->force_expose_all = TRUE;

  // we handle change of selection only in expose mode. it is needed
  // here as the selection from the filmstrip is actually what must be
  // displayed in the expose view.
  if(lib->current_layout == DT_LIGHTTABLE_LAYOUT_EXPOSE)
    _view_lighttable_collection_listener_internal(self, lib);
  else if(lib->current_layout == DT_LIGHTTABLE_LAYOUT_CULLING)
  {
    _view_lighttable_selection_listener_internal_culling(self, lib);
    dt_control_queue_redraw_center();
  }
  else if(lib->full_preview_id != -1)
  {
    _view_lighttable_selection_listener_internal_preview(self, lib);
  }
}

static void _update_collected_images(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  sqlite3_stmt *stmt;
  int32_t min_before = 0, min_after = -1;

  /* check if we can get a query from collection */
  gchar *query = g_strdup(dt_collection_get_query(darktable.collection));
  if(!query) return;

  // we have a new query for the collection of images to display. For speed reason we collect all images into
  // a temporary (in-memory) table (collected_images).
  //
  // 0. get current lower rowid
  if (lib->full_preview_id != -1)
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT MIN(rowid) FROM memory.collected_images",
                                -1, &stmt, NULL);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      min_before = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
  }

  // 1. drop previous data

  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM memory.collected_images", NULL, NULL,
                        NULL);
  // reset autoincrement. need in star_key_accel_callback
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM memory.sqlite_sequence WHERE "
                                                       "name='collected_images'", NULL, NULL, NULL);

  // 2. insert collected images into the temporary table

  gchar *ins_query = NULL;
  ins_query = dt_util_dstrcat(ins_query, "INSERT INTO memory.collected_images (imgid) %s", query);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), ins_query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, 0);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, -1);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  g_free(query);
  g_free(ins_query);

  // 3. get new low-bound, then update the full preview rowid accordingly
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT MIN(rowid) FROM memory.collected_images", -1,
                              &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    min_after = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);

  if(lib->full_preview_id != -1)
  {
    // note that this adjustement is needed as for a memory table the rowid doesn't start to 1 after the DELETE
    // above, but rowid is incremented each time we INSERT.
    lib->full_preview_rowid += (min_after - min_before);

    char col_query[128] = { 0 };
    snprintf(col_query, sizeof(col_query), "SELECT imgid FROM memory.collected_images WHERE rowid=%d", lib->full_preview_rowid);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), col_query, -1, &stmt, NULL);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      const int nid = sqlite3_column_int(stmt, 0);
      if (nid != lib->full_preview_id)
      {
        lib->full_preview_id = sqlite3_column_int(stmt, 0);
        dt_control_set_mouse_over_id(lib->full_preview_id);
      }
    }
    sqlite3_finalize(stmt);
  }

  if(lib->single_img_id != -1 && min_after != -1)
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT rowid FROM memory.collected_images WHERE imgid=?1", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, lib->single_img_id);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      const int new_rowid = sqlite3_column_int(stmt, 0);
      lib->first_visible_filemanager = lib->offset = new_rowid - min_after;
    }
    sqlite3_finalize(stmt);
  }

  /* if we have a statment lets clean it */
  if(lib->statements.main_query) sqlite3_finalize(lib->statements.main_query);

  /* prepare a new main query statement for collection */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT imgid FROM memory.collected_images ORDER BY rowid LIMIT ?1, ?2", -1,
                              &lib->statements.main_query, NULL);

  dt_control_queue_redraw_center();
}

static void _set_position(dt_view_t *self, uint32_t pos)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  // only reset position when not already with a changed offset, this is because if the offset is
  // already changed it means that we are about to change the display (zoom in or out for example).
  // And in this case a new offset is already positioned and we don't want to reset it.
  if(!lib->offset_changed)
  {
    lib->first_visible_filemanager = lib->first_visible_zoomable = lib->offset = pos;
    lib->offset_changed = TRUE;
    dt_control_queue_redraw_center();
  }
}

static uint32_t _get_position(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  const dt_lighttable_layout_t layout = get_layout();

  if(layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER)
    return MAX(0, lib->first_visible_filemanager);
  else
    return MAX(0, lib->first_visible_zoomable);
}

static int _get_images_in_row(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  return lib->images_in_row;
}

static int _get_full_preview_id(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  return lib->full_preview_id;
}

static inline int _get_max_in_memory_images()
{
  const int max_in_memory_images = dt_conf_get_int("plugins/lighttable/preview/max_in_memory_images");
  return MIN(max_in_memory_images, FULL_PREVIEW_IN_MEMORY_LIMIT);
}

static void _sort_preview_surface(dt_library_t *lib, dt_layout_image_t *images, const int sel_img_count,
                                  const int max_in_memory_images)
{
#define SWAP_PREVIEW_SURFACE(x1, x2)                                                                              \
  {                                                                                                               \
    dt_preview_surface_t surf_tmp = lib->fp_surf[x1];                                                             \
    lib->fp_surf[x1] = lib->fp_surf[x2];                                                                          \
    lib->fp_surf[x2] = surf_tmp;                                                                                  \
  }

  const int in_memory_limit = MIN(max_in_memory_images, FULL_PREVIEW_IN_MEMORY_LIMIT);

  for(int i = 0; i < sel_img_count; i++)
  {
    // we assume that there's only one cache per image
    if(images[i].imgid != lib->fp_surf[i].imgid)
    {
      int j = 0;
      // search the image in cache
      while(j < in_memory_limit && lib->fp_surf[j].imgid != images[i].imgid) j++;
      // found one, swap it
      if(j < in_memory_limit)
        SWAP_PREVIEW_SURFACE(i, j)
      else if(lib->fp_surf[i].imgid >= 0)
      {
        // check if there's an empty entry so we can save this cache
        j = 0;
        while(j < in_memory_limit && lib->fp_surf[j].imgid >= 0) j++;
        // found one, swap it
        if(j < in_memory_limit)
          SWAP_PREVIEW_SURFACE(i, j)
        else
        {
          // cache is full, get rid of the farthest one
          const int offset_current = dt_collection_image_offset(images[i].imgid);
          int offset_max = -1;
          int max_i = -1;
          j = i;
          while(j < in_memory_limit)
          {
            const int offset = dt_collection_image_offset(lib->fp_surf[j].imgid);
            if(abs(offset_current - offset) > offset_max)
            {
              offset_max = abs(offset_current - offset);
              max_i = j;
            }
            j++;
          }
          if(max_i >= 0 && max_i != i) SWAP_PREVIEW_SURFACE(i, max_i)
        }
      }
    }
  }

  // keep only the first max_in_memory_images cache entries
  for(int i = max_in_memory_images; i < FULL_PREVIEW_IN_MEMORY_LIMIT; i++)
  {
    _destroy_preview_surface(lib->fp_surf + i);
  }

#undef SWAP_PREVIEW_SURFACE
}

void init(dt_view_t *self)
{
  self->data = calloc(1, sizeof(dt_library_t));
  dt_library_t *lib = (dt_library_t *)self->data;

  darktable.view_manager->proxy.lighttable.set_position = _set_position;
  darktable.view_manager->proxy.lighttable.get_position = _get_position;
  darktable.view_manager->proxy.lighttable.get_images_in_row = _get_images_in_row;
  darktable.view_manager->proxy.lighttable.get_full_preview_id = _get_full_preview_id;
  darktable.view_manager->proxy.lighttable.view = self;

  lib->select_offset_x = lib->select_offset_y = 0.5f;
  lib->last_selected_idx = -1;
  lib->selection_origin_idx = -1;
  lib->key_jump_offset = 0;
  lib->first_visible_zoomable = -1;
  lib->first_visible_filemanager = -1;
  lib->button = 0;
  lib->modifiers = 0;
  lib->center = lib->pan = lib->track = 0;
  lib->activate_on_release = DT_VIEW_ERR;
  lib->zoom_x = dt_conf_get_float("lighttable/ui/zoom_x");
  lib->zoom_y = dt_conf_get_float("lighttable/ui/zoom_y");
  lib->full_preview_id = -1;
  lib->display_focus = 0;
  lib->last_mouse_over_id = -1;
  lib->full_res_thumb = 0;
  lib->full_res_thumb_id = -1;
  lib->audio_player_id = -1;
  lib->single_img_id = -1;

  lib->thumb_size = -1;
  lib->pointed_img_over = DT_VIEW_ERR;
  lib->last_exposed_id = -1;
  lib->force_expose_all = FALSE;
  lib->offset_x = 0;
  lib->offset_y = 0;

  lib->missing_thumbnails = 0;
  lib->full_zoom = 1.0f;
  lib->full_x = 0;
  lib->full_y = 0;

  lib->last_first_selected = -1;

  for(int i = 0; i < FULL_PREVIEW_IN_MEMORY_LIMIT; i++)
  {
    lib->fp_surf[i].mip = 0;
    lib->fp_surf[i].imgid = -1;
    lib->fp_surf[i].width = 0;
    lib->fp_surf[i].height = 0;
    lib->fp_surf[i].surface = NULL;
    lib->fp_surf[i].rgbbuf = NULL;
    lib->fp_surf[i].w_lock = 0;
    lib->fp_surf[i].zoom_100 = 40.0f;
    lib->fp_surf[i].w_fit = 0.0f;
    lib->fp_surf[i].h_fit = 0.0f;
  }

  lib->thumbs_table = g_hash_table_new(g_int_hash, g_int_equal);

  /* setup collection listener and initialize main_query statement */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_view_lighttable_collection_listener_callback), (gpointer)self);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_COLLECTION_QUERY_CHANGED,
                            G_CALLBACK(_view_lighttable_query_listener_callback), (gpointer)self);

  dt_control_signal_connect(darktable.signals, DT_SIGNAL_SELECTION_CHANGED,
                            G_CALLBACK(_view_lighttable_selection_listener_callback), (gpointer)self);

  _view_lighttable_collection_listener_callback(NULL, self);

  /* initialize reusable sql statements */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM main.selected_images WHERE imgid != ?1",
                              -1, &lib->statements.delete_except_arg, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT id FROM main.images WHERE group_id = ?1 AND id != ?2", -1,
                              &lib->statements.is_grouped, NULL); // TODO: only check in displayed images?
}


void cleanup(dt_view_t *self)
{
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_view_lighttable_collection_listener_callback), self);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_view_lighttable_query_listener_callback), self);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_view_lighttable_selection_listener_callback), self);

  dt_library_t *lib = (dt_library_t *)self->data;
  dt_conf_set_float("lighttable/ui/zoom_x", lib->zoom_x);
  dt_conf_set_float("lighttable/ui/zoom_y", lib->zoom_y);
  if(lib->audio_player_id != -1) _stop_audio(lib);
  g_hash_table_destroy(lib->thumbs_table);
  free(lib->full_res_thumb);
  free(self->data);
}

/**
 * \brief A helper function to convert grid coordinates to an absolute index
 *
 * \param[in] row The row
 * \param[in] col The column
 * \param[in] stride The stride (number of columns per row)
 * \param[in] offset The zero-based index of the top-left image (aka the count of images above the viewport,
 *minus 1)
 * \return The absolute, zero-based index of the specified grid location
 */

static int expose_filemanager(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx,
                               int32_t pointery)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  const gboolean offset_changed = lib->offset_changed;
  int missing = 0;

  lib->zoom_x = lib->zoom_y = 0;

  /* query new collection count */
  lib->collection_count = dt_collection_get_count(darktable.collection);

  if(darktable.gui->center_tooltip == 1) darktable.gui->center_tooltip = 2;

  /* get grid stride */
  const int iir = get_zoom();

  /* get image over id */
  lib->image_over = DT_VIEW_DESERT;
  lib->pointed_img_over = DT_VIEW_ERR;
  int32_t mouse_over_id = dt_control_get_mouse_over_id(), mouse_over_group = -1;
  /* need to keep this one as it needs to be refreshed */
  const int initial_mouse_over_id = mouse_over_id;

  /* fill background */
  if (mouse_over_id == -1 || lib->force_expose_all || iir == 1 || offset_changed || lib->images_in_row != iir)
  {
    lib->force_expose_all = TRUE;
    lib->last_exposed_id = -1;
    dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_LIGHTTABLE_BG);
    cairo_paint(cr);
  }

  lib->images_in_row = iir;

  const float wd = width / (float)iir;
  const float ht = width / (float)iir;
  lib->thumb_size = wd;

  int pi = pointerx / (float)wd;
  int pj = pointery / (float)ht;
  if(pointerx < 0 || pointery < 0) pi = pj = -1;

  const int img_pointerx = iir == 1 ? pointerx : fmodf(pointerx, wd);
  const int img_pointery = iir == 1 ? pointery : fmodf(pointery, ht);

  const int max_rows = 1 + (int)((height) / ht + .5);
  lib->max_rows = max_rows;
  lib->visible_rows = height / ht;
  const int max_cols = iir;

  int id;

  /* get the count of current collection */

  if(lib->collection_count == 0)
  {
    const float fs = DT_PIXEL_APPLY_DPI(15.0f);
    const float ls = 1.5f * fs;
    const float offy = height * 0.2f;
    const float offx = DT_PIXEL_APPLY_DPI(60);
    const float at = 0.3f;
    PangoLayout *layout;
    PangoRectangle ink;
    PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
    pango_font_description_set_absolute_size(desc, fs * PANGO_SCALE);
    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, desc);
    cairo_set_font_size(cr, fs);
    cairo_set_source_rgba(cr, .7, .7, .7, 1.0f);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    pango_layout_set_text(layout, _("there are no images in this collection"), -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    cairo_move_to(cr, offx, offy - ink.height - ink.x);
    pango_cairo_show_layout(cr, layout);
    pango_layout_set_text(layout, _("if you have not imported any images yet"), -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    cairo_move_to(cr, offx, offy + 2 * ls - ink.height - ink.x);
    pango_cairo_show_layout(cr, layout);
    pango_layout_set_text(layout, _("you can do so in the import module"), -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    cairo_move_to(cr, offx, offy + 3 * ls - ink.height - ink.x);
    pango_cairo_show_layout(cr, layout);
    cairo_move_to(cr, offx - DT_PIXEL_APPLY_DPI(10.0f), offy + 3 * ls - ls * .25f);
    cairo_line_to(cr, 0.0f, 10.0f);
    cairo_set_source_rgba(cr, .7, .7, .7, at);
    cairo_stroke(cr);
    pango_layout_set_text(layout, _("try to relax the filter settings in the top panel"), -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    cairo_move_to(cr, offx, offy + 5 * ls - ink.height - ink.x);
    cairo_set_source_rgba(cr, .7, .7, .7, 1.0f);
    pango_cairo_show_layout(cr, layout);
    cairo_rel_move_to(cr, 10.0f + ink.width, ink.height * 0.5f);
    cairo_line_to(cr, width * 0.5f, 0.0f);
    cairo_set_source_rgba(cr, .7, .7, .7, at);
    cairo_stroke(cr);
    pango_layout_set_text(layout, _("or add images in the collection module in the left panel"), -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    cairo_move_to(cr, offx, offy + 6 * ls - ink.height - ink.x);
    cairo_set_source_rgba(cr, .7, .7, .7, 1.0f);
    pango_cairo_show_layout(cr, layout);
    cairo_move_to(cr, offx - DT_PIXEL_APPLY_DPI(10.0f), offy + 6 * ls - ls * 0.25f);
    cairo_rel_line_to(cr, -offx + 10.0f, 0.0f);
    cairo_set_source_rgba(cr, .7, .7, .7, at);
    cairo_stroke(cr);

    pango_font_description_free(desc);
    g_object_unref(layout);
    return 0;
  }

  /* do we have a main query collection statement */
  if(!lib->statements.main_query) return 0;

  int32_t offset = lib->offset
      = MIN(lib->first_visible_filemanager, ((lib->collection_count + iir - 1) / iir - 1) * iir);

  int32_t drawing_offset = 0;
  if(offset < 0)
  {
    drawing_offset = offset;
    offset = 0;
  }

  /* update scroll borders */
  int shown_rows = ceilf((float)lib->collection_count / iir);
  if(iir > 1) shown_rows += max_rows - 2;
  dt_view_set_scrollbar(self, 0, 0, 1, 1, offset, 0, shown_rows * iir, (max_rows - 1) * iir);

  /* let's reset and reuse the main_query statement */
  DT_DEBUG_SQLITE3_CLEAR_BINDINGS(lib->statements.main_query);
  DT_DEBUG_SQLITE3_RESET(lib->statements.main_query);

  /* setup offset and row for the main query */
  DT_DEBUG_SQLITE3_BIND_INT(lib->statements.main_query, 1, offset);
  DT_DEBUG_SQLITE3_BIND_INT(lib->statements.main_query, 2, max_rows * iir);

  if(mouse_over_id != -1)
  {
    const dt_image_t *mouse_over_image = dt_image_cache_get(darktable.image_cache, mouse_over_id, 'r');
    mouse_over_group = mouse_over_image->group_id;
    dt_image_cache_read_release(darktable.image_cache, mouse_over_image);
    DT_DEBUG_SQLITE3_CLEAR_BINDINGS(lib->statements.is_grouped);
    DT_DEBUG_SQLITE3_RESET(lib->statements.is_grouped);
    DT_DEBUG_SQLITE3_BIND_INT(lib->statements.is_grouped, 1, mouse_over_group);
    DT_DEBUG_SQLITE3_BIND_INT(lib->statements.is_grouped, 2, mouse_over_id);
    if(sqlite3_step(lib->statements.is_grouped) != SQLITE_ROW) mouse_over_group = -1;
  }

  // prefetch the ids so that we can peek into the future to see if there are adjacent images in the same
  // group.
  int *query_ids = (int *)calloc(max_rows * max_cols, sizeof(int));
  if(!query_ids) goto after_drawing;
  for(int row = 0; row < max_rows; row++)
  {
    for(int col = 0; col < max_cols; col++)
    {
      if(sqlite3_step(lib->statements.main_query) == SQLITE_ROW)
        query_ids[row * iir + col] = sqlite3_column_int(lib->statements.main_query, 0);
      else
        goto end_query_cache;
    }
  }

end_query_cache:
  mouse_over_id = -1;
  cairo_save(cr);
  int current_image = 0;
  int before_mouse_over_id = 0;
  const int before_last_exposed_id = lib->last_exposed_id;

  if (lib->using_arrows)
  {
    before_mouse_over_id = dt_control_get_mouse_over_id();
  }

  for(int row = 0; row < max_rows; row++)
  {
    for(int col = 0; col < max_cols; col++)
    {
      // curidx = grid_to_index(row, col, iir, offset);

      /* skip drawing images until we reach a non-negative offset.
       * This is needed for zooming, so that the image under the
       * mouse cursor can stay there. */
      if(drawing_offset < 0)
      {
        drawing_offset++;
        cairo_translate(cr, wd, 0.0f);
        continue;
      }

      id = query_ids[current_image];
      current_image++;

      if(id > 0)
      {
        if(iir == 1 && row) continue;

        /* set mouse over id if pointer is in current row / col */
        if (lib->using_arrows)
        {
          if(before_mouse_over_id == -1)
          {
            // mouse has never been in filemanager area set mouse on first image and ignore this movement
            before_mouse_over_id = query_ids[0];
          }

          if(before_mouse_over_id == id)
          {
            // I would like to jump from before_mouse_over_id to query_ids[idx]
            const int idx = current_image+lib->key_jump_offset-1;
            const int current_row = (int)((current_image-1)/iir);
            const int current_col = current_image%iir;

            // detect if the current movement need some extra movement (page adjust)
            if(current_row  == (int)(max_rows-1.5) && lib->key_jump_offset == iir)
            {
              // going DOWN from last row
              lib->force_expose_all = TRUE;
              move_view(lib, DIRECTION_DOWN);
            }
            else if(current_row  == 0 && lib->key_jump_offset == iir*-1)
            {
              // going UP from first row
              lib->force_expose_all = TRUE;
              move_view(lib, DIRECTION_UP);
            }
            else if(current_row == (int)(max_rows-1.5) && current_col ==  0 && lib->key_jump_offset == 1)
            {
              // going RIGHT from last visible
              lib->force_expose_all = TRUE;
              move_view(lib, DIRECTION_DOWN);
            }
            else if(current_row == 0 && current_col ==  1 && lib->key_jump_offset == -1)
            {
              // going LEFT from first visible
              lib->force_expose_all = TRUE;
              move_view(lib, DIRECTION_UP);
            }

            // handle the selection from keyboard, shift + movement
            if (lib->key_jump_offset != 0 && lib->key_select)
            {
              const int direction = (lib->key_jump_offset > 0) ? DIRECTION_RIGHT : DIRECTION_LEFT;
              if (lib->key_select_direction != direction)
              {
                if(lib->key_select_direction != DIRECTION_NONE)
                  dt_selection_toggle(darktable.selection, before_mouse_over_id);
                lib->key_select_direction = direction;
              }
              int loop_count = abs(lib->key_jump_offset); // ex: from -10 to 1  // from 10 to 1
              while (loop_count--)
              {
                // ex shift + down toggle selection on images_in_row images
                const int to_toggle = idx+(-1*lib->key_jump_offset/abs(lib->key_jump_offset)*loop_count);
                if (query_ids[to_toggle])
                  dt_selection_toggle(darktable.selection, query_ids[to_toggle]);
              }
            }

            if(idx > -1 && idx < lib->collection_count && query_ids[idx])
            {
              // offset is valid..we know where to jump
              mouse_over_id = query_ids[idx];

              // we reset the key_jump_offset only in this case, we know where to jump. If we don't know it may
              // be the case that we are moving UP and the row is still not displayed. Next cycle the row will
              // be displayed (move_view) and the picture will be available.
              lib->key_jump_offset = 0;
            }
            else
            {
              // going into a non existing position. Do nothing
              mouse_over_id = before_mouse_over_id;
              lib->force_expose_all = TRUE;
            }

            // if we have moved the view we need to expose again all pictures as the first row or last one need to
            // be redrawn properly. for this we just record the missing thumbs.
            if(offset_changed && mouse_over_id != -1)
            {
              missing += iir;
            }
          }
        }
        else if(pi == col && pj == row)
        {
          mouse_over_id = id;
        }

        if(!lib->pan && (iir != 1 || mouse_over_id != -1)) dt_control_set_mouse_over_id(mouse_over_id);

        cairo_save(cr);

        if(iir == 1)
        {
          // we are on the single-image display at a time, in this case we want the selection to be updated to
          // contain this single image.
          dt_selection_select_single(darktable.selection, id);
          lib->single_img_id = id;
        }
        else
          lib->single_img_id = -1;

        if (id == mouse_over_id
            || lib->force_expose_all
            || id == before_last_exposed_id
            || id == initial_mouse_over_id
            || g_hash_table_contains(lib->thumbs_table, (gpointer)&id))
        {
          if(!lib->force_expose_all && id == mouse_over_id) lib->last_exposed_id = id;
          dt_view_image_expose_t params = { 0 };
          params.image_over = &(lib->image_over);
          params.imgid = id;
          params.cr = cr;
          params.width = wd;
          params.height = iir == 1 ? height : ht;
          params.px = pi == col && pj == row ? img_pointerx : -1;
          params.py = pi == col && pj == row ? img_pointery : -1;
          params.zoom = iir;
          const int thumb_missed = dt_view_image_expose(&params);

          if(id == mouse_over_id)
          {
            lib->pointed_img_x = col * wd;
            lib->pointed_img_y = row * ht;
            lib->pointed_img_wd = wd;
            lib->pointed_img_ht = iir == 1 ? height : ht;
            lib->pointed_img_over = dt_view_guess_image_over(lib->pointed_img_wd, lib->pointed_img_ht, iir,
                                                             img_pointerx, img_pointery);
          }

          // if thumb is missing, record it for expose in next round
          if(thumb_missed)
            g_hash_table_add(lib->thumbs_table, (gpointer)&id);
          else
            g_hash_table_remove(lib->thumbs_table, (gpointer)&id);

          missing += thumb_missed;
        }

        cairo_restore(cr);
      }
      else
        goto escape_image_loop;

      cairo_translate(cr, wd, 0.0f);
    }
    cairo_translate(cr, -max_cols * wd, ht);
  }
escape_image_loop:
  cairo_restore(cr);
  if(!lib->pan && (iir != 1 || mouse_over_id != -1)) dt_control_set_mouse_over_id(mouse_over_id);

  // and now the group borders
  cairo_save(cr);
  current_image = 0;
  if(lib->offset < 0)
  {
    drawing_offset = lib->offset;
    offset = 0;
  }

  if(iir > 1)
  {
    // clear rows & cols around thumbs, needed to clear the group borders
    cairo_save(cr);
    dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_LIGHTTABLE_BG);
    for(int row = 0; row < max_rows; row++)
    {
      cairo_move_to(cr, 0, row * ht);
      cairo_line_to(cr, width, row * ht);
    }
    for(int col = 0; col < max_cols; col++)
    {
      cairo_move_to(cr, col * wd, 0);
      cairo_line_to(cr, col * wd, height);
    }
    cairo_set_line_width(cr, 0.011 * wd);
    cairo_stroke(cr);
    cairo_restore(cr);
  }

  for(int row = 0; row < max_rows; row++)
  {
    for(int col = 0; col < max_cols; col++)
    {
      /* skip drawing images until we reach a non-negative offset.
       * This is needed for zooming, so that the image under the
       * mouse cursor can be stay there. */
      if(drawing_offset < 0)
      {
        drawing_offset++;
        cairo_translate(cr, wd, 0.0f);
        continue;
      }

      id = query_ids[current_image];

      if(id > 0)
      {
        const dt_image_t *image = dt_image_cache_get(darktable.image_cache, id, 'r');
        int group_id = -1;
        if(image) group_id = image->group_id;
        dt_image_cache_read_release(darktable.image_cache, image);

        if(iir == 1 && row) continue;

        cairo_save(cr);

        gboolean paint_border = FALSE;
        // regular highlight border
        if(group_id != -1)
        {
          if(mouse_over_group == group_id && iir > 1
             && ((!darktable.gui->grouping && dt_conf_get_bool("plugins/lighttable/draw_group_borders"))
                 || group_id == darktable.gui->expanded_group_id))
          {
            cairo_set_source_rgb(cr, 1, 0.8, 0);
            paint_border = TRUE;
          }
          // border of expanded group
          else if(darktable.gui->grouping && group_id == darktable.gui->expanded_group_id && iir > 1)
          {
            cairo_set_source_rgb(cr, 0, 0, 1);
            paint_border = TRUE;
          }
        }

        if(paint_border)
        {
          int neighbour_group = -1;
          // top border
          if(row > 0 && ((current_image - iir) >= 0))
          {
            int _id = query_ids[current_image - iir];
            if(_id > 0)
            {
              const dt_image_t *_img = dt_image_cache_get(darktable.image_cache, _id, 'r');
              neighbour_group = _img->group_id;
              dt_image_cache_read_release(darktable.image_cache, _img);
            }
          }
          if(neighbour_group != group_id)
          {
            cairo_move_to(cr, 0, 0);
            cairo_line_to(cr, wd, 0);
          }
          // left border
          neighbour_group = -1;
          if(col > 0 && current_image > 0)
          {
            int _id = query_ids[current_image - 1];
            if(_id > 0)
            {
              const dt_image_t *_img = dt_image_cache_get(darktable.image_cache, _id, 'r');
              neighbour_group = _img->group_id;
              dt_image_cache_read_release(darktable.image_cache, _img);
            }
          }
          if(neighbour_group != group_id)
          {
            cairo_move_to(cr, 0, 0);
            cairo_line_to(cr, 0, ht);
          }
          // bottom border
          neighbour_group = -1;
          if(row < max_rows - 1)
          {
            int _id = query_ids[current_image + iir];
            if(_id > 0)
            {
              const dt_image_t *_img = dt_image_cache_get(darktable.image_cache, _id, 'r');
              neighbour_group = _img->group_id;
              dt_image_cache_read_release(darktable.image_cache, _img);
            }
          }
          if(neighbour_group != group_id)
          {
            cairo_move_to(cr, 0, ht);
            cairo_line_to(cr, wd, ht);
          }
          // right border
          neighbour_group = -1;
          if(col < max_cols - 1)
          {
            int _id = query_ids[current_image + 1];
            if(_id > 0)
            {
              const dt_image_t *_img = dt_image_cache_get(darktable.image_cache, _id, 'r');
              neighbour_group = _img->group_id;
              dt_image_cache_read_release(darktable.image_cache, _img);
            }
          }
          if(neighbour_group != group_id)
          {
            cairo_move_to(cr, wd, 0);
            cairo_line_to(cr, wd, ht);
          }
          cairo_set_line_width(cr, 0.01 * wd);
          cairo_stroke(cr);
        }

        cairo_restore(cr);
        current_image++;
      }
      else
        goto escape_border_loop;

      cairo_translate(cr, wd, 0.0f);
    }
    cairo_translate(cr, -max_cols * wd, ht);
  }
escape_border_loop:
  cairo_restore(cr);
after_drawing:
  /* check if offset was changed and we need to prefetch thumbs */
  if(offset_changed)
  {
    int32_t imgids_num = 0;
    const int prefetchrows = .5 * max_rows + 1;
    int32_t *imgids = malloc(prefetchrows * iir * sizeof(int32_t));

    /* clear and reset main query */
    DT_DEBUG_SQLITE3_CLEAR_BINDINGS(lib->statements.main_query);
    DT_DEBUG_SQLITE3_RESET(lib->statements.main_query);

    /* setup offest and row for prefetch */
    DT_DEBUG_SQLITE3_BIND_INT(lib->statements.main_query, 1, offset + max_rows * iir);
    DT_DEBUG_SQLITE3_BIND_INT(lib->statements.main_query, 2, prefetchrows * iir);

    // prefetch jobs in inverse order: supersede previous jobs: most important last
    while(sqlite3_step(lib->statements.main_query) == SQLITE_ROW && imgids_num < prefetchrows * iir)
      imgids[imgids_num++] = sqlite3_column_int(lib->statements.main_query, 0);

    float imgwd = iir == 1 ? 0.97 : 0.8;
    dt_mipmap_size_t mip = dt_mipmap_cache_get_matching_size(darktable.mipmap_cache, imgwd * wd,
                                                             imgwd * (iir == 1 ? height : ht));
    while(imgids_num > 0)
    {
      imgids_num--;
      dt_mipmap_cache_get(darktable.mipmap_cache, NULL, imgids[imgids_num], mip, DT_MIPMAP_PREFETCH, 'r');
    }

    free(imgids);
  }

  free(query_ids);
  // oldpan = pan;
  if(darktable.unmuted & DT_DEBUG_CACHE) dt_mipmap_cache_print(darktable.mipmap_cache);

  if(darktable.gui->center_tooltip == 1) // set in this round
  {
    char *tooltip = dt_history_get_items_as_string(mouse_over_id);
    if(tooltip != NULL)
    {
      gtk_widget_set_tooltip_text(dt_ui_center(darktable.gui->ui), tooltip);
      g_free(tooltip);
    }
  }
  else if(darktable.gui->center_tooltip == 2) // not set in this round
  {
    darktable.gui->center_tooltip = 0;
    gtk_widget_set_tooltip_text(dt_ui_center(darktable.gui->ui), "");
  }

  lib->offset_changed = FALSE;

  return missing;
}


// TODO: this is also defined in lib/tools/lighttable.c
//       fix so this value is shared.. DT_CTL_SET maybe ?

#define DT_LIBRARY_MAX_ZOOM 13

static int expose_zoomable(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx,
                            int32_t pointery)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  float zoom, zoom_x, zoom_y;
  int32_t mouse_over_id, pan, track, center;
  int missing = 0;
  /* query new collection count */
  lib->collection_count = dt_collection_get_count(darktable.collection);

  mouse_over_id = dt_control_get_mouse_over_id();
  /* need to keep this one as it needs to be refreshed */
  const int initial_mouse_over_id = mouse_over_id;
  zoom = get_zoom();
  zoom_x = lib->zoom_x;
  zoom_y = lib->zoom_y;
  pan = lib->pan;
  center = lib->center;
  track = lib->track;

  lib->images_in_row = zoom;
  lib->image_over = DT_VIEW_DESERT;
  lib->pointed_img_over = DT_VIEW_ERR;

  if(mouse_over_id == -1 || lib->force_expose_all || pan || zoom == 1)
  {
    lib->force_expose_all = TRUE;
    dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_LIGHTTABLE_BG);
    cairo_paint(cr);
  }

  const float wd = width / zoom;
  const float ht = width / zoom;
  lib->thumb_size = wd;

  static float oldzoom = -1;
  if(oldzoom < 0) oldzoom = zoom;

  // TODO: exaggerate mouse gestures to pan when zoom == 1

  // 10000 and -1 are introduced in src/views/view.c:dt_view_manager_expose()
  // when the pointer is out of the window. No idea why these numbers, however
  // sometimes they arrive here and we must check.
  if(pan && (pointerx != 10000 || pointery != -1)) // && mouse_over_id >= 0)
  {
    zoom_x = lib->select_offset_x - /* (zoom == 1 ? 2. : 1.)*/ pointerx;
    zoom_y = lib->select_offset_y - /* (zoom == 1 ? 2. : 1.)*/ pointery;
  }

  if(!lib->statements.main_query) return 0;

  if(track == 0)
    ;
  else if(track > 1)
    zoom_y += ht;
  else if(track > 0)
    zoom_x += wd;
  else if(track > -2)
    zoom_x -= wd;
  else
    zoom_y -= ht;
  if(zoom > DT_LIBRARY_MAX_ZOOM)
  {
    // double speed.
    if(track == 0)
      ;
    else if(track > 1)
      zoom_y += ht;
    else if(track > 0)
      zoom_x += wd;
    else if(track > -2)
      zoom_x -= wd;
    else
      zoom_y -= ht;
    if(zoom > 1.5 * DT_LIBRARY_MAX_ZOOM)
    {
      // quad speed.
      if(track == 0)
        ;
      else if(track > 1)
        zoom_y += ht;
      else if(track > 0)
        zoom_x += wd;
      else if(track > -2)
        zoom_x -= wd;
      else
        zoom_y -= ht;
    }
  }

  if(oldzoom != zoom)
  {
    float oldx = (pointerx + zoom_x) * oldzoom / width;
    float oldy = (pointery + zoom_y) * oldzoom / width;
    if(zoom == 1)
    {
      zoom_x = (int)oldx * wd;
      zoom_y = (int)oldy * ht;
      lib->offset = 0x7fffffff;
    }
    else
    {
      zoom_x = oldx * wd - pointerx;
      zoom_y = oldy * ht - pointery;
    }
  }
  oldzoom = zoom;

  // TODO: replace this with center on top of selected/developed image
  if(center)
  {
    if(mouse_over_id >= 0)
    {
      zoom_x = wd * ((int)(zoom_x) / (int)wd);
      zoom_y = ht * ((int)(zoom_y) / (int)ht);
    }
    else
      zoom_x = zoom_y = 0.0;
    center = 0;
  }

  // mouse left the area, but we leave mouse over as it was, especially during panning
  // if(!pan && pointerx > 0 && pointerx < width && pointery > 0 && pointery < height)
  if(!pan && zoom != 1) dt_control_set_mouse_over_id(-1);

  // set scrollbar positions, clamp zoom positions

  if(lib->collection_count == 0)
  {
    zoom_x = zoom_y = 0.0f;
  }
  else if(zoom < 1.01)
  {
    if(zoom == 1 && zoom_x < 0 && zoom_y > 0) // full view, wrap around
    {
      zoom_x = wd * DT_LIBRARY_MAX_ZOOM - wd;
      zoom_y -= ht;
    }
    if(zoom_x < 0) zoom_x = 0;
    if(zoom == 1 && zoom_x > wd * DT_LIBRARY_MAX_ZOOM - wd) // full view, wrap around
    {
      zoom_x = 0;
      zoom_y += ht;
    }
    if(zoom_x > wd * DT_LIBRARY_MAX_ZOOM - wd) zoom_x = wd * DT_LIBRARY_MAX_ZOOM - wd;
    if(zoom_y < 0) zoom_y = 0;
    if(zoom_y > ht * lib->collection_count / MIN(DT_LIBRARY_MAX_ZOOM, zoom) - ht)
      zoom_y = ht * lib->collection_count / MIN(DT_LIBRARY_MAX_ZOOM, zoom) - ht;
  }
  else
  {
    if(zoom_x < -width + wd) zoom_x = -width + wd;
    if(zoom_x > wd * DT_LIBRARY_MAX_ZOOM - wd) zoom_x = wd * DT_LIBRARY_MAX_ZOOM - wd;
    if(zoom_y < -height + ht) zoom_y = -height + ht;
    if(zoom_y > ht * ceilf((float)lib->collection_count / DT_LIBRARY_MAX_ZOOM) - ht)
      zoom_y = ht * ceilf((float)lib->collection_count / DT_LIBRARY_MAX_ZOOM) - ht;
  }

  lib->offset_x = zoom_x;
  lib->offset_y = zoom_y;

  int offset_i = (int)(zoom_x / wd);
  int offset_j = (int)(zoom_y / ht);
  if(lib->first_visible_filemanager >= 0)
  {
    offset_i = lib->first_visible_filemanager % DT_LIBRARY_MAX_ZOOM;
    offset_j = lib->first_visible_filemanager / DT_LIBRARY_MAX_ZOOM;
  }
  lib->first_visible_filemanager = -1;
  lib->first_visible_zoomable = offset_i + DT_LIBRARY_MAX_ZOOM * offset_j;
  // arbitrary 1000 to avoid bug due to round towards zero using (int)
  int seli = zoom == 1 ? 0 : ((int)(1000 + (pointerx + zoom_x) / wd) - MAX(offset_i, 0) - 1000);
  int selj = zoom == 1 ? 0 : ((int)(1000 + (pointery + zoom_y) / ht) - offset_j - 1000);
  float offset_x = (zoom == 1) ? 0.0 : (zoom_x / wd - (int)(zoom_x / wd));
  float offset_y = (zoom == 1) ? 0.0 : (zoom_y / ht - (int)(zoom_y / ht));
  const int max_rows = (zoom == 1) ? 1 : (2 + (int)((height) / ht + .5));
  lib->max_rows = max_rows;
  const int max_cols = (zoom == 1) ? 1 : (MIN(DT_LIBRARY_MAX_ZOOM - MAX(0, offset_i), 1 + (int)(zoom + .5)));

  int offset = MAX(0, offset_i) + DT_LIBRARY_MAX_ZOOM * offset_j;
  const int img_pointerx = zoom == 1 ? pointerx : fmodf(pointerx + zoom_x, wd);
  const int img_pointery = zoom == 1 ? pointery : fmodf(pointery + zoom_y, ht);

  // assure 1:1 is not switching images on resize/tab events:
  if(!track && lib->offset != 0x7fffffff && zoom == 1)
  {
    offset = lib->offset;
    zoom_x = wd * (offset % DT_LIBRARY_MAX_ZOOM);
    zoom_y = ht * (offset / DT_LIBRARY_MAX_ZOOM);
  }
  else
    lib->offset = offset;

  int id;

  dt_view_set_scrollbar(self,
                        zoom_x, -width + wd, wd * DT_LIBRARY_MAX_ZOOM - wd + width, width,
                        zoom_y,  -height + ht,
                        ht * ceilf((float)lib->collection_count / DT_LIBRARY_MAX_ZOOM) - ht + height, height);

  cairo_translate(cr, -offset_x * wd, -offset_y * ht);
  cairo_translate(cr, -MIN(offset_i * wd, 0.0), 0.0);
  const int before_last_exposed_id = lib->last_exposed_id;

  for(int row = 0; row < max_rows; row++)
  {
    if(offset < 0)
    {
      cairo_translate(cr, 0, ht);
      offset += DT_LIBRARY_MAX_ZOOM;
      continue;
    }

    /* clear and reset main query */
    DT_DEBUG_SQLITE3_CLEAR_BINDINGS(lib->statements.main_query);
    DT_DEBUG_SQLITE3_RESET(lib->statements.main_query);

    DT_DEBUG_SQLITE3_BIND_INT(lib->statements.main_query, 1, offset);
    DT_DEBUG_SQLITE3_BIND_INT(lib->statements.main_query, 2, max_cols);
    for(int col = 0; col < max_cols; col++)
    {
      if(sqlite3_step(lib->statements.main_query) == SQLITE_ROW)
      {
        id = sqlite3_column_int(lib->statements.main_query, 0);

        // set mouse over id
        if((zoom == 1 && mouse_over_id < 0) || ((!pan || track) && seli == col && selj == row && pointerx > 0
                                                && pointerx < width && pointery > 0 && pointery < height))
        {
          mouse_over_id = id;
          dt_control_set_mouse_over_id(mouse_over_id);
        }

        cairo_save(cr);

        if (id == mouse_over_id
            || lib->force_expose_all
            || id == before_last_exposed_id
            || id == initial_mouse_over_id
            || g_hash_table_contains(lib->thumbs_table, (gpointer)&id))
        {
          if(!lib->force_expose_all && id == mouse_over_id) lib->last_exposed_id = id;
          dt_view_image_expose_t params = { 0 };
          params.image_over = &(lib->image_over);
          params.imgid = id;
          params.cr = cr;
          params.width = wd;
          params.height = zoom == 1 ? height : ht;
          params.px = img_pointerx;
          params.py = img_pointery;
          params.zoom = zoom;
          const int thumb_missed = dt_view_image_expose(&params);

          if(id == mouse_over_id)
          {
            lib->pointed_img_x = -offset_x * wd - MIN(offset_i * wd, 0.0) + col * wd;
            lib->pointed_img_y = -offset_y * ht + row * ht;
            lib->pointed_img_wd = wd;
            lib->pointed_img_ht = zoom == 1 ? height : ht;
            lib->pointed_img_over = dt_view_guess_image_over(lib->pointed_img_wd, lib->pointed_img_ht, zoom,
                                                             img_pointerx, img_pointery);
          }

          // if thumb is missing, record it for expose in next round
          if(thumb_missed)
            g_hash_table_add(lib->thumbs_table, (gpointer)&id);
          else
            g_hash_table_remove(lib->thumbs_table, (gpointer)&id);

          missing += thumb_missed;
        }

        cairo_restore(cr);
        if(zoom == 1)
        {
          // we are on the single-image display at a time, in this case we want the selection to be updated to
          // contain this single image.
          dt_selection_select_single(darktable.selection, id);
          lib->single_img_id = id;
        }
        else
          lib->single_img_id = -1;
      }
      else
        goto failure;
      cairo_translate(cr, wd, 0.0f);
    }
    cairo_translate(cr, -max_cols * wd, ht);
    offset += DT_LIBRARY_MAX_ZOOM;
  }
failure:

  lib->zoom_x = zoom_x;
  lib->zoom_y = zoom_y;
  lib->track = 0;
  lib->center = center;
  if(darktable.unmuted & DT_DEBUG_CACHE) dt_mipmap_cache_print(darktable.mipmap_cache);
  return missing;
}

static int expose_expose(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx,
                         int32_t pointery, const dt_lighttable_layout_t layout)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  int32_t mouse_over_id;
  int missing = 0;

  lib->image_over = DT_VIEW_DESERT;
  lib->pointed_img_over = DT_VIEW_ERR;

  dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_LIGHTTABLE_BG);
  cairo_paint(cr);

  dt_view_set_scrollbar(self, 0, 0, 1, 1, 0, 0, 1, 1);

  int sel_img_count = 0;
  GList *selected = NULL;

  if(layout == DT_LIGHTTABLE_LAYOUT_EXPOSE)
  {
    selected = dt_collection_get_selected(darktable.collection, -1);
    sel_img_count = g_list_length(selected);
  }
  else if(layout == DT_LIGHTTABLE_LAYOUT_CULLING)
  {
    selected = dt_collection_get_all(darktable.collection, -1);
    sel_img_count = g_list_length(selected);
  }

  if(sel_img_count == 0) return 0;

  mouse_over_id = dt_control_get_mouse_over_id();

  dt_layout_image_t *images = malloc(sel_img_count * sizeof(dt_layout_image_t));

  gchar *imgids = NULL;

  // build the image ids for the SQL 'in' where clause.
  if(layout == DT_LIGHTTABLE_LAYOUT_EXPOSE)
  {
    GList *l = selected;
    while(l)
    {
      const int imgid = GPOINTER_TO_INT(l->data);
      if(imgids)
        imgids = dt_util_dstrcat(imgids, ", %d", imgid);
      else
        imgids = dt_util_dstrcat(imgids, "%d", imgid);
      l = g_list_next(l);
    }
  }
  else if(layout == DT_LIGHTTABLE_LAYOUT_CULLING)
  {
    // number of images to be displayed
    const int display_num_images = get_display_num_images();
    // starting with the first selected image
    GList *first_selected = dt_collection_get_selected(darktable.collection, 1);
    int display_first_image = (first_selected) ? GPOINTER_TO_INT(first_selected->data) : -1;
    if(display_first_image < 0 && lib->last_first_selected >= 0)
    {
      GList *l = g_list_nth(selected, lib->last_first_selected);
      if(l) display_first_image = GPOINTER_TO_INT(l->data);
    }

    // skip images until we reach the first selected
    // if no selection start with the first image
    int i = 0;
    GList *l = selected;
    if(display_first_image >= 0)
    {
      while(l && i + display_num_images < sel_img_count)
      {
        const int imgid = GPOINTER_TO_INT(l->data);
        if(imgid == display_first_image) break;

        l = g_list_next(l);
        i++;
      }
    }

    // now make the sql sentece with the number of images to display
    i = 0;
    while(l && i < display_num_images)
    {
      const int imgid = GPOINTER_TO_INT(l->data);
      if(imgids)
        imgids = dt_util_dstrcat(imgids, ", %d", imgid);
      else
        imgids = dt_util_dstrcat(imgids, "%d", imgid);
      l = g_list_next(l);
      i++;
    }
    sel_img_count = i;

    if(first_selected) g_list_free(first_selected);
  }

  g_list_free(selected);

  gchar *query =  g_strdup_printf("SELECT id, aspect_ratio, width, height FROM images WHERE id IN (%s) ORDER BY INSTR('%s', id)",
                                  imgids, imgids);

  g_free(imgids);

  /* prepare a new main query statement for collection */
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  if(stmt == NULL)
  {
    free(images);
    g_free(query);
    return 0;
  }

  int i = 0;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int32_t id = sqlite3_column_int(stmt, 0);
    double aspect_ratio = sqlite3_column_double(stmt, 1);
    if(!aspect_ratio)
    {
      aspect_ratio = (double)sqlite3_column_int(stmt, 2) / (double)sqlite3_column_int(stmt, 3);
      // record aspect ratio now
      dt_image_set_aspect_ratio_to(id, aspect_ratio);
    }

    images[i].imgid = id;
    images[i].width = (gint) (sqrt(aspect_ratio) * 100);
    images[i].height = (gint) (1/sqrt(aspect_ratio) * 100);
    i++;
  }

  int sum_w = 0, max_h = 0, max_w = 0;

  GList *slots = NULL;

  unsigned int total_width = 0, total_height = 0;
  int distance = 1;
  float avg_ratio = 0;

  // Get total window width and max window width/height
  for(i = 0; i < sel_img_count; i++)
  {
    sum_w += images[i].width;
    max_w = MAX(max_w, images[i].width);
    max_h = MAX(max_h, images[i].height);
    avg_ratio += images[i].width / (float) images[i].height;
  }

  avg_ratio /= sel_img_count;

  int per_row, tmp_per_row, per_col, tmp_per_col;
  per_row = tmp_per_row = ceil(sqrt(sel_img_count));
  per_col = tmp_per_col = (sel_img_count + per_row - 1) / per_row; // ceil(sel_img_count/per_row)

  float tmp_slot_ratio, slot_ratio;
  tmp_slot_ratio = slot_ratio = (width/ (float) per_row) / (height/ (float) per_col);

  do
  {
    per_row = tmp_per_row;
    per_col = tmp_per_col;
    slot_ratio = tmp_slot_ratio;

    if(avg_ratio > slot_ratio)
    {
      tmp_per_row = per_row - 1;
    }
    else
    {
      tmp_per_row = per_row + 1;
    }

    if(tmp_per_row == 0) break;

    tmp_per_col = (sel_img_count + tmp_per_row - 1) / tmp_per_row; //ceil(sel_img_count / tmp_per_row);

    tmp_slot_ratio = (width/ (float) tmp_per_row) / (height/( float) tmp_per_col);

  } while(per_row > 0 && per_row <= sel_img_count && absmul(tmp_slot_ratio, avg_ratio) < absmul(slot_ratio, avg_ratio));


  // Vertical layout
  for(i = 0; i < sel_img_count; i++)
  {
    GList *slot_iter = g_list_first(slots);
    for (; slot_iter; slot_iter = slot_iter->next)
    {
      GList *slot = (GList *) slot_iter->data;
      // Calculate current total height of slot
      int slot_h = distance;
      GList *slot_cw_iter = slot;
      while(slot_cw_iter != NULL)
      {
        dt_layout_image_t *slot_cw = (dt_layout_image_t *) slot_cw_iter->data;
        slot_h = slot_h + slot_cw->height + distance;
        slot_cw_iter = slot_cw_iter->next;
      }
      // Add window to slot if the slot height after adding the window
      // doesn't exceed max window height
      if (slot_h + distance + images[i].height < max_h) {
        slot_iter->data = g_list_append(slot, &(images[i]));
        break;
      }
    }
    // Otherwise, create a new slot with only this window
    if (!slot_iter)
      slots = g_list_append(slots, g_list_append(NULL, &(images[i])));
  }

  GList *rows = g_list_append(NULL, NULL);
  {
    int row_y = 0, x = 0, row_h = 0;
    int max_row_w = sum_w/per_col;//sqrt((float) sum_w * max_h);// * pow((float) width/height, 0.02);
    for (GList *slot_iter = slots; slot_iter != NULL; slot_iter = slot_iter->next)
    {
      GList *slot = (GList *) slot_iter->data;

      // Max width of windows in the slot
      int slot_max_w = 0;
      for (GList *slot_cw_iter = slot; slot_cw_iter != NULL; slot_cw_iter = slot_cw_iter->next)
      {
        dt_layout_image_t *cw = (dt_layout_image_t *) slot_cw_iter->data;
        slot_max_w = MAX(slot_max_w, cw->width);
      }

      int y = row_y;
      for (GList *slot_cw_iter = slot; slot_cw_iter != NULL; slot_cw_iter = slot_cw_iter->next)
      {
        dt_layout_image_t *cw = (dt_layout_image_t *) slot_cw_iter->data;
        cw->x = x + (slot_max_w - cw->width) / 2;
        cw->y = y;
        y += cw->height + distance;
        rows->data = g_list_append(rows->data, cw);
      }

      row_h = MAX(row_h, y - row_y);
      total_height = MAX(total_height, y);
      x += slot_max_w + distance;
      total_width = MAX(total_width, x);

      if (x > max_row_w)
      {
        x = 0;
        row_y += row_h;
        row_h = 0;
        rows = g_list_append(rows, 0);
        rows = rows->next;
      }
      g_list_free(slot);
    }
    g_list_free(slots);
    slots = NULL;
  }

  total_width -= distance;
  total_height -= distance;

  for (GList *iter = rows; iter != NULL; iter = iter->next)
  {
    GList *row = (GList *) iter->data;
    int row_w = 0, xoff;

    for (GList *slot_cw_iter = row; slot_cw_iter != NULL; slot_cw_iter = slot_cw_iter->next)
    {
      dt_layout_image_t *cw = (dt_layout_image_t *) slot_cw_iter->data;
      row_w = MAX(row_w, cw->x + cw->width);
    }

    xoff = (total_width - row_w) / 2;

    for (GList *cw_iter = row; cw_iter != NULL; cw_iter = cw_iter->next)
    {
      dt_layout_image_t *cw = (dt_layout_image_t *) cw_iter->data;
      cw->x += xoff;
    }
    g_list_free(row);
  }

  g_list_free(rows);

  float factor;
  factor = (float) (width - 1) / total_width;
  if (factor * total_height > height - 1)
    factor = (float) (height - 1) / total_height;

  int xoff = (width - (float) total_width * factor) / 2;
  int yoff = (height - (float) total_height * factor) / 2;

  for(i = 0; i < sel_img_count; i++)
  {
    images[i].width = images[i].width * factor;
    images[i].height = images[i].height * factor;
    images[i].x = images[i].x * factor + xoff;
    images[i].y = images[i].y * factor + yoff;
  }

  const int max_in_memory_images = _get_max_in_memory_images();

  // sort lib->fp_surf to re-use cached thumbs & surface
  if(layout == DT_LIGHTTABLE_LAYOUT_CULLING)
    _sort_preview_surface(lib, images, sel_img_count, max_in_memory_images);

  for(i = 0; i < sel_img_count; i++)
  {
    cairo_save(cr);
    // if(zoom == 1) dt_image_prefetch(image, DT_IMAGE_MIPF);
    cairo_translate(cr, images[i].x, images[i].y);
    int img_pointerx =
      pointerx > images[i].x
      && pointerx < images[i].x + images[i].width
      ? pointerx - images[i].x
      : images[i].width;
    int img_pointery =
      pointery > images[i].y
      && pointery < images[i].y + images[i].height
      ? pointery - images[i].y
      : images[i].height;

    dt_view_image_expose_t params = { 0 };
    params.image_over = &(lib->image_over);
    params.imgid = images[i].imgid;
    params.cr = cr;
    params.width = images[i].width;
    params.height = images[i].height;
    params.px = img_pointerx;
    params.py = img_pointery;
    params.zoom = 1;
    params.full_preview = TRUE;
    if(sel_img_count <= max_in_memory_images)
    {
      params.full_zoom = lib->full_zoom;
      params.full_x = lib->full_x;
      params.full_y = lib->full_y;
      params.full_surface = &lib->fp_surf[i].surface;
      params.full_rgbbuf = &lib->fp_surf[i].rgbbuf;
      params.full_surface_mip = &lib->fp_surf[i].mip;
      params.full_surface_id = &lib->fp_surf[i].imgid;
      params.full_surface_wd = &lib->fp_surf[i].width;
      params.full_surface_ht = &lib->fp_surf[i].height;
      params.full_surface_w_lock = &lib->fp_surf[i].w_lock;
      params.full_zoom100 = &lib->fp_surf[i].zoom_100;
      params.full_w1 = &lib->fp_surf[i].w_fit;
      params.full_h1 = &lib->fp_surf[i].h_fit;
      params.full_maxdx = &lib->fp_surf[i].max_dx;
      params.full_maxdy = &lib->fp_surf[i].max_dy;
    }

    missing += dt_view_image_expose(&params);
    cairo_restore(cr);

    // set mouse over id
    if(pointerx > images[i].x && pointerx < images[i].x + images[i].width && pointery > images[i].y && pointery < images[i].y + images[i].height)
    {
      mouse_over_id = images[i].imgid;
      dt_control_set_mouse_over_id(mouse_over_id);
    }
  }

  free(images);

  sqlite3_finalize(stmt);

  g_free(query);

  if(darktable.unmuted & DT_DEBUG_CACHE) dt_mipmap_cache_print(darktable.mipmap_cache);
  return missing;
}

/**
 * Displays a full screen preview of the image currently under the mouse pointer.
 */
static int expose_full_preview(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx,
                               int32_t pointery)
{
  dt_library_t *lib = (dt_library_t *)self->data;

  lib->pointed_img_over = DT_VIEW_ERR;

  int offset = 0;
  if(lib->track > 2) offset = 1;
  if(lib->track < -2) offset = -1;
  lib->track = 0;

  int n_width = width * lib->full_zoom;
  int n_height = height * lib->full_zoom;
  // only look for images to preload or update the one shown when we moved to another image
  if(offset != 0)
  {
    /* If more than one image is selected, iterate over these. */
    /* If only one image is selected, scroll through all known images. */
    sqlite3_stmt *stmt;
    int sel_group_count = 0;
    int current_group = -1;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.selected_images", -1,
        &stmt, NULL);
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
      uint32_t imgid  = sqlite3_column_int(stmt, 0);
      const dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'r');
      if (image->group_id != current_group)
      {
        sel_group_count++;
        current_group = image->group_id;
      }
      dt_image_cache_read_release(darktable.image_cache, image);
    }
    sqlite3_finalize(stmt);
    dt_print(DT_DEBUG_LIGHTTABLE, "[lighttable] selected group: %d\n", sel_group_count);

    /* How many images to preload in advance. */
    int preload_num = dt_conf_get_int("plugins/lighttable/preview/full_size_preload_count");
    gboolean preload = preload_num > 0;
    preload_num = CLAMPS(preload_num, 1, 99999);

    gchar *stmt_string = g_strdup_printf("SELECT col.imgid AS id, col.rowid FROM memory.collected_images AS col %s "
                                         "WHERE col.rowid %s %d ORDER BY col.rowid %s LIMIT %d",
                                         (sel_group_count <= 1) ?
                                           /* We want to operate on the currently collected images,
                                            * so there's no need to match against the selection */
                                           "" :
                                           /* Limit the matches to the current selection */
                                           "INNER JOIN main.selected_images AS sel ON col.imgid = sel.imgid",
                                         (offset >= 0) ? ">" : "<",
                                         lib->full_preview_rowid,
                                         /* Direction of our navigation -- when showing for the first time,
                                          * i.e. when offset == 0, assume forward navigation */
                                         (offset >= 0) ? "ASC" : "DESC",
                                         preload_num);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), stmt_string, -1, &stmt, NULL);

    /* Walk through the "next" images, activate preload and find out where to go if moving */
    int *preload_stack = malloc(preload_num * sizeof(int));
    for(int i = 0; i < preload_num; ++i)
    {
      preload_stack[i] = -1;
    }
    int count = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
      /* Check if we're about to move */
      if(count == 0 && offset != 0)
      {
        /* We're moving, so let's update the "next image" bits */
        lib->full_preview_id = sqlite3_column_int(stmt, 0);
        lib->full_preview_rowid = sqlite3_column_int(stmt, 1);
        dt_control_set_mouse_over_id(lib->full_preview_id);
      }
      /* Store the image details for preloading, see below. */
      preload_stack[count] = sqlite3_column_int(stmt, 0);
      ++count;
    }
    g_free(stmt_string);
    sqlite3_finalize(stmt);

    if(preload)
    {
      dt_mipmap_size_t mip = dt_mipmap_cache_get_matching_size(darktable.mipmap_cache, n_width, n_height);
      /* Preload these images.
      * The job queue is not a queue, but a stack, so we have to do it backwards.
      * Simply swapping DESC and ASC in the SQL won't help because we rely on the LIMIT clause, and
      * that LIMIT has to work with the "correct" sort order. One could use a subquery, but I don't
      * think that would be terribly elegant, either. */
      while(--count >= 0 && preload_stack[count] != -1)
      {
        dt_mipmap_cache_get(darktable.mipmap_cache, NULL, preload_stack[count], mip, DT_MIPMAP_PREFETCH, 'r');
      }
    }

    free(preload_stack);
  }

  lib->image_over = DT_VIEW_DESERT;
  dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_LIGHTTABLE_PREVIEW_BG);
  cairo_paint(cr);

  const int frows = 5, fcols = 5;
  if(lib->display_focus)
  {
    if(lib->full_res_thumb_id != lib->full_preview_id)
    {
      gboolean from_cache = TRUE;
      char filename[PATH_MAX] = { 0 };
      dt_image_full_path(lib->full_preview_id, filename, sizeof(filename), &from_cache);
      free(lib->full_res_thumb);
      lib->full_res_thumb = NULL;
      dt_colorspaces_color_profile_type_t color_space;
      if(!dt_imageio_large_thumbnail(filename, &lib->full_res_thumb,
                                               &lib->full_res_thumb_wd,
                                               &lib->full_res_thumb_ht,
                                               &color_space))
      {
        lib->full_res_thumb_orientation = ORIENTATION_NONE;
        lib->full_res_thumb_id = lib->full_preview_id;
      }

      if(lib->full_res_thumb_id == lib->full_preview_id)
      {
        dt_focus_create_clusters(lib->full_res_focus, frows, fcols, lib->full_res_thumb,
                                 lib->full_res_thumb_wd, lib->full_res_thumb_ht);
      }
    }
  }

  dt_view_image_expose_t params = { 0 };
  params.image_over = &(lib->image_over);
  params.imgid = lib->full_preview_id;
  params.cr = cr;
  params.width = width;
  params.height = height;
  params.px = pointerx;
  params.py = pointery;
  params.zoom = 1;
  params.full_preview = TRUE;
  params.full_zoom = lib->full_zoom;
  params.full_zoom100 = &lib->fp_surf[0].zoom_100;
  params.full_maxdx = &lib->fp_surf[0].max_dx;
  params.full_maxdy = &lib->fp_surf[0].max_dy;
  params.full_w1 = &lib->fp_surf[0].w_fit;
  params.full_h1 = &lib->fp_surf[0].h_fit;
  params.full_x = lib->full_x;
  params.full_y = lib->full_y;
  params.full_surface = &lib->fp_surf[0].surface;
  params.full_rgbbuf = &lib->fp_surf[0].rgbbuf;
  params.full_surface_mip = &lib->fp_surf[0].mip;
  params.full_surface_id = &lib->fp_surf[0].imgid;
  params.full_surface_wd = &lib->fp_surf[0].width;
  params.full_surface_ht = &lib->fp_surf[0].height;
  params.full_surface_w_lock = &lib->fp_surf[0].w_lock;

  const int missing = dt_view_image_expose(&params);

  if(lib->display_focus && (lib->full_res_thumb_id == lib->full_preview_id))
    dt_focus_draw_clusters(cr, width, height, lib->full_preview_id, lib->full_res_thumb_wd, lib->full_res_thumb_ht,
                           lib->full_res_focus, frows, fcols, lib->full_zoom, lib->full_x, lib->full_y);
  return missing;
}

static gboolean _expose_again(gpointer user_data)
{
  // unfortunately there might have been images without thumbnails during expose.
  // this can have multiple reasons: not loaded yet (we'll receive a signal when done)
  // or still locked for writing.. we won't be notified when this changes.
  // so we just track whether there were missing images and expose again.
  dt_control_queue_redraw_center();
  return FALSE; // don't call again
}

static gboolean _expose_again_full(gpointer user_data)
{
  dt_view_t *self = (dt_view_t *)user_data;
  dt_library_t *lib = (dt_library_t *)self->data;
  // unfortunately there might have been images without thumbnails during expose.
  // this can have multiple reasons: not loaded yet (we'll receive a signal when done)
  // or still locked for writing.. we won't be notified when this changes.
  // so we just track whether there were missing images and expose again.
  lib->force_expose_all = TRUE;
  dt_control_queue_redraw_center();
  return FALSE; // don't call again
}

void begin_pan(dt_library_t *lib, double x, double y)
{
  lib->select_offset_x = lib->zoom_x + x;
  lib->select_offset_y = lib->zoom_y + y;
  lib->pan_x = x;
  lib->pan_y = y;
  lib->pan = 1;
}

void expose(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  const double start = dt_get_wtime();
  const dt_lighttable_layout_t layout = get_layout();

  // Let's show full preview if in that state...
  dt_library_t *lib = (dt_library_t *)self->data;

  lib->missing_thumbnails = 0;

  check_layout(self);

  if(lib->full_preview_id != -1)
  {
    lib->missing_thumbnails = expose_full_preview(self, cr, width, height, pointerx, pointery);
  }
  else // we do pass on expose to manager or zoomable
  {
    switch(layout)
    {
      case DT_LIGHTTABLE_LAYOUT_FILEMANAGER:
        lib->missing_thumbnails = expose_filemanager(self, cr, width, height, pointerx, pointery);
        break;
      case DT_LIGHTTABLE_LAYOUT_ZOOMABLE: // zoomable
        lib->missing_thumbnails = expose_zoomable(self, cr, width, height, pointerx, pointery);
        break;
      case DT_LIGHTTABLE_LAYOUT_EXPOSE: // compare
      case DT_LIGHTTABLE_LAYOUT_CULLING:
        lib->missing_thumbnails = expose_expose(self, cr, width, height, pointerx, pointery, layout);
        break;
      case DT_LIGHTTABLE_LAYOUT_FIRST:
      case DT_LIGHTTABLE_LAYOUT_LAST:
        break;
    }
  }

  if(layout != DT_LIGHTTABLE_LAYOUT_ZOOMABLE)
  {
    // file manager
    lib->activate_on_release = DT_VIEW_ERR;
  }
  else
  {
    // zoomable lt
    // If the mouse button was clicked on a control element and we are now
    // leaving that element, or the mouse was clicked on an image and it has
    // moved a little, then we decide to interpret the action as the start of
    // a pan. In the first case we begin the pan, in the second the pan was
    // already started however we did not signal it with the GDK_HAND1 pointer,
    // so we still have to set the pointer (see comments in button_pressed()).
    const float distance = fabs(pointerx - lib->pan_x) + fabs(pointery - lib->pan_y);
    if(lib->activate_on_release != lib->image_over
       || (lib->activate_on_release == DT_VIEW_DESERT && distance > DT_PIXEL_APPLY_DPI(5)))
    {
      if(lib->activate_on_release != DT_VIEW_ERR && !lib->pan)
      {
        begin_pan(lib, pointerx, pointery);
        dt_control_change_cursor(GDK_HAND1);
      }
      if(lib->activate_on_release == DT_VIEW_DESERT) dt_control_change_cursor(GDK_HAND1);
      lib->activate_on_release = DT_VIEW_ERR;
    }
  }
  const double end = dt_get_wtime();
  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_LIGHTTABLE, "[lighttable] expose took %0.04f sec\n", end - start);

  if(lib->missing_thumbnails)
    g_timeout_add(250, _expose_again, self);
  else
  {
    // clear hash map of thumb to redisplay, we are done
    g_hash_table_remove_all(lib->thumbs_table);
    lib->force_expose_all = FALSE;
  }
}

static gboolean go_up_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                         GdkModifierType modifier, gpointer data)
{
  dt_view_t *self = (dt_view_t *)data;
  dt_library_t *lib = (dt_library_t *)self->data;
  const dt_lighttable_layout_t layout = get_layout();

  if(layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER)
    move_view(lib, DIRECTION_TOP);
  else if(layout == DT_LIGHTTABLE_LAYOUT_CULLING)
  {
    // go to the first image on the collection
    GList *collected = dt_collection_get_all(darktable.collection, 1);
    const int imgid = (collected) ? GPOINTER_TO_INT(collected->data) : -1;
    if(imgid >= 0) filmstrip_set_active_image(lib, imgid);
    if(collected) g_list_free(collected);
  }
  else
    lib->offset = 0;
  dt_control_queue_redraw_center();
  return TRUE;
}

static gboolean go_down_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                           GdkModifierType modifier, gpointer data)
{
  dt_view_t *self = (dt_view_t *)data;
  dt_library_t *lib = (dt_library_t *)self->data;
  const dt_lighttable_layout_t layout = get_layout();

  if(layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER)
    move_view(lib, DIRECTION_BOTTOM);
  else if(layout == DT_LIGHTTABLE_LAYOUT_CULLING)
  {
    // go to the last image on the collection
    GList *collected = dt_collection_get_all(darktable.collection, -1);
    GList *l = g_list_last(collected);
    const int imgid = (l) ? GPOINTER_TO_INT(l->data) : -1;
    if(imgid >= 0) filmstrip_set_active_image(lib, imgid);
    if(collected) g_list_free(collected);
  }
  else
    lib->offset = 0x1fffffff;
  dt_control_queue_redraw_center();
  return TRUE;
}

static gboolean go_pgup_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                           GdkModifierType modifier, gpointer data)
{
  dt_view_t *self = (dt_view_t *)data;
  dt_library_t *lib = (dt_library_t *)self->data;
  const dt_lighttable_layout_t layout = get_layout();

  if(layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER)
    move_view(lib, DIRECTION_PGUP);
  else
  {
    const int iir = get_zoom();
    const int scroll_by_rows = 4; /* This should be the number of visible rows. */
    const int offset_delta = scroll_by_rows * iir;
    lib->offset = MAX(lib->offset - offset_delta, 0);
  }
  dt_control_queue_redraw_center();
  return TRUE;
}

static gboolean go_pgdown_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                             GdkModifierType modifier, gpointer data)
{
  dt_view_t *self = (dt_view_t *)data;
  dt_library_t *lib = (dt_library_t *)self->data;
  const dt_lighttable_layout_t layout = get_layout();

  if(layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER)
  {
    move_view(lib, DIRECTION_PGDOWN);
  }
  else
  {
    const int iir = get_zoom();
    const int scroll_by_rows = 4; /* This should be the number of visible rows. */
    const int offset_delta = scroll_by_rows * iir;
    lib->offset = MIN(lib->offset + offset_delta, lib->collection_count);
  }
  dt_control_queue_redraw_center();
  return TRUE;
}

static gboolean realign_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                           GdkModifierType modifier, gpointer data)
{
  dt_view_t *self = (dt_view_t *)data;
  dt_library_t *lib = (dt_library_t *)self->data;
  const dt_lighttable_layout_t layout = get_layout();

  if(layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER) move_view(lib, DIRECTION_CENTER);
  dt_control_queue_redraw_center();
  return TRUE;
}

static gboolean select_toggle_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                           GdkModifierType modifier, gpointer data)
{
  dt_view_t *self = (dt_view_t *)data;
  dt_library_t *lib = (dt_library_t *)self->data;
  const uint32_t id = dt_control_get_mouse_over_id();
  lib->key_select_direction = DIRECTION_NONE;
  dt_selection_toggle(darktable.selection, id);
  return TRUE;
}

static gboolean select_single_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                           GdkModifierType modifier, gpointer data)
{
  dt_view_t *self = (dt_view_t *)data;
  dt_library_t *lib = (dt_library_t *)self->data;
  const uint32_t id = dt_control_get_mouse_over_id();
  lib->key_select_direction = DIRECTION_NONE;
  dt_selection_select_single(darktable.selection, id);
  return TRUE;
}

static gboolean rating_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                          GdkModifierType modifier, gpointer data)
{
  dt_view_t *self = darktable.view_manager->proxy.lighttable.view;
  int num = GPOINTER_TO_INT(data);
  int32_t mouse_over_id;
  int next_image_rowid = -1;

  dt_library_t *lib = (dt_library_t *)self->data;
  const dt_lighttable_layout_t layout = get_layout();

  // needed as we can have a reordering of the pictures
  if(_is_rating_order_actif(self))
    lib->force_expose_all = TRUE;
  else
    _redraw_selected_images(self);

  if(lib->using_arrows)
  {
    // if using arrows may be the image I'm rating is going to disappear from the collection.
    // So, store where may be we need to jump
    int imgid_for_offset;
    sqlite3_stmt *stmt;

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT MIN(imgid) FROM main.selected_images", -1, &stmt, NULL);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      sqlite3_stmt *inner_stmt;
      imgid_for_offset = sqlite3_column_int(stmt, 0);
      if(!imgid_for_offset)
      {
        // empty selection
        imgid_for_offset = dt_control_get_mouse_over_id();
      }

      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
          //"SELECT imgid FROM memory.collected_images", -1, &inner_stmt,
          "SELECT rowid FROM memory.collected_images WHERE imgid=?1", -1, &inner_stmt,
          NULL);
      DT_DEBUG_SQLITE3_BIND_INT(inner_stmt, 1, imgid_for_offset);
      if(sqlite3_step(inner_stmt) == SQLITE_ROW)
        next_image_rowid = sqlite3_column_int(inner_stmt, 0);
      sqlite3_finalize(inner_stmt);
    }
    sqlite3_finalize(stmt);
  }

  mouse_over_id = dt_view_get_image_to_act_on();
  if(mouse_over_id <= 0)
    dt_ratings_apply_to_selection(num);
  else
    dt_ratings_apply_to_image_or_group(mouse_over_id, num);
  _update_collected_images(self);

  dt_collection_update_query(darktable.collection); // update the counter

  if(layout != DT_LIGHTTABLE_LAYOUT_EXPOSE && layout != DT_LIGHTTABLE_LAYOUT_CULLING
     && lib->collection_count != dt_collection_get_count(darktable.collection))
  {
    // some images disappeared from collection. Selection is now invisible.
    // lib->collection_count  --> before the rating
    // dt_collection_get_count(darktable.collection)  --> after the rating
    dt_selection_clear(darktable.selection);
    if(lib->using_arrows)
    {
      // Jump where stored before
      sqlite3_stmt *stmt;
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "SELECT imgid FROM memory.collected_images WHERE rowid=?1 OR rowid=?1 - 1 "
                                  "ORDER BY rowid DESC LIMIT 1", -1, &stmt,
                                  NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, next_image_rowid);
      if(sqlite3_step(stmt) == SQLITE_ROW)
        mouse_over_id = sqlite3_column_int(stmt, 0);
      sqlite3_finalize(stmt);
      dt_control_set_mouse_over_id(mouse_over_id);
    }
  }
  return TRUE;
}

static gboolean colorlabels_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                               GdkModifierType modifier, gpointer data)
{
  dt_view_t *self = darktable.view_manager->proxy.lighttable.view;
  dt_library_t *lib = (dt_library_t *)self->data;

  // needed as we can have a reordering of the pictures
  if(_is_colorlabels_order_actif(self))
    lib->force_expose_all = TRUE;
  else
    _redraw_selected_images(self);

  dt_colorlabels_key_accel_callback(NULL, NULL, 0, 0, data);

  return TRUE;
}

static void _lighttable_mipmaps_updated_signal_callback(gpointer instance, gpointer user_data)
{
  dt_control_queue_redraw_center();
}

static void drag_and_drop_received(GtkWidget *widget, GdkDragContext *context, gint x, gint y,
                                   GtkSelectionData *selection_data, guint target_type, guint time,
                                   gpointer data)
{
  gboolean success = FALSE;

  if((target_type == DND_TARGET_URI) && (selection_data != NULL) && (gtk_selection_data_get_length(selection_data) >= 0))
  {
    gchar **uri_list = g_strsplit_set((gchar *)gtk_selection_data_get_data(selection_data), "\r\n", 0);
    if(uri_list)
    {
      gchar **image_to_load = uri_list;
      while(*image_to_load)
      {
        if(**image_to_load)
        {
          dt_load_from_string(*image_to_load, FALSE, NULL); // TODO: do we want to open the image in darkroom mode? If
                                                            // yes -> set to TRUE.
        }
        image_to_load++;
      }
    }
    g_strfreev(uri_list);
    success = TRUE;
  }
  gtk_drag_finish(context, success, FALSE, time);
}

// shitf the first select image by 1 with up direction
static void shift_first_selected_image(dt_library_t *lib, const int up)
{
  // we're going to shift the selection by 1 if there's still room
  GList *collected = dt_collection_get_all(darktable.collection, -1);
  GList *first_selected = dt_collection_get_selected(darktable.collection, 1);
  int display_first_image = (first_selected) ? GPOINTER_TO_INT(first_selected->data) : -1;
  if(display_first_image < 0 && lib->last_first_selected >= 0)
  {
    GList *l = g_list_nth(collected, lib->last_first_selected);
    if(l) display_first_image = GPOINTER_TO_INT(l->data);
  }

  // get the first selected image in the collected list
  GList *l = collected;
  if(display_first_image >= 0)
  {
    while(l)
    {
      const int imgid = GPOINTER_TO_INT(l->data);
      if(imgid == display_first_image) break;

      l = g_list_next(l);
    }
  }

  // move the collected list according the the mouse wheel
  GList *l_tmp = NULL;
  if(up)
    l_tmp = g_list_previous(l);
  else
    l_tmp = g_list_next(l);
  if(l_tmp) l = l_tmp;

  // make the image active
  if(l)
  {
    const int imgid = GPOINTER_TO_INT(l->data);
    filmstrip_set_active_image(lib, imgid);
  }

  if(collected) g_list_free(collected);
  if(first_selected) g_list_free(first_selected);
}

void enter(dt_view_t *self)
{
  // clean the undo list
  dt_undo_clear(darktable.undo, DT_UNDO_LIGHTTABLE);

  // show/hide filmstrip when entering the view
  dt_lib_module_t *m = darktable.view_manager->proxy.filmstrip.module;
  dt_lib_module_t *timeline = darktable.view_manager->proxy.timeline.module;
  gboolean vs = dt_lib_is_visible(timeline);

  if(get_layout() == DT_LIGHTTABLE_LAYOUT_EXPOSE || get_layout() == DT_LIGHTTABLE_LAYOUT_CULLING)
  {
    gtk_widget_hide(GTK_WIDGET(timeline->widget));
    gtk_widget_show(GTK_WIDGET(m->widget));
  }
  else
  {
    gtk_widget_hide(GTK_WIDGET(m->widget));
    if(vs) gtk_widget_show(GTK_WIDGET(timeline->widget));
  }

  gtk_drag_dest_set(dt_ui_center(darktable.gui->ui), GTK_DEST_DEFAULT_ALL, target_list_all, n_targets_all,
                    GDK_ACTION_COPY);

  // dropping images for import
  g_signal_connect(dt_ui_center(darktable.gui->ui), "drag-data-received", G_CALLBACK(drag_and_drop_received),
                   self);

  _register_custom_image_order_drag_n_drop(self);

  /* connect to signals */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_MIPMAP_UPDATED,
                            G_CALLBACK(_lighttable_mipmaps_updated_signal_callback), (gpointer)self);

  gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));

  // clear some state variables
  dt_library_t *lib = (dt_library_t *)self->data;
  lib->button = 0;
  lib->pan = 0;
  lib->force_expose_all = TRUE;
  lib->activate_on_release = DT_VIEW_ERR;
  dt_collection_hint_message(darktable.collection);

  // hide panel if we are in full preview mode
  if(lib->full_preview_id != -1)
  {
    dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_LEFT, FALSE, FALSE);
    dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_RIGHT, FALSE, FALSE);
    dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_CENTER_BOTTOM, FALSE, FALSE);
    dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_CENTER_TOP, FALSE, FALSE);
    dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_TOP, FALSE, FALSE);
  }

  char *scrollbars_conf = dt_conf_get_string("scrollbars");

  gboolean scrollbars_visible = FALSE;
  if(scrollbars_conf)
  {
    if(strcmp(scrollbars_conf, "no scrollbars"))
      scrollbars_visible = TRUE;
    g_free(scrollbars_conf);
  }

  dt_ui_scrollbars_show(darktable.gui->ui, scrollbars_visible);
}

static void _ensure_image_visibility(dt_library_t *lib, uint32_t rowid)
{
  if(get_layout() != DT_LIGHTTABLE_LAYOUT_FILEMANAGER) return;

  // if we are before the first visible image, we move back
  int offset = lib->offset;
  while(offset > rowid)
  {
    offset -= lib->images_in_row;
  }

  // Are we after the last fully visible image ?
  while(rowid > offset + lib->images_in_row * lib->visible_rows)
  {
    offset += lib->images_in_row;
  }

  if(offset != lib->offset)
  {
    lib->first_visible_filemanager = lib->offset = offset;
    lib->offset_changed = TRUE;
    dt_control_queue_redraw_center();
  }
}

static void _preview_enter(dt_view_t *self, gboolean sticky, gboolean focus, int32_t mouse_over_id)
{
  dt_library_t *lib = (dt_library_t *)self->data;

  lib->full_preview_sticky = sticky;
  lib->full_preview_id = mouse_over_id;

  // set corresponding rowid in the collected images
  {
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT rowid FROM memory.collected_images WHERE imgid=?1", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, lib->full_preview_id);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      lib->full_preview_rowid = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
  }

  // if there's only 1 image selected, and it's the one display
  sqlite3_stmt *stmt;
  int nb_sel = 0;
  uint32_t imgid_sel = -1;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.selected_images", -1, &stmt,
                              NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    nb_sel++;
    if(nb_sel > 1) break;
    imgid_sel = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  if(nb_sel == 1 && imgid_sel == lib->full_preview_id)
    lib->full_preview_follow_sel = TRUE;
  else
    lib->full_preview_follow_sel = FALSE;

  // restore panels
  dt_lib_module_t *m = darktable.view_manager->proxy.filmstrip.module;
  dt_lib_module_t *timeline = darktable.view_manager->proxy.timeline.module;
  gtk_widget_hide(GTK_WIDGET(timeline->widget));
  gtk_widget_show(GTK_WIDGET(m->widget));
  dt_view_filmstrip_scroll_to_image(darktable.view_manager, lib->full_preview_id, FALSE);
  dt_ui_restore_panels(darktable.gui->ui);

  // preview with focus detection
  lib->display_focus = focus;

  // reset preview values
  lib->full_zoom = 1.0f;
  lib->full_x = 0.0f;
  lib->full_y = 0.0f;
  _full_preview_destroy(self);

  // we don't want want drag and drop here
  _unregister_custom_image_order_drag_n_drop(self);

  lib->force_expose_all = TRUE;
}
static void _preview_quit(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  if(lib->full_preview_follow_sel)
  {
    dt_selection_select_single(darktable.selection, lib->full_preview_id);
    _ensure_image_visibility(lib, lib->full_preview_rowid);
  }
  lib->full_preview_id = -1;
  lib->full_preview_rowid = -1;
  if(!lib->using_arrows) dt_control_set_mouse_over_id(-1);

  lib->display_focus = 0;
  _full_preview_destroy(self);
  lib->full_zoom = 1.0f;
  lib->full_x = 0.0f;
  lib->full_y = 0.0f;

  // restore panels
  dt_lib_module_t *m = darktable.view_manager->proxy.filmstrip.module;
  dt_lib_module_t *timeline = darktable.view_manager->proxy.timeline.module;
  gboolean vs = dt_lib_is_visible(timeline);

  if(lib->current_layout == DT_LIGHTTABLE_LAYOUT_EXPOSE || lib->current_layout == DT_LIGHTTABLE_LAYOUT_CULLING)
  {
    gtk_widget_hide(GTK_WIDGET(timeline->widget));
    gtk_widget_show(GTK_WIDGET(m->widget));
  }
  else
  {
    gtk_widget_hide(GTK_WIDGET(m->widget));
    if(vs) gtk_widget_show(GTK_WIDGET(timeline->widget));
    g_timeout_add(200, _expose_again_full, self);
  }
  dt_ui_restore_panels(darktable.gui->ui);
  // restore drag and drop
  _register_custom_image_order_drag_n_drop(self);

  lib->force_expose_all = TRUE;
}

void leave(dt_view_t *self)
{
  gtk_drag_dest_unset(dt_ui_center(darktable.gui->ui));

  // disconnect dropping images for import
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(drag_and_drop_received),self);

  _unregister_custom_image_order_drag_n_drop(self);

  /* disconnect from signals */
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_lighttable_mipmaps_updated_signal_callback),
                               (gpointer)self);

  // clear some state variables
  dt_library_t *lib = (dt_library_t *)self->data;
  lib->button = 0;
  lib->pan = 0;
  lib->activate_on_release = DT_VIEW_ERR;

  // exit preview mode if non-sticky
  if(lib->full_preview_id != -1 && lib->full_preview_sticky == 0)
  {
    _preview_quit(self);
  }

  // cleanup full preview image if any
  _full_preview_destroy(self);

  dt_ui_scrollbars_show(darktable.gui->ui, FALSE);
}

void reset(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  lib->center = 1;
  lib->track = lib->pan = 0;
  lib->activate_on_release = DT_VIEW_ERR;
  lib->offset = 0x7fffffff;
  lib->first_visible_zoomable = -1;
  lib->first_visible_filemanager = 0;
  dt_control_set_mouse_over_id(-1);
}


void mouse_enter(dt_view_t *self)
{
  // TODO: In gtk.c the function center_leave return true. It is not needed when using arrows. the same for mouse_leave, mouse_move
  dt_library_t *lib = (dt_library_t *)self->data;
  uint32_t id = dt_control_get_mouse_over_id();
  if (lib->using_arrows == 0)
  {
    if(id == -1)
    {
      // this seems to be needed to fix the strange events fluxbox emits
      dt_control_set_mouse_over_id(lib->last_mouse_over_id);
    }
  }
}

void mouse_leave(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  if (lib->using_arrows == 0)
  {
    lib->last_mouse_over_id = dt_control_get_mouse_over_id(); // see mouse_enter (re: fluxbox)
    if(!lib->pan && get_zoom() != 1)
    {
      dt_control_set_mouse_over_id(-1);
      dt_control_queue_redraw_center();
    }
  }
}


void scrollbar_changed(dt_view_t *self, double x, double y)
{
  const dt_lighttable_layout_t layout = get_layout();

  switch(layout)
  {
    case DT_LIGHTTABLE_LAYOUT_FILEMANAGER:
    {
      const int iir = get_zoom();
      _set_position(self, round(y/iir)*iir);
      break;
    }
    case DT_LIGHTTABLE_LAYOUT_ZOOMABLE:
    {
      dt_library_t *lib = (dt_library_t *) self->data;
      lib->zoom_x = x;
      lib->zoom_y = y;
      dt_control_queue_redraw_center();
      break;
    }
    default:
      break;
  }
}

void scrolled(dt_view_t *self, double x, double y, int up, int state)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  lib->force_expose_all = TRUE;
  const dt_lighttable_layout_t layout = get_layout();

  if((lib->full_preview_id > -1 || get_layout() == DT_LIGHTTABLE_LAYOUT_EXPOSE
      || get_layout() == DT_LIGHTTABLE_LAYOUT_CULLING)
     && (state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK)
  {
    int sel_img_count = 1;
    if(get_layout() == DT_LIGHTTABLE_LAYOUT_EXPOSE)
    {
      GList *selected = dt_collection_get_selected(darktable.collection, -1);
      sel_img_count = g_list_length(selected);
      if(selected) g_list_free(selected);
    }
    else if(get_layout() == DT_LIGHTTABLE_LAYOUT_CULLING)
    {
      sel_img_count = get_display_num_images();
    }
    const int max_in_memory_images = _get_max_in_memory_images();
    if((get_layout() == DT_LIGHTTABLE_LAYOUT_EXPOSE || get_layout() == DT_LIGHTTABLE_LAYOUT_CULLING)
       && sel_img_count > max_in_memory_images)
    {
      dt_control_log(_("zooming is limited to %d images"), max_in_memory_images);
    }
    else
    {
      float nz = 40.0f;
      if(get_layout() == DT_LIGHTTABLE_LAYOUT_EXPOSE || get_layout() == DT_LIGHTTABLE_LAYOUT_CULLING)
      {
        // we get the 100% zoom of the largest image
        nz = 1.0f;
        for(int i = 0; i < sel_img_count; i++)
        {
          if(lib->fp_surf[i].zoom_100 > nz) nz = lib->fp_surf[i].zoom_100;
        }
      }
      else
        nz = lib->fp_surf[0].zoom_100;

      if(up)
        nz = fminf(nz, lib->full_zoom + 0.5f);
      else
        nz = fmaxf(1.0f, lib->full_zoom - 0.5f);

      if(lib->full_zoom != nz)
      {
        if(get_layout() != DT_LIGHTTABLE_LAYOUT_EXPOSE && get_layout() != DT_LIGHTTABLE_LAYOUT_CULLING)
        {
          // we want to zoom "around" the pointer
          float dx = nz / lib->full_zoom
                         * (x - (self->width - lib->fp_surf[0].w_fit * lib->full_zoom) * 0.5f - lib->full_x)
                     - x + (self->width - lib->fp_surf[0].w_fit * nz) * 0.5f;
          float dy = nz / lib->full_zoom
                         * (y - (self->height - lib->fp_surf[0].h_fit * lib->full_zoom) * 0.5f - lib->full_y)
                     - y + (self->height - lib->fp_surf[0].h_fit * nz) * 0.5f;
          lib->full_x = -dx;
          lib->full_y = -dy;
        }
        lib->full_zoom = nz;
        dt_control_queue_redraw_center();
      }
    }
  }
  else if(lib->full_preview_id > -1)
  {
    if(up)
      lib->track = -DT_LIBRARY_MAX_ZOOM;
    else
      lib->track = +DT_LIBRARY_MAX_ZOOM;

    if(layout == DT_LIGHTTABLE_LAYOUT_CULLING && state == 0) shift_first_selected_image(lib, up);
  }
  else if(layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER && state == 0)
  {
    if(up)
      move_view(lib, DIRECTION_UP);
    else
      move_view(lib, DIRECTION_DOWN);
  }
  else if(layout != DT_LIGHTTABLE_LAYOUT_EXPOSE && layout != DT_LIGHTTABLE_LAYOUT_CULLING)
  {
    int zoom = get_zoom();
    if(up)
    {
      zoom--;
      if(zoom < 1)
        zoom = 1;
      else if(layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER)
        zoom_around_image(lib, x, y, self->width, self->height, zoom + 1, zoom);
    }
    else
    {
      zoom++;
      if(zoom > 2 * DT_LIBRARY_MAX_ZOOM)
        zoom = 2 * DT_LIBRARY_MAX_ZOOM;
      else if(layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER)
        zoom_around_image(lib, x, y, self->width, self->height, zoom - 1, zoom);
    }
    dt_view_lighttable_set_zoom(darktable.view_manager, zoom);
  }
  else if(layout == DT_LIGHTTABLE_LAYOUT_CULLING && state == 0)
  {
    shift_first_selected_image(lib, up);
  }
}

void activate_control_element(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  const dt_lighttable_layout_t layout = get_layout();

  switch(lib->image_over)
  {
    case DT_VIEW_DESERT:
    {
      if(layout != DT_LIGHTTABLE_LAYOUT_EXPOSE && layout != DT_LIGHTTABLE_LAYOUT_CULLING)
      {
        int32_t id = dt_control_get_mouse_over_id();
        if((lib->modifiers & (GDK_SHIFT_MASK | GDK_CONTROL_MASK)) == 0)
          dt_selection_select_single(darktable.selection, id);
        else if((lib->modifiers & (GDK_CONTROL_MASK)) == GDK_CONTROL_MASK)
          dt_selection_toggle(darktable.selection, id);
        else if((lib->modifiers & (GDK_SHIFT_MASK)) == GDK_SHIFT_MASK)
          dt_selection_select_range(darktable.selection, id);
      }
      break;
    }
    case DT_VIEW_REJECT:
    case DT_VIEW_STAR_1:
    case DT_VIEW_STAR_2:
    case DT_VIEW_STAR_3:
    case DT_VIEW_STAR_4:
    case DT_VIEW_STAR_5:
    {
      const int32_t mouse_over_id = dt_control_get_mouse_over_id();
      dt_ratings_apply_to_image_or_group(mouse_over_id, lib->image_over);
      _update_collected_images(self);
      break;
    }
    default:
      break;
  }
}

void mouse_moved(dt_view_t *self, double x, double y, double pressure, int which)
{
  dt_library_t *lib = (dt_library_t *)self->data;

  lib->using_arrows = 0;

  if(lib->pan && (lib->full_preview_id > -1 || get_layout() == DT_LIGHTTABLE_LAYOUT_EXPOSE
                  || get_layout() == DT_LIGHTTABLE_LAYOUT_CULLING)
     && lib->full_zoom > 1.0f)
  {
    // we want the images to stay in the screen
    lib->full_x += x - lib->pan_x;
    lib->full_y += y - lib->pan_y;
    if(lib->full_preview_id != -1)
    {
      lib->full_x = fminf(lib->full_x, lib->fp_surf[0].max_dx);
      lib->full_x = fmaxf(lib->full_x, -lib->fp_surf[0].max_dx);
      lib->full_y = fminf(lib->full_y, lib->fp_surf[0].max_dy);
      lib->full_y = fmaxf(lib->full_y, -lib->fp_surf[0].max_dy);
    }
    else
    {
      float dx = 0.0f;
      float dy = 0.0f;
      int sel_img_count = 1;
      if(get_layout() == DT_LIGHTTABLE_LAYOUT_EXPOSE)
      {
        GList *selected = dt_collection_get_selected(darktable.collection, -1);
        sel_img_count = g_list_length(selected);
        if(selected) g_list_free(selected);
      }
      else if(get_layout() == DT_LIGHTTABLE_LAYOUT_CULLING)
      {
        sel_img_count = get_display_num_images();
      }
      for(int i = 0; i < sel_img_count; i++)
      {
        dx = fmaxf(dx, lib->fp_surf[i].max_dx);
        dy = fmaxf(dy, lib->fp_surf[i].max_dy);
      }
      lib->full_x = fminf(lib->full_x, dx);
      lib->full_x = fmaxf(lib->full_x, -dx);
      lib->full_y = fminf(lib->full_y, dy);
      lib->full_y = fmaxf(lib->full_y, -dy);
    }

    lib->pan_x = x;
    lib->pan_y = y;
  }

  if(lib->pan || lib->pointed_img_over == DT_VIEW_ERR || x < lib->pointed_img_x || y < lib->pointed_img_y
     || x > lib->pointed_img_x + lib->pointed_img_wd || y > lib->pointed_img_y + lib->pointed_img_ht
     || lib->pointed_img_over
            != dt_view_guess_image_over(lib->pointed_img_wd, lib->pointed_img_ht, lib->images_in_row,
                                        lib->images_in_row == 1 ? x : fmodf(x + lib->zoom_x, lib->pointed_img_wd),
                                        lib->images_in_row == 1 ? y : fmodf(y + lib->zoom_y, lib->pointed_img_ht)))
  {
    dt_control_queue_redraw_center();
  }
}

int button_released(dt_view_t *self, double x, double y, int which, uint32_t state)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  // when custom order is used, we need to redraw the whole lighttable
  if ((which == 1 || which == GDK_BUTTON1_MASK) && _is_custom_image_order_actif(self))
  {
    lib->force_expose_all = TRUE;
  }
  lib->pan = 0;
  // If a control element was activated by the button press and we decided to
  // defer action until release, then now it's time to act.
  if(lib->activate_on_release != DT_VIEW_ERR)
  {
    if(lib->activate_on_release == lib->image_over)
    {
      activate_control_element(self);
      lib->force_expose_all = TRUE;
    }
    lib->activate_on_release = DT_VIEW_ERR;
  }
  if(which == 1 || which == GDK_BUTTON1_MASK) dt_control_change_cursor(GDK_LEFT_PTR);
  return 1;
}


static void _audio_child_watch(GPid pid, gint status, gpointer data)
{
  dt_library_t *lib = (dt_library_t *)data;
  lib->audio_player_id = -1;
  g_spawn_close_pid(pid);
}

static void _stop_audio(dt_library_t *lib)
{
  // make sure that the process didn't finish yet and that _audio_child_watch() hasn't run
  if(lib->audio_player_id == -1) return;
  // we don't want to trigger the callback due to a possible race condition
  g_source_remove(lib->audio_player_event_source);
#ifdef _WIN32
// TODO: add Windows code to actually kill the process
#else  // _WIN32
  if(lib->audio_player_id != -1)
  {
    if(getpgid(0) != getpgid(lib->audio_player_pid))
      kill(-lib->audio_player_pid, SIGKILL);
    else
      kill(lib->audio_player_pid, SIGKILL);
  }
#endif // _WIN32
  g_spawn_close_pid(lib->audio_player_pid);
  lib->audio_player_id = -1;
}

int button_pressed(dt_view_t *self, double x, double y, double pressure, int which, int type, uint32_t state)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  const dt_lighttable_layout_t layout = get_layout();

  lib->modifiers = state;
  lib->key_jump_offset = 0;
  lib->button = which;
  lib->select_offset_x = lib->zoom_x;
  lib->select_offset_y = lib->zoom_y;
  lib->select_offset_x += x;
  lib->select_offset_y += y;
  lib->force_expose_all = TRUE;
  lib->activate_on_release = DT_VIEW_ERR;

  if(which == 1 && type == GDK_2BUTTON_PRESS) return 0;
  // image button pressed?
  if(which == 1)
  {
    switch(lib->image_over)
    {
      case DT_VIEW_DESERT:
        // Here we begin to pan immediately, even though later we might decide
        // that the event was actually a click. For this reason we do not set
        // the pointer to GDK_HAND1 until we can exclude that it is a click,
        // namely until the pointer has moved a little distance. The code taking
        // care of this is in expose(). Pan only makes sense in zoomable lt.
        if(layout == DT_LIGHTTABLE_LAYOUT_ZOOMABLE || (lib->full_preview_id > -1 && lib->full_zoom > 1.0f)
           || ((get_layout() == DT_LIGHTTABLE_LAYOUT_EXPOSE || get_layout() == DT_LIGHTTABLE_LAYOUT_CULLING)
               && lib->full_zoom > 1.0f))
          begin_pan(lib, x, y);

        if(layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER && lib->using_arrows)
        {
          // in this case dt_control_get_mouse_over_id() means "last image visited with arrows"
          lib->using_arrows = 0;
          return 0;
        }
      // no break here intentionally
      case DT_VIEW_REJECT:
      case DT_VIEW_STAR_1:
      case DT_VIEW_STAR_2:
      case DT_VIEW_STAR_3:
      case DT_VIEW_STAR_4:
      case DT_VIEW_STAR_5:
        // In file manager we act immediatley, in zoomable lt we defer action
        // until either the button is released or the pointer leaves the
        // activated control. In the second case, we cancel the action, and
        // instead we begin to pan. We do this for those users intending to
        // pan that accidentally hit a control element.
        if(layout != DT_LIGHTTABLE_LAYOUT_ZOOMABLE) // filemanager/expose
          activate_control_element(self);
        else // zoomable lighttable --> defer action to check for pan
          lib->activate_on_release = lib->image_over;
        break;

      case DT_VIEW_GROUP:
      {
        const int32_t mouse_over_id = dt_control_get_mouse_over_id();
        const dt_image_t *image = dt_image_cache_get(darktable.image_cache, mouse_over_id, 'r');
        if(!image) return 0;
        const int group_id = image->group_id;
        const int id = image->id;
        dt_image_cache_read_release(darktable.image_cache, image);

        if(state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK)) // just add the whole group to the selection. TODO:
                                                        // make this also work for collapsed groups.
        {
          sqlite3_stmt *stmt;
          DT_DEBUG_SQLITE3_PREPARE_V2(
              dt_database_get(darktable.db),
              "INSERT OR IGNORE INTO main.selected_images SELECT id FROM main.images WHERE group_id = ?1",
              -1, &stmt, NULL);
          DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, group_id);
          sqlite3_step(stmt);
          sqlite3_finalize(stmt);
        }
        else if(group_id == darktable.gui->expanded_group_id) // the group is already expanded, so ...
        {
          if(id == darktable.gui->expanded_group_id) // ... collapse it
            darktable.gui->expanded_group_id = -1;
          else // ... make the image the new representative of the group
            darktable.gui->expanded_group_id = dt_grouping_change_representative(id);
        }
        else // expand the group
          darktable.gui->expanded_group_id = group_id;
        dt_collection_update_query(darktable.collection);
        break;
      }
      case DT_VIEW_AUDIO:
      {
        const int32_t mouse_over_id = dt_control_get_mouse_over_id();
        gboolean start_audio = TRUE;
        if(lib->audio_player_id != -1)
        {
          // don't start the audio for the image we just killed it for
          if(lib->audio_player_id == mouse_over_id) start_audio = FALSE;

          _stop_audio(lib);
        }

        if(start_audio)
        {
          // if no audio is played at the moment -> play audio
          char *player = dt_conf_get_string("plugins/lighttable/audio_player");
          if(player && *player)
          {
            char *filename = dt_image_get_audio_path(mouse_over_id);
            if(filename)
            {
              char *argv[] = { player, filename, NULL };
              gboolean ret
                  = g_spawn_async(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH
                                                    | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                                  NULL, NULL, &lib->audio_player_pid, NULL);

              if(ret)
              {
                lib->audio_player_id = mouse_over_id;
                lib->audio_player_event_source
                    = g_child_watch_add(lib->audio_player_pid, (GChildWatchFunc)_audio_child_watch, lib);
              }
              else
                lib->audio_player_id = -1;

              g_free(filename);
            }
          }
          g_free(player);
        }

        break;
      }
      default:
        begin_pan(lib, x, y);
        dt_control_change_cursor(GDK_HAND1);
        return 0;
    }
  }
  return 1;
}

int key_released(dt_view_t *self, guint key, guint state)
{
  dt_control_accels_t *accels = &darktable.control->accels;
  dt_library_t *lib = (dt_library_t *)self->data;
  const dt_lighttable_layout_t layout = get_layout();

  // in zoomable lighttable mode always expose full when a key is pressed as the whole area is
  // adjusted each time a navigation key is used.
  if (layout == DT_LIGHTTABLE_LAYOUT_ZOOMABLE)
    lib->force_expose_all = TRUE;

  if(lib->key_select && (key == GDK_KEY_Shift_L || key == GDK_KEY_Shift_R))
  {
    lib->key_select = 0;
    lib->key_select_direction = DIRECTION_NONE;
  }

  if(!darktable.control->key_accelerators_on) return 0;

  // hide/show sideborders, we need a full expose
  if(key == accels->global_sideborders.accel_key && state == accels->global_sideborders.accel_mods)
    lib->force_expose_all = TRUE;

  // hide/show timeline, we need a full expose
  if(key == accels->lighttable_timeline.accel_key && state == accels->lighttable_timeline.accel_mods)
    lib->force_expose_all = TRUE;

  if(((key == accels->lighttable_preview.accel_key && state == accels->lighttable_preview.accel_mods)
      || (key == accels->lighttable_preview_display_focus.accel_key
          && state == accels->lighttable_preview_display_focus.accel_mods)) && lib->full_preview_id != -1)
  {
    _preview_quit(self);
  }

  return 1;
}

int key_pressed(dt_view_t *self, guint key, guint state)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  dt_control_accels_t *accels = &darktable.control->accels;

  if(!darktable.control->key_accelerators_on) return 0;

  int zoom = get_zoom();

  const dt_lighttable_layout_t layout = get_layout();

  if(lib->full_preview_id != -1 && ((key == accels->lighttable_preview_sticky.accel_key
                                     && state == accels->lighttable_preview_sticky.accel_mods)
                                    || (key == accels->lighttable_preview_sticky_focus.accel_key
                                        && state == accels->lighttable_preview_sticky_focus.accel_mods)))
  {
    _preview_quit(self);
    return 1;
  }

  if((key == accels->lighttable_preview.accel_key && state == accels->lighttable_preview.accel_mods)
     || (key == accels->lighttable_preview_display_focus.accel_key
         && state == accels->lighttable_preview_display_focus.accel_mods)
     || (key == accels->lighttable_preview_sticky.accel_key
         && state == accels->lighttable_preview_sticky.accel_mods)
     || (key == accels->lighttable_preview_sticky_focus.accel_key
         && state == accels->lighttable_preview_sticky_focus.accel_mods))
  {
    const int32_t mouse_over_id = dt_control_get_mouse_over_id();
    if(lib->full_preview_id == -1 && mouse_over_id != -1)
    {
      gboolean sticky = TRUE;
      gboolean focus = FALSE;
      if((key == accels->lighttable_preview.accel_key && state == accels->lighttable_preview.accel_mods)
         || (key == accels->lighttable_preview_display_focus.accel_key
             && state == accels->lighttable_preview_display_focus.accel_mods))
      {
        sticky = FALSE;
      }
      if((key == accels->lighttable_preview_display_focus.accel_key
          && state == accels->lighttable_preview_display_focus.accel_mods)
         || (key == accels->lighttable_preview_sticky_focus.accel_key
             && state == accels->lighttable_preview_sticky_focus.accel_mods))
      {
        focus = TRUE;
      }

      _preview_enter(self, sticky, focus, mouse_over_id);
      return 1;
    }
    return 0;
  }

  if (key == GDK_KEY_Shift_L || key == GDK_KEY_Shift_R)
  {
    lib->key_select = 1;
  }

  // key move left
  if((key == accels->lighttable_left.accel_key && state == accels->lighttable_left.accel_mods)
     || (key == accels->lighttable_left.accel_key && layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER && zoom != 1))
  {
    if(lib->full_preview_id > -1)
    {
      lib->track = -DT_LIBRARY_MAX_ZOOM;
      if(layout == DT_LIGHTTABLE_LAYOUT_CULLING) shift_first_selected_image(lib, TRUE);
    }
    else if(layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER)
    {
      if (zoom == 1)
      {
        move_view(lib, DIRECTION_UP);
        lib->using_arrows = 0;
      }
      else
      {
        lib->using_arrows = 1;
        lib->key_jump_offset = -1;
        lib->track = -1;
      }
    }
    else if(layout == DT_LIGHTTABLE_LAYOUT_CULLING)
    {
      lib->track = -1;
      shift_first_selected_image(lib, TRUE);
    }
    else
      lib->track = -1;
    return 1;
  }

  // key move right
  if((key == accels->lighttable_right.accel_key && state == accels->lighttable_right.accel_mods)
     || (key == accels->lighttable_right.accel_key && layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER && zoom != 1))
  {
    if(lib->full_preview_id > -1)
    {
      lib->track = +DT_LIBRARY_MAX_ZOOM;
      if(layout == DT_LIGHTTABLE_LAYOUT_CULLING) shift_first_selected_image(lib, FALSE);
    }
    else if(layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER)
    {
      if (zoom == 1)
      {
        move_view(lib, DIRECTION_DOWN);
        lib->using_arrows = 0;
      }
      else
      {
        lib->using_arrows = 1;
        lib->key_jump_offset = 1;
        lib->track = -1;
      }
    }
    else if(layout == DT_LIGHTTABLE_LAYOUT_CULLING)
    {
      lib->track = 1;
      shift_first_selected_image(lib, FALSE);
    }
    else
      lib->track = 1;
    return 1;
  }

  // key move up
  if((key == accels->lighttable_up.accel_key && state == accels->lighttable_up.accel_mods)
     || (key == accels->lighttable_up.accel_key && layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER && zoom != 1))
  {
    if(lib->full_preview_id > -1)
    {
      lib->track = -DT_LIBRARY_MAX_ZOOM;
      if(layout == DT_LIGHTTABLE_LAYOUT_CULLING) shift_first_selected_image(lib, TRUE);
    }
    else if(layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER)
    {
      if (zoom == 1)
      {
        move_view(lib, DIRECTION_UP);
        lib->using_arrows = 0;
      }
      else {
        lib->using_arrows = 1;
        lib->key_jump_offset = zoom*-1;
      }
    }
    else if(layout == DT_LIGHTTABLE_LAYOUT_CULLING)
    {
      lib->track = -DT_LIBRARY_MAX_ZOOM;
      shift_first_selected_image(lib, TRUE);
    }
    else
      lib->track = -DT_LIBRARY_MAX_ZOOM;
    return 1;
  }

  // key move donw
  if((key == accels->lighttable_down.accel_key && state == accels->lighttable_down.accel_mods)
     || (key == accels->lighttable_down.accel_key && layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER && zoom != 1))
  {
    if(lib->full_preview_id > -1)
    {
      lib->track = +DT_LIBRARY_MAX_ZOOM;
      if(layout == DT_LIGHTTABLE_LAYOUT_CULLING) shift_first_selected_image(lib, FALSE);
    }
    else if(layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER)
    {
      if (zoom == 1)
      {
        move_view(lib, DIRECTION_DOWN);
        lib->using_arrows = 0;
      }
      else
      {
        lib->using_arrows = 1;
        lib->key_jump_offset = zoom;
      }
    }
    else if(layout == DT_LIGHTTABLE_LAYOUT_CULLING)
    {
      lib->track = DT_LIBRARY_MAX_ZOOM;
      shift_first_selected_image(lib, FALSE);
    }
    else
      lib->track = DT_LIBRARY_MAX_ZOOM;
    return 1;
  }

  if(key == accels->lighttable_center.accel_key && state == accels->lighttable_center.accel_mods)
  {
    lib->force_expose_all = TRUE;
    lib->center = 1;
    return 1;
  }

  // zoom out key
  if(key == accels->global_zoom_in.accel_key && state == accels->global_zoom_in.accel_mods)
  {
    zoom--;
    if(zoom < 1) zoom = 1;

    lib->force_expose_all = TRUE;
    dt_view_lighttable_set_zoom(darktable.view_manager, zoom);
    return 1;
  }

  // zoom in key
  if(key == accels->global_zoom_out.accel_key && state == accels->global_zoom_out.accel_mods)
  {
    zoom++;
    if(zoom > 2 * DT_LIBRARY_MAX_ZOOM) zoom = 2 * DT_LIBRARY_MAX_ZOOM;

    lib->force_expose_all = TRUE;
    dt_view_lighttable_set_zoom(darktable.view_manager, zoom);
    return 1;
  }

  return 0;
}

static gboolean timeline_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                            GdkModifierType modifier, gpointer data)
{
  dt_lib_module_t *m = darktable.view_manager->proxy.timeline.module;
  if(get_layout() == DT_LIGHTTABLE_LAYOUT_EXPOSE || get_layout() == DT_LIGHTTABLE_LAYOUT_CULLING)
  {
    gtk_widget_hide(GTK_WIDGET(m->widget)); // to be sure
  }
  else
  {
    gboolean vs = dt_lib_is_visible(m);
    dt_lib_set_visible(m, !vs);
  }
  return TRUE;
}

void init_key_accels(dt_view_t *self)
{
  // Initializing accelerators

  // Color labels keys
  dt_accel_register_view(self, NC_("accel", "color red"), GDK_KEY_F1, 0);
  dt_accel_register_view(self, NC_("accel", "color yellow"), GDK_KEY_F2, 0);
  dt_accel_register_view(self, NC_("accel", "color green"), GDK_KEY_F3, 0);
  dt_accel_register_view(self, NC_("accel", "color blue"), GDK_KEY_F4, 0);
  dt_accel_register_view(self, NC_("accel", "color purple"), GDK_KEY_F5, 0);
  dt_accel_register_view(self, NC_("accel", "clear color labels"), 0, 0);

  // Rating keys
  dt_accel_register_view(self, NC_("accel", "rate 0"), GDK_KEY_0, 0);
  dt_accel_register_view(self, NC_("accel", "rate 1"), GDK_KEY_1, 0);
  dt_accel_register_view(self, NC_("accel", "rate 2"), GDK_KEY_2, 0);
  dt_accel_register_view(self, NC_("accel", "rate 3"), GDK_KEY_3, 0);
  dt_accel_register_view(self, NC_("accel", "rate 4"), GDK_KEY_4, 0);
  dt_accel_register_view(self, NC_("accel", "rate 5"), GDK_KEY_5, 0);
  dt_accel_register_view(self, NC_("accel", "rate reject"), GDK_KEY_r, 0);

  // Navigation keys
  dt_accel_register_view(self, NC_("accel", "navigate up"), GDK_KEY_g, 0);
  dt_accel_register_view(self, NC_("accel", "navigate down"), GDK_KEY_g, GDK_SHIFT_MASK);
  dt_accel_register_view(self, NC_("accel", "navigate page up"), GDK_KEY_Page_Up, 0);
  dt_accel_register_view(self, NC_("accel", "navigate page down"), GDK_KEY_Page_Down, 0);

  // Scroll keys
  dt_accel_register_view(self, NC_("accel", "scroll up"), GDK_KEY_Up, 0);
  dt_accel_register_view(self, NC_("accel", "scroll down"), GDK_KEY_Down, 0);
  dt_accel_register_view(self, NC_("accel", "scroll left"), GDK_KEY_Left, 0);
  dt_accel_register_view(self, NC_("accel", "scroll right"), GDK_KEY_Right, 0);
  dt_accel_register_view(self, NC_("accel", "scroll center"), GDK_KEY_apostrophe, 0);
  dt_accel_register_view(self, NC_("accel", "realign images to grid"), GDK_KEY_l, 0);
  dt_accel_register_view(self, NC_("accel", "select toggle image"), GDK_KEY_space, 0);
  dt_accel_register_view(self, NC_("accel", "select single image"), GDK_KEY_Return, 0);

  // Preview key
  dt_accel_register_view(self, NC_("accel", "preview"), GDK_KEY_z, 0);
  dt_accel_register_view(self, NC_("accel", "preview with focus detection"), GDK_KEY_z, GDK_CONTROL_MASK);
  dt_accel_register_view(self, NC_("accel", "sticky preview"), 0, 0);
  dt_accel_register_view(self, NC_("accel", "sticky preview with focus detection"), 0, 0);

  // undo/redo
  dt_accel_register_view(self, NC_("accel", "undo"), GDK_KEY_z, GDK_CONTROL_MASK);
  dt_accel_register_view(self, NC_("accel", "redo"), GDK_KEY_y, GDK_CONTROL_MASK);

  // zoom for full preview
  dt_accel_register_view(self, NC_("accel", "preview zoom 100%"), 0, 0);
  dt_accel_register_view(self, NC_("accel", "preview zoom fit"), 0, 0);

  // timeline
  dt_accel_register_view(self, NC_("accel", "toggle timeline"), GDK_KEY_f, GDK_CONTROL_MASK);
}

static gboolean _lighttable_undo_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                          GdkModifierType modifier, gpointer data)
{
  dt_view_t *self = darktable.view_manager->proxy.lighttable.view;
  dt_library_t *lib = (dt_library_t *)self->data;

  dt_undo_do_undo(darktable.undo, DT_UNDO_LIGHTTABLE);

  lib->force_expose_all = TRUE;
  return TRUE;
}

static gboolean _lighttable_redo_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                          GdkModifierType modifier, gpointer data)
{
  dt_view_t *self = darktable.view_manager->proxy.lighttable.view;
  dt_library_t *lib = (dt_library_t *)self->data;

  dt_undo_do_redo(darktable.undo, DT_UNDO_LIGHTTABLE);

  lib->force_expose_all = TRUE;
  return TRUE;
}

static gboolean _lighttable_preview_zoom_100(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                             GdkModifierType modifier, gpointer data)
{
  dt_view_t *self = darktable.view_manager->proxy.lighttable.view;
  dt_library_t *lib = (dt_library_t *)self->data;

  if(lib->full_preview_id > -1)
  {
    lib->full_zoom = 100.0f; // this is ugly, but I don't find a way to know image output size at this stage
    dt_control_queue_redraw_center();
    return TRUE;
  }

  return FALSE;
}
static gboolean _lighttable_preview_zoom_fit(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                             GdkModifierType modifier, gpointer data)
{
  dt_view_t *self = darktable.view_manager->proxy.lighttable.view;
  dt_library_t *lib = (dt_library_t *)self->data;

  if(lib->full_preview_id > -1)
  {
    lib->full_zoom = 1.0f;
    lib->full_x = 0;
    lib->full_y = 0;
    dt_control_queue_redraw_center();
    return TRUE;
  }

  return FALSE;
}

void connect_key_accels(dt_view_t *self)
{
  GClosure *closure;

  // Color labels keys
  closure = g_cclosure_new(G_CALLBACK(colorlabels_key_accel_callback), GINT_TO_POINTER(0), NULL);
  dt_accel_connect_view(self, "color red", closure);
  closure = g_cclosure_new(G_CALLBACK(colorlabels_key_accel_callback), GINT_TO_POINTER(1), NULL);
  dt_accel_connect_view(self, "color yellow", closure);
  closure = g_cclosure_new(G_CALLBACK(colorlabels_key_accel_callback), GINT_TO_POINTER(2), NULL);
  dt_accel_connect_view(self, "color green", closure);
  closure = g_cclosure_new(G_CALLBACK(colorlabels_key_accel_callback), GINT_TO_POINTER(3), NULL);
  dt_accel_connect_view(self, "color blue", closure);
  closure = g_cclosure_new(G_CALLBACK(colorlabels_key_accel_callback), GINT_TO_POINTER(4), NULL);
  dt_accel_connect_view(self, "color purple", closure);
  closure = g_cclosure_new(G_CALLBACK(colorlabels_key_accel_callback), GINT_TO_POINTER(5), NULL);
  dt_accel_connect_view(self, "clear color labels", closure);

  // Rating keys
  closure = g_cclosure_new(G_CALLBACK(rating_key_accel_callback), GINT_TO_POINTER(DT_VIEW_DESERT), NULL);
  dt_accel_connect_view(self, "rate 0", closure);
  closure = g_cclosure_new(G_CALLBACK(rating_key_accel_callback), GINT_TO_POINTER(DT_VIEW_STAR_1), NULL);
  dt_accel_connect_view(self, "rate 1", closure);
  closure = g_cclosure_new(G_CALLBACK(rating_key_accel_callback), GINT_TO_POINTER(DT_VIEW_STAR_2), NULL);
  dt_accel_connect_view(self, "rate 2", closure);
  closure = g_cclosure_new(G_CALLBACK(rating_key_accel_callback), GINT_TO_POINTER(DT_VIEW_STAR_3), NULL);
  dt_accel_connect_view(self, "rate 3", closure);
  closure = g_cclosure_new(G_CALLBACK(rating_key_accel_callback), GINT_TO_POINTER(DT_VIEW_STAR_4), NULL);
  dt_accel_connect_view(self, "rate 4", closure);
  closure = g_cclosure_new(G_CALLBACK(rating_key_accel_callback), GINT_TO_POINTER(DT_VIEW_STAR_5), NULL);
  dt_accel_connect_view(self, "rate 5", closure);
  closure = g_cclosure_new(G_CALLBACK(rating_key_accel_callback), GINT_TO_POINTER(DT_VIEW_REJECT), NULL);
  dt_accel_connect_view(self, "rate reject", closure);

  // Navigation keys
  closure = g_cclosure_new(G_CALLBACK(go_up_key_accel_callback), (gpointer)self, NULL);
  dt_accel_connect_view(self, "navigate up", closure);
  closure = g_cclosure_new(G_CALLBACK(go_down_key_accel_callback), (gpointer)self, NULL);
  dt_accel_connect_view(self, "navigate down", closure);
  closure = g_cclosure_new(G_CALLBACK(go_pgup_key_accel_callback), (gpointer)self, NULL);
  dt_accel_connect_view(self, "navigate page up", closure);
  closure = g_cclosure_new(G_CALLBACK(go_pgdown_key_accel_callback), (gpointer)self, NULL);
  dt_accel_connect_view(self, "navigate page down", closure);
  closure = g_cclosure_new(G_CALLBACK(select_toggle_callback), (gpointer)self, NULL);
  dt_accel_connect_view(self, "select toggle image", closure);
  closure = g_cclosure_new(G_CALLBACK(select_single_callback), (gpointer)self, NULL);
  dt_accel_connect_view(self, "select single image", closure);
  closure = g_cclosure_new(G_CALLBACK(realign_key_accel_callback), (gpointer)self, NULL);
  dt_accel_connect_view(self, "realign images to grid", closure);

  // undo/redo
  closure = g_cclosure_new(G_CALLBACK(_lighttable_undo_callback), (gpointer)self, NULL);
  dt_accel_connect_view(self, "undo", closure);
  closure = g_cclosure_new(G_CALLBACK(_lighttable_redo_callback), (gpointer)self, NULL);
  dt_accel_connect_view(self, "redo", closure);

  // full_preview zoom
  closure = g_cclosure_new(G_CALLBACK(_lighttable_preview_zoom_100), (gpointer)self, NULL);
  dt_accel_connect_view(self, "preview zoom 100%", closure);
  closure = g_cclosure_new(G_CALLBACK(_lighttable_preview_zoom_fit), (gpointer)self, NULL);
  dt_accel_connect_view(self, "preview zoom fit", closure);

  // timeline
  closure = g_cclosure_new(G_CALLBACK(timeline_key_accel_callback), (gpointer)self, NULL);
  dt_accel_connect_view(self, "toggle timeline", closure);
}

static void display_intent_callback(GtkWidget *combo, gpointer user_data)
{
  const int pos = dt_bauhaus_combobox_get(combo);

  dt_iop_color_intent_t new_intent = darktable.color_profiles->display_intent;

  // we are not using the int value directly so it's robust against changes on lcms' side
  switch(pos)
  {
    case 0:
      new_intent = DT_INTENT_PERCEPTUAL;
      break;
    case 1:
      new_intent = DT_INTENT_RELATIVE_COLORIMETRIC;
      break;
    case 2:
      new_intent = DT_INTENT_SATURATION;
      break;
    case 3:
      new_intent = DT_INTENT_ABSOLUTE_COLORIMETRIC;
      break;
  }

  if(new_intent != darktable.color_profiles->display_intent)
  {
    darktable.color_profiles->display_intent = new_intent;
    pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);
    dt_colorspaces_update_display_transforms();
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
    dt_control_queue_redraw_center();
  }
}

static void display_profile_callback(GtkWidget *combo, gpointer user_data)
{
  gboolean profile_changed = FALSE;
  const int pos = dt_bauhaus_combobox_get(combo);
  for(GList *profiles = darktable.color_profiles->profiles; profiles; profiles = g_list_next(profiles))
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)profiles->data;
    if(pp->display_pos == pos)
    {
      if(darktable.color_profiles->display_type != pp->type
        || (darktable.color_profiles->display_type == DT_COLORSPACE_FILE
        && strcmp(darktable.color_profiles->display_filename, pp->filename)))
      {
        darktable.color_profiles->display_type = pp->type;
        g_strlcpy(darktable.color_profiles->display_filename, pp->filename,
                  sizeof(darktable.color_profiles->display_filename));
        profile_changed = TRUE;
      }
      goto end;
    }
  }

  // profile not found, fall back to system display profile. shouldn't happen
  fprintf(stderr, "can't find display profile `%s', using system display profile instead\n", dt_bauhaus_combobox_get_text(combo));
  profile_changed = darktable.color_profiles->display_type != DT_COLORSPACE_DISPLAY;
  darktable.color_profiles->display_type = DT_COLORSPACE_DISPLAY;
  darktable.color_profiles->display_filename[0] = '\0';

end:
  if(profile_changed)
  {
    pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);
    dt_colorspaces_update_display_transforms();
    pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
    dt_control_queue_redraw_center();
  }
}

void gui_init(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;

  // create display profile button
  GtkWidget *const profile_button = dtgtk_button_new(dtgtk_cairo_paint_display, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER,
                                                     NULL);
  gtk_widget_set_tooltip_text(profile_button, _("set display profile"));
  dt_view_manager_module_toolbox_add(darktable.view_manager, profile_button, DT_VIEW_LIGHTTABLE);

  // and the popup window
  const int panel_width = dt_conf_get_int("panel_width");
  lib->profile_floating_window = gtk_popover_new(profile_button);

  gtk_widget_set_size_request(GTK_WIDGET(lib->profile_floating_window), panel_width, -1);
#if GTK_CHECK_VERSION(3, 16, 0)
  g_object_set(G_OBJECT(lib->profile_floating_window), "transitions-enabled", FALSE, NULL);
#endif
  g_signal_connect_swapped(G_OBJECT(profile_button), "button-press-event", G_CALLBACK(gtk_widget_show_all), lib->profile_floating_window);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_widget_set_margin_start(vbox, DT_PIXEL_APPLY_DPI(8));
  gtk_widget_set_margin_end(vbox, DT_PIXEL_APPLY_DPI(8));
  gtk_widget_set_margin_top(vbox, DT_PIXEL_APPLY_DPI(8));
  gtk_widget_set_margin_bottom(vbox, DT_PIXEL_APPLY_DPI(8));

  gtk_container_add(GTK_CONTAINER(lib->profile_floating_window), vbox);

  /** let's fill the encapsulating widgets */
  char datadir[PATH_MAX] = { 0 };
  char confdir[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(confdir, sizeof(confdir));
  dt_loc_get_datadir(datadir, sizeof(datadir));

  GtkWidget *display_intent = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(display_intent, NULL, _("display intent"));
  gtk_box_pack_start(GTK_BOX(vbox), display_intent, TRUE, TRUE, 0);
  dt_bauhaus_combobox_add(display_intent, _("perceptual"));
  dt_bauhaus_combobox_add(display_intent, _("relative colorimetric"));
  dt_bauhaus_combobox_add(display_intent, C_("rendering intent", "saturation"));
  dt_bauhaus_combobox_add(display_intent, _("absolute colorimetric"));

  GtkWidget *display_profile = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(display_profile, NULL, _("display profile"));
  gtk_box_pack_start(GTK_BOX(vbox), display_profile, TRUE, TRUE, 0);

  for(GList *profiles = darktable.color_profiles->profiles; profiles; profiles = g_list_next(profiles))
  {
    dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)profiles->data;
    if(prof->display_pos > -1)
    {
      dt_bauhaus_combobox_add(display_profile, prof->name);
      if(prof->type == darktable.color_profiles->display_type
        && (prof->type != DT_COLORSPACE_FILE
        || !strcmp(prof->filename, darktable.color_profiles->display_filename)))
      {
        dt_bauhaus_combobox_set(display_profile, prof->display_pos);
      }
    }
  }

  char *system_profile_dir = g_build_filename(datadir, "color", "out", NULL);
  char *user_profile_dir = g_build_filename(confdir, "color", "out", NULL);
  char *tooltip = g_strdup_printf(_("display ICC profiles in %s or %s"), user_profile_dir, system_profile_dir);
  gtk_widget_set_tooltip_text(display_profile, tooltip);
  g_free(system_profile_dir);
  g_free(user_profile_dir);
  g_free(tooltip);


  g_signal_connect(G_OBJECT(display_intent), "value-changed", G_CALLBACK(display_intent_callback), NULL);
  g_signal_connect(G_OBJECT(display_profile), "value-changed", G_CALLBACK(display_profile_callback), NULL);

  GList *first_selected = dt_collection_get_selected(darktable.collection, 1);
  GList *collected = dt_collection_get_all(darktable.collection, -1);
  if(first_selected && collected)
  {
    int offset = -1;
    int i = 0;
    const int imgid = GPOINTER_TO_INT(first_selected->data);
    GList *l = collected;
    while(l)
    {
      const int id = GPOINTER_TO_INT(l->data);
      if(imgid == id)
      {
        offset = i;
        break;
      }
      i++;

      l = g_list_next(l);
    }

    lib->last_first_selected = offset;
  }
  if(first_selected) g_list_free(first_selected);
  if(collected) g_list_free(collected);
}

static gboolean _is_order_actif(dt_view_t *self, dt_collection_sort_t sort)
{
  if (darktable.gui)
  {
    const dt_lighttable_layout_t layout = get_layout();

    // only in file manager
    // only in light table
    // only if custom image order is selected
    dt_view_t *current_view = darktable.view_manager->current_view;
    if (layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER
        && darktable.collection->params.sort == sort
        && current_view
        && current_view->view(self) == DT_VIEW_LIGHTTABLE)
    {
      // not in full_preview mode
      dt_library_t *lib = (dt_library_t *)self->data;
      if(lib->full_preview_id == -1) return TRUE;
    }
  }

  return FALSE;
}

static gboolean _is_custom_image_order_actif(dt_view_t *self)
{
  return _is_order_actif(self, DT_COLLECTION_SORT_CUSTOM_ORDER);
}

static gboolean _is_rating_order_actif(dt_view_t *self)
{
  return _is_order_actif(self, DT_COLLECTION_SORT_RATING);
}

static gboolean _is_colorlabels_order_actif(dt_view_t *self)
{
  return _is_order_actif(self, DT_COLLECTION_SORT_COLOR);
}

static void _redraw_selected_images(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.selected_images", -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int imgid  = sqlite3_column_int(stmt, 0);
    g_hash_table_add(lib->thumbs_table, (gpointer)&imgid);
  }
  sqlite3_finalize(stmt);
}

static void _register_custom_image_order_drag_n_drop(dt_view_t *self)
{
  // register drag and drop for custom image ordering only
  // if "custom order" is selected and if the view "Lighttable"
  // is active
  if (_is_custom_image_order_actif(self))
  {
    // drag and drop for custom order of picture sequence (dnd) and drag&drop of external files/folders into darktable
    gtk_drag_source_set(dt_ui_center(darktable.gui->ui), GDK_BUTTON1_MASK, target_list_internal, n_targets_internal, GDK_ACTION_COPY);

    // check if already connected
    const int is_connected = g_signal_handler_find(dt_ui_center(darktable.gui->ui),
                                     G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA,
                                     0, 0, NULL, G_CALLBACK(_dnd_begin_picture_reorder), (gpointer)self) != 0;

    if (!is_connected)
    {
      g_signal_connect(dt_ui_center(darktable.gui->ui), "drag-begin",    G_CALLBACK(_dnd_begin_picture_reorder), (gpointer)self);
      g_signal_connect(dt_ui_center(darktable.gui->ui), "drag-data-get", G_CALLBACK(_dnd_get_picture_reorder),   (gpointer)self);
      g_signal_connect(dt_ui_center(darktable.gui->ui), "drag_motion",   G_CALLBACK(_dnd_drag_picture_motion),   (gpointer)self);
    }
  }
}

static void _unregister_custom_image_order_drag_n_drop(dt_view_t *self)
{
  if (darktable.gui)
  {
    gtk_drag_source_unset(dt_ui_center(darktable.gui->ui));

    g_signal_handlers_disconnect_matched(dt_ui_center(darktable.gui->ui), G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA, 0, 0, NULL, G_CALLBACK(_dnd_begin_picture_reorder), (gpointer)self);
    g_signal_handlers_disconnect_matched(dt_ui_center(darktable.gui->ui), G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA, 0, 0, NULL, G_CALLBACK(_dnd_get_picture_reorder), (gpointer)self);
    g_signal_handlers_disconnect_matched(dt_ui_center(darktable.gui->ui), G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA, 0, 0, NULL, G_CALLBACK(_dnd_drag_picture_motion), (gpointer)self);
  }
}

static void _dnd_get_picture_reorder(GtkWidget *widget, GdkDragContext *context, gint x, gint y,
                                    GtkSelectionData *selection_data, guint target_type, guint time,
                                    gpointer data)
{
  GList *selected_images = dt_collection_get_selected(darktable.collection, -1);
  const int32_t mouse_over_id = dt_control_get_mouse_over_id();
  dt_collection_move_before(mouse_over_id, selected_images);

  dt_control_button_released(x, y, GDK_BUTTON1_MASK, 0 & 0xf);
  //gtk_widget_queue_draw(widget);
  _update_collected_images(darktable.view_manager->proxy.lighttable.view);
  g_list_free(selected_images);
}

static void _dnd_begin_picture_reorder(GtkWidget *widget, GdkDragContext *context, gpointer user_data)
{
  const int ts = DT_PIXEL_APPLY_DPI(64);

  GList *selected_images = dt_collection_get_selected(darktable.collection, 1);

  // if we are dragging a single image -> use the thumbnail of that image
  // otherwise use the generic d&d icon
  // TODO: have something pretty in the 2nd case, too.
  if(dt_collection_get_selected_count(NULL) == 1 && selected_images)
  {
    const int imgid = GPOINTER_TO_INT(selected_images->data);

    dt_mipmap_buffer_t buf;
    dt_mipmap_size_t mip = dt_mipmap_cache_get_matching_size(darktable.mipmap_cache, ts, ts);
    dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid, mip, DT_MIPMAP_BLOCKING, 'r');

    if(buf.buf)
    {
      const int32_t width = buf.width;
      const int32_t height = buf.height;

      if (width > 0 && height > 0)
      {
        for(size_t i = 3; i < (size_t)4 * width * height; i += 4) buf.buf[i] = UINT8_MAX;

        int w = ts, h = ts;
        if(width < height)
          w = (width * ts) / height; // portrait
        else
          h = (height * ts) / width; // landscape

        GdkPixbuf *source = gdk_pixbuf_new_from_data(buf.buf, GDK_COLORSPACE_RGB, TRUE, 8, width,
                                                     height, width * 4, NULL, NULL);
        GdkPixbuf *scaled = gdk_pixbuf_scale_simple(source, w, h, GDK_INTERP_HYPER);
        gtk_drag_set_icon_pixbuf(context, scaled, 0, h);

        if(source) g_object_unref(source);
        if(scaled) g_object_unref(scaled);
      }
    }

    dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
  }

  g_list_free(selected_images);
}

static gboolean _dnd_drag_picture_motion(GtkWidget *dest_button, GdkDragContext *dc, gint x, gint y, guint time, gpointer user_data)
{
  dt_control_queue_redraw_center();
  return FALSE;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
