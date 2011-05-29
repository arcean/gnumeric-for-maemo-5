/* vim: set sw=8: -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Gnumeric's extended canvas used to display a pane
 *
 * Author:
 *     Miguel de Icaza (miguel@kernel.org)
 *     Jody Goldberg (jody@gnome.org)
 *
 * Port to Maemo:
 *	Eduardo Lima  (eduardo.lima@indt.org.br)
 *	Renato Araujo (renato.filho@indt.org.br)
 */
#include <gnumeric-config.h>
#include <glib/gi18n-lib.h>
#include "gnumeric.h"
#include "gnm-pane-impl.h"
#include "gnm-pane.h"

#include "sheet-control-gui-priv.h"
#include "gui-util.h"
#include "mstyle.h"
#include "selection.h"
#include "parse-util.h"
#include "ranges.h"
#include "sheet.h"
#include "sheet-view.h"
#include "application.h"
#include "workbook-view.h"
#include "wbc-gtk-impl.h"
#include "workbook.h"
#include "workbook-cmd-format.h"
#include "commands.h"
#include "cmd-edit.h"
#include "clipboard.h"
#include "sheet-filter-combo.h"
#include "widgets/gnm-cell-combo-view.h"
#include "item-bar.h"
#include "item-cursor.h"
#include "item-edit.h"
#include "item-grid.h"
#include "gnumeric-gconf.h"
#include "dead-kittens.h"

#include <gsf/gsf-impl-utils.h>

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>

#include <string.h>

#define SCROLL_LOCK_MASK GDK_MOD5_MASK

typedef GocCanvasClass GnmPaneClass;
static GocCanvasClass *parent_klass;

static void cb_pane_popup_menu (GnmPane *pane);
static void gnm_pane_clear_obj_size_tip (GnmPane *pane);
static void gnm_pane_display_obj_size_tip (GnmPane *pane, GocItem *ctrl_pt);

/**
 * For now, application/x-gnumeric is disabled. It handles neither
 * images nor graphs correctly.
 */
static GtkTargetEntry const drag_types_in[] = {
	{(char *) "GNUMERIC_SAME_PROC", GTK_TARGET_SAME_APP, 0},
	/* {(char *) "application/x-gnumeric", 0, 0}, */
};

static GtkTargetEntry const drag_types_out[] = {
	{(char *) "GNUMERIC_SAME_PROC", GTK_TARGET_SAME_APP, 0},
	{(char *) "application/x-gnumeric", 0, 0},
};

static gboolean
gnm_pane_guru_key (WBCGtk const *wbcg, GdkEventKey *event)
{
	GtkWidget *entry, *guru = wbc_gtk_get_guru (wbcg);

	if (guru == NULL)
		return FALSE;

	entry = wbcg_get_entry_underlying (wbcg);
	gtk_widget_event ((entry != NULL) ? entry : guru, (GdkEvent *) event);
	return TRUE;
}

static gboolean
gnm_pane_object_key_press (GnmPane *pane, GdkEventKey *ev)
{
	SheetControlGUI *scg = pane->simple.scg;
	SheetControl    *sc = SHEET_CONTROL (scg);
	gboolean const shift	= 0 != (ev->state & GDK_SHIFT_MASK);
	gboolean const control	= 0 != (ev->state & GDK_CONTROL_MASK);
	gboolean const alt	= 0 != (ev->state & GDK_MOD1_MASK);
	gboolean const symmetric = control && alt;
	double   const delta = 1.0 / GOC_CANVAS (pane)->pixels_per_unit;

	switch (ev->keyval) {
	case GDK_KEY_Escape:
		scg_mode_edit (scg);
		gnm_app_clipboard_unant ();
		return TRUE;

	case GDK_KEY_BackSpace: /* Ick! */
	case GDK_KEY_KP_Delete:
	case GDK_KEY_Delete:
		if (scg->selected_objects != NULL) {
			cmd_objects_delete (sc->wbc,
					    go_hash_keys (scg->selected_objects), NULL);
			return TRUE;
		}
		sc_mode_edit (sc);
		break;

	case GDK_KEY_Tab:
	case GDK_KEY_ISO_Left_Tab:
	case GDK_KEY_KP_Tab:
		if ((scg_sheet (scg))->sheet_objects != NULL) {
			scg_object_select_next (scg, (ev->state & GDK_SHIFT_MASK) != 0);
			return TRUE;
		}
		break;

	case GDK_KEY_KP_Left: case GDK_KEY_Left:
		scg_objects_nudge (scg, pane, (alt ? 4 : (control ? 3 : 8)), -delta , 0, symmetric, shift);
		gnm_pane_display_obj_size_tip (pane, NULL);
		return TRUE;
	case GDK_KEY_KP_Right: case GDK_KEY_Right:
		scg_objects_nudge (scg, pane, (alt ? 4 : (control ? 3 : 8)), delta, 0, symmetric, shift);
		gnm_pane_display_obj_size_tip (pane, NULL);
		return TRUE;
	case GDK_KEY_KP_Up: case GDK_KEY_Up:
		scg_objects_nudge (scg, pane, (alt ? 6 : (control ? 1 : 8)), 0, -delta, symmetric, shift);
		gnm_pane_display_obj_size_tip (pane, NULL);
		return TRUE;
	case GDK_KEY_KP_Down: case GDK_KEY_Down:
		scg_objects_nudge (scg, pane, (alt ? 6 : (control ? 1 : 8)), 0, delta, symmetric, shift);
		gnm_pane_display_obj_size_tip (pane, NULL);
		return TRUE;

	default:
		break;
	}
	return FALSE;
}

static gboolean
gnm_pane_key_mode_sheet (GnmPane *pane, GdkEventKey *event,
			 gboolean allow_rangesel)
{
	SheetControlGUI *scg = pane->simple.scg;
	SheetControl *sc = (SheetControl *) scg;
	SheetView *sv = sc->view;
	Sheet *sheet = sv->sheet;
	WBCGtk *wbcg = scg->wbcg;
	gboolean delayed_movement = FALSE;
	gboolean jump_to_bounds = event->state & GDK_CONTROL_MASK;
	gboolean is_enter = FALSE;
	int first_tab_col;
	int state = gnumeric_filter_modifiers (event->state);
	void (*movefn) (SheetControlGUI *, int n, gboolean jump, gboolean horiz);

	gboolean transition_keys = gnm_conf_get_core_gui_editing_transitionkeys ();
	gboolean const end_mode = wbcg->last_key_was_end;

	/* Update end-mode for magic end key stuff. */
	if (event->keyval != GDK_KEY_End && event->keyval != GDK_KEY_KP_End)
		wbcg_set_end_mode (wbcg, FALSE);

	if (allow_rangesel)
		movefn = (event->state & GDK_SHIFT_MASK)
			? scg_rangesel_extend
			: scg_rangesel_move;
	else
		movefn = (event->state & GDK_SHIFT_MASK)
			? scg_cursor_extend
			: scg_cursor_move;

	switch (event->keyval) {
	case GDK_KEY_KP_Left:
	case GDK_KEY_Left:
		//Maemo5 specific: for kbd with 2 arrows
		//if (event->state & GDK_MOD1_MASK)
		//	return TRUE; /* Alt is used for accelerators */

		/*if (event->state & SCROLL_LOCK_MASK)
			scg_set_left_col (scg, pane->first.col - 1);
		else */if (transition_keys && jump_to_bounds) {
			delayed_movement = TRUE;
			scg_queue_movement (scg, movefn,
				-(pane->last_visible.col - pane->first.col),
				FALSE, TRUE);
		} else
			(*movefn) (scg, sheet->text_is_rtl ? 1 : -1,
				   jump_to_bounds || end_mode, TRUE);
		break;

	case GDK_KEY_KP_Right:
	case GDK_KEY_Right:
		//Maemo5 specific: for kbd with 2 arrows
		//if (event->state & GDK_MOD1_MASK)
			//return TRUE; /* Alt is used for accelerators */

		/*if (event->state & SCROLL_LOCK_MASK)
			scg_set_left_col (scg, pane->first.col + 1);
		else */if (transition_keys && jump_to_bounds) {
			delayed_movement = TRUE;
			scg_queue_movement (scg, movefn,
				pane->last_visible.col - pane->first.col,
				FALSE, TRUE);
		} else
			(*movefn) (scg, sheet->text_is_rtl ? -1 : 1,
				   jump_to_bounds || end_mode, TRUE);
		break;

	case GDK_KEY_KP_Up:
	case GDK_KEY_Up:
		//Maemo5 specific: for kbd with 2 arrows
		/*if (event->state & SCROLL_LOCK_MASK)
			scg_set_top_row (scg, pane->first.row - 1);
		else */if (transition_keys && jump_to_bounds) {
			delayed_movement = TRUE;
			scg_queue_movement (scg, movefn,
				-(pane->last_visible.row - pane->first.row),
				FALSE, FALSE);
		} else
			(*movefn) (scg, -1, jump_to_bounds || end_mode, FALSE);
		break;

	case GDK_KEY_KP_Down:
	case GDK_KEY_Down:
		if (gnumeric_filter_modifiers (event->state) == GDK_MOD1_MASK) {
			/* 1) Any in cell combos ? */
			SheetObject *so = sv_wbv (sv)->in_cell_combo;

			/* 2) How about any autofilters ? */
			if (NULL == so) {
				GnmRange r;
				GSList *objs = sheet_objects_get (sheet,
					range_init_cellpos (&r, &sv->edit_pos),
					GNM_FILTER_COMBO_TYPE);
				if (objs != NULL)
					so = objs->data, g_slist_free (objs);
			}

			if (NULL != so) {
				SheetObjectView	*sov = sheet_object_get_view (so,
					(SheetObjectViewContainer *)pane);
				gnm_cell_combo_view_popdown (sov, event->time);
				break;
			}
		}
		//Maemo5 specific: for kbd with 2 arrows
		/*if (event->state & SCROLL_LOCK_MASK)
			scg_set_top_row (scg, pane->first.row + 1);
		else*/ if (transition_keys && jump_to_bounds) {
			delayed_movement = TRUE;
			scg_queue_movement (scg, movefn,
					    pane->last_visible.row - pane->first.row,
					    FALSE, FALSE);
		} else
			(*movefn) (scg, 1, jump_to_bounds || end_mode, FALSE);
		break;

	case GDK_KEY_KP_Page_Up:
	case GDK_KEY_Page_Up:
		if ((event->state & GDK_CONTROL_MASK) != 0)
			gnm_notebook_prev_page (wbcg->bnotebook);
		else if ((event->state & GDK_MOD1_MASK) == 0) {
			delayed_movement = TRUE;
			scg_queue_movement (scg, movefn,
					    -(pane->last_visible.row - pane->first.row),
					    FALSE, FALSE);
		} else {
			delayed_movement = TRUE;
			scg_queue_movement (scg, movefn,
					    -(pane->last_visible.col - pane->first.col),
					    FALSE, TRUE);
		}
		break;

	case GDK_KEY_KP_Page_Down:
	case GDK_KEY_Page_Down:
		if ((event->state & GDK_CONTROL_MASK) != 0)
			gnm_notebook_next_page (wbcg->bnotebook);
		else if ((event->state & GDK_MOD1_MASK) == 0) {
			delayed_movement = TRUE;
			scg_queue_movement (scg, movefn,
					    pane->last_visible.row - pane->first.row,
					    FALSE, FALSE);
		} else {
			delayed_movement = TRUE;
			scg_queue_movement (scg, movefn,
					    pane->last_visible.col - pane->first.col,
					    FALSE, TRUE);
		}
		break;

	case GDK_KEY_KP_Home:
	case GDK_KEY_Home:
		if (event->state & SCROLL_LOCK_MASK) {
			scg_set_left_col (scg, sv->edit_pos.col);
			scg_set_top_row (scg, sv->edit_pos.row);
		} else if (end_mode) {
			/* Same as ctrl-end.  */
			GnmRange r = sheet_get_extent (sheet, FALSE);
			(*movefn) (scg, r.end.col - sv->edit_pos.col, FALSE, TRUE);
			(*movefn)(scg, r.end.row - sv->edit_pos.row, FALSE, FALSE);
		} else {
			/* do the ctrl-home jump to A1 in 2 steps */
			(*movefn)(scg, -gnm_sheet_get_max_cols (sheet), FALSE, TRUE);
			if ((event->state & GDK_CONTROL_MASK) || transition_keys)
				(*movefn)(scg, -gnm_sheet_get_max_rows (sheet), FALSE, FALSE);
		}
		break;

	case GDK_KEY_KP_End:
	case GDK_KEY_End:
		if (event->state & SCROLL_LOCK_MASK) {
			int new_col = sv->edit_pos.col - (pane->last_full.col - pane->first.col);
			int new_row = sv->edit_pos.row - (pane->last_full.row - pane->first.row);
			scg_set_left_col (scg, new_col);
			scg_set_top_row (scg, new_row);
		} else if ((event->state & GDK_CONTROL_MASK)) {
			GnmRange r = sheet_get_extent (sheet, FALSE);

			/* do the ctrl-end jump to the extent in 2 steps */
			(*movefn)(scg, r.end.col - sv->edit_pos.col, FALSE, TRUE);
			(*movefn)(scg, r.end.row - sv->edit_pos.row, FALSE, FALSE);
		} else  /* toggle end mode */
			wbcg_set_end_mode (wbcg, !end_mode);
		break;

	case GDK_KEY_KP_Insert :
	case GDK_KEY_Insert :
		if (gnm_pane_guru_key (wbcg, event))
			break;
		if (state == GDK_CONTROL_MASK)
			sv_selection_copy (sv, WORKBOOK_CONTROL (wbcg));
		else if (state == GDK_SHIFT_MASK)
			cmd_paste_to_selection (WORKBOOK_CONTROL (wbcg), sv, PASTE_DEFAULT);
		break;

	case GDK_KEY_BackSpace:
		if (wbcg_is_editing (wbcg))
			goto forward;
		else if (!wbcg_is_editing (wbcg) && (event->state & GDK_CONTROL_MASK) != 0) {
			/* Re-center the view on the active cell */
			scg_make_cell_visible (scg, sv->edit_pos.col,
					       sv->edit_pos.row, FALSE, TRUE);
			break;
		}
		/* Fall through */

	case GDK_KEY_KP_Delete:
	case GDK_KEY_Delete:
		if (wbcg_is_editing (wbcg)) {
			/* stop auto-completion. then do a quick and cheesy update */
			wbcg_auto_complete_destroy (wbcg);
			SCG_FOREACH_PANE (scg, pane, {
				if (pane->editor)
					goc_item_invalidate (GOC_ITEM (pane->editor));
			});
			return TRUE;
		}
		if (gnm_pane_guru_key (wbcg, event))
			break;
		if (state == GDK_SHIFT_MASK) {
			scg_mode_edit (scg);
			sv_selection_cut (sv, WORKBOOK_CONTROL (wbcg));
		} else
			cmd_selection_clear (WORKBOOK_CONTROL (wbcg), CLEAR_VALUES);
		break;

	/*
	 * NOTE : Keep these in sync with the condition
	 *        for tabs.
	 */
	case GDK_KP_Enter:
	case GDK_KEY_Return:
		if (!wbcg_is_editing (wbcg)) {
			//if ((event->state & (GDK_MOD1_MASK|GDK_CONTROL_MASK)) != 0)
			//	return FALSE;

			/* If the character is not printable do not start editing */
			if (event->length == 0)
				return FALSE;

			/* For Maemo changed third arg to FALSE in order to avoid bug */
			blockArrows = TRUE;
			if (!wbcg_edit_start (wbcg, FALSE, FALSE)){
				return FALSE; /* attempt to edit failed */
			}
		}
		//return gtk_widget_event(wbcg_get_entry_underlying(wbcg), (GdkEvent *) event);
		is_enter = TRUE;
		//isEnterEdit = TRUE;
		//break;
		/* Special Maemo5 change */
		if(isEnterEdit){
			isEnterEdit = FALSE;
			break;
		}

	case GDK_KEY_Tab:
	case GDK_KEY_ISO_Left_Tab:
	case GDK_KEY_KP_Tab:
		if (gnm_pane_guru_key (wbcg, event))
			break;

		/* Be careful to restore the editing sheet if we are editing */
		if (wbcg_is_editing (wbcg))
			sheet = wbcg->editing_sheet;

	       	/* registering the cmd clears it,  restore it afterwards */
		first_tab_col = sv->first_tab_col;

		if (wbcg_edit_finish (wbcg, WBC_EDIT_ACCEPT, NULL)) {
			GODirection dir = gnm_conf_get_core_gui_editing_enter_moves_dir ();

			sv->first_tab_col = first_tab_col;

			if ((event->state & GDK_MOD1_MASK) &&
			    (event->state & GDK_CONTROL_MASK) &&
			    !is_enter) {
				if (event->state & GDK_SHIFT_MASK)
					workbook_cmd_dec_indent (sc->wbc);
				else
					workbook_cmd_inc_indent	(sc->wbc);
			} else if (!is_enter || dir != GO_DIRECTION_NONE) {
				gboolean forward = TRUE;
				gboolean horizontal = TRUE;
				if (is_enter) {
					horizontal = go_direction_is_horizontal (dir);
					forward = go_direction_is_forward (dir);
				} else if ((event->state & GDK_CONTROL_MASK) && 
					   ((sc_sheet (sc))->sheet_objects != NULL)) {
					scg_object_select_next 
						(scg, (event->state & GDK_SHIFT_MASK) != 0);
					break;
				}

				if (event->state & GDK_SHIFT_MASK)
					forward = !forward;

				sv_selection_walk_step (sv, forward, horizontal);

				/* invalidate, in case Enter direction changes */
				if (is_enter)
					sv->first_tab_col = -1;
			}
		}
		break;

	case GDK_KEY_Escape:
		wbcg_edit_finish (wbcg, WBC_EDIT_REJECT, NULL);
		gnm_app_clipboard_unant ();
		break;

	case GDK_KEY_F4:
		if (wbcg_is_editing (wbcg))
			return gtk_widget_event (
				wbcg_get_entry_underlying (wbcg), (GdkEvent *) event);
		return TRUE;

	case GDK_KEY_F2:
		if (gnm_pane_guru_key (wbcg, event))
			break;

		if (wbcg_is_editing (wbcg)) {
			GtkWidget *entry = (GtkWidget *) wbcg_get_entry (wbcg);
			GtkWindow *top   = wbcg_toplevel (wbcg);
			if (entry != gtk_window_get_focus (top)) {
				gtk_window_set_focus (top, entry);
				return TRUE;
			}
		}
		if (!wbcg_edit_start (wbcg, FALSE, FALSE))
			return FALSE; /* attempt to edit failed */
		/* fall through */

	default:
		blockArrows = FALSE;
		if (!wbcg_is_editing (wbcg)) {
			//if ((event->state & (GDK_MOD1_MASK|GDK_CONTROL_MASK)) != 0)
			//	return FALSE;

			/* If the character is not printable do not start editing */
			if (event->length == 0)
				return FALSE;

			/* For Maemo changed third arg to FALSE in order to avoid bug */
			if (!wbcg_edit_start (wbcg, TRUE, FALSE)){
				blockArrows=TRUE;
				return FALSE; /* attempt to edit failed */
			}
		}
		scg_rangesel_stop (scg, FALSE);

	forward:
		/* Forward the keystroke to the input line */
		return gtk_widget_event (wbcg_get_entry_underlying (wbcg),
					 (GdkEvent *) event);
	}

	if (!delayed_movement) {
		if (wbcg_is_editing (wbcg))
			sheet_update_only_grid (sheet);
		else
			sheet_update (sheet);
	}

	return TRUE;
}

static gboolean
gnm_pane_colrow_key_press (SheetControlGUI *scg, GdkEventKey *event,
			   gboolean allow_rangesel)
{
	SheetControl *sc = (SheetControl *) scg;
	SheetView *sv = sc->view;
	GnmRange target;

	if (allow_rangesel) {
		if (scg->rangesel.active)
			target = scg->rangesel.displayed;
		else
			target.start = target.end = sv->edit_pos_real;
	} else {
		GnmRange const *r = selection_first_range (sv, NULL, NULL);
		if (NULL == r)
			return FALSE;
		target = *r;
	}

	if (event->state & GDK_SHIFT_MASK) {
		if (event->state & GDK_CONTROL_MASK)	/* full sheet */
			/* TODO : How to handle ctrl-A too ? */
			range_init_full_sheet (&target, sv->sheet);
		else {					/* full row */
			target.start.col = 0;
			target.end.col = gnm_sheet_get_last_col (sv->sheet);
		}
	} else if (event->state & GDK_CONTROL_MASK) {	/* full col */
		target.start.row = 0;
		target.end.row = gnm_sheet_get_last_row (sv->sheet);
	} else
		return FALSE;

	/* Accept during rangesel */
	if (allow_rangesel)
		scg_rangesel_bound (scg,
				    target.start.col, target.start.row,
				    target.end.col, target.end.row);
	/* actually want the ctrl/shift space keys handled by the input module
	 * filters during an edit */
	else if (!wbcg_is_editing (scg->wbcg))
		sv_selection_set (sv, &sv->edit_pos,
				  target.start.col, target.start.row,
				  target.end.col, target.end.row);
	else
		return FALSE;

	return TRUE;
}

static gint
gnm_pane_key_press (GtkWidget *widget, GdkEventKey *event)
{
	GnmPane	*pane = GNM_PANE (widget);
	SheetControlGUI *scg = pane->simple.scg;
	gboolean	 allow_rangesel;

	switch (event->keyval) {
	case GDK_KEY_Shift_L:   case GDK_KEY_Shift_R:
	case GDK_KEY_Alt_L:     case GDK_KEY_Alt_R:
	case GDK_KEY_Control_L: case GDK_KEY_Control_R:
	return (*GTK_WIDGET_CLASS (parent_klass)->key_press_event) (widget, event);
	}

	/* Object manipulation */
	if (scg->selected_objects != NULL ||
	     scg->wbcg->new_object != NULL) {
		if (wbc_gtk_get_guru (scg->wbcg) == NULL &&
		    gnm_pane_object_key_press (pane, event))
			return TRUE;
	}

	/* handle grabs after object keys to allow Esc to cancel, and arrows to
	 * fine tune position even while dragging */
	if (scg->grab_stack > 0)
		return TRUE;

	allow_rangesel = wbcg_rangesel_possible (scg->wbcg);

	/* handle ctrl/shift space before input-method filter steals it */
	if (event->keyval == GDK_KEY_space &&
	    gnm_pane_colrow_key_press (scg, event, allow_rangesel))
		return TRUE;

	pane->insert_decimal =
		event->keyval == GDK_KEY_KP_Decimal ||
		event->keyval == GDK_KEY_KP_Separator;

	if (gtk_im_context_filter_keypress (pane->im_context, event))
		return TRUE;

	/* in gtk-2.8 something changed.  gtk_im_context_reset started
	 * triggering a pre-edit-changed.  We'd end up start and finishing an
	 * empty edit every time the cursor moved */
	pane->im_block_edit_start = TRUE;
	gtk_im_context_reset (pane->im_context);
	pane->im_block_edit_start = FALSE;

	if (gnm_pane_key_mode_sheet (pane, event, allow_rangesel))
		return TRUE;

	return (*GTK_WIDGET_CLASS (parent_klass)->key_press_event) (widget, event);
}

static gint
gnm_pane_key_release (GtkWidget *widget, GdkEventKey *event)
{
	GnmPane *pane = GNM_PANE (widget);
	SheetControl *sc = (SheetControl *) pane->simple.scg;

	if (pane->simple.scg->grab_stack > 0)
		return TRUE;

	if (gtk_im_context_filter_keypress (pane->im_context, event))
		return TRUE;
	/*
	 * The status_region normally displays the current edit_pos
	 * When we extend the selection it changes to displaying the size of
	 * the selected region while we are selecting.  When the shift key
	 * is released, or the mouse button is release we need to reset
	 * to displaying the edit pos.
	 */
	if (pane->simple.scg->selected_objects == NULL &&
	    (event->keyval == GDK_KEY_Shift_L || event->keyval == GDK_KEY_Shift_R))
		wb_view_selection_desc (wb_control_view (sc->wbc), TRUE, NULL);

	return (*GTK_WIDGET_CLASS (parent_klass)->key_release_event) (widget, event);
}

static gint
gnm_pane_focus_in (GtkWidget *widget, GdkEventFocus *event)
{
#ifndef GNM_USE_HILDON
	/* The first call to focus-in was sometimes the first thing to init the
	 * imcontext.  In which case the im_context_focus_in would fire a
	 * preedit-changed, and we would start editing. */
	GnmPane *pane = GNM_PANE (widget);
	if (pane->im_first_focus)
		pane->im_block_edit_start = TRUE;
	gtk_im_context_focus_in (GNM_PANE (widget)->im_context);
	if (pane->im_first_focus) {
		pane->im_first_focus = FALSE;
		pane->im_block_edit_start = FALSE;
	}
#endif
	return (*GTK_WIDGET_CLASS (parent_klass)->focus_in_event) (widget, event);
}

static gint
gnm_pane_focus_out (GtkWidget *widget, GdkEventFocus *event)
{
	gnm_pane_clear_obj_size_tip (GNM_PANE (widget));
	gtk_im_context_focus_out (GNM_PANE (widget)->im_context);
	return (*GTK_WIDGET_CLASS (parent_klass)->focus_out_event) (widget, event);
}

static void
gnm_pane_realize (GtkWidget *w)
{
	GtkStyle  *style;

	GNM_PANE (w)->im_block_edit_start = FALSE;

	if (GTK_WIDGET_CLASS (parent_klass)->realize)
		(*GTK_WIDGET_CLASS (parent_klass)->realize) (w);

	/* Set the default background color of the canvas itself to white.
	 * This makes the redraws when the canvas scrolls flicker less. */
	style = gtk_style_copy (gtk_widget_get_style (w));
	style->bg[GTK_STATE_NORMAL] = style->white;
	gtk_widget_set_style (w, style);
	g_object_unref (style);

	gtk_im_context_set_client_window
		(GNM_PANE (w)->im_context,
		 gtk_widget_get_window (gtk_widget_get_toplevel (w)));
}

static void
gnm_pane_unrealize (GtkWidget *widget)
{
	GnmPane *pane;

	pane = GNM_PANE (widget);
	g_return_if_fail (pane != NULL);

	if (pane->im_context) {
		pane->im_block_edit_start = TRUE;
		gtk_im_context_set_client_window (pane->im_context, NULL);
	}

	(*GTK_WIDGET_CLASS (parent_klass)->unrealize)(widget);
}

static void
gnm_pane_size_allocate (GtkWidget *w, GtkAllocation *allocation)
{
	GnmPane *pane = GNM_PANE (w);
	(*GTK_WIDGET_CLASS (parent_klass)->size_allocate) (w, allocation);
	gnm_pane_compute_visible_region (pane, TRUE);
}

static GtkEditable *
gnm_pane_get_editable (GnmPane const *pane)
{
	GnmExprEntry *gee = wbcg_get_entry_logical (pane->simple.scg->wbcg);
	GtkEntry *entry = gnm_expr_entry_get_entry (gee);
	return GTK_EDITABLE (entry);
}

static void
cb_gnm_pane_commit (GtkIMContext *context, char const *str, GnmPane *pane)
{
	gint tmp_pos, length;
	WBCGtk *wbcg = pane->simple.scg->wbcg;
	GtkEditable *editable = gnm_pane_get_editable (pane);

	if (!wbcg_is_editing (wbcg) && !wbcg_edit_start (wbcg, TRUE, TRUE))
		return;

	if (pane->insert_decimal) {
		GString const *s = go_locale_get_decimal ();
		str = s->str;
		length = s->len;
	} else
		length = strlen (str);

	if (gtk_editable_get_selection_bounds (editable, NULL, NULL))
		gtk_editable_delete_selection (editable);
	else {
		tmp_pos = gtk_editable_get_position (editable);
		if (gtk_entry_get_overwrite_mode (GTK_ENTRY (editable)))
			gtk_editable_delete_text (editable,tmp_pos,tmp_pos+1);
	}

	tmp_pos = gtk_editable_get_position (editable);
	gtk_editable_insert_text (editable, str, length, &tmp_pos);
	gtk_editable_set_position (editable, tmp_pos);
}

static void
cb_gnm_pane_preedit_changed (GtkIMContext *context, GnmPane *pane)
{
	gchar *preedit_string;
	int tmp_pos;
	int cursor_pos;
	WBCGtk *wbcg = pane->simple.scg->wbcg;
	GtkEditable *editable = gnm_pane_get_editable (pane);

	tmp_pos = gtk_editable_get_position (editable);
	if (pane->preedit_attrs)
		pango_attr_list_unref (pane->preedit_attrs);
	gtk_im_context_get_preedit_string (pane->im_context, &preedit_string, &pane->preedit_attrs, &cursor_pos);

	if (!pane->im_block_edit_start &&
	    !wbcg_is_editing (wbcg) && !wbcg_edit_start (wbcg, TRUE, TRUE)) {
		gtk_im_context_reset (pane->im_context);
		pane->preedit_length = 0;
		if (pane->preedit_attrs)
			pango_attr_list_unref (pane->preedit_attrs);
		pane->preedit_attrs = NULL;
		g_free (preedit_string);
		return;
	}

	if (pane->preedit_length)
		gtk_editable_delete_text (editable,tmp_pos,tmp_pos+pane->preedit_length);
	pane->preedit_length = strlen (preedit_string);

	if (pane->preedit_length)
		gtk_editable_insert_text (editable, preedit_string, pane->preedit_length, &tmp_pos);
	g_free (preedit_string);
}

static gboolean
cb_gnm_pane_retrieve_surrounding (GtkIMContext *context, GnmPane *pane)
{
	GtkEditable *editable = gnm_pane_get_editable (pane);
	gchar *surrounding = gtk_editable_get_chars (editable, 0, -1);
	gint   cur_pos     = gtk_editable_get_position (editable);

	gtk_im_context_set_surrounding (context,
					surrounding, strlen (surrounding),
					g_utf8_offset_to_pointer (surrounding, cur_pos) - surrounding);

	g_free (surrounding);
	return TRUE;
}

static gboolean
cb_gnm_pane_delete_surrounding (GtkIMContext *context,
				gint         offset,
				gint         n_chars,
				GnmPane    *pane)
{
	GtkEditable *editable = gnm_pane_get_editable (pane);
	gint cur_pos = gtk_editable_get_position (editable);
	gtk_editable_delete_text (editable,
				  cur_pos + offset,
				  cur_pos + offset + n_chars);

	return TRUE;
}

/* create views for the sheet objects now that we exist */
static void
cb_pane_init_objs (GnmPane *pane)
{
	Sheet *sheet = scg_sheet (pane->simple.scg);
	GSList *ptr, *list;

	if (sheet != NULL) {
		/* List is stored in reverse stacking order.  Top of stack is
		 * first.  On creation new foocanvas item get added to
		 * the front, so we need to create the views in reverse order */
		list = g_slist_reverse (g_slist_copy (sheet->sheet_objects));
		for (ptr = list; ptr != NULL ; ptr = ptr->next)
			sheet_object_new_view (ptr->data,
				(SheetObjectViewContainer *)pane);
		g_slist_free (list);
	}
}

static void
cb_ctrl_pts_free (GocItem **ctrl_pts)
{
	int i = 10;
	while (i-- > 0)
		if (ctrl_pts [i] != NULL)
			g_object_unref (ctrl_pts [i]);
	g_free (ctrl_pts);
}

static void
gnm_pane_dispose (GObject *obj)
{
	GnmPane *pane = GNM_PANE (obj);

	if (pane->col.canvas != NULL) {
		gtk_widget_destroy (GTK_WIDGET (pane->col.canvas));
		pane->col.canvas = NULL;
	}

	if (pane->row.canvas != NULL) {
		gtk_widget_destroy (GTK_WIDGET (pane->row.canvas));
		pane->row.canvas = NULL;
	}

	if (pane->im_context) {
		GtkIMContext *imc = pane->im_context;

		pane->im_context = NULL;
		g_signal_handlers_disconnect_by_func
			(imc, cb_gnm_pane_commit, pane);
		g_signal_handlers_disconnect_by_func
			(imc, cb_gnm_pane_preedit_changed, pane);
		g_signal_handlers_disconnect_by_func
			(imc, cb_gnm_pane_retrieve_surrounding, pane);
		g_signal_handlers_disconnect_by_func
			(imc, cb_gnm_pane_delete_surrounding, pane);
		gtk_im_context_set_client_window (imc, NULL);
		g_object_unref (imc);
	}

	g_slist_free (pane->cursor.animated);
	pane->cursor.animated = NULL;
	go_slist_free_custom (pane->cursor.expr_range, g_object_unref);
	pane->cursor.expr_range = NULL;

	if (pane->mouse_cursor) {
		gdk_cursor_unref (pane->mouse_cursor);
		pane->mouse_cursor = NULL;
	}
	gnm_pane_clear_obj_size_tip (pane);

	if (pane->drag.ctrl_pts) {
		g_hash_table_destroy (pane->drag.ctrl_pts);
		pane->drag.ctrl_pts = NULL;
	}

	/* Be anal just in case we somehow manage to remove a pane
	 * unexpectedly.  */
	pane->grid = NULL;
	pane->editor = NULL;
	pane->cursor.std = pane->cursor.rangesel = pane->cursor.special = NULL;
	pane->size_guide.guide = NULL;
	pane->size_guide.start = NULL;
	pane->size_guide.points = NULL;

	G_OBJECT_CLASS (parent_klass)->dispose (obj);
}

static void
gnm_pane_init (GnmPane *pane)
{
	GocCanvas	*canvas = GOC_CANVAS (pane);
	GocGroup	*root_group = goc_canvas_get_root (canvas);

	pane->grid_items   = goc_group_new (root_group);
	pane->object_views = goc_group_new (root_group);
	pane->action_items = goc_group_new (root_group);

	pane->first.col = pane->last_full.col = pane->last_visible.col = 0;
	pane->first.row = pane->last_full.row = pane->last_visible.row = 0;
	pane->first_offset.x = 0;
	pane->first_offset.y = 0;

	pane->editor = NULL;
	pane->mouse_cursor = NULL;
	pane->cursor.rangesel = NULL;
	pane->cursor.special = NULL;
	pane->cursor.expr_range = NULL;
	pane->cursor.animated = NULL;
	pane->size_tip = NULL;

	pane->slide_handler = NULL;
	pane->slide_data = NULL;
	pane->sliding = -1;
	pane->sliding_x  = pane->sliding_dx = -1;
	pane->sliding_y  = pane->sliding_dy = -1;
	pane->sliding_adjacent_h = pane->sliding_adjacent_v = FALSE;

	pane->drag.button = 0;
	pane->drag.ctrl_pts = g_hash_table_new_full (g_direct_hash, g_direct_equal,
		NULL, (GDestroyNotify) cb_ctrl_pts_free);

	pane->im_context = gtk_im_multicontext_new ();
	pane->preedit_length = 0;
	pane->preedit_attrs    = NULL;
	pane->im_block_edit_start = FALSE;
	pane->im_first_focus = TRUE;

	gtk_widget_set_can_focus (GTK_WIDGET (canvas), TRUE);
	gtk_widget_set_can_default (GTK_WIDGET (canvas), TRUE);

	g_signal_connect (G_OBJECT (pane->im_context), "commit",
			  G_CALLBACK (cb_gnm_pane_commit), pane);
	g_signal_connect (G_OBJECT (pane->im_context), "preedit_changed",
			  G_CALLBACK (cb_gnm_pane_preedit_changed), pane);
	g_signal_connect (G_OBJECT (pane->im_context), "retrieve_surrounding",
			  G_CALLBACK (cb_gnm_pane_retrieve_surrounding),
			  pane);
	g_signal_connect (G_OBJECT (pane->im_context), "delete_surrounding",
			  G_CALLBACK (cb_gnm_pane_delete_surrounding),
			  pane);
}

static void
gnm_pane_class_init (GnmPaneClass *klass)
{
	GObjectClass   *gobject_class = G_OBJECT_CLASS (klass);
	GtkWidgetClass *widget_class  = (GtkWidgetClass *) klass;

	parent_klass = g_type_class_peek_parent (klass);

	gobject_class->dispose  = gnm_pane_dispose;

	widget_class->realize		   = gnm_pane_realize;
	widget_class->unrealize		   = gnm_pane_unrealize;
	widget_class->size_allocate	   = gnm_pane_size_allocate;
	widget_class->key_press_event	   = gnm_pane_key_press;
	widget_class->key_release_event	   = gnm_pane_key_release;
	widget_class->focus_in_event	   = gnm_pane_focus_in;
	widget_class->focus_out_event	   = gnm_pane_focus_out;
}

GSF_CLASS (GnmPane, gnm_pane,
	   gnm_pane_class_init, gnm_pane_init,
	   GNM_SIMPLE_CANVAS_TYPE)

static void
cb_gnm_pane_header_realized (GtkLayout *layout)
{
	GdkWindow *window = gtk_layout_get_bin_window (layout);
	gdk_window_set_back_pixmap (window, NULL, FALSE);
}

static void
gnm_pane_header_init (GnmPane *pane, SheetControlGUI *scg,
		      gboolean is_col_header)
{
	Sheet *sheet = scg_sheet (scg);
	GtkWidget *alignment;
	GocCanvas *canvas = gnm_simple_canvas_new (scg);
	GocGroup *group = goc_canvas_get_root (canvas);
	GocItem *item = goc_item_new (group,
		item_bar_get_type (),
		"pane",	pane,
		"IsColHeader", is_col_header,
		NULL);

	/* give a non-constraining default in case something scrolls before we
	 * are realized */
	if (is_col_header) {
		if (sheet && sheet->text_is_rtl)
			goc_canvas_set_direction (canvas, GOC_DIRECTION_RTL);
		pane->col.canvas = canvas;
		pane->col.item = ITEM_BAR (item);
		alignment = pane->col.alignment = gtk_alignment_new (0, 1, 1, 0);
	} else {
		pane->row.canvas = canvas;
		pane->row.item = ITEM_BAR (item);
		alignment = pane->row.alignment = gtk_alignment_new (1, 0, 0, 1);
	}
	gtk_container_add (GTK_CONTAINER (alignment), GTK_WIDGET (canvas));

	pane->size_guide.points = NULL;
	pane->size_guide.start  = NULL;
	pane->size_guide.guide  = NULL;

	if (NULL != scg &&
	    NULL != sheet &&
	    fabs (1. - sheet->last_zoom_factor_used) > 1e-6)
		goc_canvas_set_pixels_per_unit (canvas, sheet->last_zoom_factor_used);

	g_signal_connect (G_OBJECT (canvas), "realize",
		G_CALLBACK (cb_gnm_pane_header_realized), NULL);
}

static void
cb_pane_drag_data_received (GtkWidget *widget, GdkDragContext *context,
			    gint x, gint y, GtkSelectionData *selection_data,
			    guint info, guint time, GnmPane *pane)
{
	double wx, wy;

	if (gnm_debug_flag ("dnd")) {
		gchar *target_name = gdk_atom_name (gtk_selection_data_get_target (selection_data));
		g_printerr ("drag-data-received - %s\n", target_name);
		g_free (target_name);
	}

	goc_canvas_w2c (GOC_CANVAS (pane), x, y, &wx, &wy);
	scg_drag_data_received (pane->simple.scg,
		gtk_drag_get_source_widget (context),
		wx, wy, selection_data);
}

static void
cb_pane_drag_data_get (GtkWidget *widget, GdkDragContext *context,
		       GtkSelectionData *selection_data,
		       guint info, guint time,
		       SheetControlGUI *scg)
{
	if (gnm_debug_flag ("dnd")) {
		gchar *target_name = gdk_atom_name (gtk_selection_data_get_target (selection_data));
		g_printerr ("drag-data-get - %s \n", target_name);
		g_free (target_name);
	}

	scg_drag_data_get (scg, selection_data);
}

/* Move the rubber bands if we are the source */
static gboolean
cb_pane_drag_motion (GtkWidget *widget, GdkDragContext *context,
		     int x, int y, guint32 time, GnmPane *pane)
{
	GtkWidget *source_widget = gtk_drag_get_source_widget (context);
	SheetControlGUI *scg = GNM_PANE (widget)->simple.scg;

	if ((IS_GNM_PANE (source_widget) &&
	     GNM_PANE (source_widget)->simple.scg == scg)) {
		/* same scg */
		GocCanvas *canvas = GOC_CANVAS (widget);
		GdkModifierType mask;
		double wx, wy;

		g_object_set_data (&context->parent_instance,
			"wbcg", scg_wbcg (scg));
		goc_canvas_w2c (canvas, x, y, &wx, &wy);
		wx *= goc_canvas_get_pixels_per_unit (canvas);
		wy *= goc_canvas_get_pixels_per_unit (canvas);

		gdk_window_get_pointer (gtk_widget_get_parent_window (source_widget),
			NULL, NULL, &mask);
		gnm_pane_objects_drag (GNM_PANE (source_widget), NULL,
			wx, wy, 8, FALSE, (mask & GDK_SHIFT_MASK) != 0);
		gdk_drag_status (context,
				 (mask & GDK_CONTROL_MASK) != 0 ? GDK_ACTION_COPY : GDK_ACTION_MOVE,
				 time);
	}
	return TRUE;
}

static void
cb_pane_drag_end (GtkWidget *widget, GdkDragContext *context,
		  GnmPane *source_pane)
{
	/* ungrab any grabbed item */
	GocItem *item = goc_canvas_get_grabbed_item (GOC_CANVAS (source_pane));
	if (item)
		gnm_simple_canvas_ungrab (item, gtk_get_current_event_time ());
	/* sync the ctrl-pts with the object in case the drag was canceled. */
	gnm_pane_objects_drag (source_pane, NULL,
		source_pane->drag.origin_x,
		source_pane->drag.origin_y,
		8, FALSE, FALSE);
	source_pane->drag.had_motion = FALSE;
	source_pane->drag.button = 0;
}

/**
 * Move the rubber bands back to original position when curser leaves
 * the scg, but not when it moves to another pane. We use object data,
 * and rely on gtk sending drag_move to the new widget before sending
 * drag_leave to the old one.
 */
static void
cb_pane_drag_leave (GtkWidget *widget, GdkDragContext *context,
		    guint32 time, GnmPane *pane)
{
	GtkWidget *source_widget = gtk_drag_get_source_widget (context);
	GnmPane *source_pane;
	WBCGtk *wbcg;

	if (!source_widget || !IS_GNM_PANE (source_widget)) return;

	source_pane = GNM_PANE (source_widget);

	wbcg = scg_wbcg (source_pane->simple.scg);
	if (wbcg == g_object_get_data (&context->parent_instance, "wbcg"))
		return;

	gnm_pane_objects_drag (source_pane, NULL,
		source_pane->drag.origin_x,
		source_pane->drag.origin_y,
		8, FALSE, FALSE);
	source_pane->drag.had_motion = FALSE;
}

static void
gnm_pane_drag_dest_init (GnmPane *pane, SheetControlGUI *scg)
{
	GtkWidget *widget = GTK_WIDGET (pane);

	gtk_drag_dest_set (widget, GTK_DEST_DEFAULT_ALL,
			   drag_types_in, G_N_ELEMENTS (drag_types_in),
			   GDK_ACTION_COPY | GDK_ACTION_MOVE);
	gtk_drag_dest_add_uri_targets (widget);
	gtk_drag_dest_add_image_targets (widget);
	gtk_drag_dest_add_text_targets (widget);

	g_object_connect (G_OBJECT (widget),
		"signal::drag-data-received",	G_CALLBACK (cb_pane_drag_data_received), pane,
		"signal::drag-data-get",	G_CALLBACK (cb_pane_drag_data_get),	scg,
		"signal::drag-motion",		G_CALLBACK (cb_pane_drag_motion),	pane,
		"signal::drag-leave",		G_CALLBACK (cb_pane_drag_leave),	pane,
		"signal::drag-end",		G_CALLBACK (cb_pane_drag_end),		pane,
		NULL);
}

GnmPane *
gnm_pane_new (SheetControlGUI *scg,
	      gboolean col_headers, gboolean row_headers, int index)
{
	GocItem	*item;
	GnmPane	*pane;
	Sheet   *sheet;

	g_return_val_if_fail (IS_SHEET_CONTROL_GUI (scg), NULL);

	pane = g_object_new (GNM_PANE_TYPE, NULL);
	pane->index      = index;
	pane->simple.scg = scg;

	goc_canvas_set_document (GOC_CANVAS (pane), wb_control_get_doc (scg_wbc (scg)));
	if (NULL != (sheet = scg_sheet (scg)) &&
	    fabs (1. - sheet->last_zoom_factor_used) > 1e-6)
		goc_canvas_set_pixels_per_unit (GOC_CANVAS (pane),
			sheet->last_zoom_factor_used);

	gnm_pane_drag_dest_init (pane, scg);

	item = goc_item_new (pane->grid_items,
		item_grid_get_type (),
		"SheetControlGUI", scg,
		NULL);
	pane->grid = ITEM_GRID (item);

	item = goc_item_new (pane->grid_items,
		item_cursor_get_type (),
		"SheetControlGUI", scg,
		NULL);
	pane->cursor.std = ITEM_CURSOR (item);
	if (col_headers)
		gnm_pane_header_init (pane, scg, TRUE);
	else
		pane->col.canvas = NULL;
	if (row_headers)
		gnm_pane_header_init (pane, scg, FALSE);
	else
		pane->row.canvas = NULL;

	g_signal_connect_swapped (pane, "popup-menu",
		G_CALLBACK (cb_pane_popup_menu), pane);
	g_signal_connect_swapped (G_OBJECT (pane), "realize",
		G_CALLBACK (cb_pane_init_objs), pane);

	return pane;
}

/**
 * gnm_pane_find_col:
 * @pane :
 * @x : In canvas coords
 * @col_origin : optionally return the canvas coord of the col
 *
 * Returns the column containing canvas coord @x
 **/
int
gnm_pane_find_col (GnmPane const *pane, gint64 x, gint64 *col_origin)
{
	Sheet const *sheet = scg_sheet (pane->simple.scg);
	int col   = pane->first.col;
	gint64 pixel = pane->first_offset.x;

	if (x < pixel) {
		while (col > 0) {
			ColRowInfo const *ci = sheet_col_get_info (sheet, --col);
			if (ci->visible) {
				pixel -= ci->size_pixels;
				if (x >= pixel) {
					if (col_origin)
						*col_origin = pixel;
					return col;
				}
			}
		}
		if (col_origin)
			*col_origin = 0;
		return 0;
	}

	do {
		ColRowInfo const *ci = sheet_col_get_info (sheet, col);
		if (ci->visible) {
			int const tmp = ci->size_pixels;
			if (x <= pixel + tmp) {
				if (col_origin)
					*col_origin = pixel;
				return col;
			}
			pixel += tmp;
		}
	} while (++col < gnm_sheet_get_last_col (sheet));

	if (col_origin)
		*col_origin = pixel;
	return gnm_sheet_get_last_col (sheet);
}

/**
 * gnm_pane_find_row:
 * @pane :
 * @y : In canvas coords
 * @row_origin : optionally return the canvas coord of the row
 *
 * Returns the column containing canvas coord @y
 **/
int
gnm_pane_find_row (GnmPane const *pane, gint64 y, gint64 *row_origin)
{
	Sheet const *sheet = scg_sheet (pane->simple.scg);
	int row   = pane->first.row;
	gint64 pixel = pane->first_offset.y;

	if (y < pixel) {
		while (row > 0) {
			ColRowInfo const *ri = sheet_row_get_info (sheet, --row);
			if (ri->visible) {
				pixel -= ri->size_pixels;
				if (y >= pixel) {
					if (row_origin)
						*row_origin = pixel;
					return row;
				}
			}
		}
		if (row_origin)
			*row_origin = 0;
		return 0;
	}

	do {
		ColRowInfo const *ri = sheet_row_get_info (sheet, row);
		if (ri->visible) {
			int const tmp = ri->size_pixels;
			if (pixel <= y && y <= pixel + tmp) {
				if (row_origin)
					*row_origin = pixel;
				return row;
			}
			pixel += tmp;
		}
	} while (++row < gnm_sheet_get_last_row (sheet));
	if (row_origin)
		*row_origin = pixel;
	return gnm_sheet_get_last_row (sheet);
}

/*
 * gnm_pane_compute_visible_region : Keeps the top left col/row the same and
 *     recalculates the visible boundaries.
 *
 * @full_recompute :
 *       if TRUE recompute the pixel offsets of the top left row/col
 *       else assumes that the pixel offsets of the top left have not changed.
 */
void
gnm_pane_compute_visible_region (GnmPane *pane,
				 gboolean const full_recompute)
{
	SheetControlGUI const * const scg = pane->simple.scg;
	Sheet const *sheet = scg_sheet (scg);
	GocCanvas   *canvas = GOC_CANVAS (pane);
	gint64 pixels;
	int col, row, width, height;
	GtkAllocation ca;

#if 0
	g_warning ("compute_vis(W)[%d] = %d", pane->index,
		   GTK_WIDGET (pane)->allocation.width);
#endif

	gtk_widget_get_allocation (GTK_WIDGET (canvas), &ca);

	/* When col/row sizes change we need to do a full recompute */
	if (full_recompute) {
		gint64 col_offset = pane->first_offset.x = scg_colrow_distance_get (scg,
										   TRUE, 0, pane->first.col);
		if (NULL != pane->col.canvas)
			goc_canvas_scroll_to (pane->col.canvas, col_offset / canvas->pixels_per_unit, 0);

		pane->first_offset.y = scg_colrow_distance_get (scg,
								  FALSE, 0, pane->first.row);
		if (NULL != pane->row.canvas)
			goc_canvas_scroll_to (pane->row.canvas,
					      0, pane->first_offset.y / canvas->pixels_per_unit);

		goc_canvas_scroll_to (GOC_CANVAS (pane),
				      col_offset / canvas->pixels_per_unit, pane->first_offset.y / canvas->pixels_per_unit);
	}

	/* Find out the last visible col and the last full visible column */
	pixels = 0;
	col = pane->first.col;
	width = ca.width;

	do {
		ColRowInfo const * const ci = sheet_col_get_info (sheet, col);
		if (ci->visible) {
			int const bound = pixels + ci->size_pixels;

			if (bound == width) {
				pane->last_visible.col = col;
				pane->last_full.col = col;
				break;
			}
			if (bound > width) {
				pane->last_visible.col = col;
				if (col == pane->first.col)
					pane->last_full.col = pane->first.col;
				else
					pane->last_full.col = col - 1;
				break;
			}
			pixels = bound;
		}
		++col;
	} while (pixels < width && col < gnm_sheet_get_max_cols (sheet));

	if (col >= gnm_sheet_get_max_cols (sheet)) {
		pane->last_visible.col = gnm_sheet_get_last_col (sheet);
		pane->last_full.col = gnm_sheet_get_last_col (sheet);
	}

	/* Find out the last visible row and the last fully visible row */
	pixels = 0;
	row = pane->first.row;
	height = ca.height;
	do {
		ColRowInfo const * const ri = sheet_row_get_info (sheet, row);
		if (ri->visible) {
			int const bound = pixels + ri->size_pixels;

			if (bound == height) {
				pane->last_visible.row = row;
				pane->last_full.row = row;
				break;
			}
			if (bound > height) {
				pane->last_visible.row = row;
				if (row == pane->first.row)
					pane->last_full.row = pane->first.row;
				else
					pane->last_full.row = row - 1;
				break;
			}
			pixels = bound;
		}
		++row;
	} while (pixels < height && row < gnm_sheet_get_max_rows (sheet));

	if (row >= gnm_sheet_get_max_rows (sheet)) {
		pane->last_visible.row = gnm_sheet_get_last_row (sheet);
		pane->last_full.row = gnm_sheet_get_last_row (sheet);
	}

	/* Update the scrollbar sizes for the primary pane */
	if (pane->index == 0)
		sc_scrollbar_config (SHEET_CONTROL (scg));

	/* Force the cursor to update its bounds relative to the new visible region */
	gnm_pane_reposition_cursors (pane);
}

void
gnm_pane_redraw_range (GnmPane *pane, GnmRange const *r)
{
	SheetControlGUI *scg;
	gint64 x1, y1, x2, y2;
	GnmRange tmp;
	Sheet *sheet;
	double scale = goc_canvas_get_pixels_per_unit (GOC_CANVAS (pane));

	g_return_if_fail (IS_GNM_PANE (pane));

	scg = pane->simple.scg;
	sheet = scg_sheet (scg);

	if ((r->end.col < pane->first.col) ||
	    (r->end.row < pane->first.row) ||
	    (r->start.col > pane->last_visible.col) ||
	    (r->start.row > pane->last_visible.row))
		return;

	/* Only draw those regions that are visible */
	tmp.start.col = MAX (pane->first.col, r->start.col);
	tmp.start.row = MAX (pane->first.row, r->start.row);
	tmp.end.col =  MIN (pane->last_visible.col, r->end.col);
	tmp.end.row =  MIN (pane->last_visible.row, r->end.row);

	/* redraw a border of 2 pixels around the region to handle thick borders
	 * NOTE the 2nd coordinates are excluded so add 1 extra (+2border +1include)
	 */
	x1 = scg_colrow_distance_get (scg, TRUE, pane->first.col, tmp.start.col) +
		pane->first_offset.x;
	y1 = scg_colrow_distance_get (scg, FALSE, pane->first.row, tmp.start.row) +
		pane->first_offset.y;
	x2 = (tmp.end.col < gnm_sheet_get_last_col (sheet))
		? 4 + 1 + x1 + scg_colrow_distance_get (scg, TRUE,
							tmp.start.col, tmp.end.col+1)
		: G_MAXINT64;
	y2 = (tmp.end.row < gnm_sheet_get_last_row (sheet))
		? 4 + 1 + y1 + scg_colrow_distance_get (scg, FALSE,
							tmp.start.row, tmp.end.row+1)
		: G_MAXINT64;

#if 0
	g_printerr ("%s%s:", col_name (min_col), row_name (first_row));
	g_printerr ("%s%s\n", col_name (max_col), row_name (last_row));
#endif

	goc_canvas_invalidate (&pane->simple.canvas, (x1-2) / scale, (y1-2) / scale, x2 / scale, y2 / scale);
}

/*****************************************************************************/

void
gnm_pane_slide_stop (GnmPane *pane)
{
	if (pane->sliding == -1)
		return;

	g_source_remove (pane->sliding);
	pane->slide_handler = NULL;
	pane->slide_data = NULL;
	pane->sliding = -1;
}

static int
col_scroll_step (int dx, Sheet *sheet)
{
	/* FIXME: get from gdk.  */
	int dpi_x_this_screen = 90;
	int start_x = dpi_x_this_screen / 3;
	double double_dx = dpi_x_this_screen / 3.0;
	double step = pow (2.0, (dx - start_x) / double_dx);

	return (int) (CLAMP (step, 1.0, gnm_sheet_get_max_cols (sheet) / 15.0));
}

static int
row_scroll_step (int dy, Sheet *sheet)
{
	/* FIXME: get from gdk.  */
	int dpi_y_this_screen = 90;
	int start_y = dpi_y_this_screen / 4;
	double double_dy = dpi_y_this_screen / 8.0;
	double step = pow (2.0, (dy - start_y) / double_dy);

	return (int) (CLAMP (step, 1.0, gnm_sheet_get_max_rows (sheet) / 15.0));
}

static gint
cb_pane_sliding (GnmPane *pane)
{
	int const pane_index = pane->index;
	GnmPane *pane0 = scg_pane (pane->simple.scg, 0);
	GnmPane *pane1 = scg_pane (pane->simple.scg, 1);
	GnmPane *pane3 = scg_pane (pane->simple.scg, 3);
	gboolean slide_x = FALSE, slide_y = FALSE;
	int col = -1, row = -1;
	Sheet *sheet = scg_sheet (pane->simple.scg);
	GnmPaneSlideInfo info;
	GtkAllocation pa;

#if 0
	g_warning ("slide: %d, %d", pane->sliding_dx, pane->sliding_dy);
#endif

	gtk_widget_get_allocation (GTK_WIDGET (pane), &pa);

	if (pane->sliding_dx > 0) {
		GnmPane *target_pane = pane;

		slide_x = TRUE;
		if (pane_index == 1 || pane_index == 2) {
			if (!pane->sliding_adjacent_h) {
				int width = pa.width;
				int x = pane->first_offset.x + width + pane->sliding_dx;

				/* in case pane is narrow */
				col = gnm_pane_find_col (pane, x, NULL);
				if (col > pane0->last_full.col) {
					pane->sliding_adjacent_h = TRUE;
					pane->sliding_dx = 1; /* good enough */
				} else
					slide_x = FALSE;
			} else
				target_pane = pane0;
		} else
			pane->sliding_adjacent_h = FALSE;

		if (slide_x) {
			col = target_pane->last_full.col +
				col_scroll_step (pane->sliding_dx, sheet);
			if (col >= gnm_sheet_get_last_col (sheet)) {
				col = gnm_sheet_get_last_col (sheet);
				slide_x = FALSE;
			}
		}
	} else if (pane->sliding_dx < 0) {
		slide_x = TRUE;
		col = pane0->first.col - col_scroll_step (-pane->sliding_dx, sheet);

		if (pane1 != NULL) {
			if (pane_index == 0 || pane_index == 3) {
				GtkAllocation p1a;
				int width;

				gtk_widget_get_allocation (GTK_WIDGET (pane1),
							   &p1a);

				width = p1a.width;
				if (pane->sliding_dx > (-width) &&
				    col <= pane1->last_visible.col) {
					int x = pane1->first_offset.x + width + pane->sliding_dx;
					col = gnm_pane_find_col (pane, x, NULL);
					slide_x = FALSE;
				}
			}

			if (col <= pane1->first.col) {
				col = pane1->first.col;
				slide_x = FALSE;
			}
		} else if (col <= 0) {
			col = 0;
			slide_x = FALSE;
		}
	}

	if (pane->sliding_dy > 0) {
		GnmPane *target_pane = pane;

		slide_y = TRUE;
		if (pane_index == 3 || pane_index == 2) {
			if (!pane->sliding_adjacent_v) {
				int height = pa.height;
				int y = pane->first_offset.y + height + pane->sliding_dy;

				/* in case pane is short */
				row = gnm_pane_find_row (pane, y, NULL);
				if (row > pane0->last_full.row) {
					pane->sliding_adjacent_v = TRUE;
					pane->sliding_dy = 1; /* good enough */
				} else
					slide_y = FALSE;
			} else
				target_pane = pane0;
		} else
			pane->sliding_adjacent_v = FALSE;

		if (slide_y) {
			row = target_pane->last_full.row +
				row_scroll_step (pane->sliding_dy, sheet);
			if (row >= gnm_sheet_get_last_row (sheet)) {
				row = gnm_sheet_get_last_row (sheet);
				slide_y = FALSE;
			}
		}
	} else if (pane->sliding_dy < 0) {
		slide_y = TRUE;
		row = pane0->first.row - row_scroll_step (-pane->sliding_dy, sheet);

		if (pane3 != NULL) {
			if (pane_index == 0 || pane_index == 1) {
				GtkAllocation p3a;
				int height;

				gtk_widget_get_allocation (GTK_WIDGET (pane3),
							   &p3a);

				height = p3a.height;
				if (pane->sliding_dy > (-height) &&
				    row <= pane3->last_visible.row) {
					int y = pane3->first_offset.y + height + pane->sliding_dy;
					row = gnm_pane_find_row (pane3, y, NULL);
					slide_y = FALSE;
				}
			}

			if (row <= pane3->first.row) {
				row = pane3->first.row;
				slide_y = FALSE;
			}
		} else if (row <= 0) {
			row = 0;
			slide_y = FALSE;
		}
	}

	if (col < 0 && row < 0) {
		gnm_pane_slide_stop (pane);
		return TRUE;
	}

	if (col < 0) {
		col = gnm_pane_find_col (pane, pane->sliding_x, NULL);
	} else if (row < 0)
		row = gnm_pane_find_row (pane, pane->sliding_y, NULL);

	info.col = col;
	info.row = row;
	info.user_data = pane->slide_data;
	if (pane->slide_handler == NULL ||
	    (*pane->slide_handler) (pane, &info))
		scg_make_cell_visible (pane->simple.scg, col, row, FALSE, TRUE);

	if (!slide_x && !slide_y)
		gnm_pane_slide_stop (pane);
	else if (pane->sliding == -1)
		pane->sliding = g_timeout_add (
					       300, (GSourceFunc) cb_pane_sliding, pane);

	return TRUE;
}

/**
 * gnm_pane_handle_motion :
 * @pane	 : The GnmPane managing the scroll
 * @canvas	 : The Canvas the event comes from
 * @slide_flags	 :
 * @slide_handler: The handler when sliding
 * @user_data	 : closure data
 *
 * Handle a motion event from a @canvas and scroll the @pane
 * depending on how far outside the bounds of @pane the @event is.
 * Usually @canvas == @pane however as long as the canvases share a basis
 * space they can be different.
 **/
gboolean
gnm_pane_handle_motion (GnmPane *pane,
			GocCanvas *canvas, gint64 x, gint64 y,
			GnmPaneSlideFlags slide_flags,
			GnmPaneSlideHandler slide_handler,
			gpointer user_data)
{
	GnmPane *pane0, *pane1, *pane3;
	int pindex, width, height;
	gint64 dx = 0, dy = 0, left, top;
	GtkAllocation pa, p0a, p1a, p3a;

	g_return_val_if_fail (IS_GNM_PANE (pane), FALSE);
	g_return_val_if_fail (GOC_IS_CANVAS (canvas), FALSE);
	g_return_val_if_fail (slide_handler != NULL, FALSE);

	pindex = pane->index;
	left = pane->first_offset.x;
	top = pane->first_offset.y;
	gtk_widget_get_allocation (GTK_WIDGET (pane), &pa);
	width = pa.width;
	height = pa.height;

	pane0 = scg_pane (pane->simple.scg, 0);
	gtk_widget_get_allocation (GTK_WIDGET (pane0), &p0a);

	pane1 = scg_pane (pane->simple.scg, 1);
	if (pane1) gtk_widget_get_allocation (GTK_WIDGET (pane1), &p1a);

	pane3 = scg_pane (pane->simple.scg, 3);
	if (pane3) gtk_widget_get_allocation (GTK_WIDGET (pane3), &p3a);

	if (slide_flags & GNM_PANE_SLIDE_X) {
		if (x < left)
			dx = x - left;
		else if (x >= left + width)
			dx = x - width - left;
	}

	if (slide_flags & GNM_PANE_SLIDE_Y) {
		if (y < top)
			dy = y - top;
		else if (y >= top + height)
			dy = y - height - top;
	}

	if (pane->sliding_adjacent_h) {
		if (pindex == 0 || pindex == 3) {
			if (dx < 0) {
				x = pane1->first_offset.x;
				dx += p1a.width;
				if (dx > 0)
					x += dx;
				dx = 0;
			} else
				pane->sliding_adjacent_h = FALSE;
		} else {
			if (dx > 0) {
				x = pane0->first_offset.x + dx;
				dx -= p0a.width;
				if (dx < 0)
					dx = 0;
			} else if (dx == 0) {
				/* initiate a reverse scroll of panes 0,3 */
				if ((pane1->last_visible.col+1) != pane0->first.col)
					dx = x - (left + width);
			} else
				dx = 0;
		}
	}

	if (pane->sliding_adjacent_v) {
		if (pindex == 0 || pindex == 1) {
			if (dy < 0) {
				y = pane3->first_offset.y;
				dy += p3a.height;
				if (dy > 0)
					y += dy;
				dy = 0;
			} else
				pane->sliding_adjacent_v = FALSE;
		} else {
			if (dy > 0) {
				y = pane0->first_offset.y + dy;
				dy -= p0a.height;
				if (dy < 0)
					dy = 0;
			} else if (dy == 0) {
				/* initiate a reverse scroll of panes 0,1 */
				if ((pane3->last_visible.row+1) != pane0->first.row)
					dy = y - (top + height);
			} else
				dy = 0;
		}
	}

	/* Movement is inside the visible region */
	if (dx == 0 && dy == 0) {
		if (!(slide_flags & GNM_PANE_SLIDE_EXTERIOR_ONLY)) {
			GnmPaneSlideInfo info;
			info.row = gnm_pane_find_row (pane, y, NULL);
			info.col = gnm_pane_find_col (pane, x, NULL);
			info.user_data = user_data;
			(*slide_handler) (pane, &info);
		}
		gnm_pane_slide_stop (pane);
		return TRUE;
	}

	pane->sliding_x  = x;
	pane->sliding_dx = dx;
	pane->sliding_y  = y;
	pane->sliding_dy = dy;
	pane->slide_handler = slide_handler;
	pane->slide_data = user_data;

	if (pane->sliding == -1)
		cb_pane_sliding (pane);
	return FALSE;
}

/* TODO : All the slide_* members of GnmPane really aught to be in
 * SheetControlGUI, most of these routines also belong there.  However, since
 * the primary point of access is via GnmPane and SCG is very large
 * already I'm leaving them here for now.  Move them when we return to
 * investigate how to do reverse scrolling for pseudo-adjacent panes.
 */
void
gnm_pane_slide_init (GnmPane *pane)
{
	GnmPane *pane0, *pane1, *pane3;

	g_return_if_fail (IS_GNM_PANE (pane));

	pane0 = scg_pane (pane->simple.scg, 0);
	pane1 = scg_pane (pane->simple.scg, 1);
	pane3 = scg_pane (pane->simple.scg, 3);

	pane->sliding_adjacent_h = (pane1 != NULL)
		? (pane1->last_full.col == (pane0->first.col - 1))
		: FALSE;
	pane->sliding_adjacent_v = (pane3 != NULL)
		? (pane3->last_full.row == (pane0->first.row - 1))
		: FALSE;
}

static gboolean
cb_obj_autoscroll (GnmPane *pane, GnmPaneSlideInfo const *info)
{
	SheetControlGUI *scg = pane->simple.scg;
	GdkModifierType mask;

	/* Cheesy hack calculate distance we move the screen, this loses the
	 * mouse position */
	double dx = pane->first_offset.x;
	double dy = pane->first_offset.y;
	scg_make_cell_visible (scg, info->col, info->row, FALSE, TRUE);
	dx = pane->first_offset.x - dx;
	dy = pane->first_offset.y - dy;

#if 0
	g_warning ("dx = %g, dy = %g", dx, dy);
#endif

	pane->drag.had_motion = TRUE;
	gdk_window_get_pointer (gtk_widget_get_parent_window (GTK_WIDGET (pane)),
				NULL, NULL, &mask);
	scg_objects_drag (pane->simple.scg, pane,
			  NULL, &dx, &dy, 8, FALSE, (mask & GDK_SHIFT_MASK) != 0, TRUE);

	pane->drag.last_x += dx;
	pane->drag.last_y += dy;
	return FALSE;
}

void
gnm_pane_object_autoscroll (GnmPane *pane, GdkDragContext *context,
			    gint x, gint y, guint time)
{
	int const pane_index = pane->index;
	SheetControlGUI *scg = pane->simple.scg;
	GnmPane *pane0 = scg_pane (scg, 0);
	GnmPane *pane1 = scg_pane (scg, 1);
	GnmPane *pane3 = scg_pane (scg, 3);
	GtkWidget *w = GTK_WIDGET (pane);
	GtkAllocation wa;
	gint dx, dy;

	gtk_widget_get_allocation (w, &wa);

	if (y < wa.y) {
		if (pane_index < 2 && pane3 != NULL) {
			w = GTK_WIDGET (pane3);
			gtk_widget_get_allocation (w, &wa);
		}
		dy = y - wa.y;
		g_return_if_fail (dy <= 0);
	} else if (y >= (wa.y + wa.height)) {
		if (pane_index >= 2) {
			w = GTK_WIDGET (pane0);
			gtk_widget_get_allocation (w, &wa);
		}
		dy = y - (wa.y + wa.height);
		g_return_if_fail (dy >= 0);
	} else
		dy = 0;
	if (x < wa.x) {
		if ((pane_index == 0 || pane_index == 3) && pane1 != NULL) {
			w = GTK_WIDGET (pane1);
			gtk_widget_get_allocation (w, &wa);
		}
		dx = x - wa.x;
		g_return_if_fail (dx <= 0);
	} else if (x >= (wa.x + wa.width)) {
		if (pane_index >= 2) {
			w = GTK_WIDGET (pane0);
			gtk_widget_get_allocation (w, &wa);
		}
		dx = x - (wa.x + wa.width);
		g_return_if_fail (dx >= 0);
	} else
		dx = 0;

	g_object_set_data (&context->parent_instance,
			   "wbcg", scg_wbcg (scg));
	pane->sliding_dx    = dx;
	pane->sliding_dy    = dy;
	pane->slide_handler = &cb_obj_autoscroll;
	pane->slide_data    = NULL;
	pane->sliding_x     = x;
	pane->sliding_y     = y;
	if (pane->sliding == -1)
		cb_pane_sliding (pane);
}

GocGroup *
gnm_pane_object_group (GnmPane *pane)
{
	return pane->object_views;
}

static void
gnm_pane_clear_obj_size_tip (GnmPane *pane)
{
	if (pane->size_tip) {
		gtk_widget_destroy (gtk_widget_get_toplevel (pane->size_tip));
		pane->size_tip = NULL;
	}
}

static void
gnm_pane_display_obj_size_tip (GnmPane *pane, GocItem *ctrl_pt)
{
	SheetControlGUI *scg = pane->simple.scg;
	double const *coords;
	double pts[4];
	char *msg;
	SheetObjectAnchor anchor;

	if (pane->size_tip == NULL) {
		GtkWidget *cw = GTK_WIDGET (pane);
		GtkWidget *top;
		int x, y;

		if (ctrl_pt == NULL) {
			/*
			 * Keyboard navigation when we are not displaying
			 * a tooltip already.
			 */
			return;
		}

		pane->size_tip = gnumeric_create_tooltip (cw);
		top = gtk_widget_get_toplevel (pane->size_tip);

		gnm_canvas_get_screen_position (ctrl_pt->canvas,
						ctrl_pt->x1, ctrl_pt->y1,
						&x, &y);
		gtk_window_move (GTK_WINDOW (top), x + 10, y + 10);
		gtk_widget_show_all (top);
	}

	g_return_if_fail (pane->cur_object != NULL);
	g_return_if_fail (pane->size_tip != NULL);

	coords = g_hash_table_lookup (scg->selected_objects, pane->cur_object);
	anchor = *sheet_object_get_anchor (pane->cur_object);
	scg_object_coords_to_anchor (scg, coords, &anchor);
	sheet_object_anchor_to_pts (&anchor, scg_sheet (scg), pts);
	msg = g_strdup_printf (_("%.1f x %.1f pts\n%d x %d pixels"),
		MAX (fabs (pts[2] - pts[0]), 0),
		MAX (fabs (pts[3] - pts[1]), 0),
		MAX ((int)floor (fabs (coords [2] - coords [0]) + 0.5), 0),
		MAX ((int)floor (fabs (coords [3] - coords [1]) + 0.5), 0));
	gtk_label_set_text (GTK_LABEL (pane->size_tip), msg);
	g_free (msg);
}

void
gnm_pane_bound_set (GnmPane *pane,
		    int start_col, int start_row,
		    int end_col, int end_row)
{
	GnmRange r;

	g_return_if_fail (pane != NULL);

	range_init (&r, start_col, start_row, end_col, end_row);
	goc_item_set (GOC_ITEM (pane->grid),
			     "bound", &r,
			     NULL);
}

/****************************************************************************/

void
gnm_pane_size_guide_start (GnmPane *pane, gboolean vert, int colrow, int width)
{
	SheetControlGUI const *scg;
	double x0, y0, x1, y1;
	double zoom;
	GOStyle *style;

	g_return_if_fail (pane != NULL);
	g_return_if_fail (pane->size_guide.guide  == NULL);
	g_return_if_fail (pane->size_guide.start  == NULL);
	g_return_if_fail (pane->size_guide.points == NULL);

	zoom = GOC_CANVAS (pane)->pixels_per_unit;
	scg = pane->simple.scg;

	if (vert) {
		double x = (scg_colrow_distance_get (scg, TRUE,
					0, colrow) - .5) / zoom;
		x0 = x;
		y0 = scg_colrow_distance_get (scg, FALSE,
					0, pane->first.row) / zoom;
		x1= x;
		y1 = scg_colrow_distance_get (scg, FALSE,
					0, pane->last_visible.row+1) / zoom;
	} else {
		double const y = (scg_colrow_distance_get (scg, FALSE,
					0, colrow) - .5) / zoom;
		x0 = scg_colrow_distance_get (scg, TRUE,
					0, pane->first.col) / zoom;
		y0 = y;
		x1 = scg_colrow_distance_get (scg, TRUE,
					0, pane->last_visible.col+1) / zoom;
		y1 = y;
	}

	/* Guideline positioning is done in gnm_pane_size_guide_motion */
	pane->size_guide.guide = goc_item_new (pane->action_items,
		GOC_TYPE_LINE,
		"x0", x0, "y0", y0,
		"x1", x1, "y1", y1,
		NULL);
	style = go_styled_object_get_style (GO_STYLED_OBJECT (pane->size_guide.guide));
	style->line.width = width;

	/* cheat for now and differentiate between col/row resize and frozen panes
	 * using the width.  Frozen pane guides do not require a start line */
	if (width == 1) {
		style->line.color = GO_COLOR_BLACK;
		pane->size_guide.start = goc_item_new (pane->action_items,
			GOC_TYPE_LINE,
			"x0", x0, "y0", y0,
			"x1", x1, "y1", y1,
			NULL);
		style = go_styled_object_get_style (GO_STYLED_OBJECT (pane->size_guide.start));
		style->line.color = GO_COLOR_BLACK;
		style->line.width = width;
	} else {
		style->line.pattern = GO_PATTERN_GREY25;
		style->line.color = GO_COLOR_WHITE;
		style->line.fore = GO_COLOR_BLACK;
	}
}

void
gnm_pane_size_guide_stop (GnmPane *pane)
{
	g_return_if_fail (pane != NULL);

	if (pane->size_guide.start != NULL) {
		g_object_unref (G_OBJECT (pane->size_guide.start));
		pane->size_guide.start = NULL;
	}
	if (pane->size_guide.guide != NULL) {
		g_object_unref (G_OBJECT (pane->size_guide.guide));
		pane->size_guide.guide = NULL;
	}
}

/**
 * gnm_pane_size_guide_motion
 * @pane : #GnmPane
 * @vert : TRUE for a vertical guide, FALSE for horizontal
 * @guide_pos : in unscaled sheet pixel coords
 *
 * Moves the guide line to @guide_pos.
 * NOTE : gnm_pane_size_guide_start must be called before any calls to
 *	gnm_pane_size_guide_motion
 **/
void
gnm_pane_size_guide_motion (GnmPane *pane, gboolean vert, gint64 guide_pos)
{
	GocItem *resize_guide = GOC_ITEM (pane->size_guide.guide);
	double const	 scale	    = 1. / resize_guide->canvas->pixels_per_unit;
	double x;

	x = scale * (guide_pos - .5);
	if (vert)
		goc_item_set (resize_guide, "x0", x, "x1", x, NULL);
	else
		goc_item_set (resize_guide, "y0", x, "y1", x, NULL);
}

/****************************************************************************/

static void
cb_update_ctrl_pts (SheetObject *so, GocItem **ctrl_pts, GnmPane *pane)
{
	double *coords = g_hash_table_lookup (
		pane->simple.scg->selected_objects, so);
	scg_object_anchor_to_coords (pane->simple.scg, sheet_object_get_anchor (so), coords);
	gnm_pane_object_update_bbox (pane, so);
}

/* Called when the zoom changes */
void
gnm_pane_reposition_cursors (GnmPane *pane)
{
	GSList *l;

	item_cursor_reposition (pane->cursor.std);
	if (NULL != pane->cursor.rangesel)
		item_cursor_reposition (pane->cursor.rangesel);
	if (NULL != pane->cursor.special)
		item_cursor_reposition (pane->cursor.special);
	for (l = pane->cursor.expr_range; l; l = l->next)
		item_cursor_reposition (ITEM_CURSOR (l->data));
	for (l = pane->cursor.animated; l; l = l->next)
		item_cursor_reposition (ITEM_CURSOR (l->data));

	/* ctrl pts do not scale with the zoom, compensate */
	if (pane->drag.ctrl_pts != NULL)
		g_hash_table_foreach (pane->drag.ctrl_pts,
			(GHFunc) cb_update_ctrl_pts, pane);
}

gboolean
gnm_pane_cursor_bound_set (GnmPane *pane, GnmRange const *r)
{
	return item_cursor_bound_set (pane->cursor.std, r);
}

/****************************************************************************/

gboolean
gnm_pane_rangesel_bound_set (GnmPane *pane, GnmRange const *r)
{
	return item_cursor_bound_set (pane->cursor.rangesel, r);
}
void
gnm_pane_rangesel_start (GnmPane *pane, GnmRange const *r)
{
	GocItem *item;
	SheetControlGUI *scg = pane->simple.scg;
	GnmExprEntry *gee = wbcg_get_entry_logical (pane->simple.scg->wbcg);

	g_return_if_fail (pane->cursor.rangesel == NULL);

	/* Hide the primary cursor while the range selection cursor is visible
	 * and we are selecting on a different sheet than the expr being edited */
	if (scg_sheet (scg) != wb_control_cur_sheet (scg_wbc (scg)))
		item_cursor_set_visibility (pane->cursor.std, FALSE);
	if (NULL != gee)
		gnm_expr_entry_disable_highlight (gee);
	item = goc_item_new (pane->grid_items,
		item_cursor_get_type (),
		"SheetControlGUI", scg,
		"style",	ITEM_CURSOR_ANTED,
		NULL);
	pane->cursor.rangesel = ITEM_CURSOR (item);
	item_cursor_bound_set (pane->cursor.rangesel, r);
}

void
gnm_pane_rangesel_stop (GnmPane *pane)
{
	GnmExprEntry *gee = wbcg_get_entry_logical (pane->simple.scg->wbcg);
	if (NULL != gee)
		gnm_expr_entry_enable_highlight (gee);

	g_return_if_fail (pane->cursor.rangesel != NULL);
	g_object_unref (G_OBJECT (pane->cursor.rangesel));
	pane->cursor.rangesel = NULL;

	/* Make the primary cursor visible again */
	item_cursor_set_visibility (pane->cursor.std, TRUE);

	gnm_pane_slide_stop (pane);
}

/****************************************************************************/

gboolean
gnm_pane_special_cursor_bound_set (GnmPane *pane, GnmRange const *r)
{
	return item_cursor_bound_set (pane->cursor.special, r);
}

void
gnm_pane_special_cursor_start (GnmPane *pane, int style, int button)
{
	GocItem *item;
	GocCanvas *canvas = GOC_CANVAS (pane);

	g_return_if_fail (pane->cursor.special == NULL);
	item = goc_item_new (
		GOC_GROUP (canvas->root),
		item_cursor_get_type (),
		"SheetControlGUI", pane->simple.scg,
		"style",	   style,
		"button",	   button,
		NULL);
	pane->cursor.special = ITEM_CURSOR (item);
}

void
gnm_pane_special_cursor_stop (GnmPane *pane)
{
	g_return_if_fail (pane->cursor.special != NULL);

	g_object_unref (G_OBJECT (pane->cursor.special));
	pane->cursor.special = NULL;
}

void
gnm_pane_mouse_cursor_set (GnmPane *pane, GdkCursor *c)
{
	gdk_cursor_ref (c);
	if (pane->mouse_cursor)
		gdk_cursor_unref (pane->mouse_cursor);
	pane->mouse_cursor = c;
}

/****************************************************************************/


void
gnm_pane_expr_cursor_bound_set (GnmPane *pane, GnmRange const *r,
				gboolean main_color)
{
	gchar const *colours[5]
		= {"green","yellow", "orange", "red", "purple"};
	gint i;
	ItemCursor *cursor;

	i = g_slist_length (pane->cursor.expr_range) % 5;

	cursor = (ItemCursor *) goc_item_new
		(GOC_GROUP (GOC_CANVAS (pane)->root),
		 item_cursor_get_type (),
		 "SheetControlGUI",	pane->simple.scg,
		 "style",		ITEM_CURSOR_EXPR_RANGE,
		 "color",		main_color ? "blue" : colours[i],
		 NULL);

	item_cursor_bound_set (cursor, r);
	pane->cursor.expr_range = g_slist_prepend
		(pane->cursor.expr_range, cursor);
}

void
gnm_pane_expr_cursor_stop (GnmPane *pane)
{
	go_slist_free_custom (pane->cursor.expr_range, g_object_unref);
	pane->cursor.expr_range = NULL;
}

/****************************************************************************/

void
gnm_pane_edit_start (GnmPane *pane)
{
	GocCanvas *canvas = GOC_CANVAS (pane);

	g_return_if_fail (pane->editor == NULL);

	/* edit item handles visibility checks */
	pane->editor = (ItemEdit *) goc_item_new (
		GOC_GROUP (canvas->root),
		item_edit_get_type (),
		"SheetControlGUI",	pane->simple.scg,
		NULL);
}

void
gnm_pane_edit_stop (GnmPane *pane)
{
	if (pane->editor != NULL) {
		g_object_unref (G_OBJECT (pane->editor));
		pane->editor = NULL;
	}
}

void
gnm_pane_objects_drag (GnmPane *pane, SheetObject *so,
		       gdouble new_x, gdouble new_y, int drag_type,
		       gboolean symmetric,gboolean snap_to_grid)
{
	double dx, dy;
	dx = new_x - pane->drag.last_x;
	dy = new_y - pane->drag.last_y;
	pane->drag.had_motion = TRUE;
	scg_objects_drag (pane->simple.scg, pane,
		so, &dx, &dy, drag_type, symmetric, snap_to_grid, TRUE);

	pane->drag.last_x += dx;
	pane->drag.last_y += dy;
}

#define CTRL_PT_SIZE		4
#define CTRL_PT_OUTLINE		0
/* space for 2 halves and a full */
#define CTRL_PT_TOTAL_SIZE	(CTRL_PT_SIZE*4 + CTRL_PT_OUTLINE*2)

/* new_x and new_y are in world coords */
static void
gnm_pane_object_move (GnmPane *pane, GObject *ctrl_pt,
		      gdouble new_x, gdouble new_y,
		      gboolean symmetric,
		      gboolean snap_to_grid)
{
	int const idx = GPOINTER_TO_INT (g_object_get_data (ctrl_pt, "index"));
	pane->cur_object  = g_object_get_data (G_OBJECT (ctrl_pt), "so");

	gnm_pane_objects_drag (pane, pane->cur_object, new_x, new_y, idx,
			       symmetric, snap_to_grid);
	if (idx != 8)
		gnm_pane_display_obj_size_tip (pane, GOC_ITEM (ctrl_pt));
}

static gboolean
cb_slide_handler (GnmPane *pane, GnmPaneSlideInfo const *info)
{
	guint64 x, y;
	SheetControlGUI const *scg = pane->simple.scg;
	double const scale = 1. / GOC_CANVAS (pane)->pixels_per_unit;

	x = scg_colrow_distance_get (scg, TRUE, pane->first.col, info->col);
	x += pane->first_offset.x;
	y = scg_colrow_distance_get (scg, FALSE, pane->first.row, info->row);
	y += pane->first_offset.y;

	gnm_pane_object_move (pane, info->user_data,
		x * scale, y * scale, FALSE, FALSE);

	return TRUE;
}

static void
cb_so_menu_activate (GObject *menu, GocItem *view)
{
	SheetObjectAction const *a = g_object_get_data (menu, "action");
	if (a->func)
		(a->func) (sheet_object_view_get_so (SHEET_OBJECT_VIEW (view)),
			   SHEET_CONTROL (GNM_SIMPLE_CANVAS (view->canvas)->scg));
}

static GtkWidget *
build_so_menu (GnmPane *pane, SheetObjectView *view,
	       GPtrArray const *actions, unsigned *i)
{
	SheetObjectAction const *a;
	GtkWidget *item, *menu = gtk_menu_new ();

	while (*i < actions->len) {
		a = g_ptr_array_index (actions, *i);
		(*i)++;
		if (a->submenu < 0)
			break;
		if (a->icon != NULL) {
			if (a->label != NULL) {
				item = gtk_image_menu_item_new_with_mnemonic (_(a->label));
				gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item),
					gtk_image_new_from_stock (a->icon, GTK_ICON_SIZE_MENU));
			} else
				item = gtk_image_menu_item_new_from_stock (a->icon, NULL);
		} else if (a->label != NULL)
			item = gtk_menu_item_new_with_mnemonic (_(a->label));
		else
			item = gtk_separator_menu_item_new ();
		if (a->submenu > 0)
			gtk_menu_item_set_submenu (GTK_MENU_ITEM (item),
				build_so_menu (pane, view, actions, i));
		else if (a->label != NULL || a->icon != NULL) { /* not a separator or menu */
			g_object_set_data (G_OBJECT (item), "action", (gpointer)a);
			g_signal_connect_object (G_OBJECT (item), "activate",
				G_CALLBACK (cb_so_menu_activate), view, 0);
		}
		gtk_menu_shell_append (GTK_MENU_SHELL (menu),  item);
	}
	return menu;
}

static void
cb_ptr_array_free (GPtrArray *actions)
{
	g_ptr_array_free (actions, TRUE);
}

/* event and so can be NULL */
void
gnm_pane_display_object_menu (GnmPane *pane, SheetObject *so, GdkEvent *event)
{
	SheetControlGUI *scg = pane->simple.scg;
	GPtrArray *actions = g_ptr_array_new ();
	GtkWidget *menu;
	unsigned i = 0;

	if (NULL != so && (!scg->selected_objects ||
	    NULL == g_hash_table_lookup (scg->selected_objects, so)))
		scg_object_select (scg, so);

	sheet_object_populate_menu (so, actions);

	if (actions->len == 0) {
		g_ptr_array_free (actions, TRUE);
		return;
	}

	menu = build_so_menu (pane,
		sheet_object_get_view (so, (SheetObjectViewContainer *) pane),
		actions, &i);
	g_object_set_data_full (G_OBJECT (menu), "actions", actions,
		(GDestroyNotify)cb_ptr_array_free);
	gtk_widget_show_all (menu);
	gnumeric_popup_menu (GTK_MENU (menu), &event->button);
}

static void
cb_collect_selected_objs (SheetObject *so, double *coords, GSList **accum)
{
	*accum = g_slist_prepend (*accum, so);
}

static void
cb_pane_popup_menu (GnmPane *pane)
{
	SheetControlGUI *scg = pane->simple.scg;

	/* ignore new_object, it is not visible, and should not create a
	 * context menu */
	if (NULL != scg->selected_objects) {
		GSList *accum = NULL;
		g_hash_table_foreach (scg->selected_objects,
			(GHFunc) cb_collect_selected_objs, &accum);
		if (NULL != accum && NULL == accum->next)
			gnm_pane_display_object_menu (pane, accum->data, NULL);
		g_slist_free (accum);
	} else {
		/* the popup-menu signal is a binding. the grid almost always
		 * has focus we need to cheat to find out if the user
		 * realllllly wants a col/row header menu */
		gboolean is_col = FALSE;
		gboolean is_row = FALSE;
		GdkWindow *gdk_win = gdk_display_get_window_at_pointer (
			gtk_widget_get_display (GTK_WIDGET (pane)),
			NULL, NULL);

		if (gdk_win != NULL) {
			gpointer gtk_win_void = NULL;
			GtkWindow *gtk_win = NULL;
			gdk_window_get_user_data (gdk_win, &gtk_win_void);
			gtk_win = gtk_win_void;
			if (gtk_win != NULL) {
				if (gtk_win == (GtkWindow *)pane->col.canvas)
					is_col = TRUE;
				else if (gtk_win == (GtkWindow *)pane->row.canvas)
					is_row = TRUE;
			}
		}

		scg_context_menu (scg, NULL, is_col, is_row);
	}
}

static void
control_point_set_cursor (SheetControlGUI const *scg, GocItem *ctrl_pt)
{
	SheetObject *so = g_object_get_data (G_OBJECT (ctrl_pt), "so");
	int idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (ctrl_pt), "index"));
	double const *coords = g_hash_table_lookup (scg->selected_objects, so);
	gboolean invert_h = coords [0] > coords [2];
	gboolean invert_v = coords [1] > coords [3];
	GdkCursorType cursor;

	if (goc_canvas_get_direction (ctrl_pt->canvas) == GOC_DIRECTION_RTL)
		invert_h = !invert_h;

	switch (idx) {
	case 1: invert_v = !invert_v;
		/* fallthrough */
	case 6: cursor = invert_v ? GDK_TOP_SIDE : GDK_BOTTOM_SIDE;
		break;

	case 3: invert_h = !invert_h;
		/* fallthrough */
	case 4: cursor = invert_h ? GDK_LEFT_SIDE  : GDK_RIGHT_SIDE;
		break;

	case 2: invert_h = !invert_h;
		/* fallthrough */
	case 0: cursor = invert_v
			? (invert_h ? GDK_BOTTOM_RIGHT_CORNER : GDK_BOTTOM_LEFT_CORNER)
			: (invert_h ? GDK_TOP_RIGHT_CORNER : GDK_TOP_LEFT_CORNER);
		break;

	case 7: invert_h = !invert_h;
		/* fallthrough */
	case 5: cursor = invert_v
			? (invert_h ? GDK_TOP_RIGHT_CORNER : GDK_TOP_LEFT_CORNER)
			: (invert_h ? GDK_BOTTOM_RIGHT_CORNER : GDK_BOTTOM_LEFT_CORNER);
		break;

	case 8:
	default :
		cursor = GDK_FLEUR;
	}
	gnm_widget_set_cursor_type (GTK_WIDGET (ctrl_pt->canvas), cursor);
}

static void
target_list_add_list (GtkTargetList *targets, GtkTargetList *added_targets)
{
	GList *ptr;

	g_return_if_fail (targets != NULL);

	if (added_targets == NULL)
		return;

	for (ptr = added_targets->list; ptr !=  NULL; ptr = ptr->next) {
		GtkTargetPair *tp = ptr->data;
		gtk_target_list_add (targets, tp->target, tp->flags, tp->info);
	}
}

/**
 * Drag one or more sheet objects using GTK drag and drop, to the same
 * sheet, another workbook, another gnumeric or a different application.
 */
static void
gnm_pane_drag_begin (GnmPane *pane, SheetObject *so, GdkEvent *event)
{
	GdkDragContext *context;
	GtkTargetList *targets, *im_targets;
	GocCanvas *canvas    = GOC_CANVAS (pane);
	SheetControlGUI *scg = pane->simple.scg;
	GSList *objects;
	SheetObject *imageable = NULL, *exportable = NULL;
	GSList *ptr;
	SheetObject *candidate;

	targets = gtk_target_list_new (drag_types_out,
				  G_N_ELEMENTS (drag_types_out));
	objects = go_hash_keys (scg->selected_objects);
	for (ptr = objects; ptr != NULL; ptr = ptr->next) {
		candidate = SHEET_OBJECT (ptr->data);

		if (exportable == NULL &&
		    IS_SHEET_OBJECT_EXPORTABLE (candidate))
			exportable = candidate;
		if (imageable == NULL &&
		    IS_SHEET_OBJECT_IMAGEABLE (candidate))
			imageable = candidate;
	}

	if (exportable) {
		im_targets = sheet_object_exportable_get_target_list (exportable);
		if (im_targets != NULL) {
			target_list_add_list (targets, im_targets);
			gtk_target_list_unref (im_targets);
		}
	}
	if (imageable) {
		im_targets = sheet_object_get_target_list (imageable);
		if (im_targets != NULL) {
			target_list_add_list (targets, im_targets);
			gtk_target_list_unref (im_targets);
		}
	}


	if (gnm_debug_flag ("dnd")) {
		GList *l;
		g_printerr ("%d offered formats:\n", g_list_length (targets->list));
		for (l = targets->list; l; l = l->next) {
			GtkTargetPair *pair = l->data;
			char *target_name = gdk_atom_name (pair->target);
			g_printerr ("%s\n", target_name);
			g_free (target_name);
		}
	}

	context = gtk_drag_begin (GTK_WIDGET (canvas), targets,
				  GDK_ACTION_COPY | GDK_ACTION_MOVE,
				  pane->drag.button, event);
	gtk_target_list_unref (targets);
	g_slist_free (objects);
}

void
gnm_pane_object_start_resize (GnmPane *pane, int button, guint64 x, gint64 y,
			      SheetObject *so, int drag_type, gboolean is_creation)
{
	GocItem **ctrl_pts;
	GdkEventButton *event;

	g_return_if_fail (IS_SHEET_OBJECT (so));
	g_return_if_fail (0 <= drag_type);
	g_return_if_fail (drag_type < 9);

	event = (GdkEventButton *) goc_canvas_get_cur_event (GOC_CANVAS (pane));
	ctrl_pts = g_hash_table_lookup (pane->drag.ctrl_pts, so);

	g_return_if_fail (NULL != ctrl_pts);

	gnm_simple_canvas_grab (ctrl_pts [drag_type],
		GDK_POINTER_MOTION_MASK |
		GDK_BUTTON_PRESS_MASK |
		GDK_BUTTON_RELEASE_MASK,
		NULL, event->time);
	pane->drag.created_objects = is_creation;
	pane->drag.button = button;
	pane->drag.last_x = pane->drag.origin_x = x;
	pane->drag.last_y = pane->drag.origin_y = y;
	pane->drag.had_motion = FALSE;
	gnm_pane_slide_init (pane);
	gnm_widget_set_cursor_type (GTK_WIDGET (pane), GDK_HAND2);
}

/*
 ControlCircleItem
 */
typedef GocCircle ControlCircle;
typedef GocCircleClass ControlCircleClass;

#define CONTROL_TYPE_CIRCLE	(control_circle_get_type ())
#define CONTROL_CIRCLE(o)	(G_TYPE_CHECK_INSTANCE_CAST ((o), CONTROL_TYPE_CIRCLE, ControlCircle))
#define CONTROL_IS_CIRCLE(o)	(G_TYPE_CHECK_INSTANCE_TYPE ((o), CONTROL_TYPE_CIRCLE))

GType control_circle_get_type (void);

static gboolean
control_point_button_pressed (GocItem *item, int button, double x, double y)
{
	GnmPane *pane = GNM_PANE (item->canvas);
	GdkEventButton *event = (GdkEventButton *) goc_canvas_get_cur_event (item->canvas);
	SheetObject *so;
	int idx;

	if (0 != pane->drag.button)
		return TRUE;

	x *= goc_canvas_get_pixels_per_unit (item->canvas);
	y *= goc_canvas_get_pixels_per_unit (item->canvas);
	so  = g_object_get_data (G_OBJECT (item), "so");
	idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (item), "index"));
	switch (event->button) {
	case 1:
	case 2: gnm_pane_object_start_resize (pane, button, x, y, so,  idx, FALSE);
		break;
	case 3: gnm_pane_display_object_menu (pane, so, (GdkEvent *) event);
		break;
	default: /* Ignore mouse wheel events */
		return FALSE;
	}
	return TRUE;
}

static gboolean
control_point_button_released (GocItem *item, int button, G_GNUC_UNUSED double x, G_GNUC_UNUSED double y)
{
	GnmPane *pane = GNM_PANE (item->canvas);
	GdkEventButton *event = (GdkEventButton *) goc_canvas_get_cur_event (item->canvas);
	SheetControlGUI *scg = pane->simple.scg;
	SheetObject *so;
	int idx;

	if (pane->drag.button != button)
		return TRUE;
	so  = g_object_get_data (G_OBJECT (item), "so");
	idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (item), "index"));
	pane->drag.button = 0;
	gnm_simple_canvas_ungrab (item, event->time);
	gnm_pane_slide_stop (pane);
	control_point_set_cursor (scg, item);
	if (idx == 8)
		; /* ignore fake event generated by the dnd code */
	else if (pane->drag.had_motion)
		scg_objects_drag_commit	(scg, idx,
					 pane->drag.created_objects,
					 NULL, NULL, NULL);
	else if (pane->drag.created_objects && idx == 7) {
		double w, h;
		sheet_object_default_size (so, &w, &h);
		scg_objects_drag (scg, NULL, NULL, &w, &h, 7, FALSE, FALSE, FALSE);
		scg_objects_drag_commit	(scg, 7, TRUE,
					 NULL, NULL, NULL);
	}
	gnm_pane_clear_obj_size_tip (pane);
	return TRUE;
}

static gboolean
control_point_motion (GocItem *item, double x, double y)
{
	GnmPane *pane = GNM_PANE (item->canvas);
	GdkEventMotion *event = (GdkEventMotion *) goc_canvas_get_cur_event (item->canvas);
	SheetObject *so;
	int idx;

	if (0 == pane->drag.button)
		return TRUE;

	x *= goc_canvas_get_pixels_per_unit (item->canvas);
	y *= goc_canvas_get_pixels_per_unit (item->canvas);
	/* TODO : motion is still too granular along the internal axis
	 * when the other axis is external.
	 * eg  drag from middle of sheet down.  The x axis is still internal
	 * onlt the y is external, however, since we are autoscrolling
	 * we are limited to moving with col/row coords, not x,y.
	 * Possible solution would be to change the EXTERIOR_ONLY flag
	 * to something like USE_PIXELS_INSTEAD_OF_COLROW and change
	 * the semantics of the col,row args in the callback.  However,
	 * that is more work than I want to do right now.
	 */
	so  = g_object_get_data (G_OBJECT (item), "so");
	idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (item), "index"));
	if (idx == 8)
		gnm_pane_drag_begin (pane, so, (GdkEvent *) event);
	else if (gnm_pane_handle_motion (pane,
					   item->canvas, x, y,
					   GNM_PANE_SLIDE_X | GNM_PANE_SLIDE_Y |
					   GNM_PANE_SLIDE_EXTERIOR_ONLY,
					   cb_slide_handler, item))
		gnm_pane_object_move (pane, G_OBJECT (item),
				      x, y,
				      (event->state & GDK_CONTROL_MASK) != 0,
				      (event->state & GDK_SHIFT_MASK) != 0);
	return TRUE;
}

static gboolean
control_point_button2_pressed (GocItem *item, int button, G_GNUC_UNUSED double x, G_GNUC_UNUSED double y)
{
	GnmPane *pane = GNM_PANE (item->canvas);
	SheetControlGUI *scg = pane->simple.scg;
	SheetObject *so;

	so  = g_object_get_data (G_OBJECT (item), "so");
	if (pane->drag.button == 1)
		sheet_object_get_editor (so, SHEET_CONTROL (scg));
	return TRUE;
}

static gboolean
control_point_enter_notify (GocItem *item, G_GNUC_UNUSED double x, G_GNUC_UNUSED double y)
{
	GnmPane *pane = GNM_PANE (item->canvas);
	SheetControlGUI *scg = pane->simple.scg;
	int idx;

	control_point_set_cursor (scg, item);

	pane->cur_object  = g_object_get_data (G_OBJECT (item), "so");
	idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (item), "index"));
	if (idx != 8) {
		GOStyle *style = go_styled_object_get_style (GO_STYLED_OBJECT (item));
		style->fill.pattern.back = GO_COLOR_GREEN;
		goc_item_invalidate (item);
		gnm_pane_display_obj_size_tip (pane, item);
	}
	return TRUE;
}

static gboolean
control_point_leave_notify (GocItem *item, G_GNUC_UNUSED double x, G_GNUC_UNUSED double y)
{
	GnmPane *pane = GNM_PANE (item->canvas);
	SheetControlGUI *scg = pane->simple.scg;
	int idx;
	SheetObject *so;

	control_point_set_cursor (scg, item);

	so  = g_object_get_data (G_OBJECT (item), "so");
	idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (item), "index"));
	if (idx != 8) {
		GOStyle *style = go_styled_object_get_style (GO_STYLED_OBJECT (item));
		style->fill.pattern.back = GO_COLOR_WHITE;
		goc_item_invalidate (item);
		gnm_pane_clear_obj_size_tip (pane);
	}
	pane->cur_object = NULL;
	return TRUE;
}

static void control_circle_class_init (GocItemClass *item_klass) {
	item_klass->button_pressed = control_point_button_pressed;
	item_klass->button_released = control_point_button_released;
	item_klass->motion = control_point_motion;
	item_klass->button2_pressed = control_point_button2_pressed;
	item_klass->enter_notify = control_point_enter_notify;
	item_klass->leave_notify = control_point_leave_notify;
}

GSF_CLASS (ControlCircle, control_circle,
	   control_circle_class_init, NULL,
	   GOC_TYPE_CIRCLE)

#define ITEM_ACETATE(obj)          (G_TYPE_CHECK_INSTANCE_CAST((obj), item_acetate_get_type (), ItemAcetate))
#define IS_ITEM_ACETATE(o)         (G_TYPE_CHECK_INSTANCE_TYPE((o), item_acetate_get_type ()))

#define MARGIN	10

GType item_acetate_get_type (void);

typedef GocRectangle		ItemAcetate;
typedef GocRectangleClass	ItemAcetateClass;

static double
item_acetate_distance (GocItem *item, double x, double y,
		    GocItem **actual_item)
{
	if (x < (item->x0 - MARGIN) ||
	    x > (item->x1 + MARGIN) ||
	    y < (item->y0 - MARGIN) ||
		y > (item->y1 + MARGIN))
		return DBL_MAX;
	*actual_item = item;
	return 0.;
}

static void
item_acetate_class_init (GocItemClass *item_class)
{
	item_class->distance = item_acetate_distance;
	item_class->button_pressed = control_point_button_pressed;
	item_class->button_released = control_point_button_released;
	item_class->motion = control_point_motion;
	item_class->button2_pressed = control_point_button2_pressed;
	item_class->enter_notify = control_point_enter_notify;
	item_class->leave_notify = control_point_leave_notify;
}

GSF_CLASS (ItemAcetate, item_acetate,
	   item_acetate_class_init, NULL,
	   GOC_TYPE_RECTANGLE)

/**
 * new_control_point
 * @pane: #GnmPane
 * @idx:    control point index to be created
 * @x:      x coordinate of control point
 * @y:      y coordinate of control point
 *
 * This is used to create a number of control points in a sheet
 * object, the meaning of them is used in other parts of the code
 * to belong to the following locations:
 *
 *     0 -------- 1 -------- 2
 *     |                     |
 *     3                     4
 *     |                     |
 *     5 -------- 6 -------- 7
 *
 *     8 == a clear overlay that extends slightly beyond the region
 *     9 == an optional stippled rectangle for moving/resizing expensive
 *         objects
 **/
static GocItem *
new_control_point (GnmPane *pane, SheetObject *so, int idx, double x, double y, double radius)
{
	GOStyle *style = go_style_new ();
	GocItem *item;

	style->line.width = CTRL_PT_OUTLINE;
	item = goc_item_new (
		pane->action_items,
		CONTROL_TYPE_CIRCLE,
		"x", x,
		"y", y,
		"radius", radius,
		"style", style,
		NULL);

	g_object_unref (style);
	g_object_set_data (G_OBJECT (item), "index",  GINT_TO_POINTER (idx));
	g_object_set_data (G_OBJECT (item), "so",  so);

	return item;
}

/**
 * set_item_x_y:
 * Changes the x and y position of the idx-th control point,
 * creating the control point if necessary.
 **/
static void
set_item_x_y (GnmPane *pane, SheetObject *so, GocItem **ctrl_pts,
	      int idx, double x, double y, gboolean visible)
{
	double scale = GOC_CANVAS (pane)->pixels_per_unit;
	if (ctrl_pts [idx] == NULL)
		ctrl_pts [idx] = new_control_point (pane, so, idx, x / scale, y / scale, CTRL_PT_SIZE / scale);
	else
		goc_item_set (ctrl_pts [idx], "x", x / scale, "y", y / scale,
		              "radius", CTRL_PT_SIZE / scale, NULL);
	if (visible)
		goc_item_show (ctrl_pts [idx]);
	else
		goc_item_hide (ctrl_pts [idx]);
}

#define normalize_high_low(d1,d2) if (d1<d2) { double tmp=d1; d1=d2; d2=tmp;}

static void
set_acetate_coords (GnmPane *pane, SheetObject *so, GocItem **ctrl_pts,
		    double l, double t, double r, double b)
{
	double scale = goc_canvas_get_pixels_per_unit (GOC_CANVAS (pane));
	if (!sheet_object_rubber_band_directly (so)) {
		if (NULL == ctrl_pts [9]) {
			GOStyle *style = go_style_new ();
			style->fill.auto_type = FALSE;
			style->fill.type  = GO_STYLE_FILL_PATTERN;
			style->fill.auto_back = FALSE;
			style->fill.pattern.back = 0;
			style->fill.auto_fore = FALSE;
			style->fill.pattern.fore = 0;
			style->line.pattern = GO_PATTERN_THIN_DIAG;
			style->line.width = 0.;
			style->line.auto_color = FALSE;
			style->line.color = 0;
			style->line.fore = GO_COLOR_BLACK;
			ctrl_pts [9] = goc_item_new (pane->action_items,
				GOC_TYPE_RECTANGLE,
				"style", style,
				NULL);
			g_object_unref (style);
			goc_item_lower_to_bottom (ctrl_pts [9]);
		}
		normalize_high_low (r, l);
		normalize_high_low (b, t);
		goc_item_set (ctrl_pts [9],
		       "x", l / scale, "y", t / scale,
		       "width", (r - l) / scale, "height", (b - t) / scale,
		       NULL);
	} else {
		double coords[4];
		SheetObjectView *sov = sheet_object_get_view (so, (SheetObjectViewContainer *)pane);
		if (NULL == sov)
			sov = sheet_object_new_view (so, (SheetObjectViewContainer *)pane);

		coords [0] = l; coords [2] = r; coords [1] = t; coords [3] = b;
		if (NULL != sov)
			sheet_object_view_set_bounds (sov, coords, TRUE);
		normalize_high_low (r, l);
		normalize_high_low (b, t);
	}

	l -= (CTRL_PT_SIZE + CTRL_PT_OUTLINE) / 2 - 1;
	r += (CTRL_PT_SIZE + CTRL_PT_OUTLINE) / 2;
	t -= (CTRL_PT_SIZE + CTRL_PT_OUTLINE) / 2 - 1;
	b += (CTRL_PT_SIZE + CTRL_PT_OUTLINE) / 2;

	if (NULL == ctrl_pts [8]) {
		GOStyle *style = go_style_new ();
		GocItem *item;

		style->fill.auto_type = FALSE;
		style->fill.type  = GO_STYLE_FILL_PATTERN;
		style->fill.auto_back = FALSE;
		go_pattern_set_solid (&style->fill.pattern, 0);
		style->line.auto_dash = FALSE;
		style->line.dash_type = GO_LINE_NONE;
		/* work around the screwup in shapes that adds a large
		 * border to anything that uses miter (is this required for
		 * a rectangle in goc-canvas? */
		style->line.join = CAIRO_LINE_JOIN_ROUND;
		item = goc_item_new (
			pane->action_items,
			item_acetate_get_type (),
			"style", style,
		        NULL);
		g_object_unref (style);
		g_object_set_data (G_OBJECT (item), "index",
			GINT_TO_POINTER (8));
		g_object_set_data (G_OBJECT (item), "so", so);

		ctrl_pts [8] = item;
	}
	goc_item_set (ctrl_pts [8],
	       "x", l / scale,
	       "y", t / scale,
	       "width", (r - l) / scale,
	       "height", (b - t) / scale,
	       NULL);
}

void
gnm_pane_object_unselect (GnmPane *pane, SheetObject *so)
{
	gnm_pane_clear_obj_size_tip (pane);
	g_hash_table_remove (pane->drag.ctrl_pts, so);
}

/**
 * gnm_pane_object_update_bbox :
 * @pane : #GnmPane
 * @so : #SheetObject
 *
 * Updates the position and potentially creates control points
 * for manipulating the size/position of @so.
 **/
void
gnm_pane_object_update_bbox (GnmPane *pane, SheetObject *so)
{
	GocItem **ctrl_pts = g_hash_table_lookup (pane->drag.ctrl_pts, so);
	double const *pts = g_hash_table_lookup (
		pane->simple.scg->selected_objects, so);

	if (ctrl_pts == NULL) {
		ctrl_pts = g_new0 (GocItem *, 10);
		g_hash_table_insert (pane->drag.ctrl_pts, so, ctrl_pts);
	}

	g_return_if_fail (ctrl_pts != NULL);

	/* set the acetate 1st so that the other points will override it */
	set_acetate_coords (pane, so, ctrl_pts, pts[0], pts[1], pts[2], pts[3]);
	set_item_x_y (pane, so, ctrl_pts, 0, pts[0], pts[1], TRUE);
	set_item_x_y (pane, so, ctrl_pts, 1, (pts[0] + pts[2]) / 2, pts[1],
		      fabs (pts[2]-pts[0]) >= CTRL_PT_TOTAL_SIZE);
	set_item_x_y (pane, so, ctrl_pts, 2, pts[2], pts[1], TRUE);
	set_item_x_y (pane, so, ctrl_pts, 3, pts[0], (pts[1] + pts[3]) / 2,
		      fabs (pts[3]-pts[1]) >= CTRL_PT_TOTAL_SIZE);
	set_item_x_y (pane, so, ctrl_pts, 4, pts[2], (pts[1] + pts[3]) / 2,
		      fabs (pts[3]-pts[1]) >= CTRL_PT_TOTAL_SIZE);
	set_item_x_y (pane, so, ctrl_pts, 5, pts[0], pts[3], TRUE);
	set_item_x_y (pane, so, ctrl_pts, 6, (pts[0] + pts[2]) / 2, pts[3],
		      fabs (pts[2]-pts[0]) >= CTRL_PT_TOTAL_SIZE);
	set_item_x_y (pane, so, ctrl_pts, 7, pts[2], pts[3], TRUE);
}

static void
cb_bounds_changed (SheetObject *so, GocItem *sov)
{
	double coords[4], *cur;
	SheetControlGUI *scg = GNM_SIMPLE_CANVAS (sov->canvas)->scg;
	if (GNM_PANE (sov->canvas)->drag.button != 0)
		return; /* do not reset bounds during drag */

	scg_object_anchor_to_coords (scg, sheet_object_get_anchor (so), coords);
	if (NULL != scg->selected_objects &&
	    NULL != (cur = g_hash_table_lookup (scg->selected_objects, so))) {
		int i;
		for (i = 4; i-- > 0 ;) cur[i] = coords[i];
		gnm_pane_object_update_bbox (GNM_PANE (sov->canvas), so);
	}

	sheet_object_view_set_bounds (SHEET_OBJECT_VIEW (sov),
		coords, so->flags & SHEET_OBJECT_IS_VISIBLE);
}

/**
 * gnm_pane_object_register :
 * @so : A sheet object
 * @view   : A canvas item acting as a view for @so
 * @selectable : Add handlers for selecting and editing the object
 *
 * Setup some standard callbacks for manipulating a view of a sheet object.
 **/
SheetObjectView *
gnm_pane_object_register (SheetObject *so, GocItem *view, gboolean selectable)
{
	g_signal_connect_object (so, "bounds-changed",
		G_CALLBACK (cb_bounds_changed), view, 0);
	return SHEET_OBJECT_VIEW (view);
}

/**
 * gnm_pane_object_widget_register :
 *
 * @so : A sheet object
 * @widget : The widget for the sheet object view
 * @view   : A canvas item acting as a view for @so
 *
 * Setup some standard callbacks for manipulating widgets as views of sheet
 * objects.
 **/
void
gnm_pane_widget_register (SheetObject *so, GtkWidget *w, GocItem *view)
{
	if (GTK_IS_CONTAINER (w)) {
		GList *ptr, *children = gtk_container_get_children (GTK_CONTAINER (w));
		for (ptr = children ; ptr != NULL; ptr = ptr->next)
			gnm_pane_widget_register (so, ptr->data, view);
		g_list_free (children);
	}
}

void
gnm_pane_set_direction (GnmPane *pane, GocDirection direction)
{
	goc_canvas_set_direction (GOC_CANVAS (pane), direction);
	if (pane->col.canvas != NULL)
		goc_canvas_set_direction (pane->col.canvas, direction);
}
