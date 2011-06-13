/* vim: set sw=8: -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * cell.c: Cell content and simple management.
 *
 * Author:
 *    Jody Goldberg 2000-2006 (jody@gnome.org)
 *    Miguel de Icaza 1998, 1999 (miguel@kernel.org)
 *    Copyright (C) 2000-2009 Morten Welinder (terra@gnome.org)
 */
#include <gnumeric-config.h>
#include "gnumeric.h"
#include "cell.h"

#include "gutils.h"
#include "workbook.h"
#include "sheet.h"
#include "expr.h"
#include "expr-impl.h"
#include "rendered-value.h"
#include "value.h"
#include "style.h"
#include "ranges.h"
#include "gnm-format.h"
#include "number-match.h"
#include "sheet-object-cell-comment.h"
#include "sheet-style.h"
#include "parse-util.h"

#include <goffice/goffice.h>

/**
 * gnm_cell_cleanout :
 *      Empty a cell's
 *		- value.
 *		- rendered_value.
 *		- expression.
 *		- parse format.
 *
 *      Clears the flags to
 *		- not queued for recalc.
 *		- has no expression.
 *
 *      Does NOT change
 *		- Comments.
 *		- Spans.
 *		- unqueue a previously queued recalc.
 *		- Mark sheet as dirty.
 */
void
gnm_cell_cleanout (GnmCell *cell)
{
	g_return_if_fail (cell != NULL);

	/* A cell can have either an expression or entered text */
	if (gnm_cell_has_expr (cell)) {
		/* Clipboard cells, e.g., are not attached to a sheet.  */
		if (gnm_cell_expr_is_linked (cell))
			dependent_unlink (GNM_CELL_TO_DEP (cell));
		gnm_expr_top_unref (cell->base.texpr);
		cell->base.texpr = NULL;
	}

	value_release (cell->value);
	cell->value = NULL;

	gnm_cell_unrender (cell);

	if (cell->row_info != NULL)
		cell->row_info->needs_respan = TRUE;
}

/****************************************************************************/

/*
 * gnm_cell_set_text : Parses the supplied text for storage as a value or
 *		expression.  It marks the sheet as dirty.
 *
 * If the text is an expression it IS queued for recalc.
 *        the format prefered by the expression is stored for later use.
 * If the text is a value it is rendered and spans are NOT calculated.
 *        the format that matched the text is stored for later use.
 *
 * WARNING : This is an internal routine that does not queue redraws,
 *           does not auto-resize, and does not calculate spans.
 *
 * NOTE : This DOES check for array partitioning.
 */
void
gnm_cell_set_text (GnmCell *cell, char const *text)
{
	GnmExprTop const *texpr;
	GnmValue      *val;
	GnmParsePos    pos;

	g_return_if_fail (cell != NULL);
	g_return_if_fail (text != NULL);
	g_return_if_fail (!gnm_cell_is_nonsingleton_array (cell));

	parse_text_value_or_expr (parse_pos_init_cell (&pos, cell),
		text, &val, &texpr);

	if (val != NULL) {	/* String was a value */
		gnm_cell_cleanout (cell);
		cell->value = val;
	} else {		/* String was an expression */
		gnm_cell_set_expr (cell, texpr);
		gnm_expr_top_unref (texpr);
	}
}

/*
 * gnm_cell_assign_value : Stores (WITHOUT COPYING) the supplied value.
 *    no changes are made to the expression or entered text.  This
 *    is for use by routines that wish to store values directly such
 *    as expression calculation or import for array formulas.
 *
 * WARNING : This is an internal routine that does not
 *	- queue redraws,
 *	- auto-resize
 *	- calculate spans
 *	- does not render.
 *	- mark anything as dirty.
 *
 * NOTE : This DOES NOT check for array partitioning.
 */
void
gnm_cell_assign_value (GnmCell *cell, GnmValue *v)
{
	g_return_if_fail (cell);
	g_return_if_fail (v);

	value_release (cell->value);
	cell->value = v;
}

/**
 * gnm_cell_set_value : Stores (WITHOUT COPYING) the supplied value.  It marks the
 *          sheet as dirty.
 *
 * WARNING : This is an internal routine that does not
 *	- queue redraws,
 *	- auto-resize
 *	- calculate spans
 *	- does not render.
 *
 * NOTE : This DOES check for array partitioning.
 **/
void
gnm_cell_set_value (GnmCell *cell, GnmValue *v)
{
	g_return_if_fail (cell != NULL);
	g_return_if_fail (v != NULL);
	if (gnm_cell_is_nonsingleton_array (cell)) {
		value_release (v);
		g_return_if_fail (!gnm_cell_is_nonsingleton_array (cell));
	}

	gnm_cell_cleanout (cell);
	cell->value = v;
}

/*
 * gnm_cell_set_expr_and_value : Stores (WITHOUT COPYING) the supplied value, and
 *        references the supplied expression and links it into the expression
 *        list.  It marks the sheet as dirty. It is intended for use by import
 *        routines or operations that do bulk assignment.
 *
 * WARNING : This is an internal routine that does not queue redraws,
 *           does not auto-resize, does not calculate spans, and does
 *           not render the value.
 *
 * NOTE : This DOES check for array partitioning.
 */
void
gnm_cell_set_expr_and_value (GnmCell *cell, GnmExprTop const *texpr, GnmValue *v,
			     gboolean link_expr)
{
	g_return_if_fail (cell != NULL);
	g_return_if_fail (texpr != NULL);
	if (gnm_cell_is_nonsingleton_array (cell)) {
		value_release (v);
		g_return_if_fail (!gnm_cell_is_nonsingleton_array (cell));
	}

	/* Repeat after me.  Ref before unref. */
	gnm_expr_top_ref (texpr);
	gnm_cell_cleanout (cell);

	cell->base.flags |= GNM_CELL_HAS_NEW_EXPR;
	cell->base.texpr = texpr;
	cell->value = v;
	if (link_expr)
		dependent_link (GNM_CELL_TO_DEP (cell));
}

/**
 * cell_set_expr_internal:
 * @cell: the cell to set the expr for
 * @expr: an expression
 *
 * A private internal utility to store an expression.
 * Does NOT
 *	- check for array subdivision
 *	- queue recalcs.
 *	- render value, calc dimension, compute spans
 *	- link the expression into the master list.
 */
static void
cell_set_expr_internal (GnmCell *cell, GnmExprTop const *texpr)
{
	gnm_expr_top_ref (texpr);

	gnm_cell_cleanout (cell);

	cell->base.flags |= GNM_CELL_HAS_NEW_EXPR;
	cell->base.texpr = texpr;

	/* Until the value is recomputed, we put in this value.
	 *
	 * We should consider using 0 instead and take out the
	 * gnm_cell_needs_recalc call in sheet_foreach_cell_in_range.
	 */
	cell->value = value_new_empty ();
}

/*
 * gnm_cell_set_expr_unsafe : Stores and references the supplied expression.  It
 *         marks the sheet as dirty.  Intented for use by import routines that
 *         do bulk assignment.  The resulting cell is NOT linked into the
 *         dependent list.  Nor marked for recalc.
 *
 * WARNING : This is an internal routine that does not queue redraws,
 *           does not auto-resize, and does not calculate spans.
 *           It also DOES NOT CHECK FOR ARRAY DIVISION.  Be very careful
 *           using this.
 */
void
gnm_cell_set_expr_unsafe (GnmCell *cell, GnmExprTop const *texpr)
{
	g_return_if_fail (cell != NULL);
	g_return_if_fail (texpr != NULL);

	cell_set_expr_internal (cell, texpr);
}

/**
 * gnm_cell_set_expr :  Stores and references the supplied expression
 *         marks the sheet as dirty.  Intented for use by import routines that
 *         do bulk assignment.  The resulting cell _is_ linked into the
 *         dependent list, but NOT marked for recalc.
 *
 * WARNING : This is an internal routine that does not queue redraws,
 *           does not auto-resize, and does not calculate spans.
 *           Be very careful using this.
 */
void
gnm_cell_set_expr (GnmCell *cell, GnmExprTop const *texpr)
{
	g_return_if_fail (!gnm_cell_is_nonsingleton_array (cell));
	g_return_if_fail (cell != NULL);
	g_return_if_fail (texpr != NULL);

	cell_set_expr_internal (cell, texpr);
	dependent_link (GNM_CELL_TO_DEP (cell));
}

/**
 * gnm_cell_set_array_formula:
 * @sheet:   The sheet to set the expr in.
 * @col_a:   The left column in the destination region.
 * @row_a:   The top row in the destination region.
 * @col_b:   The right column in the destination region.
 * @row_b:   The bottom row in the destination region.
 * @texpr:   an expression (the inner expression, not a corner or element)
 *
 * Uses cell_set_expr_internal to store the expr as an
 * 'array-formula'.  The supplied expression is wrapped in an array
 * operator for each cell in the range and scheduled for recalc.
 *
 * NOTE : Does not add a reference to the expression.  It takes over the
 *        caller's reference.
 *
 * Does not regenerate spans, dimensions or autosize cols/rows.
 *
 * DOES NOT CHECK for array partitioning.
 */
void
gnm_cell_set_array_formula (Sheet *sheet,
			    int col_a, int row_a, int col_b, int row_b,
			    GnmExprTop const *texpr)
{
	int const num_rows = 1 + row_b - row_a;
	int const num_cols = 1 + col_b - col_a;
	int x, y;
	GnmCell *corner;
	GnmExprTop const *wrapper;

	g_return_if_fail (sheet != NULL);
	g_return_if_fail (texpr != NULL);
	g_return_if_fail (0 <= col_a);
	g_return_if_fail (col_a <= col_b);
	g_return_if_fail (col_b < gnm_sheet_get_max_cols (sheet));
	g_return_if_fail (0 <= row_a);
	g_return_if_fail (row_a <= row_b);
	g_return_if_fail (row_b < gnm_sheet_get_max_rows (sheet));

	corner = sheet_cell_fetch (sheet, col_a, row_a);
	g_return_if_fail (corner != NULL);

	wrapper = gnm_expr_top_new_array_corner (num_cols, num_rows, gnm_expr_copy (texpr->expr));
	gnm_expr_top_unref (texpr);
	cell_set_expr_internal (corner, wrapper);
	gnm_expr_top_unref (wrapper);

	for (x = 0; x < num_cols; ++x) {
		for (y = 0; y < num_rows; ++y) {
			GnmCell *cell;
			GnmExprTop const *te;

			if (x == 0 && y == 0)
				continue;

			cell = sheet_cell_fetch (sheet, col_a + x, row_a + y);
			te = gnm_expr_top_new_array_elem (x, y);
			cell_set_expr_internal (cell, te);
			dependent_link (GNM_CELL_TO_DEP (cell));
			gnm_expr_top_unref (te);
		}
	}

	dependent_link (GNM_CELL_TO_DEP (corner));
}

static void
gnm_cell_set_array_formula_cb (GnmSheetRange const *sr, GnmExprTop const  *texpr)
{
	sheet_region_queue_recalc (sr->sheet, &sr->range);
	gnm_expr_top_ref (texpr);
	gnm_cell_set_array_formula (sr->sheet,
				    sr->range.start.col, sr->range.start.row,
				    sr->range.end.col,   sr->range.end.row,
				    texpr);
	sheet_region_queue_recalc (sr->sheet, &sr->range);
	sheet_flag_status_update_range (sr->sheet, &sr->range);
	sheet_queue_respan (sr->sheet, sr->range.start.row, sr->range.end.row);
}

GOUndo *
gnm_cell_set_array_formula_undo (GnmSheetRange *sr, GnmExprTop const  *texpr)
{
	gnm_expr_top_ref (texpr);
	return go_undo_binary_new (sr, (gpointer)texpr,
				   (GOUndoBinaryFunc) gnm_cell_set_array_formula_cb,
				   (GFreeFunc) gnm_sheet_range_free,
				   (GFreeFunc) gnm_expr_top_unref);
}

/**
 * gnm_cell_set_array: set an array expression for a range.
 * @sheet:   The sheet to set the expr in.
 * @r:       The range to set.
 * @texpr:   an expression (the inner expression, not a corner or element)
 *
 * Uses cell_set_expr_internal to store the expr as an
 * 'array-formula'.  The supplied expression is wrapped in an array
 * operator for each cell in the range and scheduled for recalc.
 *
 * Returns: TRUE if the operation succeded.
 *
 * NOTE : This adds a reference to the expression.
 *
 * Does not regenerate spans, dimensions or autosize cols/rows.
 *
 * DOES CHECK for array partitioning.
 */

gboolean
gnm_cell_set_array (Sheet *sheet,
		    const GnmRange *r,
		    GnmExprTop const *texpr)
{
	g_return_val_if_fail (sheet != NULL, FALSE);
	g_return_val_if_fail (range_is_sane (r), FALSE);
	g_return_val_if_fail (texpr != NULL, FALSE);

	if (sheet_range_splits_array (sheet, r, NULL, NULL, NULL))
		return FALSE;

	gnm_expr_top_ref (texpr);
	gnm_cell_set_array_formula (sheet,
				    r->start.col, r->start.row,
				    r->end.col, r->end.row,
				    texpr);
	return TRUE;
}

/***************************************************************************/

/**
 * gnm_cell_is_empty :
 * @cell : #GnmCell
 *
 * If the cell has not been created, or has VALUE_EMPTY.
 **/
gboolean
gnm_cell_is_empty (GnmCell const * cell)
{
	return cell == NULL || VALUE_IS_EMPTY (cell->value);
}

/**
 * gnm_cell_is_blank :
 * @cell : #GnmCell
 *
 * If the cell has not been created, has VALUE_EMPTY, or has a VALUE_STRING == ""
 **/
gboolean
gnm_cell_is_blank (GnmCell const * cell)
{
	return gnm_cell_is_empty (cell) ||
		(VALUE_IS_STRING (cell->value) &&
		 *value_peek_string (cell->value) == '\0');
}

GnmValue *
gnm_cell_is_error (GnmCell const * cell)
{
	g_return_val_if_fail (cell != NULL, NULL);
	g_return_val_if_fail (cell->value != NULL, NULL);

	if (VALUE_IS_ERROR (cell->value))
		return cell->value;
	return NULL;
}

gboolean
gnm_cell_is_number (GnmCell const *cell)
{
	/* FIXME : This does not handle arrays or ranges */
	return (cell->value && VALUE_IS_NUMBER (cell->value));
}

gboolean
gnm_cell_is_zero (GnmCell const *cell)
{
	GnmValue const * const v = cell->value;
	return v && VALUE_IS_NUMBER (v) && gnm_abs (value_get_as_float (v)) < 64 * GNM_EPSILON;
}

gboolean
gnm_cell_array_bound (GnmCell const *cell, GnmRange *res)
{
	GnmExprTop const *texpr;
	GnmExprArrayCorner const *array;
	int x, y;

	if (NULL == cell || !gnm_cell_has_expr (cell))
		return FALSE;

	g_return_val_if_fail (res != NULL, FALSE);

	texpr = cell->base.texpr;
	if (gnm_expr_top_is_array_elem (texpr, &x, &y)) {
		cell = sheet_cell_get (cell->base.sheet, cell->pos.col - x, cell->pos.row - y);

		g_return_val_if_fail (cell != NULL, FALSE);
		g_return_val_if_fail (gnm_cell_has_expr (cell), FALSE);

		texpr = cell->base.texpr;
	}

	array = gnm_expr_top_get_array_corner (texpr);
	if (!array)
		return FALSE;

	range_init (res, cell->pos.col, cell->pos.row,
		cell->pos.col + array->cols - 1,
		cell->pos.row + array->rows - 1);
	return TRUE;
}

GnmExprArrayCorner const *
gnm_cell_is_array_corner (GnmCell const *cell)
{
	return cell && gnm_cell_has_expr (cell)
		? gnm_expr_top_get_array_corner (cell->base.texpr)
		: NULL;
}

/**
 * gnm_cell_is_array :
 * @cell : #GnmCell const *
 *
 * Return TRUE is @cell is part of an array
 **/
gboolean
gnm_cell_is_array (GnmCell const *cell)
{
	return cell != NULL && gnm_cell_has_expr (cell) &&
		(gnm_expr_top_is_array_corner (cell->base.texpr) ||
		 gnm_expr_top_is_array_elem (cell->base.texpr, NULL, NULL));
}

/**
 * gnm_cell_is_nonsingleton_array :
 * @cell : #GnmCell const *
 *
 * Return TRUE is @cell is part of an array larger than 1x1
 **/
gboolean
gnm_cell_is_nonsingleton_array (GnmCell const *cell)
{
	GnmExprArrayCorner const *corner;

	if ((cell == NULL) || !gnm_cell_has_expr (cell))
		return FALSE;
	if (gnm_expr_top_is_array_elem (cell->base.texpr, NULL, NULL))
		return TRUE;

	corner 	= gnm_expr_top_get_array_corner (cell->base.texpr);
	return corner && (corner->cols > 1 || corner->rows > 1);
}

/***************************************************************************/

GnmRenderedValue *
gnm_cell_get_rendered_value (GnmCell const *cell)
{
	g_return_val_if_fail (cell != NULL, NULL);

	return gnm_rvc_query (cell->base.sheet->rendered_values, cell);
}

GnmRenderedValue *
gnm_cell_fetch_rendered_value (GnmCell const *cell,
			       gboolean allow_variable_width)
{
	GnmRenderedValue *rv;

	g_return_val_if_fail (cell != NULL, NULL);

	rv = gnm_cell_get_rendered_value (cell);
	if (rv)
		return rv;

	return gnm_cell_render_value (cell, allow_variable_width);
}

void
gnm_cell_unrender (GnmCell const *cell)
{
	gnm_rvc_remove (cell->base.sheet->rendered_values, cell);
}

/**
 * gnm_cell_render_value :
 * @cell: The cell whose value needs to be rendered
 * @allow_variable_width : Allow format to depend on column width.
 */
GnmRenderedValue *
gnm_cell_render_value (GnmCell const *cell, gboolean allow_variable_width)
{
	GnmRenderedValue *rv;
	Sheet *sheet;

	g_return_val_if_fail (cell != NULL, NULL);

	sheet = cell->base.sheet;
	rv = gnm_rendered_value_new (cell,
				     sheet->rendered_values->context,
				     allow_variable_width,
				     sheet->last_zoom_factor_used);

	gnm_rvc_store (sheet->rendered_values, cell, rv);

	return rv;
}

/*
 * gnm_cell_get_rendered_text:
 *
 * Warning: use this only when you really want what is displayed on the
 * screen.  If the user has decided to display formulas instead of values
 * then that is what you get.
 */
char *
gnm_cell_get_rendered_text (GnmCell *cell)
{
	GnmRenderedValue *rv;

	g_return_val_if_fail (cell != NULL, g_strdup ("ERROR"));

	rv = gnm_cell_fetch_rendered_value (cell, TRUE);

	return g_strdup (gnm_rendered_value_get_text (rv));
}

/**
 * gnm_cell_get_render_color:
 * @cell: the cell from which we want to pull the color from
 *
 * The returned value is a pointer to a PangoColor describing
 * the foreground colour.
 */
GOColor
gnm_cell_get_render_color (GnmCell const *cell)
{
	GnmRenderedValue *rv;

	g_return_val_if_fail (cell != NULL, GO_COLOR_BLACK);

	rv = gnm_cell_fetch_rendered_value (cell, TRUE);

	return rv->go_fore_color;
}

/**
 * gnm_cell_get_entered_text:
 * @cell: the cell from which we want to pull the content from
 *
 * This returns a g_malloc()ed region of memory with a text representation
 * of the cell contents.
 *
 * This will return a text expression if the cell contains a formula, or
 * a string representation of the value.
 */
char *
gnm_cell_get_entered_text (GnmCell const *cell)
{
	GnmValue const *v;
	Sheet *sheet;

	g_return_val_if_fail (cell != NULL, NULL);

	sheet = cell->base.sheet;

	if (gnm_cell_has_expr (cell)) {
		GnmParsePos pp;
		GnmConventionsOut out;

		out.accum = g_string_new ("=");
		out.pp = parse_pos_init_cell (&pp, cell);
		out.convs = sheet->convs;

		gnm_expr_top_as_gstring (cell->base.texpr, &out);
		return g_string_free (out.accum, FALSE);
	}

	v = cell->value;
	if (v != NULL) {
		GODateConventions const *date_conv =
			workbook_date_conv (sheet->workbook);

		if (VALUE_IS_STRING (v)) {
			/* Try to be reasonably smart about adding a leading quote */
			char const *tmp = value_peek_string (v);

			if (tmp[0] != '\'' &&
			    tmp[0] != 0 &&
			    !gnm_expr_char_start_p (tmp)) {
				GnmValue *val = format_match_number
					(tmp,
					 gnm_cell_get_format (cell),
					 date_conv);
				if (val == NULL)
					return g_strdup (tmp);
				value_release (val);
			}
			return g_strconcat ("\'", tmp, NULL);
		} else {
			GOFormat const *fmt = gnm_cell_get_format (cell);
			return format_value (fmt, v, NULL, -1,	date_conv);
		}
	}

	g_warning ("A cell with no expression, and no value ??");
	return g_strdup ("<ERROR>");
}


/*
 * Return the height of the rendered layout after rotation.
 */
int
gnm_cell_rendered_height (GnmCell const *cell)
{
	const GnmRenderedValue *rv;

	g_return_val_if_fail (cell != NULL, 0);

	rv = gnm_cell_get_rendered_value (cell);
	return rv
		? PANGO_PIXELS (rv->layout_natural_height)
		: 0;
}

/*
 * Return the width of the rendered layout after rotation.
 */
int
gnm_cell_rendered_width (GnmCell const *cell)
{
	const GnmRenderedValue *rv;

	g_return_val_if_fail (cell != NULL, 0);

	rv = gnm_cell_get_rendered_value (cell);
	return rv
		? PANGO_PIXELS (rv->layout_natural_width)
		: 0;
}

int
gnm_cell_rendered_offset (GnmCell const * cell)
{
	const GnmRenderedValue *rv;

	g_return_val_if_fail (cell != NULL, 0);

	rv = gnm_cell_get_rendered_value (cell);
	return rv
		? rv->indent_left + rv->indent_right
		: 0;
}

GnmStyle const *
gnm_cell_get_style (GnmCell const *cell)
{
	g_return_val_if_fail (cell != NULL, NULL);
	return sheet_style_get (cell->base.sheet,
				cell->pos.col,
				cell->pos.row);
}

/**
 * gnm_cell_get_format :
 * @cell :
 *
 * Get the display format.  If the assigned format is General,
 * the format of the value will be used.
 **/
GOFormat const *
gnm_cell_get_format (GnmCell const *cell)
{
	GOFormat const *fmt;

	g_return_val_if_fail (cell != NULL, go_format_general ());

	fmt = gnm_style_get_format (gnm_cell_get_style (cell));

	g_return_val_if_fail (fmt != NULL, go_format_general ());

	if (go_format_is_general (fmt) &&
	    cell->value != NULL && VALUE_FMT (cell->value))
		fmt = VALUE_FMT (cell->value);

	return fmt;
}

/*
 * gnm_cell_set_format:
 *
 * Changes the format for CELL to be FORMAT.  FORMAT should be
 * a number display format as specified on the manual
 *
 * Does not render, redraw, or respan.
 */
void
gnm_cell_set_format (GnmCell *cell, char const *format)
{
	GnmRange r;
	GnmStyle *mstyle;

	g_return_if_fail (cell != NULL);
	g_return_if_fail (format != NULL);

	mstyle = gnm_style_new ();
	gnm_style_set_format_text (mstyle, format);

	r.start = r.end = cell->pos;
	sheet_style_apply_range (cell->base.sheet, &r, mstyle);
}

static GnmValue *
cb_set_array_value (GnmCellIter const *iter, gpointer user)
{
	GnmValue const *value = user;
	GnmCell *cell = iter->cell;
	int x, y;

	/* Clipboard cells, e.g., are not attached to a sheet.  */
	if (gnm_cell_expr_is_linked (cell))
		dependent_unlink (GNM_CELL_TO_DEP (cell));

	if (!gnm_expr_top_is_array_elem (cell->base.texpr, &x, &y))
		return NULL;

	gnm_expr_top_unref (cell->base.texpr);
	cell->base.texpr = NULL;
	value_release (cell->value);
	cell->value = value_dup (value_area_get_x_y (value, x, y, NULL));

	return NULL;
}

/**
 * gnm_cell_convert_expr_to_value : drops the expression keeps its value.  Then uses the formatted
 *      result as if that had been entered.
 *
 * NOTE : the cell's expression cannot be linked into the expression * list.
 *
 * The cell is rendered but spans are not calculated,  the cell is NOT marked for
 * recalc.
 *
 * WARNING : This is an internal routine that does not queue redraws,
 *           does not auto-resize, and does not calculate spans.
 */
void
gnm_cell_convert_expr_to_value (GnmCell *cell)
{
	GnmExprArrayCorner const *array;

	g_return_if_fail (cell != NULL);
	g_return_if_fail (gnm_cell_has_expr (cell));

	/* Clipboard cells, e.g., are not attached to a sheet.  */
	if (gnm_cell_expr_is_linked (cell))
		dependent_unlink (GNM_CELL_TO_DEP (cell));

	array = gnm_expr_top_get_array_corner (cell->base.texpr);
	if (array) {
		sheet_foreach_cell_in_range (cell->base.sheet, CELL_ITER_ALL,
					     cell->pos.col, cell->pos.row,
					     cell->pos.col + array->cols - 1,
					     cell->pos.row + array->rows - 1,
					     cb_set_array_value,
					     array->value);
	} else {
		g_return_if_fail (!gnm_cell_is_array (cell));
	}

	gnm_expr_top_unref (cell->base.texpr);
	cell->base.texpr = NULL;
}

