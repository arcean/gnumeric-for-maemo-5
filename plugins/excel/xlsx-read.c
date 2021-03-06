/* vim: set sw=8: -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * xlsx-read.c : Read MS Excel 2007 Office Open xml
 *
 * Copyright (C) 2006-2007 Jody Goldberg (jody@gnome.org)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301
 * USA
 */
#include <gnumeric-config.h>
#include "xlsx-utils.h"

#include "sheet-view.h"
#include "sheet-style.h"
#include "sheet-merge.h"
#include "sheet.h"
#include "ranges.h"
#include "style.h"
#include "style-border.h"
#include "style-color.h"
#include "style-conditions.h"
#include "gnm-format.h"
#include "cell.h"
#include "position.h"
#include "expr.h"
#include "expr-name.h"
#include "print-info.h"
#include "validation.h"
#include "input-msg.h"
#include "value.h"
#include "sheet-filter.h"
#include "hlink.h"
#include "selection.h"
#include "command-context.h"
#include "workbook-view.h"
#include "workbook.h"
#include "gutils.h"
#include "graph.h"
#include "sheet-object-graph.h"
#include "sheet-object-cell-comment.h"
#include "gnm-sheet-slicer.h"
#include "gnm-so-filled.h"
#include "gnm-so-line.h"
#include "sheet-object-image.h"

#include <goffice/goffice.h>


#include "goffice-data.h"		/* MOVE TO GOFFCE with slicer code */
#include "go-data-slicer-field.h"	/* MOVE TO GOFFCE with slicer code */

#include <gsf/gsf-libxml.h>
#include <gsf/gsf-input.h>
#include <gsf/gsf-infile.h>
#include <gsf/gsf-infile-zip.h>
#include <gsf/gsf-open-pkg-utils.h>
#include <gsf/gsf-meta-names.h>
#include <gsf/gsf-doc-meta-data.h>
#include <gsf/gsf-docprop-vector.h>
#include <gsf/gsf-timestamp.h>

#include <glib/gi18n-lib.h>
#include <gmodule.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/*****************************************************************************/

#define CXML2C(s) ((char const *)(s))

typedef enum {
	XLXS_TYPE_NUM,
	XLXS_TYPE_SST_STR,	/* 0 based index into sst */
	XLXS_TYPE_BOOL,
	XLXS_TYPE_ERR,
	XLXS_TYPE_INLINE_STR,	/* inline string */
	/* How is this different from inlineStr ?? */
	XLXS_TYPE_STR2
} XLSXValueType;
typedef enum {
	XLSX_PANE_TOP_LEFT	= 0,
	XLSX_PANE_TOP_RIGHT	= 1,
	XLSX_PANE_BOTTOM_LEFT	= 2,
	XLSX_PANE_BOTTOM_RIGHT	= 3
} XLSXPanePos;

typedef enum {
	XLSX_AXIS_UNKNOWN,
	XLSX_AXIS_CAT,
	XLSX_AXIS_VAL,
	XLSX_AXIS_DATE
} XLSXAxisType;
typedef struct {
	char	*id;
	GogAxis	*axis;
	GSList	*plots;
	XLSXAxisType type;
	GogObjectPosition compass;
	GogAxisPosition	  cross;
	char	*cross_id;
	gnm_float cross_value;

	gboolean deleted;
} XLSXAxisInfo;

typedef struct {
	GsfInfile	*zip;

	GOIOContext	*context;	/* The IOcontext managing things */
	WorkbookView	*wb_view;	/* View for the new workbook */
	Workbook	*wb;		/* The new workbook */

	Sheet		 *sheet;	/* current sheet */
	GnmCellPos	  pos;		/* current cell */
	XLSXValueType	  pos_type;
	GnmValue	 *val;
	GnmExprTop const *texpr;
	GnmRange	  array;
	char		 *shared_id;
	GHashTable	 *shared_exprs;
	GnmConventions   *convs;

	SheetView	*sv;		/* current sheetview */

	GArray		*sst;
	PangoAttrList	*rich_attrs;
	PangoAttrList	*run_attrs;

	GHashTable	*num_fmts;
	GOFormat	*date_fmt;
	GHashTable	*cell_styles;
	GPtrArray	*fonts;
	GPtrArray	*fills;
	GPtrArray	*borders;
	GPtrArray	*xfs;
	GPtrArray	*style_xfs;
	GPtrArray	*dxfs;
	GPtrArray	*table_styles;
	GnmStyle	*style_accum;
	gboolean	 style_accum_partial;
	GnmStyleBorderType  border_style;
	GnmColor	*border_color;

	GHashTable	*theme_colors_by_name;

	GPtrArray	*collection;	/* utility for the shared collection handlers */
	unsigned	 count;
	XLSXPanePos	 pane_pos;

	GnmStyleConditions *conditions;
	GSList		   *cond_regions;
	GnmStyleCond	    cond;

	GnmFilter	   *filter;
	int		    filter_cur_field;
	GSList		   *filter_items; /* an accumulator */

	GSList		   *validation_regions;
	GnmValidation	   *validation;
	GnmInputMsg	   *input_msg;

	GnmPageBreaks	   *page_breaks;

	/* Rows/Cols state */
	GnmStyle          *pending_rowcol_style;
	GnmRange           pending_rowcol_range;

	/* Drawing state */
	SheetObject	   *so;
	gint64		    drawing_pos[8];
	int		    drawing_pos_flags;
	GnmExprTop const *link_texpr;

	/* Legacy drawing state */
	double		  grp_offset[4];
	GSList		 *grp_stack;

	/* Charting state */
	GogGraph	 *graph;
	GogChart	 *chart;
	GogPlot		 *plot;
	GogSeries	 *series;
	int		  dim_type;
	GogObject	 *series_pt;
	gboolean	  series_pt_has_index;
	GOStyle	 *cur_style;
	GOColor		 *gocolor;
	gboolean	 *auto_color;
	void (*color_setter) (gpointer data, GOColor color);
	GOColor		  color;
	gpointer	  color_data;
	GOMarker	 *marker;
	GOMarkerShape	  marker_symbol;
	GogObject	 *cur_obj;
	GSList		 *obj_stack;
	GSList		 *style_stack;
	unsigned int	  sp_type;
	char		 *chart_tx;
	gnm_float	  chart_pos[4];
	gboolean	  chart_pos_mode[2];
	gboolean	  chart_pos_target; /* true if "inner" */

	struct {
		GogAxis *obj;
		int	 type;
		GHashTable *by_id;
		GHashTable *by_obj;
		XLSXAxisInfo *info;
	} axis;

	char *defined_name;
	Sheet *defined_name_sheet;
	GList *delayed_names;

	/* external refs */
       	Workbook *external_ref;
	Sheet 	 *external_ref_sheet;

	/* Pivot state */
	struct {
		GnmSheetSlicer	  *slicer;
		GODataSlicerField *slicer_field;

		GHashTable	  *cache_by_id;
		GODataCache	  *cache;
		GODataCacheSource *cache_src;
		GODataCacheField  *cache_field;
		GPtrArray	  *cache_field_values;

		unsigned int	   field_count, record_count;
		char		  *cache_record_part_id;
	} pivot;

	/* Comment state */
	GPtrArray	*authors;
	GObject		*comment;
	GString		*comment_text;

	/* Document Properties */
	GsfDocMetaData   *metadata;
	char *meta_prop_name;
} XLSXReadState;
typedef struct {
	GOString	*str;
	GOFormat	*markup;
} XLSXStr;

static GsfXMLInNS const xlsx_ns[] = {
	GSF_XML_IN_NS (XL_NS_SS,	"http://schemas.openxmlformats.org/spreadsheetml/2006/main"),		  /* Office 12 */
	GSF_XML_IN_NS (XL_NS_SS,	"http://schemas.openxmlformats.org/spreadsheetml/2006/7/main"),		  /* Office 12 BETA-2 Technical Refresh */
	GSF_XML_IN_NS (XL_NS_SS,	"http://schemas.openxmlformats.org/spreadsheetml/2006/5/main"),		  /* Office 12 BETA-2 */
	GSF_XML_IN_NS (XL_NS_SS,	"http://schemas.microsoft.com/office/excel/2006/2"),			  /* Office 12 BETA-1 Technical Refresh */
	GSF_XML_IN_NS (XL_NS_SS_DRAW,	"http://schemas.openxmlformats.org/drawingml/2006/spreadsheetDrawing"),	  /* Office 12 BETA-2 */
	GSF_XML_IN_NS (XL_NS_SS_DRAW,	"http://schemas.openxmlformats.org/drawingml/2006/3/spreadsheetDrawing"), /* Office 12 BETA-2 Technical Refresh */
	GSF_XML_IN_NS (XL_NS_CHART,	"http://schemas.openxmlformats.org/drawingml/2006/3/chart"),		  /* Office 12 BETA-2 */
	GSF_XML_IN_NS (XL_NS_CHART,	"http://schemas.openxmlformats.org/drawingml/2006/chart"),		  /* Office 12 BETA-2 Technical Refresh */
	GSF_XML_IN_NS (XL_NS_CHART_DRAW,    "http://schemas.openxmlformats.org/drawingml/2006/chartDrawing"),
	GSF_XML_IN_NS (XL_NS_DRAW,	"http://schemas.openxmlformats.org/drawingml/2006/3/main"),		  /* Office 12 BETA-2 */
	GSF_XML_IN_NS (XL_NS_DRAW,	"http://schemas.openxmlformats.org/drawingml/2006/main"),		  /* Office 12 BETA-2 Technical Refresh */
	GSF_XML_IN_NS (XL_NS_DOC_REL,	"http://schemas.openxmlformats.org/officeDocument/2006/relationships"),
	GSF_XML_IN_NS (XL_NS_PKG_REL,	"http://schemas.openxmlformats.org/package/2006/relationships"),
	GSF_XML_IN_NS (XL_NS_LEG_OFF,   "urn:schemas-microsoft-com:office:office"),
	GSF_XML_IN_NS (XL_NS_LEG_XL,    "urn:schemas-microsoft-com:office:excel"),
	GSF_XML_IN_NS (XL_NS_LEG_VML,   "urn:schemas-microsoft-com:vml"),
	GSF_XML_IN_NS (XL_NS_PROP_CP,   "http://schemas.openxmlformats.org/package/2006/metadata/core-properties"),
	GSF_XML_IN_NS (XL_NS_PROP_DC,   "http://purl.org/dc/elements/1.1/"),
	GSF_XML_IN_NS (XL_NS_PROP_DCMITYPE, "http://purl.org/dc/dcmitype"),
	GSF_XML_IN_NS (XL_NS_PROP_DCTERMS,  "http://purl.org/dc/terms/"),
	GSF_XML_IN_NS (XL_NS_PROP_XSI,  "http://www.w3.org/2001/XMLSchema-instance"),
	GSF_XML_IN_NS (XL_NS_PROP,      "http://schemas.openxmlformats.org/officeDocument/2006/extended-properties"),
	GSF_XML_IN_NS (XL_NS_PROP_VT,   "http://schemas.openxmlformats.org/officeDocument/2006/docPropsVTypes"),
	GSF_XML_IN_NS (XL_NS_PROP_CUSTOM,   "http://schemas.openxmlformats.org/officeDocument/2006/custom-properties"),
	{ NULL }
};

static gboolean
xlsx_parse_stream (XLSXReadState *state, GsfInput *in, GsfXMLInNode const *dtd)
{
	gboolean  success = FALSE;

	if (NULL != in) {
		GsfXMLInDoc *doc = gsf_xml_in_doc_new (dtd, xlsx_ns);

		success = gsf_xml_in_doc_parse (doc, in, state);

		if (!success)
			go_io_warning (state->context,
				_("'%s' is corrupt!"),
				gsf_input_name (in));

		gsf_xml_in_doc_free (doc);
		g_object_unref (G_OBJECT (in));
	}
	return success;
}

static void
xlsx_parse_rel_by_id (GsfXMLIn *xin, char const *part_id,
		      GsfXMLInNode const *dtd,
		      GsfXMLInNS const *ns)
{
	GError *err;

#ifdef DEBUG_PARSER
	g_print ("{ /* Parsing  : %s :: %s */\n",
		 gsf_input_name (gsf_xml_in_get_input (xin)), part_id);
#endif

	err = gsf_open_pkg_parse_rel_by_id (xin, part_id, dtd, ns);
	if (NULL != err) {
		XLSXReadState *state = (XLSXReadState *)xin->user_state;
		go_io_warning (state->context, "%s", err->message);
		g_error_free (err);
	}

#ifdef DEBUG_PARSER
	g_print ("} /* DONE : %s :: %s */\n",
		 gsf_input_name (gsf_xml_in_get_input (xin)), part_id);
#endif
}

/****************************************************************************/

static gboolean xlsx_warning (GsfXMLIn *xin, char const *fmt, ...)
	G_GNUC_PRINTF (2, 3);

static gboolean
xlsx_warning (GsfXMLIn *xin, char const *fmt, ...)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	char *msg;
	va_list args;

	va_start (args, fmt);
	msg = g_strdup_vprintf (fmt, args);
	va_end (args);

	if (IS_SHEET (state->sheet)) {
		char *tmp;
		if (state->pos.col >= 0 && state->pos.row >= 0)
			tmp = g_strdup_printf ("%s!%s : %s",
				state->sheet->name_quoted,
				cellpos_as_string (&state->pos), msg);
		else
			tmp = g_strdup_printf ("%s : %s",
				state->sheet->name_quoted, msg);
		g_free (msg);
		msg = tmp;
	}

	go_io_warning (state->context, "%s", msg);
	g_printerr ("%s\n", msg);
	g_free (msg);

	return FALSE; /* convenience */
}

typedef struct {
	char const * const name;
	int val;
} EnumVal;

static gboolean
attr_enum (GsfXMLIn *xin, xmlChar const **attrs,
	   char const *target, EnumVal const *enums,
	   int *res)
{
	g_return_val_if_fail (attrs != NULL, FALSE);
	g_return_val_if_fail (attrs[0] != NULL, FALSE);
	g_return_val_if_fail (attrs[1] != NULL, FALSE);

	if (strcmp (attrs[0], target))
		return FALSE;

	for (; enums->name != NULL ; enums++)
		if (!strcmp (enums->name, attrs[1])) {
			*res = enums->val;
			return TRUE;
		}
	return xlsx_warning (xin,
		_("Unknown enum value '%s' for attribute %s"),
		attrs[1], target);
}

/**
 * Take an _int_ as a result to allow the caller to use -1 as an undefined state.
 **/
static gboolean
attr_bool (GsfXMLIn *xin, xmlChar const **attrs,
	   char const *target,
	   int *res)
{
	g_return_val_if_fail (attrs != NULL, FALSE);
	g_return_val_if_fail (attrs[0] != NULL, FALSE);
	g_return_val_if_fail (attrs[1] != NULL, FALSE);

	if (strcmp (attrs[0], target))
		return FALSE;

	*res = 0 == strcmp (attrs[1], "1");

	return TRUE;
}

static gboolean
attr_int (GsfXMLIn *xin, xmlChar const **attrs,
	  char const *target,
	  int *res)
{
	char *end;
	long tmp;

	g_return_val_if_fail (attrs != NULL, FALSE);
	g_return_val_if_fail (attrs[0] != NULL, FALSE);
	g_return_val_if_fail (attrs[1] != NULL, FALSE);

	if (strcmp (attrs[0], target))
		return FALSE;

	errno = 0;
	tmp = strtol (attrs[1], &end, 10);
	if (errno == ERANGE)
		return xlsx_warning (xin,
			_("Integer '%s' is out of range, for attribute %s"),
			attrs[1], target);
	if (*end)
		return xlsx_warning (xin,
			_("Invalid integer '%s' for attribute %s"),
			attrs[1], target);

	*res = tmp;
	return TRUE;
}
static gboolean
attr_int64 (GsfXMLIn *xin, xmlChar const **attrs,
	    char const *target,
	    gint64 *res)
{
	char *end;
	gint64 tmp;

	g_return_val_if_fail (attrs != NULL, FALSE);
	g_return_val_if_fail (attrs[0] != NULL, FALSE);
	g_return_val_if_fail (attrs[1] != NULL, FALSE);

	if (strcmp (attrs[0], target))
		return FALSE;

	errno = 0;
	tmp = g_ascii_strtoll (attrs[1], &end, 10);
	if (errno == ERANGE)
		return xlsx_warning (xin,
			_("Integer '%s' is out of range, for attribute %s"),
			attrs[1], target);
	if (*end)
		return xlsx_warning (xin,
			_("Invalid integer '%s' for attribute %s"),
			attrs[1], target);

	*res = tmp;
	return TRUE;
}

static gboolean
attr_gocolor (GsfXMLIn *xin, xmlChar const **attrs,
	      char const *target,
	      GOColor *res)
{
	char *end;
	unsigned long rgb;

	g_return_val_if_fail (attrs != NULL, FALSE);
	g_return_val_if_fail (attrs[0] != NULL, FALSE);
	g_return_val_if_fail (attrs[1] != NULL, FALSE);

	if (strcmp (attrs[0], target))
		return FALSE;

	errno = 0;
	rgb = strtoul (attrs[1], &end, 16);
	if (errno == ERANGE || *end)
		return xlsx_warning (xin,
			_("Invalid RRGGBB color '%s' for attribute %s"),
			attrs[1], target);

	{
		guint8 const r = (rgb >> 16) & 0xff;
		guint8 const g = (rgb >>  8) & 0xff;
		guint8 const b = (rgb >>  0) & 0xff;
		*res = GO_COLOR_FROM_RGB (r, g, b);
	}

	return TRUE;
}

static gboolean
attr_float (GsfXMLIn *xin, xmlChar const **attrs,
	    char const *target,
	    gnm_float *res)
{
	char *end;
	double tmp;

	g_return_val_if_fail (attrs != NULL, FALSE);
	g_return_val_if_fail (attrs[0] != NULL, FALSE);
	g_return_val_if_fail (attrs[1] != NULL, FALSE);

	if (strcmp (attrs[0], target))
		return FALSE;

	tmp = gnm_strto (attrs[1], &end);
	if (*end)
		return xlsx_warning (xin,
			_("Invalid number '%s' for attribute %s"),
			attrs[1], target);
	*res = tmp;
	return TRUE;
}

static gboolean
attr_pos (GsfXMLIn *xin, xmlChar const **attrs,
	  char const *target,
	  GnmCellPos *res)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	char const *end;
	GnmCellPos tmp;

	g_return_val_if_fail (attrs != NULL, FALSE);
	g_return_val_if_fail (attrs[0] != NULL, FALSE);
	g_return_val_if_fail (attrs[1] != NULL, FALSE);

	if (strcmp (attrs[0], target))
		return FALSE;

	end = cellpos_parse (attrs[1], gnm_sheet_get_size (state->sheet), &tmp, TRUE);
	if (NULL == end || *end != '\0')
		return xlsx_warning (xin,
			_("Invalid cell position '%s' for attribute %s"),
			attrs[1], target);
	*res = tmp;
	return TRUE;
}

static gboolean
attr_range (GsfXMLIn *xin, xmlChar const **attrs,
	    char const *target,
	    GnmRange *res)
{
	static const GnmSheetSize xlsx_size = {
		XLSX_MaxCol, XLSX_MaxRow
	};

	g_return_val_if_fail (attrs != NULL, FALSE);
	g_return_val_if_fail (attrs[0] != NULL, FALSE);
	g_return_val_if_fail (attrs[1] != NULL, FALSE);

	if (strcmp (attrs[0], target))
		return FALSE;

	if (!range_parse (res, attrs[1], &xlsx_size))
		xlsx_warning (xin, _("Invalid range '%s' for attribute %s"),
			attrs[1], target);
	return TRUE;
}

static GnmValue *
attr_datetime (GsfXMLIn *xin, xmlChar const **attrs,
	       char const *target)
{
	unsigned y, m, d, h, mi, n;
	GnmValue *res = NULL;
	gnm_float s;

	g_return_val_if_fail (attrs != NULL, NULL);
	g_return_val_if_fail (attrs[0] != NULL, NULL);
	g_return_val_if_fail (attrs[1] != NULL, NULL);

	if (strcmp (attrs[0], target))
		return NULL;

	n = sscanf (attrs[1], "%u-%u-%uT%u:%u:%" GNM_SCANF_g,
		    &y, &m, &d, &h, &mi, &s);

	if (n >= 3) {
		GDate date;
		g_date_set_dmy (&date, d, m, y);
		if (g_date_valid (&date)) {
			XLSXReadState *state = (XLSXReadState *)xin->user_state;
			unsigned d_serial = go_date_g_to_serial (&date,
				workbook_date_conv (state->wb));
			if (n >= 6) {
				double time_frac = h + (gnm_float)mi / 60 + s / 3600;
				res = value_new_float (d_serial + time_frac / 24.);
				value_set_fmt (res, state->date_fmt);
			} else {
				res = value_new_int (d_serial);
				value_set_fmt (res, go_format_default_date ());
			}
		}
	}

	return res;
}

/***********************************************************************/

static gboolean
simple_bool (GsfXMLIn *xin, xmlChar const **attrs, int *res)
{
	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_bool (xin, attrs, "val", res))
			return TRUE;
	return FALSE;
}
static gboolean
simple_int (GsfXMLIn *xin, xmlChar const **attrs, int *res)
{
	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_int (xin, attrs, "val", res))
			return TRUE;
	return FALSE;
}
static gboolean
simple_float (GsfXMLIn *xin, xmlChar const **attrs, gnm_float *res)
{
	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_float (xin, attrs, "val", res))
			return TRUE;
	return FALSE;
}

static gboolean
simple_enum (GsfXMLIn *xin, xmlChar const **attrs, EnumVal const *enums, int *res)
{
	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_enum (xin, attrs, "val", enums, res))
			return TRUE;
	return FALSE;
}

/***********************************************************************
 * These indexes look like the values in xls.  Dup some code from there.
 * TODO : Can we merge the code ?
 *	  Will the 'indexedColors' look like a palette ?
 */
static struct {
	guint8 r, g, b;
} excel_default_palette_v8 [] = {
	{  0,  0,  0}, {255,255,255}, {255,  0,  0}, {  0,255,  0},
	{  0,  0,255}, {255,255,  0}, {255,  0,255}, {  0,255,255},

	{128,  0,  0}, {  0,128,  0}, {  0,  0,128}, {128,128,  0},
	{128,  0,128}, {  0,128,128}, {192,192,192}, {128,128,128},

	{153,153,255}, {153, 51,102}, {255,255,204}, {204,255,255},
	{102,  0,102}, {255,128,128}, {  0,102,204}, {204,204,255},

	{  0,  0,128}, {255,  0,255}, {255,255,  0}, {  0,255,255},
	{128,  0,128}, {128,  0,  0}, {  0,128,128}, {  0,  0,255},

	{  0,204,255}, {204,255,255}, {204,255,204}, {255,255,153},
	{153,204,255}, {255,153,204}, {204,153,255}, {255,204,153},

	{ 51,102,255}, { 51,204,204}, {153,204,  0}, {255,204,  0},
	{255,153,  0}, {255,102,  0}, {102,102,153}, {150,150,150},

	{  0, 51,102}, { 51,153,102}, {  0, 51,  0}, { 51, 51,  0},
	{153, 51,  0}, {153, 51,102}, { 51, 51,153}, { 51, 51, 51}
};

static GOColor
indexed_color (XLSXReadState *state, gint idx)
{
	/* NOTE: not documented but seems close
	 * If you find a normative reference please forward it.
	 *
	 * The color index field seems to use
	 *	8-63 = Palette index 0-55
	 *	64  = auto pattern, auto border
	 *      65  = auto background
	 *      127 = auto font
	 *
	 *      65 is always white, and 127 always black. 64 is black
	 *      if the fDefaultHdr flag in WINDOW2 is unset, otherwise it's
	 *      the grid color from WINDOW2.
	 */

	if (idx == 1 || idx == 65)
		return GO_COLOR_WHITE;
	switch (idx) {
	case 0:   /* black */
	case 64 : /* system text ? */
	case 81 : /* tooltip text */
	case 0x7fff : /* system text ? */
		return GO_COLOR_BLACK;

	case 1 :  /* white */
	case 65 : /* system back ? */
		return GO_COLOR_WHITE;

	case 80 : /* tooltip background */
		return GO_COLOR_YELLOW;

	case 2 : return GO_COLOR_RED;
	case 3 : return GO_COLOR_GREEN;
	case 4 : return GO_COLOR_BLUE;
	case 5 : return GO_COLOR_YELLOW;
	case 6 : return GO_COLOR_VIOLET;
	case 7 : return GO_COLOR_CYAN;

	default :
		 break;
	}

	idx -= 8;
	if (idx < 0 || (int) G_N_ELEMENTS (excel_default_palette_v8) <= idx) {
		g_warning ("EXCEL: color index (%d) is out of range (8..%d). Defaulting to black",
			   idx + 8, (int)G_N_ELEMENTS (excel_default_palette_v8) + 8);
		return GO_COLOR_BLACK;
	}

	/* TODO cache and ref */
	return GO_COLOR_FROM_RGB (excel_default_palette_v8[idx].r,
			     excel_default_palette_v8[idx].g,
			     excel_default_palette_v8[idx].b);
}
static GOColor
themed_color (GsfXMLIn *xin, gint idx)
{
	static char const * const theme_elements [] = {
		"lt1",	"dk1", "lt2", "dk2",
		"accent1", "accent2", "accent3", "accent4", "accent5", "accent6",
		"hlink", "folHlink"
	};
	XLSXReadState	*state = (XLSXReadState *)xin->user_state;

	/* MAGIC :
	 * looks like the indicies map to hard coded names rather than the
	 * order in the file.  Indeed the order in the file seems wrong
	 * it inverts the first to pairs
	 *	1,0,3,2, 4,5,6.....
	 * see:	http://openxmldeveloper.org/forums/thread/1306.aspx
	 * OOo seems to do something similar
	 *
	 * I'll make the assumption we should work by name rather than
	 * index. */
	if (idx >= 0 && idx < (int) G_N_ELEMENTS (theme_elements)) {
		gpointer color = g_hash_table_lookup (state->theme_colors_by_name,
						      theme_elements [idx]);
		if (NULL != color)
			return GPOINTER_TO_UINT (color);

		xlsx_warning (xin, _("Unknown theme color %d"), idx);
	} else {
		xlsx_warning (xin, "Color index (%d) is out of range (0..%d). Defaulting to black",
			      idx, (int) G_N_ELEMENTS (theme_elements));
	}

	return GO_COLOR_BLACK;
}

static GOFormat *
xlsx_get_num_fmt (GsfXMLIn *xin, char const *id)
{
	static char const * const std_builtins[] = {
		/* 0 */	 "General",
		/* 1 */	 "0",
		/* 2 */	 "0.00",
		/* 3 */	 "#,##0",
		/* 4 */	 "#,##0.00",
		/* 5 */	 NULL,
		/* 6 */	 NULL,
		/* 7 */	 NULL,
		/* 8 */	 NULL,
		/* 9 */  "0%",
		/* 10 */ "0.00%",
		/* 11 */ "0.00E+00",
		/* 12 */ "# ?/?",
		/* 13 */ "# ?""?/?""?",	/* silly trick to avoid using a trigraph */
		/* 14 */ "mm-dd-yy",
		/* 15 */ "d-mmm-yy",
		/* 16 */ "d-mmm",
		/* 17 */ "mmm-yy",
		/* 18 */ "h:mm AM/PM",
		/* 19 */ "h:mm:ss AM/PM",
		/* 20 */ "h:mm",
		/* 21 */ "h:mm:ss",
		/* 22 */ "m/d/yy h:mm",
		/* 23 */ NULL,
		/* 24 */ NULL,
		/* 25 */ NULL,
		/* 26 */ NULL,
		/* 27 */ NULL,
		/* 28 */ NULL,
		/* 29 */ NULL,
		/* 30 */ NULL,
		/* 31 */ NULL,
		/* 32 */ NULL,
		/* 33 */ NULL,
		/* 34 */ NULL,
		/* 35 */ NULL,
		/* 36 */ NULL,
		/* 37 */ "#,##0 ;(#,##0)",
		/* 38 */ "#,##0 ;[Red](#,##0)",
		/* 39 */ "#,##0.00;(#,##0.00)",
		/* 40 */ "#,##0.00;[Red](#,##0.00)",
		/* 41 */ NULL,
		/* 42 */ NULL,
		/* 43 */ NULL,
		/* 44 */ NULL,
		/* 45 */ "mm:ss",
		/* 46 */ "[h]:mm:ss",
		/* 47 */ "mmss.0",
		/* 48 */ "##0.0E+0",
		/* 49 */ "@"
	};

#if 0
	CHT						CHS
27 [$-404]e/m/d					yyyy"5E74"m"6708"
28 [$-404]e"5E74"m"6708"d"65E5"			m"6708"d"65E5"
29 [$-404]e"5E74"m"6708"d"65E5"			m"6708"d"65E5"
30 m/d/yy					m-d-yy
31 yyyy"5E74"m"6708"d"65E5"			yyyy"5E74"m"6708"d"65E5"
32 hh"6642"mm"5206"				h"65F6"mm"5206"
33 hh"6642"mm"5206"ss"79D2"			h"65F6"mm"5206"ss"79D2"
34 4E0A5348/4E0B5348hh"6642"mm"5206"		4E0A5348/4E0B5348h"65F6"mm"5206"
35 4E0A5348/4E0B5348hh"6642"mm"5206"ss"79D2"	4E0A5348/4E0B5348h"65F6"mm"5206"ss"79D2"
36 [$-404]e/m/d					yyyy"5E74"m"6708"
50 [$-404]e/m/d					yyyy"5E74"m"6708"
51 [$-404]e"5E74"m"6708"d"65E5"			m"6708"d"65E5"
52 4E0A5348/4E0B5348hh"6642"mm"5206"		yyyy"5E74"m"6708"
53 4E0A5348/4E0B5348hh"6642"mm"5206"ss"79D2"	m"6708"d"65E5"
54 [$-404]e"5E74"m"6708"d"65E5"			m"6708"d"65E5"
55 4E0A5348/4E0B5348hh"6642"mm"5206"		4E0A5348/4E0B5348h"65F6"mm"5206"
56 4E0A5348/4E0B5348hh"6642"mm"5206"ss"79D2"	4E0A5348/4E0B5348h"65F6"mm"5206"ss"79D2"
57 [$-404]e/m/d					yyyy"5E74"m"6708"
58 [$-404]e"5E74"m"6708"d"65E5"			m"6708"d"65E5"

	JPN						KOR
27 [$-411]ge.m.d				yyyy"5E74" mm"6708" dd"65E5"
28 [$-411]ggge"5E74"m"6708"d"65E5"		mm-dd
29 [$-411]ggge"5E74"m"6708"d"65E5"		mm-dd
30 m/d/yy					mm-dd-yy
31 yyyy"5E74"m"6708"d"65E5"			yyyy"B144" mm"C6D4" dd"C77C"
32 h"6642"mm"5206"				h"C2DC" mm"BD84"
33 h"6642"mm"5206"ss"79D2"			h"C2DC" mm"BD84" ss"CD08"
34 yyyy"5E74"m"6708"				yyyy-mm-dd
35 m"6708"d"65E5"				yyyy-mm-dd
36 [$-411]ge.m.d				yyyy"5E74" mm"6708" dd"65E5"
50 [$-411]ge.m.d				yyyy"5E74" mm"6708" dd"65E5"
51 [$-411]ggge"5E74"m"6708"d"65E5"		mm-dd
52 yyyy"5E74"m"6708"				yyyy-mm-dd
53 m"6708"d"65E5"				yyyy-mm-dd
54 [$-411]ggge"5E74"m"6708"d"65E5"		mm-dd
55 yyyy"5E74"m"6708"				yyyy-mm-dd
56 m"6708"d"65E5"				yyyy-mm-dd
57 [$-411]ge.m.d				yyyy"5E74" mm"6708" dd"65E5"
58 [$-411]ggge"5E74"m"6708"d"65E5"		mm-dd

	THA
59 "t0"
60 "t0.00"
61 "t#,##0"
62 "t#,##0.00"
67 "t0%"
68 "t0.00%"
69 "t# ?/?"
70 "t# ?""?/?""?" /* silly trick to avoid using a trigraph */
71 0E27/0E14/0E1B0E1B0E1B0E1B
72 0E27-0E140E140E14-0E1B0E1B
73 0E27-0E140E140E14
74 0E140E140E14-0E1B0E1B
75 0E0A:0E190E19
76 0E0A:0E190E19:0E170E17
77 0E27/0E14/0E1B0E1B0E1B0E1B 0E0A:0E190E19
78 0E190E19:0E170E17
79 [0E0A]:0E190E19:0E170E17
80 0E190E19:0E170E17.0
81 d/m/bb
#endif

	XLSXReadState	*state = (XLSXReadState *)xin->user_state;
	GOFormat *res = g_hash_table_lookup (state->num_fmts, id);
	char *end;
	long i;

	if (NULL != res)
		return res;

	/* builtins */
	i = strtol (id, &end, 10);
	if (end != id && *end == '\0' &&
	    i >= 0 && i < (int) G_N_ELEMENTS (std_builtins) &&
	    std_builtins[i] != NULL) {
		res = go_format_new_from_XL (std_builtins[i]);
		g_hash_table_replace (state->num_fmts, g_strdup (id), res);
	} else
		xlsx_warning (xin, _("Undefined number format id '%s'"), id);
	return res;
}

static GnmExprTop const *
xlsx_parse_expr (GsfXMLIn *xin, xmlChar const *expr_str,
		 GnmParsePos const *pp)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	GnmParseError err;
	GnmExprTop const *texpr;

	/* Odd, some time IF and CHOOSE show up with leading spaces ??
	 * = IF(....
	 * = CHOOSE(...
	 * I wonder if it is related to some of the funky old
	 * optimizations in * xls ? */
	while (' ' == *expr_str)
		expr_str++;

	texpr = gnm_expr_parse_str (expr_str, pp,
		GNM_EXPR_PARSE_DEFAULT, state->convs,
		parse_error_init (&err));
	if (NULL == texpr)
		xlsx_warning (xin, "'%s' %s", expr_str, err.err->message);
	parse_error_free (&err);

	return texpr;
}

/* Returns: a GSList of GnmRange in _reverse_ order
 * caller frees the list and the content */
static GSList *
xlsx_parse_sqref (GsfXMLIn *xin, xmlChar const *refs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	GnmRange  r;
	xmlChar const *tmp;
	GSList	 *res = NULL;

	while (NULL != refs && *refs) {
		if (NULL == (tmp = cellpos_parse (refs, gnm_sheet_get_size (state->sheet), &r.start, FALSE))) {
			xlsx_warning (xin, "unable to parse reference list '%s'", refs);
			return res;
		}

		refs = tmp;
		if (*refs == '\0' || *refs == ' ')
			r.end = r.start;
		else if (*refs != ':' ||
			 NULL == (tmp = cellpos_parse (refs + 1, gnm_sheet_get_size (state->sheet), &r.end, FALSE))) {
			xlsx_warning (xin, "unable to parse reference list '%s'", refs);
			return res;
		}

		range_normalize (&r); /* be anal */
		res = g_slist_prepend (res, gnm_range_dup (&r));

		for (refs = tmp ; *refs == ' ' ; refs++ ) ;
	}

	return res;
}

/***********************************************************************/

#include "xlsx-read-pivot.c"

/***********************************************************************/

#include "xlsx-read-drawing.c"

/***********************************************************************/

/* RGBMAX, HLSMAX must each fit in a byte. */
/* HLSMAX BEST IF DIVISIBLE BY 6 */
#define  HLSMAX   240 /* H,L, and S vary over 0-HLSMAX */
#define  RGBMAX   255 /* R,G, and B vary over 0-RGBMAX */

/* Hue is undefined if Saturation is 0 (grey-scale) */
/* This value determines where the Hue scrollbar is */
/* initially set for achromatic colors */
#define UNDEFINED (HLSMAX*2/3)

/* utility routine for HLStoRGB */
static int
hue_to_color (int m1, int m2, int h)
{
	if (h < 0)
		h += HLSMAX;
	if (h > HLSMAX)
		h -= HLSMAX;

	/* return r,g, or b value from this tridrant */
	if (h < (HLSMAX/6))
		return m1 + (((m2 - m1)*h + (HLSMAX/12))/(HLSMAX/6));
	if (h < (HLSMAX/2))
		return m2;
	if (h < ((HLSMAX*2)/3))
		return m1 + (((m2 - m1)*(((HLSMAX*2)/3)-h)+(HLSMAX/12))/(HLSMAX/6));

	return m1;
}

static GOColor
apply_tint (GOColor orig, double tint)
{
	int r = GO_COLOR_UINT_R (orig);
	int g = GO_COLOR_UINT_G (orig);
	int b = GO_COLOR_UINT_B (orig);
	int a = GO_COLOR_UINT_A (orig);
	int maxC = b, minC = b, delta, sum, h, l, s, m1, m2;

	if (fabs (tint) < .005)
		return orig;

	maxC = MAX (MAX (r,g),b);
	minC = MIN (MIN (r,g),b);
	l = (((maxC + minC)*HLSMAX) + RGBMAX)/(2*RGBMAX);

	delta = maxC - minC;
	sum   = maxC + minC;
	if (delta != 0) {
		if (l <= (HLSMAX/2))
			s = ( (delta*HLSMAX) + (sum/2) ) / sum;
		else
			s = ( (delta*HLSMAX) + ((2*RGBMAX - sum)/2) ) / (2*RGBMAX - sum);

		if (r == maxC)
			h =                ((g - b) * HLSMAX) / (6 * delta);
		else if (g == maxC)
			h = (  HLSMAX/3) + ((b - r) * HLSMAX) / (6 * delta);
		else if (b == maxC)
			h = (2*HLSMAX/3) + ((r - g) * HLSMAX) / (6 * delta);

		if (h < 0)
			h += HLSMAX;
		else if (h >= HLSMAX)
			h -= HLSMAX;
	} else {
		h = 0;
		s = 0;
	}

	if (tint < 0.)
		l = l * (1. + tint);
	else
		l = l * (1. - tint) + (HLSMAX - HLSMAX * (1.0 - tint));

	if (s == 0) {            /* achromatic case */
		r = (l * RGBMAX) / HLSMAX;
		return GO_COLOR_FROM_RGBA (r, r, r, a);
	}

	if (l <= (HLSMAX/2))
		m2 = (l*(HLSMAX + s) + (HLSMAX/2))/HLSMAX;
	else
		m2 = l + s - ((l*s) + (HLSMAX/2))/HLSMAX;
	m1 = 2*l - m2;

	r = (hue_to_color (m1, m2, h + (HLSMAX/3))*RGBMAX + (HLSMAX/2)) / HLSMAX;
	g = (hue_to_color (m1, m2, h             )*RGBMAX + (HLSMAX/2)) / HLSMAX;
	b = (hue_to_color (m1, m2, h - (HLSMAX/3))*RGBMAX + (HLSMAX/2)) / HLSMAX;

	return GO_COLOR_FROM_RGBA (r,g,b,a);
}

static GnmColor *
elem_color (GsfXMLIn *xin, xmlChar const **attrs, gboolean allow_alpha)
{
	XLSXReadState	*state = (XLSXReadState *)xin->user_state;
	int indx;
	GOColor c = GO_COLOR_BLACK;
	gnm_float tint = 0.;
	gboolean has_color = FALSE;

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2) {
		if (0 == strcmp (attrs[0], "rgb")) {
			guint a, r, g, b;
			if (4 != sscanf (attrs[1], "%02x%02x%02x%02x", &a, &r, &g, &b)) {
				xlsx_warning (xin,
					_("Invalid color '%s' for attribute rgb"),
					attrs[1]);
				return NULL;
			}
			has_color = TRUE;
			c = GO_COLOR_FROM_RGBA (r,g,b,a);
		} else if (attr_int (xin, attrs, "indexed", &indx)) {
			has_color = TRUE;
			c = indexed_color (state, indx);
		} else if (attr_int (xin, attrs, "theme", &indx)) {
			has_color = TRUE;
			c = themed_color (xin, indx);
		} else if (attr_float (xin, attrs, "tint", &tint))
			tint = CLAMP (tint, -1., 1.);
	}

	if (!has_color)
		return NULL;
	c = apply_tint (c, tint);
	if (!allow_alpha)
		c |= 0xFF;
	return style_color_new_go (c);
}

static GnmStyle *
xlsx_get_style_xf (GsfXMLIn *xin, int xf)
{
	XLSXReadState	*state = (XLSXReadState *)xin->user_state;
	if (0 <= xf && NULL != state->style_xfs && xf < (int)state->style_xfs->len)
		return g_ptr_array_index (state->style_xfs, xf);
	xlsx_warning (xin, _("Undefined style record '%d'"), xf);
	return NULL;
}
static GnmStyle *
xlsx_get_xf (GsfXMLIn *xin, int xf)
{
	XLSXReadState	*state = (XLSXReadState *)xin->user_state;
	if (0 <= xf && NULL != state->xfs && xf < (int)state->xfs->len)
		return g_ptr_array_index (state->xfs, xf);
	xlsx_warning (xin, _("Undefined style record '%d'"), xf);
	return NULL;
}
static GnmStyle *
xlsx_get_dxf (GsfXMLIn *xin, int dxf)
{
	XLSXReadState	*state = (XLSXReadState *)xin->user_state;
	if (0 <= dxf && NULL != state->dxfs && dxf < (int)state->dxfs->len)
		return g_ptr_array_index (state->dxfs, dxf);
	xlsx_warning (xin, _("Undefined partial style record '%d'"), dxf);
	return NULL;
}

/****************************************************************************/

static void
xlsx_cell_val_end (GsfXMLIn *xin, G_GNUC_UNUSED GsfXMLBlob *blob)
{
	XLSXReadState	*state = (XLSXReadState *)xin->user_state;
	XLSXStr const	*entry;
	char		*end;
	long		 i;

	switch (state->pos_type) {
	case XLXS_TYPE_NUM :
		if (*xin->content->str)
			state->val = value_new_float (gnm_strto (xin->content->str, &end));
		break;
	case XLXS_TYPE_SST_STR :
		i = strtol (xin->content->str, &end, 10);
		if (end != xin->content->str && *end == '\0' &&
		    0 <= i  && i < (int)state->sst->len) {
			entry = &g_array_index (state->sst, XLSXStr, i);
			go_string_ref (entry->str);
			state->val = value_new_string_str (entry->str);
			if (NULL != entry->markup)
				value_set_fmt (state->val, entry->markup);
		} else {
			xlsx_warning (xin, _("Invalid sst ref '%s'"), xin->content->str);
		}
		break;
	case XLXS_TYPE_BOOL :
		if (*xin->content->str)
			state->val = value_new_bool (*xin->content->str != '0');
		break;
	case XLXS_TYPE_ERR :
		if (*xin->content->str)
			state->val = value_new_error (NULL, xin->content->str);
		break;

	case XLXS_TYPE_STR2 : /* What is this ? */
	case XLXS_TYPE_INLINE_STR :
		state->val = value_new_string (xin->content->str);
		break;
	default :
		g_warning ("Unknown val type %d", state->pos_type);
	}
}

static void
xlsx_cell_expr_begin (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	gboolean has_range = FALSE, is_array = FALSE, is_shared = FALSE;
	GnmRange range;
	xmlChar const *shared_id = NULL;

	/* See https://bugzilla.gnome.org/show_bug.cgi?id=642850 */
	/* for some of the issues surrounding shared formulas.   */

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (0 == strcmp (attrs[0], "t")) {
			if (0 == strcmp (attrs[1], "array"))
				is_array = TRUE;
			else if (0 == strcmp (attrs[1], "shared"))
				is_shared = TRUE;
		} else if (0 == strcmp (attrs[0], "si"))
			shared_id = attrs[1];
		else if (attr_range (xin, attrs, "ref", &range))
			has_range = TRUE;

	state->shared_id = NULL;
	if (is_shared &&  NULL != shared_id) {
		if (!has_range)
			state->texpr = g_hash_table_lookup (state->shared_exprs, shared_id);
		if (NULL != state->texpr)
			gnm_expr_top_ref (state->texpr);
		else
			state->shared_id = g_strdup (shared_id);
	} else
		state->texpr = NULL;

	/* if the shared expr is already parsed expression do not even collect content */
	((GsfXMLInNode *)(xin->node))->has_content =
		(NULL != state->texpr) ? GSF_XML_NO_CONTENT : GSF_XML_CONTENT;

	if (is_array && has_range)
		state->array = range;
}

static void
xlsx_cell_expr_end (GsfXMLIn *xin, G_GNUC_UNUSED GsfXMLBlob *blob)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	GnmParsePos pp;

	if (NULL == state->texpr) {
		parse_pos_init (&pp, NULL, state->sheet,
			state->pos.col, state->pos.row);
		state->texpr = xlsx_parse_expr (xin, xin->content->str, &pp);
		if (NULL != state->texpr &&
		    NULL != state->shared_id) {
			gnm_expr_top_ref (state->texpr);
			g_hash_table_replace (state->shared_exprs,
				state->shared_id, (gpointer)state->texpr);
			state->shared_id = NULL;
		}
	}
	g_free (state->shared_id);
	state->shared_id = NULL;
}

static void
xlsx_cell_begin (GsfXMLIn *xin, xmlChar const **attrs)
{
	static EnumVal const types[] = {
		{ "n",		XLXS_TYPE_NUM },
		{ "s",		XLXS_TYPE_SST_STR },
		{ "str",	XLXS_TYPE_STR2 },
		{ "b",		XLXS_TYPE_BOOL },
		{ "inlineStr",	XLXS_TYPE_INLINE_STR },
		{ "e",		XLXS_TYPE_ERR },
		{ NULL, 0 }
	};
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	int tmp;
	GnmStyle *style = NULL;

	state->pos.col = state->pos.row = -1;
	state->pos_type = XLXS_TYPE_NUM; /* the default */
	state->val = NULL;
	state->texpr = NULL;
	range_init (&state->array, -1, -1, -1, -1); /* invalid */

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_pos (xin, attrs, "r", &state->pos)) ;
		else if (attr_enum (xin, attrs, "t", types, &tmp))
			state->pos_type = tmp;
		else if (attr_int (xin, attrs, "s", &tmp))
			style = xlsx_get_xf (xin, tmp);

	if (NULL != style) {
		gnm_style_ref (style);
		/* There may already be a row style set!*/
		sheet_style_apply_pos (state->sheet,
			state->pos.col, state->pos.row, style);
	}
}
static void
xlsx_cell_end (GsfXMLIn *xin, G_GNUC_UNUSED GsfXMLBlob *blob)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	GnmCell *cell = sheet_cell_fetch (state->sheet,
		state->pos.col, state->pos.row);

	if (NULL == cell) {
		xlsx_warning (xin, _("Invalid cell %s"),
			cellpos_as_string (&state->pos));
		value_release (state->val);
		if (NULL != state->texpr)
			gnm_expr_top_unref (state->texpr);
	} else if (NULL != state->texpr) {
		if (state->array.start.col >= 0) {
			gnm_cell_set_array (state->sheet,
					    &state->array,
					    state->texpr);
			gnm_expr_top_unref (state->texpr);
			if (NULL != state->val)
				gnm_cell_assign_value (cell, state->val);
		} else if (NULL != state->val) {
			gnm_cell_set_expr_and_value	(cell,
				state->texpr, state->val, TRUE);
			gnm_expr_top_unref (state->texpr);
		} else {
			gnm_cell_set_expr (cell, state->texpr);
			gnm_expr_top_unref (state->texpr);
		}
		state->texpr = NULL;
	} else if (NULL != state->val)
		gnm_cell_assign_value (cell, state->val);
	state->val = NULL;
}

static void
xlsx_CT_Row (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	int row = -1, xf_index;
	gnm_float h = -1.;
	int cust_fmt = FALSE, cust_height = FALSE, collapsed = FALSE;
	int hidden = -1;
	int outline = -1;
	GnmStyle *style = NULL;

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_int (xin, attrs, "r", &row)) ;
		else if (attr_float (xin, attrs, "ht", &h)) ;
		else if (attr_bool (xin, attrs, "customFormat", &cust_fmt)) ;
		else if (attr_bool (xin, attrs, "customHeight", &cust_height)) ;
		else if (attr_int (xin, attrs, "s", &xf_index))
			style = xlsx_get_xf (xin, xf_index);
		else if (attr_int (xin, attrs, "outlineLevel", &outline)) ;
		else if (attr_bool (xin, attrs, "hidden", &hidden)) ;
		else if (attr_bool (xin, attrs, "collapsed", &collapsed)) ;

	if (row > 0) {
		row--;
		if (h >= 0.)
			sheet_row_set_size_pts (state->sheet, row, h, cust_height);
		if (hidden > 0)
			colrow_set_visibility (state->sheet, FALSE, FALSE, row, row);
		if (outline >= 0)
			colrow_set_outline (sheet_row_fetch (state->sheet, row),
				outline, collapsed);

		if (NULL != style && cust_fmt) {
			GnmRange r;
			r.start.row = r.end.row = row;
			r.start.col = 0;
			r.end.col  = gnm_sheet_get_max_cols (state->sheet) - 1;
			gnm_style_ref (style);
			sheet_style_set_range (state->sheet, &r, style);
		}
	}
}

static void
xlsx_CT_RowsCols_end (GsfXMLIn *xin, G_GNUC_UNUSED GsfXMLBlob *blob)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;

	if (!state->pending_rowcol_style)
		return;

	sheet_style_set_range (state->sheet,
			       &state->pending_rowcol_range,
			       state->pending_rowcol_style);

	state->pending_rowcol_style = NULL;
}

static void
xlsx_CT_Col (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	int first = -1, last = -1, xf_index;
	gnm_float width = -1.;
	gboolean cust_width = FALSE, best_fit = FALSE, collapsed = FALSE;
	int i, hidden = -1;
	int outline = -1;
	GnmStyle *style = NULL;

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_int (xin, attrs, "min", &first)) ;
		else if (attr_int (xin, attrs, "max", &last)) ;
		else if (attr_float (xin, attrs, "width", &width))
			/* FIXME FIXME FIXME arbitrary map from 130 pixels to
			 * the value stored for a column with 130 pixel width*/
			width *= (130. / 18.5703125) * (72./96.);
		else if (attr_bool (xin, attrs, "customWidth", &cust_width)) ;
		else if (attr_bool (xin, attrs, "bestFit", &best_fit)) ;
		else if (attr_int (xin, attrs, "style", &xf_index))
			style = xlsx_get_xf (xin, xf_index);
		else if (attr_int (xin, attrs, "outlineLevel", &outline)) ;
		else if (attr_bool (xin, attrs, "hidden", &hidden)) ;
		else if (attr_bool (xin, attrs, "collapsed", &collapsed)) ;

	if (first < 0) {
		if (last < 0) {
			xlsx_warning (xin, _("Ignoring column information that does not specify first or last."));
			return;
		}
		first = --last;
	} else if (last < 0)
		last = --first;
	else {
		first--;
		last--;
	}


	if (last >= gnm_sheet_get_max_cols (state->sheet))
		last = gnm_sheet_get_max_cols (state->sheet) - 1;
	for (i = first; i <= last; i++) {
		if (width > 4)
			sheet_col_set_size_pts (state->sheet, i, width,
				cust_width && !best_fit);
		if (outline > 0)
			colrow_set_outline (sheet_col_fetch (state->sheet, i),
				outline, collapsed);
	}
	if (NULL != style) {
		GnmRange r;
		r.start.col = first;
		r.end.col   = last;
		r.start.row = 0;
		r.end.row  = gnm_sheet_get_max_rows (state->sheet) - 1;

		/*
		 * Sometimes we see a lot of columns with the same style.
		 * We delay applying the style because applying column
		 * by column leads to style fragmentation.  #622365
		 */

		if (style != state->pending_rowcol_style ||
		    state->pending_rowcol_range.start.row != r.start.row ||
		    state->pending_rowcol_range.end.row != r.end.row ||
		    state->pending_rowcol_range.end.col + 1 != r.start.col)
			xlsx_CT_RowsCols_end (xin, NULL);

		if (state->pending_rowcol_style)
			state->pending_rowcol_range.end.col = r.end.col;
		else {
			gnm_style_ref (style);
			state->pending_rowcol_style = style;
			state->pending_rowcol_range = r;
		}
	}
	if (hidden > 0)
		colrow_set_visibility (state->sheet, TRUE, FALSE, first, last);
}

static void
xlsx_CT_SheetPr (GsfXMLIn *xin, xmlChar const **attrs)
{
#if 0
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
    <xsd:attribute name="syncHorizontal" type="xsd:boolean" use="optional" default="false">
    <xsd:attribute name="syncVertical" type="xsd:boolean" use="optional" default="false">
    <xsd:attribute name="syncRef" type="ST_Ref" use="optional">
    <xsd:attribute name="transitionEvaluation" type="xsd:boolean" use="optional" default="false">
    <xsd:attribute name="transitionEntry" type="xsd:boolean" use="optional" default="false">
    <xsd:attribute name="published" type="xsd:boolean" use="optional" default="true">
    <xsd:attribute name="codeName" type="xsd:string" use="optional">
    <xsd:attribute name="filterMode" type="xsd:boolean" use="optional" default="false">
    <xsd:attribute name="enableFormatConditionsCalculation" type="xsd:boolean" use="optional" default="true">
#endif
}

static void
xlsx_sheet_tabcolor (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	GnmColor *text_color, *color = elem_color (xin, attrs, TRUE);
	if (NULL != color) {
		int contrast =
			GO_COLOR_UINT_R (color->go_color) +
			GO_COLOR_UINT_G (color->go_color) +
			GO_COLOR_UINT_B (color->go_color);
		if (contrast >= 0x180)
			text_color = style_color_black ();
		else
			text_color = style_color_white ();
		g_object_set (state->sheet,
			      "tab-foreground", text_color,
			      "tab-background", color,
			      NULL);
		style_color_unref (text_color);
		style_color_unref (color);
	}
}

static void
xlsx_sheet_page_setup (GsfXMLIn *xin, xmlChar const **attrs)
{
	/* XLSXReadState *state = (XLSXReadState *)xin->user_state; */
}

static void
xlsx_CT_SheetFormatPr (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	gnm_float h;
	int i;

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_float (xin, attrs, "defaultRowHeight", &h))
			sheet_row_set_default_size_pts (state->sheet, h);
		else if (attr_int (xin, attrs, "outlineLevelRow", &i)) {
			if (i > 0)
				sheet_colrow_gutter (state->sheet, FALSE, i);
		} else if (attr_int (xin, attrs, "outlineLevelCol", &i)) {
			if (i > 0)
				sheet_colrow_gutter (state->sheet, TRUE, i);
		}
}

static void
xlsx_CT_PageMargins (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	gnm_float margin;
	PrintInformation *pi = state->sheet->print_info;

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_float (xin, attrs, "left", &margin))
			print_info_set_margin_left (pi, GO_IN_TO_PT (margin));
		else if (attr_float (xin, attrs, "right", &margin))
			print_info_set_margin_right (pi, GO_IN_TO_PT (margin));
		else if (attr_float (xin, attrs, "top", &margin))
			print_info_set_edge_to_below_header (pi, GO_IN_TO_PT (margin));
		else if (attr_float (xin, attrs, "bottom", &margin))
			print_info_set_edge_to_above_footer (pi, GO_IN_TO_PT (margin));
		else if (attr_float (xin, attrs, "header", &margin))
			print_info_set_margin_header (pi, GO_IN_TO_PT (margin));
		else if (attr_float (xin, attrs, "footer", &margin))
			print_info_set_margin_footer (pi, GO_IN_TO_PT (margin));
}

static void
xlsx_CT_PageBreak (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState    *state = (XLSXReadState *)xin->user_state;
	GnmPageBreakType  type = GNM_PAGE_BREAK_AUTO;
	gboolean tmp;
	int	 pos;

	if (NULL == state->page_breaks)
		return;

	pos = 0;

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_int  (xin, attrs, "id", &pos)) ;
		else if (attr_bool (xin, attrs, "man", &tmp)) { if (tmp) type = GNM_PAGE_BREAK_MANUAL; }
		else if (attr_bool (xin, attrs, "pt", &tmp))  { if (tmp) type = GNM_PAGE_BREAK_DATA_SLICE; }
#if 0 /* Ignored */
		else if (attr_int  (xin, attrs, "min", &first)) ;
		else if (attr_int  (xin, attrs, "max", &last)) ;
#endif

	gnm_page_breaks_append_break (state->page_breaks, pos, type);
}

static void
xlsx_CT_PageBreaks_begin (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	int count = 0;

	g_return_if_fail (state->page_breaks == NULL);

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_int  (xin, attrs, "count", &count)) ;
#if 0 /* Ignored */
		else if (attr_int  (xin, attrs, "manualBreakCount", &manual_count)) ;
#endif

	state->page_breaks = gnm_page_breaks_new (xin->node->user_data.v_int);
}

static void
xlsx_CT_PageBreaks_end (GsfXMLIn *xin, G_GNUC_UNUSED GsfXMLBlob *blob)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;

	if (NULL != state->page_breaks) {
		print_info_set_breaks (state->sheet->print_info,
			state->page_breaks);
		state->page_breaks = NULL;
	}
}

static void
xlsx_CT_DataValidation_begin (GsfXMLIn *xin, xmlChar const **attrs)
{
	static EnumVal const val_styles[] = {
		{ "stop",	 VALIDATION_STYLE_STOP },
		{ "warning",	 VALIDATION_STYLE_WARNING },
		{ "information", VALIDATION_STYLE_INFO },
		{ NULL, 0 }
	};
	static EnumVal const val_types[] = {
		{ "none",	VALIDATION_TYPE_ANY },
		{ "whole",	VALIDATION_TYPE_AS_INT },
		{ "decimal",	VALIDATION_TYPE_AS_NUMBER },
		{ "list",	VALIDATION_TYPE_IN_LIST },
		{ "date",	VALIDATION_TYPE_AS_DATE },
		{ "time",	VALIDATION_TYPE_AS_TIME },
		{ "textLength",	VALIDATION_TYPE_TEXT_LENGTH },
		{ "custom",	VALIDATION_TYPE_CUSTOM },
		{ NULL, 0 }
	};
	static EnumVal const val_ops[] = {
		{ "between",	VALIDATION_OP_BETWEEN },
		{ "notBetween",	VALIDATION_OP_NOT_BETWEEN },
		{ "equal",	VALIDATION_OP_EQUAL },
		{ "notEqual",	VALIDATION_OP_NOT_EQUAL },
		{ "lessThan",		VALIDATION_OP_LT },
		{ "lessThanOrEqual",	VALIDATION_OP_LTE },
		{ "greaterThan",	VALIDATION_OP_GT },
		{ "greaterThanOrEqual",	VALIDATION_OP_GTE },
		{ NULL, 0 }
	};
#if 0
	/* Get docs on this */
	"imeMode" default="noControl"
		"noControl"
		"off"
		"on"
		"disabled"
		"hiragana"
		"fullKatakana"
		"halfKatakana"
		"fullAlpha"
		"halfAlpha"
		"fullHangul"
		"halfHangul"
#endif

	XLSXReadState *state = (XLSXReadState *)xin->user_state;

	/* defaults */
	ValidationStyle	val_style = VALIDATION_STYLE_STOP;
	ValidationType	val_type  = VALIDATION_TYPE_ANY;
	ValidationOp	val_op	  = VALIDATION_OP_BETWEEN;
	gboolean allowBlank = FALSE;
	gboolean showDropDown = FALSE;
	gboolean showInputMessage = FALSE;
	gboolean showErrorMessage = FALSE;
	xmlChar const *errorTitle = NULL;
	xmlChar const *error = NULL;
	xmlChar const *promptTitle = NULL;
	xmlChar const *prompt = NULL;
	xmlChar const *refs = NULL;
	int tmp;

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (0 == strcmp (attrs[0], "sqref"))
			refs = attrs[1];
		else if (attr_enum (xin, attrs, "errorStyle", val_styles, &tmp))
			val_style = tmp;
		else if (attr_enum (xin, attrs, "type", val_types, &tmp))
			val_type = tmp;
		else if (attr_enum (xin, attrs, "operator", val_ops, &tmp))
			val_op = tmp;

		else if (attr_bool (xin, attrs, "allowBlank", &allowBlank)) ;
		else if (attr_bool (xin, attrs, "showDropDown", &showDropDown)) ;
		else if (attr_bool (xin, attrs, "showInputMessage", &showInputMessage)) ;
		else if (attr_bool (xin, attrs, "showErrorMessage", &showErrorMessage)) ;

		else if (0 == strcmp (attrs[0], "errorTitle"))
			errorTitle = attrs[1];
		else if (0 == strcmp (attrs[0], "error"))
			error = attrs[1];
		else if (0 == strcmp (attrs[0], "promptTitle"))
			promptTitle = attrs[1];
		else if (0 == strcmp (attrs[0], "prompt"))
			prompt = attrs[1];

	/* order matters, we need the 1st item */
	state->validation_regions = g_slist_reverse (
		xlsx_parse_sqref (xin, refs));

	if (NULL == state->validation_regions)
		return;

	if (showErrorMessage) {
		GnmRange const *r = state->validation_regions->data;
		state->pos = r->start;
		state->validation = validation_new (val_style, val_type, val_op,
			errorTitle, error, NULL, NULL, allowBlank, showDropDown);
	}

	if (showInputMessage && (NULL != promptTitle || NULL != prompt))
		state->input_msg = gnm_input_msg_new (prompt, promptTitle);
}

static void
xlsx_CT_DataValidation_end (GsfXMLIn *xin, G_GNUC_UNUSED GsfXMLBlob *blob)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	GError   *err;
	GnmStyle *style = NULL;
	GSList   *ptr;

	if (NULL != state->validation &&
	    NULL != (err = validation_is_ok (state->validation))) {
		xlsx_warning (xin, _("Ignoring invalid data validation because : %s"),
			      _(err->message));
		validation_unref (state->validation);
		state->validation = NULL;
	}

	if (NULL != state->validation) {
		style = gnm_style_new ();
		gnm_style_set_validation (style, state->validation);
		state->validation = NULL;
	}

	if (NULL != state->input_msg) {
		if (NULL == style)
			style = gnm_style_new ();
		gnm_style_set_input_msg (style, state->input_msg);
		state->input_msg = NULL;
	}

	for (ptr = state->validation_regions ; ptr != NULL ; ptr = ptr->next) {
		if (NULL != style) {
			gnm_style_ref (style);
			sheet_style_apply_range	(state->sheet, ptr->data, style);
		}
		g_free (ptr->data);
	}
	if (NULL != style)
		gnm_style_unref (style);
	g_slist_free (state->validation_regions);
	state->validation_regions = NULL;
	state->pos.col = state->pos.row = -1;
}

static void
xlsx_validation_expr (GsfXMLIn *xin, G_GNUC_UNUSED GsfXMLBlob *blob)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	GnmParsePos pp;
	GnmExprTop const *texpr;

	/*  Sneaky buggers, parse relative to the 1st sqRef */
	parse_pos_init (&pp, NULL, state->sheet,
		state->pos.col, state->pos.row);
	texpr = xlsx_parse_expr (xin, xin->content->str, &pp);
	if (NULL != texpr) {
		validation_set_expr (state->validation, texpr,
			xin->node->user_data.v_int);
		gnm_expr_top_unref (texpr);
	}
}

static void
xlsx_CT_AutoFilter_begin (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	GnmRange r;

	g_return_if_fail (state->filter == NULL);

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_range (xin, attrs, "ref", &r))
			state->filter = gnm_filter_new (state->sheet, &r);
}

static void
xlsx_CT_AutoFilter_end (GsfXMLIn *xin, G_GNUC_UNUSED GsfXMLBlob *blob)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	g_return_if_fail (state->filter != NULL);
	state->filter = NULL;
}

static void
xlsx_CT_FilterColumn_begin (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	int id = -1;
	gboolean hidden = FALSE;
	gboolean show = TRUE;

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_int  (xin, attrs, "colId", &id)) ;
		else if (attr_bool (xin, attrs, "hiddenButton", &hidden)) ;
		else if (attr_bool (xin, attrs, "showButton", &show)) ;

	state->filter_cur_field = id;
}

static void
xlsx_CT_Filters_begin (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (0 == strcmp (attrs[0], "val")) {
		}
	state->filter_items = NULL;
}
static void
xlsx_CT_Filters_end (GsfXMLIn *xin, G_GNUC_UNUSED GsfXMLBlob *blob)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	state->filter_items = NULL;
}
static void
xlsx_CT_Filter (GsfXMLIn *xin, xmlChar const **attrs)
{
#if 0
	XLSXReadState *state = (XLSXReadState *)xin->user_state;

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (0 == strcmp (attrs[0], "val")) {
		}
#endif
}

static void
xlsx_CT_CustomFilters_begin (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;

#if 0
	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (0 == strcmp (attrs[0], "val")) {
		}
#endif
	state->filter_items = NULL;
}
static void
xlsx_CT_CustomFilters_end (GsfXMLIn *xin, G_GNUC_UNUSED GsfXMLBlob *blob)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	state->filter_items = NULL;
}

static void
xlsx_CT_CustomFilter (GsfXMLIn *xin, xmlChar const **attrs)
{
#if 0
	static EnumVal const ops[] = {
		{ "lessThan",		GNM_STYLE_COND_LT },
		{ "lessThanOrEqual",	GNM_STYLE_COND_LTE },
		{ "equal",		GNM_STYLE_COND_EQUAL },
		{ "notEqual",		GNM_STYLE_COND_NOT_EQUAL },
		{ "greaterThanOrEqual",	GNM_STYLE_COND_GTE },
		{ "greaterThan",	GNM_STYLE_COND_GT },
		{ NULL, 0 }
	};
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	int tmp;
	GnmFilterOp op = GNM_STYLE_COND_EQUAL;

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (0 == strcmp (attrs[0], "val")) {
		} else if (attr_enum (xin, attrs, "operator", ops, &tmp))
			op = tmp;
#endif
}

static void
xlsx_CT_Top10 (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	gboolean top = TRUE;
	gboolean percent = FALSE;
	gnm_float val = -1.;
	GnmFilterCondition *cond;

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_float (xin, attrs, "val", &val)) ;
		else if (attr_bool (xin, attrs, "top", &top)) ;
		else if (attr_bool (xin, attrs, "percent", &percent)) ;

	if (NULL != (cond = gnm_filter_condition_new_bucket (top, !percent, val)))
		gnm_filter_set_condition (state->filter, state->filter_cur_field,
			cond, FALSE);
}

static void
xlsx_CT_DynamicFilter (GsfXMLIn *xin, xmlChar const **attrs)
{
#if 0
	static EnumVal const types[] = {
		{ "null", 0 },
		{ "aboveAverage", 0 },
		{ "belowAverage", 0 },
		{ "tomorrow", 0 },
		{ "today", 0 },
		{ "yesterday", 0 },
		{ "nextWeek", 0 },
		{ "thisWeek", 0 },
		{ "lastWeek", 0 },
		{ "nextMonth", 0 },
		{ "thisMonth", 0 },
		{ "lastMonth", 0 },
		{ "nextQuarter", 0 },
		{ "thisQuarter", 0 },
		{ "lastQuarter", 0 },
		{ "nextYear", 0 },
		{ "thisYear", 0 },
		{ "lastYear", 0 },
		{ "yearToDate", 0 },
		{ "Q1", 0 },
		{ "Q2", 0 },
		{ "Q3", 0 },
		{ "Q4", 0 },
		{ "M1", 0 },
		{ "M2", 0 },
		{ "M3", 0 },
		{ "M4", 0 },
		{ "M5", 0 },
		{ "M6", 0 },
		{ "M7", 0 },
		{ "M8", 0 },
		{ "M9", 0 },
		{ "M10", 0 },
		{ "M11", 0 },
		{ "M12", 0 },
		{ NULL, 0 }
	};
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	int type = -1;

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_enum (xin, attrs, "type", types, &type)) ;
#endif
}

static void
xlsx_CT_MergeCell (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	GnmRange r;

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_range (xin, attrs, "ref", &r))
			gnm_sheet_merge_add (state->sheet, &r, FALSE,
				GO_CMD_CONTEXT (state->context));
}

static void
xlsx_CT_SheetProtection (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	gboolean sheet			= FALSE;
	gboolean objects		= FALSE;
	gboolean scenarios		= FALSE;
	gboolean formatCells		= TRUE;
	gboolean formatColumns		= TRUE;
	gboolean formatRows		= TRUE;
	gboolean insertColumns		= TRUE;
	gboolean insertRows		= TRUE;
	gboolean insertHyperlinks	= TRUE;
	gboolean deleteColumns		= TRUE;
	gboolean deleteRows		= TRUE;
	gboolean selectLockedCells	= FALSE;
	gboolean sort			= TRUE;
	gboolean autoFilter		= TRUE;
	gboolean pivotTables		= TRUE;
	gboolean selectUnlockedCells	= FALSE;

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_bool (xin, attrs, "sheet", &sheet)) ;
		else if (attr_bool (xin, attrs, "objects", &objects)) ;
		else if (attr_bool (xin, attrs, "scenarios", &scenarios)) ;
		else if (attr_bool (xin, attrs, "formatCells", &formatCells)) ;
		else if (attr_bool (xin, attrs, "formatColumns", &formatColumns)) ;
		else if (attr_bool (xin, attrs, "formatRows", &formatRows)) ;
		else if (attr_bool (xin, attrs, "insertColumns", &insertColumns)) ;
		else if (attr_bool (xin, attrs, "insertRows", &insertRows)) ;
		else if (attr_bool (xin, attrs, "insertHyperlinks", &insertHyperlinks)) ;
		else if (attr_bool (xin, attrs, "deleteColumns", &deleteColumns)) ;
		else if (attr_bool (xin, attrs, "deleteRows", &deleteRows)) ;
		else if (attr_bool (xin, attrs, "selectLockedCells", &selectLockedCells)) ;
		else if (attr_bool (xin, attrs, "sort", &sort)) ;
		else if (attr_bool (xin, attrs, "autoFilter", &autoFilter)) ;
		else if (attr_bool (xin, attrs, "pivotTables", &pivotTables)) ;
		else if (attr_bool (xin, attrs, "selectUnlockedCells", &selectUnlockedCells)) ;

	g_object_set (state->sheet,
		"protected",				 sheet,
		"protected-allow-edit-objects",		 objects,
		"protected-allow-edit-scenarios",	 scenarios,
		"protected-allow-cell-formatting",	 formatCells,
		"protected-allow-column-formatting",	 formatColumns,
		"protected-allow-row-formatting",	 formatRows,
		"protected-allow-insert-columns",	 insertColumns,
		"protected-allow-insert-rows",		 insertRows,
		"protected-allow-insert-hyperlinks",	 insertHyperlinks,
		"protected-allow-delete-columns",	 deleteColumns,
		"protected-allow-delete-rows",		 deleteRows,
		"protected-allow-select-locked-cells",	 selectLockedCells,
		"protected-allow-sort-ranges",		 sort,
		"protected-allow-edit-auto-filters",	 autoFilter,
		"protected-allow-edit-pivottable",	 pivotTables,
		"protected-allow-select-unlocked-cells", selectUnlockedCells,
		NULL);
}

static void
xlsx_cond_fmt_begin (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	char const *refs = NULL;

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (0 == strcmp (attrs[0], "sqref"))
			refs = attrs[1];

	state->cond_regions = xlsx_parse_sqref (xin, refs);

	/* create in first call xlsx_cond_rule to avoid creating condition with
	 * no rules */
	state->conditions = NULL;
}

static void
xlsx_cond_fmt_end (GsfXMLIn *xin, G_GNUC_UNUSED GsfXMLBlob *blob)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	GnmStyle *style = NULL;
	GSList   *ptr;

	if (NULL != state->conditions) {
		style = gnm_style_new ();
		gnm_style_set_conditions (style, state->conditions);
		for (ptr = state->cond_regions ; ptr != NULL ; ptr = ptr->next) {
			gnm_style_ref (style);
			sheet_style_apply_range	(state->sheet, ptr->data, style);
			g_free (ptr->data);
		}
		gnm_style_unref (style);
	} else for (ptr = state->cond_regions ; ptr != NULL ; ptr = ptr->next)
		g_free (ptr->data);
	g_slist_free (state->cond_regions);
	state->cond_regions = NULL;
}

typedef enum {
	XLSX_CF_TYPE_UNDEFINED,

	XLSX_CF_TYPE_EXPRESSION,
	XLSX_CF_TYPE_CELL_IS,
	XLSX_CF_TYPE_COLOR_SCALE,
	XLSX_CF_TYPE_DATA_BAR,
	XLSX_CF_TYPE_ICON_SET,
	XLSX_CF_TYPE_TOP10,
	XLSX_CF_TYPE_UNIQUE_VALUES,
	XLSX_CF_TYPE_DUPLICATE_VALUES,
	XLSX_CF_TYPE_CONTAINS_STR		= GNM_STYLE_COND_CONTAINS_STR,
	XLSX_CF_TYPE_NOT_CONTAINS_STR		= GNM_STYLE_COND_NOT_CONTAINS_STR,
	XLSX_CF_TYPE_BEGINS_WITH		= GNM_STYLE_COND_BEGINS_WITH_STR,
	XLSX_CF_TYPE_ENDS_WITH			= GNM_STYLE_COND_ENDS_WITH_STR,
	XLSX_CF_TYPE_CONTAINS_BLANKS		= GNM_STYLE_COND_CONTAINS_BLANKS,
	XLSX_CF_TYPE_NOT_CONTAINS_BLANKS	= GNM_STYLE_COND_NOT_CONTAINS_BLANKS,
	XLSX_CF_TYPE_CONTAINS_ERRORS		= GNM_STYLE_COND_CONTAINS_ERR,
	XLSX_CF_TYPE_NOT_CONTAINS_ERRORS	= GNM_STYLE_COND_NOT_CONTAINS_ERR,
	XLSX_CF_TYPE_COMPARE_COLUMNS,
	XLSX_CF_TYPE_TIME_PERIOD,
	XLSX_CF_TYPE_ABOVE_AVERAGE
} XlsxCFTypes;
static void
xlsx_cond_fmt_rule_begin (GsfXMLIn *xin, xmlChar const **attrs)
{
	static EnumVal const ops[] = {
		{ "lessThan",		GNM_STYLE_COND_LT },
		{ "lessThanOrEqual",	GNM_STYLE_COND_LTE },
		{ "equal",		GNM_STYLE_COND_EQUAL },
		{ "notEqual",		GNM_STYLE_COND_NOT_EQUAL },
		{ "greaterThanOrEqual",	GNM_STYLE_COND_GTE },
		{ "greaterThan",	GNM_STYLE_COND_GT },
		{ "between",		GNM_STYLE_COND_BETWEEN },
		{ "notBetween",		GNM_STYLE_COND_NOT_BETWEEN },
		{ "containsText",	GNM_STYLE_COND_CONTAINS_STR },
		{ "notContainsText",	GNM_STYLE_COND_NOT_CONTAINS_STR },
		{ "beginsWith",		GNM_STYLE_COND_BEGINS_WITH_STR },
		{ "endsWith",		GNM_STYLE_COND_ENDS_WITH_STR },
		{ "notContain",		GNM_STYLE_COND_NOT_CONTAINS_STR },
		{ NULL, 0 }
	};

	static EnumVal const types[] = {
		{ "expression",		XLSX_CF_TYPE_EXPRESSION },
		{ "cellIs",		XLSX_CF_TYPE_CELL_IS },
		{ "colorScale",		XLSX_CF_TYPE_COLOR_SCALE },
		{ "dataBar",		XLSX_CF_TYPE_DATA_BAR },
		{ "iconSet",		XLSX_CF_TYPE_ICON_SET },
		{ "top10",		XLSX_CF_TYPE_TOP10 },
		{ "uniqueValues",	XLSX_CF_TYPE_UNIQUE_VALUES },
		{ "duplicateValues",	XLSX_CF_TYPE_DUPLICATE_VALUES },
		{ "containsText",	XLSX_CF_TYPE_CONTAINS_STR },
		{ "doesNotContainText",	XLSX_CF_TYPE_NOT_CONTAINS_STR },
		{ "beginsWith",		XLSX_CF_TYPE_BEGINS_WITH },
		{ "endsWith",		XLSX_CF_TYPE_ENDS_WITH },
		{ "containsBlanks",	XLSX_CF_TYPE_CONTAINS_BLANKS },
		{ "containsNoBlanks",	XLSX_CF_TYPE_NOT_CONTAINS_BLANKS },
		{ "containsErrors",	XLSX_CF_TYPE_CONTAINS_ERRORS },
		{ "containsNoErrors",	XLSX_CF_TYPE_NOT_CONTAINS_ERRORS },
		{ "compareColumns",	XLSX_CF_TYPE_COMPARE_COLUMNS },
		{ "timePeriod",		XLSX_CF_TYPE_TIME_PERIOD },
		{ "aboveAverage",	XLSX_CF_TYPE_ABOVE_AVERAGE },
		{ NULL, 0 }
	};

	XLSXReadState  *state = (XLSXReadState *)xin->user_state;
	gboolean	formatRow = FALSE;
	gboolean	stopIfTrue = FALSE;
	gboolean	above = TRUE;
	gboolean	percent = FALSE;
	gboolean	bottom = FALSE;
	int		tmp, dxf = -1;
	/* use custom invalid flag, it is not in MS enum */
	GnmStyleCondOp	op = GNM_STYLE_COND_CUSTOM;
	XlsxCFTypes	type = XLSX_CF_TYPE_UNDEFINED;
	char const	*type_str = _("Undefined");

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_bool (xin, attrs, "formatRow", &formatRow)) ;
		else if (attr_bool (xin, attrs, "stopIfTrue", &stopIfTrue)) ;
		else if (attr_bool (xin, attrs, "above", &above)) ;
		else if (attr_bool (xin, attrs, "percent", &percent)) ;
		else if (attr_bool (xin, attrs, "bottom", &bottom)) ;
		else if (attr_int  (xin, attrs, "dxfId", &dxf)) ;
		else if (attr_enum (xin, attrs, "operator", ops, &tmp))
			op = tmp;
		else if (attr_enum (xin, attrs, "type", types, &tmp)) {
			type = tmp;
			type_str = attrs[1];
		}
#if 0
	"numFmtId"	="ST_NumFmtId" use="optional">
	"priority"	="xs:int" use="required">
	"text"		="xs:string" use="optional">
	"timePeriod"	="ST_TimePeriod" use="optional">
	"col1"		="xs:unsignedInt" use="optional">
	"col2"		="xs:unsignedInt" use="optional">
#endif

	if (dxf >= 0 && NULL != (state->cond.overlay = xlsx_get_dxf (xin, dxf)))
		gnm_style_ref (state->cond.overlay);

	switch (type) {
	case XLSX_CF_TYPE_CELL_IS :
		state->cond.op = op;
		break;
	case XLSX_CF_TYPE_CONTAINS_STR :
	case XLSX_CF_TYPE_NOT_CONTAINS_STR :
	case XLSX_CF_TYPE_BEGINS_WITH :
	case XLSX_CF_TYPE_ENDS_WITH :
	case XLSX_CF_TYPE_CONTAINS_BLANKS :
	case XLSX_CF_TYPE_NOT_CONTAINS_BLANKS :
	case XLSX_CF_TYPE_CONTAINS_ERRORS :
	case XLSX_CF_TYPE_NOT_CONTAINS_ERRORS :
		state->cond.op = type;
		break;

	default :
		xlsx_warning (xin, _("Ignoring unhandled conditional format of type '%s'"), type_str);
	}
	state->count = 0;
}

static void
xlsx_cond_fmt_rule_end (GsfXMLIn *xin, G_GNUC_UNUSED GsfXMLBlob *blob)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	if (gnm_style_cond_is_valid (&state->cond)) {
		if (NULL == state->conditions)
			state->conditions = gnm_style_conditions_new ();
		gnm_style_conditions_insert (state->conditions, &state->cond, -1);
	} else {
		if (NULL != state->cond.texpr[0])
			gnm_expr_top_unref (state->cond.texpr[0]);
		if (NULL != state->cond.texpr[1])
			gnm_expr_top_unref (state->cond.texpr[1]);
		if (NULL != state->cond.overlay)
			gnm_style_unref (state->cond.overlay);
	}
	state->cond.texpr[0] = state->cond.texpr[1] = NULL;
	state->cond.overlay = NULL;
}

static void
xlsx_cond_fmt_formula_end (GsfXMLIn *xin, G_GNUC_UNUSED GsfXMLBlob *blob)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	GnmParsePos pp;
	if (state->count > 1)
		return;

	state->cond.texpr[state->count++] = xlsx_parse_expr (xin, xin->content->str,
		parse_pos_init_sheet (&pp, state->sheet));
}

static void
xlsx_CT_SheetView_begin (GsfXMLIn *xin, xmlChar const **attrs)
{
	static EnumVal const view_types[] = {
		{ "normal",		GNM_SHEET_VIEW_NORMAL_MODE },
		{ "pageBreakPreview",	GNM_SHEET_VIEW_PAGE_BREAK_MODE },
		{ "pageLayout",		GNM_SHEET_VIEW_LAYOUT_MODE },
		{ NULL, 0 }
	};

	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	int showGridLines	= TRUE;
	int showFormulas	= FALSE;
	int showRowColHeaders	= TRUE;
	int showZeros		= TRUE;
	int frozen		= FALSE;
	int frozenSplit		= TRUE;
	int rightToLeft		= FALSE;
	int tabSelected		= FALSE;
	int active		= FALSE;
	int showRuler		= TRUE;
	int showOutlineSymbols	= TRUE;
	int defaultGridColor	= TRUE;
	int showWhiteSpace	= TRUE;
	int scale		= 100;
	int grid_color_index	= -1;
	int tmp;
	GnmSheetViewMode	view_mode = GNM_SHEET_VIEW_NORMAL_MODE;
	GnmCellPos topLeft = { -1, -1 };

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_pos (xin, attrs, "topLeftCell", &topLeft)) ;
		else if (attr_bool (xin, attrs, "showGridLines", &showGridLines)) ;
		else if (attr_bool (xin, attrs, "showFormulas", &showFormulas)) ;
		else if (attr_bool (xin, attrs, "showRowColHeaders", &showRowColHeaders)) ;
		else if (attr_bool (xin, attrs, "showZeros", &showZeros)) ;
		else if (attr_bool (xin, attrs, "frozen", &frozen)) ;
		else if (attr_bool (xin, attrs, "frozenSplit", &frozenSplit)) ;
		else if (attr_bool (xin, attrs, "rightToLeft", &rightToLeft)) ;
		else if (attr_bool (xin, attrs, "tabSelected", &tabSelected)) ;
		else if (attr_bool (xin, attrs, "active", &active)) ;
		else if (attr_bool (xin, attrs, "showRuler", &showRuler)) ;
		else if (attr_bool (xin, attrs, "showOutlineSymbols", &showOutlineSymbols)) ;
		else if (attr_bool (xin, attrs, "defaultGridColor", &defaultGridColor)) ;
		else if (attr_bool (xin, attrs, "showWhiteSpace", &showWhiteSpace)) ;
		else if (attr_int (xin, attrs, "zoomScale", &scale)) ;
		else if (attr_int (xin, attrs, "colorId", &grid_color_index)) ;
		else if (attr_enum (xin, attrs, "view", view_types, &tmp))
			view_mode = tmp;
#if 0
"zoomScaleNormal"		type="xs:unsignedInt" use="optional" default="0"
"zoomScaleSheetLayoutView"	type="xs:unsignedInt" use="optional" default="0"
"zoomScalePageLayoutView"	type="xs:unsignedInt" use="optional" default="0"
"workbookViewId"		type="xs:unsignedInt" use="required"
#endif

	/* get this from the workbookViewId */
	g_return_if_fail (state->sv == NULL);
	state->sv = sheet_get_view (state->sheet, state->wb_view);
	state->pane_pos = XLSX_PANE_TOP_LEFT;

	/* until we import multiple views unfreeze just in case a previous view
	 * had frozen */
	sv_freeze_panes (state->sv, NULL, NULL);

	if (topLeft.col >= 0)
		sv_set_initial_top_left (state->sv, topLeft.col, topLeft.row);
	g_object_set (state->sheet,
		"text-is-rtl",		rightToLeft,
		"display-formulas",	showFormulas,
		"display-zeros",	showZeros,
		"display-grid",		showGridLines,
		"display-column-header", showRowColHeaders,
		"display-row-header",	showRowColHeaders,
		"display-outlines",	showOutlineSymbols,
		"zoom-factor",		((double)scale) / 100.,
		NULL);
#if 0
		gboolean active			= FALSE;
		gboolean showRuler		= TRUE;
		gboolean showWhiteSpace		= TRUE;
#endif

#if 0
	g_object_set (state->sv,
		"displayMode",	view_mode,
		NULL);
#endif

	if (!defaultGridColor && grid_color_index >= 0)
		sheet_style_set_auto_pattern_color (state->sheet,
			style_color_new_go (indexed_color (state, grid_color_index)));
	if (tabSelected)
		wb_view_sheet_focus (state->wb_view, state->sheet);
}
static void
xlsx_CT_SheetView_end (GsfXMLIn *xin, G_GNUC_UNUSED GsfXMLBlob *blob)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	g_return_if_fail (state->sv != NULL);
	state->sv = NULL;
}

static EnumVal const pane_types[] = {
	{ "topLeft",     XLSX_PANE_TOP_LEFT },
	{ "topRight",    XLSX_PANE_TOP_RIGHT },
	{ "bottomLeft",  XLSX_PANE_BOTTOM_LEFT },
	{ "bottomRight", XLSX_PANE_BOTTOM_RIGHT },
	{ NULL, 0 }
};
static void
xlsx_CT_Selection (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	GnmCellPos edit_pos = { -1, -1 };
	int i, sel_with_edit_pos = 0;
	char const *refs = NULL;
	XLSXPanePos pane_pos = XLSX_PANE_TOP_LEFT;
	GnmRange r;
	GSList *ptr, *accum = NULL;

	g_return_if_fail (state->sv != NULL);

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (0 == strcmp (attrs[0], "sqref"))
			refs = attrs[1];
		else if (attr_enum (xin, attrs, "activePane", pane_types, &i))
			pane_pos = i;
		else if (attr_pos (xin, attrs, "activeCell", &edit_pos)) ;
		else if (attr_int (xin, attrs, "activeCellId", &sel_with_edit_pos))
			;

	if (pane_pos != state->pane_pos)
		return;

	for (i = 0 ; NULL != refs && *refs ; i++) {
		if (NULL == (refs = cellpos_parse (refs, gnm_sheet_get_size (state->sheet), &r.start, FALSE)))
			return;

		if (*refs == '\0' || *refs == ' ')
			r.end = r.start;
		else if (*refs != ':' ||
			 NULL == (refs = cellpos_parse (refs + 1, gnm_sheet_get_size (state->sheet), &r.end, FALSE)))
			return;

		if (i == 0)
			sv_selection_reset (state->sv);

		/* gnumeric assumes the edit_pos is in the last selected range.
		 * We need to re-order the selection list. */
		if (i <= sel_with_edit_pos && edit_pos.col >= 0)
			accum = g_slist_prepend (accum, gnm_range_dup (&r));
		else
			sv_selection_add_range (state->sv, &r);
		while (*refs == ' ')
			refs++;
	}

	if (NULL != accum) {
		accum = g_slist_reverse (accum);
		for (ptr = accum ; ptr != NULL ; ptr = ptr->next) {
			sv_selection_add_range (state->sv, ptr->data);
			g_free (ptr->data);
		}
		sv_set_edit_pos (state->sv, &edit_pos);
		g_slist_free (accum);
	}
}

static void
xlsx_CT_PivotSelection (GsfXMLIn *xin, xmlChar const **attrs)
{
#if 0
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
    <xsd:attribute name="pane" type="ST_Pane" use="optional" default="topLeft">
    <xsd:attribute name="showHeader" type="xsd:boolean" default="false">
    <xsd:attribute name="label" type="xsd:boolean" default="false">
    <xsd:attribute name="data" type="xsd:boolean" default="false">
    <xsd:attribute name="extendable" type="xsd:boolean" default="false">
    <xsd:attribute name="count" type="xsd:unsignedInt" default="0">
    <xsd:attribute name="axis" type="ST_Axis" use="optional">
    <xsd:attribute name="dimension" type="xsd:unsignedInt" default="0">
    <xsd:attribute name="start" type="xsd:unsignedInt" default="0">
    <xsd:attribute name="min" type="xsd:unsignedInt" default="0">
    <xsd:attribute name="max" type="xsd:unsignedInt" default="0">
    <xsd:attribute name="activeRow" type="xsd:unsignedInt" default="0">
    <xsd:attribute name="activeCol" type="xsd:unsignedInt" default="0">
    <xsd:attribute name="previousRow" type="xsd:unsignedInt" default="0">
    <xsd:attribute name="previousCol" type="xsd:unsignedInt" default="0">
    <xsd:attribute name="click" type="xsd:unsignedInt" default="0">
    <xsd:attribute ref="r:id" use="optional">
#endif
}

static void
xlsx_CT_PivotArea (GsfXMLIn *xin, xmlChar const **attrs)
{
#if 0
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
    <xsd:attribute name="field" use="optional" type="xsd:int">
    <xsd:attribute name="type" type="ST_PivotAreaType" default="normal">
    <xsd:attribute name="dataOnly" type="xsd:boolean" default="true">
    <xsd:attribute name="labelOnly" type="xsd:boolean" default="false">
    <xsd:attribute name="grandRow" type="xsd:boolean" default="false">
    <xsd:attribute name="grandCol" type="xsd:boolean" default="false">
    <xsd:attribute name="cacheIndex" type="xsd:boolean" default="false">
    <xsd:attribute name="outline" type="xsd:boolean" default="true">
    <xsd:attribute name="offset" type="ST_Ref">
    <xsd:attribute name="collapsedLevelsAreSubtotals" type="xsd:boolean" default="false">
    <xsd:attribute name="axis" type="ST_Axis" use="optional">
    <xsd:attribute name="fieldPosition" type="xsd:unsignedInt" use="optional">
#endif
}
static void
xlsx_CT_PivotAreaReferences (GsfXMLIn *xin, xmlChar const **attrs)
{
#if 0
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
#endif
}
static void
xlsx_CT_PivotAreaReference (GsfXMLIn *xin, xmlChar const **attrs)
{
#if 0
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
    <xsd:attribute name="field" use="optional" type="xsd:unsignedInt">
    <xsd:attribute name="count" type="xsd:unsignedInt">
    <xsd:attribute name="selected" type="xsd:boolean" default="true">
    <xsd:attribute name="byPosition" type="xsd:boolean" default="false">
    <xsd:attribute name="relative" type="xsd:boolean" default="false">
    <xsd:attribute name="defaultSubtotal" type="xsd:boolean" default="false">
    <xsd:attribute name="sumSubtotal" type="xsd:boolean" default="false">
    <xsd:attribute name="countASubtotal" type="xsd:boolean" default="false">
    <xsd:attribute name="avgSubtotal" type="xsd:boolean" default="false">
    <xsd:attribute name="maxSubtotal" type="xsd:boolean" default="false">
    <xsd:attribute name="minSubtotal" type="xsd:boolean" default="false">
    <xsd:attribute name="productSubtotal" type="xsd:boolean" default="false">
    <xsd:attribute name="countSubtotal" type="xsd:boolean" default="false">
    <xsd:attribute name="stdDevSubtotal" type="xsd:boolean" default="false">
    <xsd:attribute name="stdDevPSubtotal" type="xsd:boolean" default="false">
    <xsd:attribute name="varSubtotal" type="xsd:boolean" default="false">
    <xsd:attribute name="varPSubtotal" type="xsd:boolean" default="false">
#endif
}

static void
xlsx_CT_Pane (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	GnmCellPos topLeft = { 0, 0 };
	int tmp;
	gnm_float xSplit = -1., ySplit = -1.;
	gboolean frozen = FALSE;

	g_return_if_fail (state->sv != NULL);

	/* <pane xSplit="2" ySplit="3" topLeftCell="J15" activePane="bottomRight" state="frozen"/> */
	state->pane_pos = XLSX_PANE_TOP_LEFT;
	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (0 == strcmp (attrs[0], "state"))
			frozen = (0 == strcmp (attrs[1], "frozen"));
		else if (attr_pos (xin, attrs, "topLeftCell", &topLeft)) ;
		else if (attr_float (xin, attrs, "xSplit", &xSplit)) ;
		else if (attr_float (xin, attrs, "ySplit", &ySplit)) ;
		else if (attr_enum (xin, attrs, "pane", pane_types, &tmp))
			state->pane_pos = tmp;

	if (frozen) {
		GnmCellPos frozen, unfrozen;
		frozen = unfrozen = state->sv->initial_top_left;
		if (xSplit > 0)
			unfrozen.col += xSplit;
		else
			topLeft.col = state->sv->initial_top_left.col;
		if (ySplit > 0)
			unfrozen.row += ySplit;
		else
			topLeft.row = state->sv->initial_top_left.row;
		sv_freeze_panes (state->sv, &frozen, &unfrozen);
		sv_set_initial_top_left (state->sv, topLeft.col, topLeft.row);
	}
}

static void
xlsx_ole_object (GsfXMLIn *xin, xmlChar const **attrs)
{
#if 0
	XLSXReadState *state = (XLSXReadState *)xin->user_state;

	/* <oleObject progId="Wordpad.Document.1" shapeId="1032" r:id="rId5"/> */
	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		;
#endif
}

static void
xlsx_CT_HyperLinks (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	gboolean has_ref = FALSE;
	GnmStyle *style;
	GnmRange r;
	GType link_type = 0;
	GnmHLink *link = NULL;
	xmlChar const *target = NULL;
	xmlChar const *tooltip = NULL;
	xmlChar const *extern_id = NULL;

	/* <hyperlink ref="A42" r:id="rId1"/> */
	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_range (xin, attrs, "ref", &r))
			has_ref = TRUE;
		else if (0 == strcmp (attrs[0], "location"))
			target = attrs[1];
		else if (0 == strcmp (attrs[0], "tooltip"))
			tooltip = attrs[1];
		else if (gsf_xml_in_namecmp (xin, attrs[0], XL_NS_DOC_REL, "id"))
			extern_id = attrs[1];
#if 0 /* ignore "display" on import, it always seems to be the cell content */
		else if (0 == strcmp (attrs[0], "display"))
#endif
	if (!has_ref)
		return;

	if (NULL != target)
		link_type = gnm_hlink_cur_wb_get_type ();
	else if (NULL != extern_id) {
		GsfOpenPkgRel const *rel = gsf_open_pkg_lookup_rel_by_id (
			gsf_xml_in_get_input (xin), extern_id);
		if (NULL != rel &&
		    gsf_open_pkg_rel_is_extern (rel) &&
		    0 == strcmp (gsf_open_pkg_rel_get_type (rel),
				 "http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink")) {
			target = gsf_open_pkg_rel_get_target (rel);
			if (NULL != target) {
				if (0 == strncmp (target, "mailto:", 7))
					link_type = gnm_hlink_email_get_type ();
				else
					link_type = gnm_hlink_url_get_type ();
			}
		}
	}

	if (0 == link_type) {
		xlsx_warning (xin, _("Unknown type of hyperlink"));
		return;
	}

	link = g_object_new (link_type, NULL);
	if (NULL != target)
		gnm_hlink_set_target (link, target);
	if (NULL != tooltip)
		gnm_hlink_set_tip  (link, tooltip);
	style = gnm_style_new ();
	gnm_style_set_hlink (style, link);
	sheet_style_apply_range	(state->sheet, &r, style);
}

#ifdef HAVE_GSF_OPEN_PKG_FOREACH_REL
static void
cb_find_pivots (GsfInput *opkg, GsfOpenPkgRel const *rel, gpointer    user_data)
{
	XLSXReadState *state = user_data;
	GsfInput *part_stream;
	char const *t = gsf_open_pkg_rel_get_type (rel);

	if (NULL != t &&
	    0 == strcmp (t, "http://schemas.openxmlformats.org/officeDocument/2006/relationships/pivotTable") &&
	    NULL != (part_stream = gsf_open_pkg_open_rel (opkg, rel, NULL)))
		xlsx_parse_stream (state, part_stream, xlsx_pivot_table_dtd);
}
#endif

static void
xlsx_CT_worksheet (GsfXMLIn *xin, G_GNUC_UNUSED GsfXMLBlob *blob)
{
#ifdef HAVE_GSF_OPEN_PKG_FOREACH_REL
	gsf_open_pkg_foreach_rel (gsf_xml_in_get_input (xin),
		&cb_find_pivots, (XLSXReadState *)xin->user_state);
#endif
}

static GsfXMLInNode const xlsx_sheet_dtd[] = {
GSF_XML_IN_NODE_FULL (START, START, -1, NULL, GSF_XML_NO_CONTENT, FALSE, TRUE, NULL, NULL, 0),
GSF_XML_IN_NODE_FULL (START, SHEET, XL_NS_SS, "worksheet", GSF_XML_NO_CONTENT, FALSE, TRUE, NULL, &xlsx_CT_worksheet, 0),
  GSF_XML_IN_NODE (SHEET, PROPS, XL_NS_SS, "sheetPr", GSF_XML_NO_CONTENT, &xlsx_CT_SheetPr, NULL),
    GSF_XML_IN_NODE (PROPS, OUTLINE_PROPS, XL_NS_SS, "outlinePr", GSF_XML_NO_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (PROPS, TAB_COLOR, XL_NS_SS, "tabColor", GSF_XML_NO_CONTENT, &xlsx_sheet_tabcolor, NULL),
    GSF_XML_IN_NODE (PROPS, PAGE_SETUP, XL_NS_SS, "pageSetUpPr", GSF_XML_NO_CONTENT, &xlsx_sheet_page_setup, NULL),
  GSF_XML_IN_NODE (SHEET, DIMENSION, XL_NS_SS, "dimension", GSF_XML_NO_CONTENT, NULL, NULL),
  GSF_XML_IN_NODE (SHEET, VIEWS, XL_NS_SS, "sheetViews", GSF_XML_NO_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (VIEWS, VIEW, XL_NS_SS, "sheetView",  GSF_XML_NO_CONTENT, &xlsx_CT_SheetView_begin, &xlsx_CT_SheetView_end),
      GSF_XML_IN_NODE (VIEW, PANE, XL_NS_SS, "pane",  GSF_XML_NO_CONTENT, &xlsx_CT_Pane, NULL),
      GSF_XML_IN_NODE (VIEW, SELECTION, XL_NS_SS, "selection",  GSF_XML_NO_CONTENT, &xlsx_CT_Selection, NULL),
      GSF_XML_IN_NODE (VIEW, PIV_SELECTION, XL_NS_SS, "pivotSelection",  GSF_XML_NO_CONTENT, &xlsx_CT_PivotSelection, NULL),
        GSF_XML_IN_NODE (PIV_SELECTION, PIV_AREA, XL_NS_SS, "pivotArea",  GSF_XML_NO_CONTENT, &xlsx_CT_PivotArea, NULL),
          GSF_XML_IN_NODE (PIV_AREA, PIV_AREA_REFS, XL_NS_SS, "references",  GSF_XML_NO_CONTENT, &xlsx_CT_PivotAreaReferences, NULL),
            GSF_XML_IN_NODE (PIV_AREA_REFS, PIV_AREA_REF, XL_NS_SS, "reference",  GSF_XML_NO_CONTENT, &xlsx_CT_PivotAreaReference, NULL),

  GSF_XML_IN_NODE (SHEET, DEFAULT_FMT, XL_NS_SS, "sheetFormatPr", GSF_XML_NO_CONTENT, &xlsx_CT_SheetFormatPr, NULL),

  GSF_XML_IN_NODE (SHEET, COLS,	XL_NS_SS, "cols", GSF_XML_NO_CONTENT, NULL, xlsx_CT_RowsCols_end),
    GSF_XML_IN_NODE (COLS, COL,	XL_NS_SS, "col", GSF_XML_NO_CONTENT, &xlsx_CT_Col, NULL),

  GSF_XML_IN_NODE (SHEET, CONTENT, XL_NS_SS, "sheetData", GSF_XML_NO_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (CONTENT, ROW, XL_NS_SS, "row", GSF_XML_NO_CONTENT, &xlsx_CT_Row, NULL),
      GSF_XML_IN_NODE (ROW, CELL, XL_NS_SS, "c", GSF_XML_NO_CONTENT, &xlsx_cell_begin, &xlsx_cell_end),
	GSF_XML_IN_NODE (CELL, VALUE, XL_NS_SS, "v", GSF_XML_CONTENT, NULL, &xlsx_cell_val_end),
	GSF_XML_IN_NODE (CELL, FMLA, XL_NS_SS,  "f", GSF_XML_CONTENT, &xlsx_cell_expr_begin, &xlsx_cell_expr_end),

  GSF_XML_IN_NODE (SHEET, CALC_PR, XL_NS_SS, "sheetCalcPr", GSF_XML_NO_CONTENT, NULL, NULL),
  GSF_XML_IN_NODE (SHEET, CT_SortState, XL_NS_SS, "sortState", GSF_XML_NO_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (CT_SortState, CT_SortCondition, XL_NS_SS, "sortCondition", GSF_XML_NO_CONTENT, NULL, NULL),
  GSF_XML_IN_NODE (SHEET, SCENARIOS, XL_NS_SS, "scenarios", GSF_XML_NO_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (SCENARIOS, INPUT_CELLS, XL_NS_SS, "inputCells", GSF_XML_NO_CONTENT, NULL, NULL),
  GSF_XML_IN_NODE (SHEET, PROTECTED_RANGES, XL_NS_SS, "protectedRanges", GSF_XML_NO_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (SHEET, PROTECTED_RANGE, XL_NS_SS, "protectedRange", GSF_XML_NO_CONTENT, NULL, NULL),

  GSF_XML_IN_NODE (SHEET, CT_AutoFilter, XL_NS_SS, "autoFilter", GSF_XML_NO_CONTENT,
		   &xlsx_CT_AutoFilter_begin, &xlsx_CT_AutoFilter_end),
    GSF_XML_IN_NODE (CT_AutoFilter, CT_SortState, XL_NS_SS, "sortState", GSF_XML_NO_CONTENT, NULL, NULL), /* 2nd Def */
    GSF_XML_IN_NODE (CT_AutoFilter, CT_FilterColumn, XL_NS_SS,    "filterColumn", GSF_XML_NO_CONTENT,
		     &xlsx_CT_FilterColumn_begin, NULL),
      GSF_XML_IN_NODE (CT_FilterColumn, CT_Filters, XL_NS_SS, "filters", GSF_XML_NO_CONTENT,
		       &xlsx_CT_Filters_begin, &xlsx_CT_Filters_end),
        GSF_XML_IN_NODE (CT_Filters, CT_Filter, XL_NS_SS, "filter", GSF_XML_NO_CONTENT, &xlsx_CT_Filter, NULL),
      GSF_XML_IN_NODE (CT_FilterColumn, CT_CustomFilters, XL_NS_SS, "customFilters", GSF_XML_NO_CONTENT,
		       &xlsx_CT_CustomFilters_begin, &xlsx_CT_CustomFilters_end),
        GSF_XML_IN_NODE (CT_CustomFilters, CT_CustomFilter, XL_NS_SS, "customFilter", GSF_XML_NO_CONTENT, &xlsx_CT_CustomFilter, NULL),
      GSF_XML_IN_NODE (CT_FilterColumn, CT_Top10, XL_NS_SS, "top10", GSF_XML_NO_CONTENT, &xlsx_CT_Top10, NULL),
      GSF_XML_IN_NODE (CT_FilterColumn, CT_DynamicFilter, XL_NS_SS, "dynamicFilter", GSF_XML_NO_CONTENT, &xlsx_CT_DynamicFilter, NULL),
      GSF_XML_IN_NODE (CT_FilterColumn, CT_ColorFilter, XL_NS_SS, "colorFilter", GSF_XML_NO_CONTENT, NULL, NULL),
      GSF_XML_IN_NODE (CT_FilterColumn, CT_IconFilter, XL_NS_SS, "iconFilter", GSF_XML_NO_CONTENT, NULL, NULL),

  GSF_XML_IN_NODE (SHEET, CT_DataValidations, XL_NS_SS, "dataValidations", GSF_XML_NO_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (CT_DataValidations, CT_DataValidation, XL_NS_SS, "dataValidation", GSF_XML_NO_CONTENT,
		     &xlsx_CT_DataValidation_begin, &xlsx_CT_DataValidation_end),
      GSF_XML_IN_NODE_FULL (CT_DataValidation, VAL_FORMULA1, XL_NS_SS, "formula1", GSF_XML_CONTENT, FALSE, FALSE, NULL, &xlsx_validation_expr, 0),
      GSF_XML_IN_NODE_FULL (CT_DataValidation, VAL_FORMULA2, XL_NS_SS, "formula2", GSF_XML_CONTENT, FALSE, FALSE, NULL, &xlsx_validation_expr, 1),

  GSF_XML_IN_NODE (SHEET, MERGES, XL_NS_SS, "mergeCells", GSF_XML_NO_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (MERGES, MERGE, XL_NS_SS, "mergeCell", GSF_XML_NO_CONTENT, &xlsx_CT_MergeCell, NULL),

  GSF_XML_IN_NODE (SHEET, DRAWING, XL_NS_SS, "drawing", GSF_XML_NO_CONTENT, &xlsx_sheet_drawing, NULL),

  GSF_XML_IN_NODE (SHEET, PROTECTION, XL_NS_SS, "sheetProtection", GSF_XML_NO_CONTENT, &xlsx_CT_SheetProtection, NULL),
  GSF_XML_IN_NODE (SHEET, PHONETIC, XL_NS_SS, "phoneticPr", GSF_XML_NO_CONTENT, NULL, NULL),
  GSF_XML_IN_NODE (SHEET, COND_FMTS, XL_NS_SS, "conditionalFormatting", GSF_XML_NO_CONTENT,
		   &xlsx_cond_fmt_begin, &xlsx_cond_fmt_end),
    GSF_XML_IN_NODE (COND_FMTS, COND_RULE, XL_NS_SS, "cfRule", GSF_XML_NO_CONTENT,
		   &xlsx_cond_fmt_rule_begin, &xlsx_cond_fmt_rule_end),
      GSF_XML_IN_NODE (COND_RULE, COND_FMLA, XL_NS_SS, "formula", GSF_XML_CONTENT, NULL, &xlsx_cond_fmt_formula_end),
      GSF_XML_IN_NODE (COND_RULE, COND_COLOR_SCALE, XL_NS_SS, "colorScale", GSF_XML_NO_CONTENT, NULL, NULL),
        GSF_XML_IN_NODE (COND_COLOR_SCALE, CFVO, XL_NS_SS, "cfvo", GSF_XML_NO_CONTENT, NULL, NULL),
        GSF_XML_IN_NODE (COND_COLOR_SCALE, COND_COLOR, XL_NS_SS, "color", GSF_XML_NO_CONTENT, NULL, NULL),
      GSF_XML_IN_NODE (COND_RULE, COND_DATA_BAR, XL_NS_SS, "dataBar", GSF_XML_NO_CONTENT, NULL, NULL),
      GSF_XML_IN_NODE (COND_RULE, COND_ICON_SET, XL_NS_SS, "iconSet", GSF_XML_NO_CONTENT, NULL, NULL),
        GSF_XML_IN_NODE (COND_ICON_SET, CFVO, XL_NS_SS, "cfvo", GSF_XML_NO_CONTENT, NULL, NULL),	/* 2nd Def */

  GSF_XML_IN_NODE (SHEET, HYPERLINKS, XL_NS_SS, "hyperlinks", GSF_XML_NO_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (HYPERLINKS, HYPERLINK, XL_NS_SS, "hyperlink", GSF_XML_NO_CONTENT, &xlsx_CT_HyperLinks, NULL),

  GSF_XML_IN_NODE (SHEET, PRINT_OPTS, XL_NS_SS, "printOptions", GSF_XML_NO_CONTENT, NULL, NULL),
  GSF_XML_IN_NODE (SHEET, PRINT_MARGINS, XL_NS_SS, "pageMargins", GSF_XML_NO_CONTENT, &xlsx_CT_PageMargins, NULL),
  GSF_XML_IN_NODE (SHEET, PRINT_SETUP, XL_NS_SS, "pageSetup", GSF_XML_NO_CONTENT, NULL, NULL),
  GSF_XML_IN_NODE (SHEET, PRINT_HEADER_FOOTER, XL_NS_SS, "headerFooter", GSF_XML_NO_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (PRINT_HEADER_FOOTER, ODD_HEADER, XL_NS_SS, "oddHeader", GSF_XML_NO_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (PRINT_HEADER_FOOTER, ODD_FOOTER, XL_NS_SS, "oddFooter", GSF_XML_NO_CONTENT, NULL, NULL),

  GSF_XML_IN_NODE_FULL (SHEET, ROW_BREAKS, XL_NS_SS, "rowBreaks", GSF_XML_NO_CONTENT,
			FALSE, FALSE, &xlsx_CT_PageBreaks_begin, &xlsx_CT_PageBreaks_end, 1),
    GSF_XML_IN_NODE (ROW_BREAKS, CT_PageBreak, XL_NS_SS, "brk", GSF_XML_NO_CONTENT, &xlsx_CT_PageBreak, NULL),
  GSF_XML_IN_NODE_FULL (SHEET, COL_BREAKS, XL_NS_SS, "colBreaks", GSF_XML_NO_CONTENT,
			FALSE, FALSE, &xlsx_CT_PageBreaks_begin, &xlsx_CT_PageBreaks_end, 0),
    GSF_XML_IN_NODE (COL_BREAKS, CT_PageBreak, XL_NS_SS, "brk", GSF_XML_NO_CONTENT, NULL, NULL), /* 2nd Def */

  GSF_XML_IN_NODE (SHEET, LEGACY_DRAW, XL_NS_SS, "legacyDrawing", GSF_XML_NO_CONTENT, &xlsx_sheet_legacy_drawing, NULL),
  GSF_XML_IN_NODE (SHEET, OLE_OBJECTS, XL_NS_SS, "oleObjects", GSF_XML_NO_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (OLE_OBJECTS, OLE_OBJECT, XL_NS_SS, "oleObject", GSF_XML_NO_CONTENT, &xlsx_ole_object, NULL),

GSF_XML_IN_NODE_END
};

/****************************************************************************/

static void
xlsx_CT_PivotCache (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	xmlChar const *id = NULL;
	xmlChar const *cacheId = NULL;

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (gsf_xml_in_namecmp (xin, attrs[0], XL_NS_DOC_REL, "id"))
			id = attrs[1];
		else if (0 == strcmp (attrs[0], "cacheId"))
			cacheId = attrs[1];

	if (NULL != id && NULL != cacheId) {
		g_return_if_fail (NULL == state->pivot.cache);

		xlsx_parse_rel_by_id (xin, id,
			xlsx_pivot_cache_def_dtd, xlsx_ns);

		g_return_if_fail (NULL != state->pivot.cache);

		/* absorb the reference to the cache */
		g_hash_table_replace (state->pivot.cache_by_id,
			g_strdup (cacheId), state->pivot.cache);
		state->pivot.cache = NULL;
	}
}

static void
xlsx_CT_WorkbookPr (GsfXMLIn *xin, xmlChar const **attrs)
{
	static EnumVal const switchModes[] = {
		{ "on",	 TRUE },
		{ "1",	 TRUE },
		{ "true", TRUE },
		{ "off", FALSE },
		{ "0", FALSE },
		{ "false", FALSE },
		{ NULL, 0 }
	};
	int tmp;
	XLSXReadState *state = (XLSXReadState *)xin->user_state;

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_enum (xin, attrs, "date1904", switchModes, &tmp))
			workbook_set_1904 (state->wb, tmp);
}

static void
xlsx_CT_CalcPr (GsfXMLIn *xin, xmlChar const **attrs)
{
	static EnumVal const calcModes[] = {
		{ "manual",	 FALSE },
		{ "auto",	 TRUE },
		{ "autoNoTable", TRUE },
		{ NULL, 0 }
	};
	static EnumVal const refModes[] = {
		{ "A1",		TRUE },
		{ "R1C1",	FALSE },
		{ NULL, 0 }
	};
	int tmp;
	gnm_float delta;
	XLSXReadState *state = (XLSXReadState *)xin->user_state;

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_enum (xin, attrs, "calcMode", calcModes, &tmp))
			workbook_set_recalcmode (state->wb, tmp);
		else if (attr_bool (xin, attrs, "fullCalcOnLoad", &tmp))
			;
		else if (attr_enum (xin, attrs, "refMode", refModes, &tmp))
			;
		else if (attr_bool (xin, attrs, "iterate", &tmp))
			workbook_iteration_enabled (state->wb, tmp);
		else if (attr_int (xin, attrs, "iterateCount", &tmp))
			workbook_iteration_max_number (state->wb, tmp);
		else if (attr_float (xin, attrs, "iterateDelta", &delta))
			workbook_iteration_tolerance (state->wb, delta);
		else if (attr_bool (xin, attrs, "fullPrecision", &tmp))
			;
		else if (attr_bool (xin, attrs, "calcCompleted", &tmp))
			;
		else if (attr_bool (xin, attrs, "calcOnSave", &tmp))
			;
		else if (attr_bool (xin, attrs, "conncurrentCalc", &tmp))
			;
		else if (attr_bool (xin, attrs, "forceFullCalc", &tmp))
			;
		else if (attr_int (xin, attrs, "concurrentManualCalc", &tmp))
			;
}

static void
xlsx_sheet_begin (GsfXMLIn *xin, xmlChar const **attrs)
{
	static EnumVal const visibilities[] = {
		{ "visible",	GNM_SHEET_VISIBILITY_VISIBLE },
		{ "hidden",	GNM_SHEET_VISIBILITY_HIDDEN },
		{ "veryHidden",	GNM_SHEET_VISIBILITY_VERY_HIDDEN },
		{ NULL, 0 }
	};
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	char const *name = NULL;
	char const *part_id = NULL;
	Sheet *sheet;
	int viz = (int)GNM_SHEET_VISIBILITY_VISIBLE;

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (0 == strcmp (attrs[0], "name"))
			name = attrs[1];
		else if (attr_enum (xin, attrs, "state", visibilities, &viz))
			; /* Nothing */
		else if (gsf_xml_in_namecmp (xin, attrs[0], XL_NS_DOC_REL, "id"))
			part_id = attrs[1];

	if (NULL == name) {
		xlsx_warning (xin, _("Ignoring a sheet without a name"));
		return;
	}

	sheet =  workbook_sheet_by_name (state->wb, name);
	if (NULL == sheet) {
		sheet = sheet_new (state->wb, name, XLSX_MaxCol, XLSX_MaxRow);
		workbook_sheet_attach (state->wb, sheet);
	}
	g_object_set (sheet, "visibility", viz, NULL);

	g_object_set_data_full (G_OBJECT (sheet), "_XLSX_RelID", g_strdup (part_id),
		(GDestroyNotify) g_free);
}

static void
xlsx_wb_name_begin (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	const char *name = NULL;
	int sheet_idx = -1;

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2) {
		if (0 == strcmp (attrs[0], "name"))
			name = attrs[1];
		else if (attr_int (xin, attrs, "localSheetId", &sheet_idx))
			; /* Nothing */
	}

	state->defined_name = g_strdup (name);
	state->defined_name_sheet =
		sheet_idx >= 0
		? workbook_sheet_by_index (state->wb, sheet_idx)
		: NULL;
}

static void
xlsx_wb_name_end (GsfXMLIn *xin, G_GNUC_UNUSED GsfXMLBlob *blob)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	GnmParsePos pp;
	Sheet *sheet = state->defined_name_sheet;
	GnmNamedExpr *nexpr;
	char *error_msg = NULL;

	g_return_if_fail (state->defined_name != NULL);

	parse_pos_init (&pp, state->wb, sheet, 0, 0);

	if (g_str_has_prefix (state->defined_name, "_xlnm.")) {
		gboolean editable = (0 == strcmp (state->defined_name + 6, "Sheet_Title"));
		nexpr = expr_name_add (&pp, state->defined_name + 6,
				       gnm_expr_top_new_constant (value_new_empty ()),
				       &error_msg, TRUE, NULL);
		nexpr->is_permanent = TRUE;
		nexpr->is_editable = editable;
	} else
		nexpr = expr_name_add (&pp, state->defined_name,
				       gnm_expr_top_new_constant (value_new_empty ()),
				       &error_msg, TRUE, NULL);

	if (nexpr) {
		state->delayed_names =
			g_list_prepend (state->delayed_names, sheet);
		state->delayed_names =
			g_list_prepend (state->delayed_names,
					g_strdup (xin->content->str));
		state->delayed_names =
			g_list_prepend (state->delayed_names, nexpr);
	} else {
		xlsx_warning (xin, _("Failed to define name: %s"), error_msg);
		g_free (error_msg);
	}

	g_free (state->defined_name);
	state->defined_name = NULL;
}

static void
handle_delayed_names (GsfXMLIn *xin)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	GList *l;

	for (l = state->delayed_names; l; l = l->next->next->next) {
		GnmNamedExpr *nexpr = l->data;
		char *expr_str = l->next->data;
		Sheet *sheet = l->next->next->data;
		GnmExprTop const *texpr;
		GnmParsePos pp;

		parse_pos_init (&pp, state->wb, sheet, 0, 0);
		texpr = xlsx_parse_expr (xin, expr_str, &pp);
		if (texpr) {
			expr_name_set_expr (nexpr, texpr);
		}
		g_free (expr_str);
	}

	g_list_free (state->delayed_names);
	state->delayed_names = NULL;
}

static void
xlsx_wb_names_end (GsfXMLIn *xin, G_GNUC_UNUSED GsfXMLBlob *blob)
{
	handle_delayed_names (xin);
}


/**************************************************************************************************/

static void
xlsx_read_external_book (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	GsfOpenPkgRel const *rel = gsf_open_pkg_lookup_rel_by_type (
		gsf_xml_in_get_input (xin),
		"http://schemas.openxmlformats.org/officeDocument/2006/relationships/externalLinkPath");
	if (NULL != rel && gsf_open_pkg_rel_is_extern (rel))
		state->external_ref = xlsx_conventions_add_extern_ref (
			state->convs, gsf_open_pkg_rel_get_target (rel));
}
static void
xlsx_read_external_book_end (GsfXMLIn *xin, G_GNUC_UNUSED GsfXMLBlob *blob)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	state->external_ref = NULL;
}
static void
xlsx_read_external_sheetname (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (0 == strcmp (attrs[0], "val"))
			workbook_sheet_attach (state->external_ref,
				state->external_ref_sheet = sheet_new (state->external_ref, attrs[1], 256, 65536));
}
static void
xlsx_read_external_sheetname_end (GsfXMLIn *xin, G_GNUC_UNUSED GsfXMLBlob *blob)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	state->external_ref_sheet = NULL;
}

static GsfXMLInNode const xlsx_extern_dtd[] = {
GSF_XML_IN_NODE_FULL (START, START, -1, NULL, GSF_XML_NO_CONTENT, FALSE, TRUE, NULL, NULL, 0),
GSF_XML_IN_NODE_FULL (START, LINK, XL_NS_SS, "externalLink", GSF_XML_NO_CONTENT, TRUE, TRUE, xlsx_read_external_book, xlsx_read_external_book_end, 0),
  GSF_XML_IN_NODE (LINK, BOOK, XL_NS_SS, "externalBook", GSF_XML_NO_CONTENT, NULL, NULL),
  GSF_XML_IN_NODE (BOOK, SHEET_NAMES, XL_NS_SS, "sheetNames", GSF_XML_NO_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (SHEET_NAMES, SHEET_NAME, XL_NS_SS, "sheetName", GSF_XML_NO_CONTENT, xlsx_read_external_sheetname, xlsx_read_external_sheetname_end),
  GSF_XML_IN_NODE (BOOK, SHEET_DATASET, XL_NS_SS, "sheetDataSet", GSF_XML_NO_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (SHEET_DATASET, SHEET_DATA, XL_NS_SS, "sheetData", GSF_XML_NO_CONTENT, NULL, NULL),
      GSF_XML_IN_NODE (SHEET_DATA, ROW, XL_NS_SS, "row", GSF_XML_NO_CONTENT, NULL, NULL),
        GSF_XML_IN_NODE (ROW, CELL, XL_NS_SS, "cell", GSF_XML_NO_CONTENT, NULL, NULL),
          GSF_XML_IN_NODE (CELL, VAL, XL_NS_SS, "v", GSF_XML_NO_CONTENT, NULL, NULL),

GSF_XML_IN_NODE_END
};

static void
xlsx_wb_external_ref (GsfXMLIn *xin, xmlChar const **attrs)
{
	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (gsf_xml_in_namecmp (xin, attrs[0], XL_NS_DOC_REL, "id"))
			xlsx_parse_rel_by_id (xin, attrs[1], xlsx_extern_dtd, xlsx_ns);
}

/**************************************************************************************************/

static void
xlsx_run_weight (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (gsf_xml_in_namecmp (xin, attrs[0], XL_NS_SS, "val")) {
			PangoAttribute *attr = pango_attr_weight_new (strcmp (attrs[1], "true")? PANGO_WEIGHT_NORMAL: PANGO_WEIGHT_BOLD);
			if (state->run_attrs == NULL)
				state->run_attrs = pango_attr_list_new ();
			pango_attr_list_insert (state->run_attrs, attr);

		}
}

static void
xlsx_run_style (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (gsf_xml_in_namecmp (xin, attrs[0], XL_NS_SS, "val")) {
			PangoAttribute *attr = pango_attr_style_new (strcmp (attrs[1], "true")? PANGO_STYLE_NORMAL: PANGO_STYLE_ITALIC);
			if (state->run_attrs == NULL)
				state->run_attrs = pango_attr_list_new ();
			pango_attr_list_insert (state->run_attrs, attr);

		}
}

static void
xlsx_run_family (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (gsf_xml_in_namecmp (xin, attrs[0], XL_NS_SS, "val")) {
			PangoAttribute *attr = pango_attr_family_new (attrs[1]);
			if (state->run_attrs == NULL)
				state->run_attrs = pango_attr_list_new ();
			pango_attr_list_insert (state->run_attrs, attr);

		}
}

static void
xlsx_run_size (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (gsf_xml_in_namecmp (xin, attrs[0], XL_NS_SS, "val")) {
			PangoAttribute *attr = pango_attr_size_new (atoi (attrs[1]) * PANGO_SCALE);
			if (state->run_attrs == NULL)
				state->run_attrs = pango_attr_list_new ();
			pango_attr_list_insert (state->run_attrs, attr);

		}
}

static void
xlsx_run_strikethrough (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (gsf_xml_in_namecmp (xin, attrs[0], XL_NS_SS, "val")) {
			PangoAttribute *attr = pango_attr_strikethrough_new (!strcmp (attrs[1], "true"));
			if (state->run_attrs == NULL)
				state->run_attrs = pango_attr_list_new ();
			pango_attr_list_insert (state->run_attrs, attr);

		}
}

static void
xlsx_run_underline (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (gsf_xml_in_namecmp (xin, attrs[0], XL_NS_SS, "val")) {
			PangoAttribute *attr;
			if (!strcmp (attrs[1], "single"))
			    attr = pango_attr_underline_new (PANGO_UNDERLINE_SINGLE);
			else if (!strcmp (attrs[1], "singleAccounting"))
			    attr = pango_attr_underline_new (PANGO_UNDERLINE_LOW);
			else if (!strcmp (attrs[1], "double") || !strcmp (attrs[1], "doubleAccounting"))
			    attr = pango_attr_underline_new (PANGO_UNDERLINE_DOUBLE);
			else
			    attr = pango_attr_underline_new (PANGO_UNDERLINE_NONE);
			if (state->run_attrs == NULL)
				state->run_attrs = pango_attr_list_new ();
			pango_attr_list_insert (state->run_attrs, attr);

		}
}

static void
xlsx_run_color (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (gsf_xml_in_namecmp (xin, attrs[0], XL_NS_SS, "rgb")) {
			PangoAttribute *attr;
			unsigned a, r = 0, g = 0, b = 0;
			if (4 != sscanf (attrs[1], "%02x%02x%02x%02x", &a, &r, &g, &b)) {
				xlsx_warning (xin,
					_("Invalid color '%s' for attribute rgb"),
					attrs[1]);
			}
			attr = pango_attr_foreground_new (r, g, b);
			if (state->run_attrs == NULL)
				state->run_attrs = pango_attr_list_new ();
			pango_attr_list_insert (state->run_attrs, attr);

		}
}

static void
xlsx_comments_start (GsfXMLIn *xin, G_GNUC_UNUSED xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	state->authors = g_ptr_array_new_with_free_func ((GDestroyNotify) g_free);
}

static void
xlsx_comments_end (GsfXMLIn *xin, G_GNUC_UNUSED GsfXMLBlob *blob)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	g_ptr_array_unref (state->authors);
	state->authors = NULL;
}

static void
xlsx_comment_author_end (GsfXMLIn *xin, G_GNUC_UNUSED GsfXMLBlob *blob)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	int i = strlen (xin->content->str);
	char *name = xin->content->str;
	/* remove any trailing white space */
	/* not sure this is correct, we might be careful about encoding */
	while (i > 0 && g_ascii_isspace (name[i-1]))
		i--;
	name = g_new (char, i + 1);
	memcpy (name, xin->content->str, i);
	name[i] = 0;
	g_ptr_array_add (state->authors, name);
}

static void
xlsx_comment_start (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	SheetObject *so;
	GnmRange anchor_r;
	SheetObjectAnchor	anchor;

	state->comment = g_object_new (cell_comment_get_type (), NULL);
	so = SHEET_OBJECT (state->comment);
	anchor_r = sheet_object_get_anchor (so)->cell_bound;

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (gsf_xml_in_namecmp (xin, attrs[0], XL_NS_SS, "ref"))
			range_parse (&anchor_r, attrs[1], gnm_sheet_get_size (state->sheet));
		else if (gsf_xml_in_namecmp (xin, attrs[0], XL_NS_SS, "authorId")) {
			unsigned id = atoi (attrs[1]);
			char const *name;
			if (id < state->authors->len) {
				name = g_ptr_array_index (state->authors, id);
				if (*name) /* do not set an empty name */
					g_object_set (state->comment, "author", name, NULL);
			}
		}

	sheet_object_anchor_init (&anchor, &anchor_r, NULL, GOD_ANCHOR_DIR_UNKNOWN);
	sheet_object_set_anchor (so, &anchor);
	state->comment_text = g_string_new ("");
}

static void
xlsx_comment_end (GsfXMLIn *xin, G_GNUC_UNUSED GsfXMLBlob *blob)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	char *text = g_string_free (state->comment_text, FALSE);
	state->comment_text = NULL;
	g_object_set (state->comment, "text", text, NULL);
	g_free (text);
	if (state->rich_attrs) {
		g_object_set (state->comment, "markup", state->rich_attrs, NULL);
		pango_attr_list_unref (state->rich_attrs);
		state->rich_attrs = NULL;
	}
	sheet_object_set_sheet (SHEET_OBJECT (state->comment), state->sheet);
	state->comment = NULL;

}

static void
xlsx_comment_text (GsfXMLIn *xin, G_GNUC_UNUSED GsfXMLBlob *blob)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	g_string_append (state->comment_text, xin->content->str);
}

static void
xlsx_comment_rich_text (GsfXMLIn *xin, G_GNUC_UNUSED GsfXMLBlob *blob)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	if (state->run_attrs) {
		unsigned start, end;
		start = state->comment_text->len;
		end = start + strlen (xin->content->str);
		if (state->rich_attrs == NULL)
			state->rich_attrs = pango_attr_list_new ();
		pango_attr_list_splice (state->rich_attrs, state->run_attrs, start, end);
		pango_attr_list_unref (state->run_attrs);
		state->run_attrs = NULL;
	}
	g_string_append (state->comment_text, xin->content->str);
}

static GsfXMLInNode const xlsx_comments_dtd[] = {
GSF_XML_IN_NODE_FULL (START, START, -1, NULL, GSF_XML_NO_CONTENT, FALSE, TRUE, NULL, NULL, 0),
GSF_XML_IN_NODE_FULL (START, COMMENTS, XL_NS_SS, "comments", GSF_XML_NO_CONTENT, TRUE, TRUE, xlsx_comments_start, xlsx_comments_end, 0),
  GSF_XML_IN_NODE (COMMENTS, AUTHORS, XL_NS_SS, "authors", GSF_XML_NO_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (AUTHORS, AUTHOR, XL_NS_SS, "author", GSF_XML_CONTENT, NULL, xlsx_comment_author_end),
  GSF_XML_IN_NODE (COMMENTS, COMMENTLIST, XL_NS_SS, "commentList", GSF_XML_NO_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (COMMENTLIST, COMMENT, XL_NS_SS, "comment", GSF_XML_NO_CONTENT, xlsx_comment_start, xlsx_comment_end),
      GSF_XML_IN_NODE (COMMENT, TEXTITEM, XL_NS_SS, "text", GSF_XML_NO_CONTENT, NULL, NULL),
        GSF_XML_IN_NODE (TEXTITEM, TEXT, XL_NS_SS, "t", GSF_XML_CONTENT, NULL, xlsx_comment_text),
        GSF_XML_IN_NODE (TEXTITEM, RICH, XL_NS_SS, "r", GSF_XML_NO_CONTENT, NULL, NULL),
          GSF_XML_IN_NODE (RICH, RICH_TEXT, XL_NS_SS, "t", GSF_XML_CONTENT, NULL, xlsx_comment_rich_text),
          GSF_XML_IN_NODE (RICH, RICH_PROPS, XL_NS_SS, "rPr", GSF_XML_NO_CONTENT, NULL, NULL),
#if 0
	GSF_XML_IN_NODE (RICH_PROPS, RICH_FONT, XL_NS_SS, "font", GSF_XML_NO_CONTENT, NULL, NULL),
	/* docs say 'font' xl is generating rFont */
#endif
    	  GSF_XML_IN_NODE (RICH_PROPS, RICH_FONT, XL_NS_SS, "rFont", GSF_XML_NO_CONTENT, NULL, NULL),
/* Are all these really used by excel? */
	    GSF_XML_IN_NODE (RICH_PROPS, RICH_CHARSET, XL_NS_SS, "charset", GSF_XML_NO_CONTENT, NULL, NULL),
	    GSF_XML_IN_NODE (RICH_PROPS, RICH_FAMILY, XL_NS_SS, "family", GSF_XML_NO_CONTENT, xlsx_run_family, NULL),
	    GSF_XML_IN_NODE (RICH_PROPS, RICH_BOLD, XL_NS_SS, "b", GSF_XML_NO_CONTENT, xlsx_run_weight, NULL),
	    GSF_XML_IN_NODE (RICH_PROPS, RICH_ITALIC, XL_NS_SS, "i", GSF_XML_NO_CONTENT, xlsx_run_style, NULL),
	    GSF_XML_IN_NODE (RICH_PROPS, RICH_STRIKE, XL_NS_SS, "strike", GSF_XML_NO_CONTENT, xlsx_run_strikethrough, NULL),
	    GSF_XML_IN_NODE (RICH_PROPS, RICH_OUTLINE, XL_NS_SS, "outline", GSF_XML_NO_CONTENT, NULL, NULL),
	    GSF_XML_IN_NODE (RICH_PROPS, RICH_SHADOW, XL_NS_SS, "shadow", GSF_XML_NO_CONTENT, NULL, NULL),
	    GSF_XML_IN_NODE (RICH_PROPS, RICH_CONDENSE, XL_NS_SS, "condense", GSF_XML_NO_CONTENT, NULL, NULL),
	    GSF_XML_IN_NODE (RICH_PROPS, RICH_EXTEND, XL_NS_SS, "extend", GSF_XML_NO_CONTENT, NULL, NULL),
	    GSF_XML_IN_NODE (RICH_PROPS, RICH_COLOR, XL_NS_SS, "color", GSF_XML_NO_CONTENT, xlsx_run_color, NULL),
	    GSF_XML_IN_NODE (RICH_PROPS, RICH_SZ, XL_NS_SS, "sz", GSF_XML_NO_CONTENT, xlsx_run_size, NULL),
	    GSF_XML_IN_NODE (RICH_PROPS, RICH_ULINE, XL_NS_SS, "u", GSF_XML_NO_CONTENT, xlsx_run_underline, NULL),
	    GSF_XML_IN_NODE (RICH_PROPS, RICH_VALIGN, XL_NS_SS, "vertAlign", GSF_XML_NO_CONTENT, NULL, NULL),
	    GSF_XML_IN_NODE (RICH_PROPS, RICH_SCHEME, XL_NS_SS, "scheme", GSF_XML_NO_CONTENT, NULL, NULL),
          GSF_XML_IN_NODE (RICH, RICH_PROPS, XL_NS_SS, "rPr", GSF_XML_NO_CONTENT, NULL, NULL),
        GSF_XML_IN_NODE (TEXTITEM, ITEM_PHONETIC_RUN, XL_NS_SS, "rPh", GSF_XML_NO_CONTENT, NULL, NULL),
          GSF_XML_IN_NODE (ITEM_PHONETIC_RUN, PHONETIC_TEXT, XL_NS_SS, "t", GSF_XML_CONTENT, NULL, NULL),
        GSF_XML_IN_NODE (TEXTITEM, ITEM_PHONETIC, XL_NS_SS, "phoneticPr", GSF_XML_NO_CONTENT, NULL, NULL),

GSF_XML_IN_NODE_END
};

/**************************************************************************************************/

static void
xlsx_wb_end (GsfXMLIn *xin, G_GNUC_UNUSED GsfXMLBlob *blob)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	int i, n = workbook_sheet_count (state->wb);
	char const *part_id;
	GnmStyle *style;
	GsfInput *sin, *cin;
	GError *err = NULL;

	/* Load sheets after setting up the workbooks to give us time to create
	 * all of them and parse names */
	for (i = 0 ; i < n ; i++, state->sheet = NULL) {
		if (NULL == (state->sheet = workbook_sheet_by_index (state->wb, i)))
			continue;
		if (NULL == (part_id = g_object_get_data (G_OBJECT (state->sheet), "_XLSX_RelID"))) {
			xlsx_warning (xin, _("Missing part-id for sheet '%s'"),
				      state->sheet->name_unquoted);
			continue;
		}

		/* Apply the 'Normal' style (aka builtin 0) to the entire sheet */
		if (NULL != (style = g_hash_table_lookup(state->cell_styles, "0"))) {
			GnmRange r;
			gnm_style_ref (style);
			range_init_full_sheet (&r, state->sheet);
			sheet_style_set_range (state->sheet, &r, style);
		}

		sin = gsf_open_pkg_open_rel_by_id (gsf_xml_in_get_input (xin), part_id, &err);
		if (NULL != err) {
			XLSXReadState *state = (XLSXReadState *)xin->user_state;
			go_io_warning (state->context, "%s", err->message);
			g_error_free (err);
			err = NULL;
			continue;
		}
		/* load comments */

		cin = gsf_open_pkg_open_rel_by_type (sin,
			"http://schemas.openxmlformats.org/officeDocument/2006/relationships/comments", NULL);
		xlsx_parse_stream (state, sin, xlsx_sheet_dtd);
		if (cin != NULL)
			xlsx_parse_stream (state, cin, xlsx_comments_dtd);

		/* Flag a respan here in case nothing else does */
		sheet_flag_recompute_spans (state->sheet);
	}
}

static GsfXMLInNode const xlsx_workbook_dtd[] = {
GSF_XML_IN_NODE_FULL (START, START, -1, NULL, GSF_XML_NO_CONTENT, FALSE, TRUE, NULL, NULL, 0),
GSF_XML_IN_NODE_FULL (START, WORKBOOK, XL_NS_SS, "workbook", GSF_XML_NO_CONTENT, FALSE, TRUE, NULL, &xlsx_wb_end, 0),
  GSF_XML_IN_NODE (WORKBOOK, VERSION, XL_NS_SS,	   "fileVersion", GSF_XML_NO_CONTENT, NULL, NULL),
  GSF_XML_IN_NODE (WORKBOOK, PROPERTIES, XL_NS_SS, "workbookPr", GSF_XML_NO_CONTENT, &xlsx_CT_WorkbookPr, NULL),
  GSF_XML_IN_NODE (WORKBOOK, CALC_PROPS, XL_NS_SS, "calcPr", GSF_XML_NO_CONTENT, &xlsx_CT_CalcPr, NULL),

  GSF_XML_IN_NODE (WORKBOOK, VIEWS,	 XL_NS_SS, "bookViews",	GSF_XML_NO_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (VIEWS,  VIEW,	 XL_NS_SS, "workbookView",  GSF_XML_NO_CONTENT, NULL, NULL),
  GSF_XML_IN_NODE (WORKBOOK, CUSTOMWVIEWS, XL_NS_SS, "customWorkbookViews", GSF_XML_NO_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (CUSTOMWVIEWS, CUSTOMWVIEW , XL_NS_SS, "customWorkbookView", GSF_XML_NO_CONTENT, NULL, NULL),
      GSF_XML_IN_NODE (CUSTOMWVIEW, EXTLST, XL_NS_SS, "extLst", GSF_XML_NO_CONTENT, NULL, NULL),

  GSF_XML_IN_NODE (WORKBOOK, SHEETS,	 XL_NS_SS, "sheets", GSF_XML_NO_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (SHEETS, SHEET,	 XL_NS_SS, "sheet", GSF_XML_NO_CONTENT, &xlsx_sheet_begin, NULL),
  GSF_XML_IN_NODE (WORKBOOK, FGROUPS,	 XL_NS_SS, "functionGroups", GSF_XML_NO_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (FGROUPS, FGROUP,	 XL_NS_SS, "functionGroup", GSF_XML_NO_CONTENT, NULL, NULL),
  GSF_XML_IN_NODE (WORKBOOK, WEB_PUB,	 XL_NS_SS, "webPublishing", GSF_XML_NO_CONTENT, NULL, NULL),
  GSF_XML_IN_NODE (WORKBOOK, EXTERNS,	 XL_NS_SS, "externalReferences", GSF_XML_NO_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (EXTERNS, EXTERN,	 XL_NS_SS, "externalReference", GSF_XML_NO_CONTENT, xlsx_wb_external_ref, NULL),
  GSF_XML_IN_NODE (WORKBOOK, NAMES,	 XL_NS_SS, "definedNames", GSF_XML_NO_CONTENT, NULL, xlsx_wb_names_end),
    GSF_XML_IN_NODE (NAMES, NAME,	 XL_NS_SS, "definedName", GSF_XML_CONTENT, xlsx_wb_name_begin, xlsx_wb_name_end),
  GSF_XML_IN_NODE (WORKBOOK, PIVOTCACHES,      XL_NS_SS, "pivotCaches", GSF_XML_NO_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (PIVOTCACHES, PIVOTCACHE,      XL_NS_SS, "pivotCache", GSF_XML_NO_CONTENT, &xlsx_CT_PivotCache, NULL),

  GSF_XML_IN_NODE (WORKBOOK, RECOVERY,	 XL_NS_SS, "fileRecoveryPr", GSF_XML_NO_CONTENT, NULL, NULL),
  GSF_XML_IN_NODE (WORKBOOK, OLESIZE,   XL_NS_SS, "oleSize", GSF_XML_NO_CONTENT, NULL, NULL),
  GSF_XML_IN_NODE (WORKBOOK, SMARTTAGPR,         XL_NS_SS, "smartTagPr", GSF_XML_NO_CONTENT, NULL, NULL),
  GSF_XML_IN_NODE (WORKBOOK, SMARTTTYPES,        XL_NS_SS, "smartTagTypes", GSF_XML_NO_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (SMARTTTYPES, SMARTTTYPE,      XL_NS_SS, "smartTagType", GSF_XML_NO_CONTENT, NULL, NULL),
  GSF_XML_IN_NODE (WORKBOOK, WEB_PUB_OBJS,     XL_NS_SS, "webPublishObjects", GSF_XML_NO_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (WEB_PUB_OBJS, WEB_PUB_OBJ,  XL_NS_SS, "webPublishObject", GSF_XML_NO_CONTENT, NULL, NULL),

GSF_XML_IN_NODE_END
};

/****************************************************************************/

static void
xlsx_sst_begin (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	int count;

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_int (xin, attrs, "uniqueCount", &count))
			g_array_set_size (state->sst, count);
	state->count = 0;
}

static void
xlsx_sstitem_end (GsfXMLIn *xin, G_GNUC_UNUSED GsfXMLBlob *blob)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	XLSXStr *entry;

	if (state->count >= state->sst->len)
		g_array_set_size (state->sst, state->count+1);
	entry = &g_array_index (state->sst, XLSXStr, state->count);
	state->count++;
	entry->str = go_string_new (xin->content->str);
	if (state->rich_attrs) {
		entry->markup = go_format_new_markup (state->rich_attrs, FALSE);
		state->rich_attrs = NULL;
	}

	/* sst does not have content so that we can ignore whitespace outside
	 * the <t> elements, but the <t>s do have SHARED content */
	g_string_truncate (xin->content, 0);
}

static GsfXMLInNode const xlsx_shared_strings_dtd[] = {
GSF_XML_IN_NODE_FULL (START, START, -1, NULL, GSF_XML_NO_CONTENT, FALSE, TRUE, NULL, NULL, 0),
GSF_XML_IN_NODE_FULL (START, SST, XL_NS_SS, "sst", GSF_XML_NO_CONTENT, FALSE, TRUE, &xlsx_sst_begin, NULL, 0),
  GSF_XML_IN_NODE (SST, ITEM, XL_NS_SS, "si", GSF_XML_NO_CONTENT, NULL, &xlsx_sstitem_end),		/* beta2 */
    GSF_XML_IN_NODE (ITEM, TEXT, XL_NS_SS, "t", GSF_XML_SHARED_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (ITEM, RICH, XL_NS_SS, "r", GSF_XML_NO_CONTENT, NULL, NULL),
      GSF_XML_IN_NODE (RICH, RICH_TEXT, XL_NS_SS, "t", GSF_XML_SHARED_CONTENT, NULL, NULL),
      GSF_XML_IN_NODE (RICH, RICH_PROPS, XL_NS_SS, "rPr", GSF_XML_NO_CONTENT, NULL, NULL),
#if 0
	GSF_XML_IN_NODE (RICH_PROPS, RICH_FONT, XL_NS_SS, "font", GSF_XML_NO_CONTENT, NULL, NULL),
	/* docs say 'font' xl is generating rFont */
#endif
	GSF_XML_IN_NODE (RICH_PROPS, RICH_FONT, XL_NS_SS, "rFont", GSF_XML_NO_CONTENT, NULL, NULL),

	GSF_XML_IN_NODE (RICH_PROPS, RICH_CHARSET, XL_NS_SS, "charset", GSF_XML_NO_CONTENT, NULL, NULL),
	GSF_XML_IN_NODE (RICH_PROPS, RICH_FAMILY, XL_NS_SS, "family", GSF_XML_NO_CONTENT, NULL, NULL),
	GSF_XML_IN_NODE (RICH_PROPS, RICH_BOLD, XL_NS_SS, "b", GSF_XML_NO_CONTENT, NULL, NULL),
	GSF_XML_IN_NODE (RICH_PROPS, RICH_ITALIC, XL_NS_SS, "i", GSF_XML_NO_CONTENT, NULL, NULL),
	GSF_XML_IN_NODE (RICH_PROPS, RICH_STRIKE, XL_NS_SS, "strike", GSF_XML_NO_CONTENT, NULL, NULL),
	GSF_XML_IN_NODE (RICH_PROPS, RICH_OUTLINE, XL_NS_SS, "outline", GSF_XML_NO_CONTENT, NULL, NULL),
	GSF_XML_IN_NODE (RICH_PROPS, RICH_SHADOW, XL_NS_SS, "shadow", GSF_XML_NO_CONTENT, NULL, NULL),
	GSF_XML_IN_NODE (RICH_PROPS, RICH_CONDENSE, XL_NS_SS, "condense", GSF_XML_NO_CONTENT, NULL, NULL),
	GSF_XML_IN_NODE (RICH_PROPS, RICH_EXTEND, XL_NS_SS, "extend", GSF_XML_NO_CONTENT, NULL, NULL),
	GSF_XML_IN_NODE (RICH_PROPS, RICH_COLOR, XL_NS_SS, "color", GSF_XML_NO_CONTENT, NULL, NULL),
	GSF_XML_IN_NODE (RICH_PROPS, RICH_SZ, XL_NS_SS, "sz", GSF_XML_NO_CONTENT, NULL, NULL),
	GSF_XML_IN_NODE (RICH_PROPS, RICH_ULINE, XL_NS_SS, "u", GSF_XML_NO_CONTENT, NULL, NULL),
	GSF_XML_IN_NODE (RICH_PROPS, RICH_VALIGN, XL_NS_SS, "vertAlign", GSF_XML_NO_CONTENT, NULL, NULL),
	GSF_XML_IN_NODE (RICH_PROPS, RICH_SCHEME, XL_NS_SS, "scheme", GSF_XML_NO_CONTENT, NULL, NULL),
      GSF_XML_IN_NODE (RICH, RICH_PROPS, XL_NS_SS, "rPr", GSF_XML_NO_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (ITEM, ITEM_PHONETIC_RUN, XL_NS_SS, "rPh", GSF_XML_NO_CONTENT, NULL, NULL),
      GSF_XML_IN_NODE (ITEM_PHONETIC_RUN, PHONETIC_TEXT, XL_NS_SS, "t", GSF_XML_SHARED_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (ITEM, ITEM_PHONETIC, XL_NS_SS, "phoneticPr", GSF_XML_NO_CONTENT, NULL, NULL),

GSF_XML_IN_NODE_END
};

/****************************************************************************/

static void
xlsx_style_numfmt (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	xmlChar const *fmt = NULL;
	xmlChar const *id = NULL;

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (0 == strcmp (attrs[0], "numFmtId"))
			id = attrs[1];
		else if (0 == strcmp (attrs[0], "formatCode"))
			fmt = attrs[1];

	if (NULL != id && NULL != fmt)
		g_hash_table_replace (state->num_fmts, g_strdup (id),
			go_format_new_from_XL (fmt));
}

enum {
	XLSX_COLLECT_FONT,
	XLSX_COLLECT_FILLS,
	XLSX_COLLECT_BORDERS,
	XLSX_COLLECT_XFS,
	XLSX_COLLECT_STYLE_XFS,
	XLSX_COLLECT_DXFS,
	XLSX_COLLECT_TABLE_STYLES
};

static void
xlsx_collection_begin (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	int count = 0;

	g_return_if_fail (NULL == state->collection);

	state->count = 0;
	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_int (xin, attrs, "count", &count))
			;
	state->collection = g_ptr_array_new ();
	g_ptr_array_set_size (state->collection, count);

	switch (xin->node->user_data.v_int) {
	case XLSX_COLLECT_FONT :	state->fonts = state->collection;	 break;
	case XLSX_COLLECT_FILLS :	state->fills = state->collection;	 break;
	case XLSX_COLLECT_BORDERS :	state->borders = state->collection;	 break;
	case XLSX_COLLECT_XFS :		state->xfs = state->collection;		 break;
	case XLSX_COLLECT_STYLE_XFS :	state->style_xfs = state->collection;	 break;
	case XLSX_COLLECT_DXFS :	state->dxfs = state->collection;	 break;
	case XLSX_COLLECT_TABLE_STYLES: state->table_styles = state->collection; break;
	}
}

static void
xlsx_collection_end (GsfXMLIn *xin, G_GNUC_UNUSED GsfXMLBlob *blob)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;

	/* resize just in case the count hint was wrong */
	g_ptr_array_set_size (state->collection, state->count);
	state->count = 0;
	state->collection = NULL;
}

static void
xlsx_col_elem_end (GsfXMLIn *xin, G_GNUC_UNUSED GsfXMLBlob *blob)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;

	if (!state->style_accum_partial) {
		GnmStyle *res = state->style_accum;
		state->style_accum = NULL;
		if (state->count >= state->collection->len)
			g_ptr_array_add (state->collection, res);
		else if (NULL != g_ptr_array_index (state->collection, state->count)) {
			g_warning ("dup @ %d = %p", state->count, res);
			gnm_style_unref (res);
		} else
			g_ptr_array_index (state->collection, state->count) = res;
		state->count++;
	}
}

static void
xlsx_col_elem_begin (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	if (!state->style_accum_partial) {
		g_return_if_fail (NULL == state->style_accum);
		state->style_accum = gnm_style_new ();
	}
}

static void
xlsx_col_border_begin (GsfXMLIn *xin, xmlChar const **attrs)
{	
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	gboolean diagonal_down = FALSE, diagonal_up = FALSE;

	xlsx_col_elem_begin (xin, attrs);
	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_bool (xin, attrs, "diagonalDown", &diagonal_down)) ;
		else (attr_bool (xin, attrs, "diagonalUp", &diagonal_up)) ;
	
	if (diagonal_up) {
		GnmBorder *border = gnm_style_border_fetch 
			(GNM_STYLE_BORDER_THIN, style_color_black (), GNM_STYLE_BORDER_DIAGONAL);
		gnm_style_set_border (state->style_accum,
				      MSTYLE_BORDER_DIAGONAL,
				      border);
	}
	if (diagonal_down) {
		GnmBorder *border = gnm_style_border_fetch 
			(GNM_STYLE_BORDER_HAIR, style_color_black (), GNM_STYLE_BORDER_DIAGONAL);
		gnm_style_set_border (state->style_accum,
				      MSTYLE_BORDER_REV_DIAGONAL,
				      border);
	}
}

static void
xlsx_font_name (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (0 == strcmp (attrs[0], "val"))
			gnm_style_set_font_name	(state->style_accum, attrs[1]);
}
static void
xlsx_font_bold (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	int val = TRUE;
	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_bool (xin, attrs, "val", &val)) ;
			;
	gnm_style_set_font_bold (state->style_accum, val);
}
static void
xlsx_font_italic (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	int val = TRUE;
	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_bool (xin, attrs, "val", &val)) ;
			;
	gnm_style_set_font_italic (state->style_accum, val);
}
static void
xlsx_font_strike (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	int val = TRUE;
	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_bool (xin, attrs, "val", &val))
			;
	gnm_style_set_font_strike (state->style_accum, val);
}
static void
xlsx_font_color (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	/* LibreOffice 3.3.2 sets the alpha to 0, so text becomes invisible */
	/* (Excel drops the alpha too it seems.) */
	GnmColor *color = elem_color (xin, attrs, FALSE);
	if (NULL != color)
		gnm_style_set_font_color (state->style_accum, color);
}
static void
xlsx_CT_FontSize (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	gnm_float val;

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_float (xin, attrs, "val", &val))
			gnm_style_set_font_size	(state->style_accum, val);
}
static void
xlsx_font_uline (GsfXMLIn *xin, xmlChar const **attrs)
{
	static EnumVal const types[] = {
		{ "single", UNDERLINE_SINGLE },
		{ "double", UNDERLINE_DOUBLE },
		{ "singleAccounting", UNDERLINE_SINGLE_LOW },
		{ "doubleAccounting", UNDERLINE_DOUBLE_LOW },
		{ "none", UNDERLINE_NONE },
		{ NULL, 0 }
	};
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	int val = UNDERLINE_SINGLE;

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_enum (xin, attrs, "val", types, &val))
			;
	gnm_style_set_font_uline (state->style_accum, val);
}

static void
xlsx_font_valign (GsfXMLIn *xin, xmlChar const **attrs)
{
	static EnumVal const types[] = {
		{ "baseline",	 GO_FONT_SCRIPT_STANDARD },
		{ "superscript", GO_FONT_SCRIPT_SUPER },
		{ "subscript",   GO_FONT_SCRIPT_SUB },
		{ NULL, 0 }
	};
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	int val = GO_FONT_SCRIPT_STANDARD;

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_enum (xin, attrs, "val", types, &val))
			gnm_style_set_font_script (state->style_accum, val);
}

static void
xlsx_pattern (GsfXMLIn *xin, xmlChar const **attrs)
{
	static EnumVal const patterns[] = {
		{ "none",		0 },
		{ "solid",		1 },
		{ "mediumGray",		3 },
		{ "darkGray",		2 },
		{ "lightGray",		4 },
		{ "darkHorizontal",	7 },
		{ "darkVertical",	8 },
		{ "darkDown",		10},
		{ "darkUp",		9 },
		{ "darkGrid",		11 },
		{ "darkTrellis",	12 },
		{ "lightHorizontal",	13 },
		{ "lightVertical",	14 },
		{ "lightDown",		15 },
		{ "lightUp",		16 },
		{ "lightGrid",		17 },
		{ "lightTrellis",	18 },
		{ "gray125",		5 },
		{ "gray0625",		6 },
		{ NULL, 0 }
	};
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	int val = 0; /* none */

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_enum (xin, attrs, "patternType", patterns, &val))
			gnm_style_set_pattern (state->style_accum, val);
}
static void
xlsx_pattern_fg_bg (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	gboolean solid_pattern = gnm_style_is_element_set (state->style_accum, MSTYLE_PATTERN)
		&& (1 == gnm_style_get_pattern (state->style_accum));
	/* MAGIC :
	 * Looks like pattern background and forground colours are inverted for
	 * dxfs with solid fills for no apparent reason. */
	gboolean const invert = state->style_accum_partial
		&& solid_pattern;
	/* LibreOffice 3.3.2 sets the alpha to 0, so solid fill becomes invisible */
	/* (Excel drops the alpha too it seems.) */
	GnmColor *color = elem_color (xin, attrs, !solid_pattern);
	if (NULL == color)
		return;

	if (xin->node->user_data.v_int ^ invert)
		gnm_style_set_back_color (state->style_accum, color);
	else
		gnm_style_set_pattern_color (state->style_accum, color);
}

static void
xlsx_CT_GradientFill (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;

	gnm_style_set_pattern (state->style_accum, 1);

#if 0
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
    <xsd:attribute name="type" type="ST_GradientType" use="optional" default="linear">
    <xsd:attribute name="degree" type="xsd:double" use="optional" default="0">
    <xsd:attribute name="left" type="xsd:double" use="optional" default="0">
    <xsd:attribute name="right" type="xsd:double" use="optional" default="0">
    <xsd:attribute name="top" type="xsd:double" use="optional" default="0">
    <xsd:attribute name="bottom" type="xsd:double" use="optional" default="0">
#endif
}

static void
xlsx_border_begin (GsfXMLIn *xin, xmlChar const **attrs)
{
	static EnumVal const borders[] = {
		{ "none",		GNM_STYLE_BORDER_NONE },
		{ "thin",		GNM_STYLE_BORDER_THIN },
		{ "medium",		GNM_STYLE_BORDER_MEDIUM },
		{ "dashed",		GNM_STYLE_BORDER_DASHED },
		{ "dotted",		GNM_STYLE_BORDER_DOTTED },
		{ "thick",		GNM_STYLE_BORDER_THICK },
		{ "double",		GNM_STYLE_BORDER_DOUBLE },
		{ "hair",		GNM_STYLE_BORDER_HAIR },
		{ "mediumDashed",	GNM_STYLE_BORDER_MEDIUM_DASH },
		{ "dashDot",		GNM_STYLE_BORDER_DASH_DOT },
		{ "mediumDashDot",	GNM_STYLE_BORDER_MEDIUM_DASH_DOT },
		{ "dashDotDot",		GNM_STYLE_BORDER_DASH_DOT_DOT },
		{ "mediumDashDotDot",	GNM_STYLE_BORDER_MEDIUM_DASH_DOT_DOT },
		{ "slantDashDot",	GNM_STYLE_BORDER_SLANTED_DASH_DOT },
		{ NULL, 0 }
	};
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	int border_style = GNM_STYLE_BORDER_NONE;

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_enum (xin, attrs, "style", borders, &border_style))
			;
	state->border_style = border_style;
	state->border_color = NULL;
}

static void
xlsx_border_end (GsfXMLIn *xin, G_GNUC_UNUSED GsfXMLBlob *blob)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	GnmBorder *border;
	GnmStyleBorderLocation const loc = xin->node->user_data.v_int;

	if (NULL == state->border_color)
		state->border_color = style_color_black ();
	border = gnm_style_border_fetch (state->border_style,
		state->border_color, gnm_style_border_get_orientation (loc));
	gnm_style_set_border (state->style_accum,
		GNM_STYLE_BORDER_LOCATION_TO_STYLE_ELEMENT (loc),
		border);
	state->border_color = NULL;
}

static void
xlsx_border_diagonal_end (GsfXMLIn *xin, G_GNUC_UNUSED GsfXMLBlob *blob)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	GnmBorder *border, *new_border;

	if (NULL == state->border_color)
		state->border_color = style_color_black ();
	new_border = gnm_style_border_fetch 
		(state->border_style, state->border_color, GNM_STYLE_BORDER_DIAGONAL);

	border = gnm_style_get_border (state->style_accum, MSTYLE_BORDER_REV_DIAGONAL);
	if (border != NULL && border->line_type != GNM_STYLE_BORDER_NONE) {
		gnm_style_border_ref (new_border);
		gnm_style_set_border (state->style_accum,
				      MSTYLE_BORDER_REV_DIAGONAL,
				      new_border);
	}
	border = gnm_style_get_border (state->style_accum, MSTYLE_BORDER_DIAGONAL);
	if (border != NULL && border->line_type != GNM_STYLE_BORDER_NONE) {
		gnm_style_border_ref (new_border);
		gnm_style_set_border (state->style_accum,
				      MSTYLE_BORDER_DIAGONAL,
				      new_border);
	}
	gnm_style_border_unref (new_border);
	state->border_color = NULL;
}

static void
xlsx_border_color (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	GnmColor *color = elem_color (xin, attrs, TRUE);
	if (state->border_color)
		style_color_unref (state->border_color);
	state->border_color = color;
}

static void
xlsx_xf_begin (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	GnmStyle *accum = gnm_style_new ();
	GnmStyle *parent = NULL;
	GnmStyle *result;
	GPtrArray *elem = NULL;
	int indx;

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2) {
		if (0 == strcmp (attrs[0], "numFmtId")) {
			GOFormat *fmt = xlsx_get_num_fmt (xin, attrs[1]);
			if (NULL != fmt)
				gnm_style_set_format (accum, fmt);
		} else if (attr_int (xin, attrs, "fontId", &indx))
			elem = state->fonts;
		else if (attr_int (xin, attrs, "fillId", &indx))
			elem = state->fills;
		else if (attr_int (xin, attrs, "borderId", &indx))
			elem = state->borders;
		else if (attr_int (xin, attrs, "xfId", &indx))
			parent = xlsx_get_style_xf (xin, indx);

		if (NULL != elem) {
			GnmStyle const *component = NULL;
			if (0 <= indx && indx < (int)elem->len)
				component = g_ptr_array_index (elem, indx);
			if (NULL != component) {
#if 0
				gnm_style_merge (accum, component);
#else
				GnmStyle *merged = gnm_style_new_merged (accum, component);
				gnm_style_unref (accum);
				accum = merged;
#endif
			} else
				xlsx_warning (xin, "Missing record '%d' for %s", indx, attrs[0]);
			elem = NULL;
		}
	}
	if (NULL == parent) {
		result = gnm_style_new_default ();
		gnm_style_merge (result, accum);
	} else
		result = gnm_style_new_merged (parent, accum);
	gnm_style_unref (accum);

	state->style_accum = result;
#if 0
		"quotePrefix"			??

		"applyNumberFormat"
		"applyFont"
		"applyFill"
		"applyBorder"
		"applyAlignment"
		"applyProtection"
#endif
}
static void
xlsx_xf_end (GsfXMLIn *xin, GsfXMLBlob *blob)
{
	xlsx_col_elem_end (xin, blob);
}

static void
xlsx_xf_align (GsfXMLIn *xin, xmlChar const **attrs)
{
	static EnumVal const haligns[] = {
		{ "general" , HALIGN_GENERAL },
		{ "left" , HALIGN_LEFT },
		{ "center" , HALIGN_CENTER },
		{ "right" , HALIGN_RIGHT },
		{ "fill" , HALIGN_FILL },
		{ "justify" , HALIGN_JUSTIFY },
		{ "centerContinuous" , HALIGN_CENTER_ACROSS_SELECTION },
		{ "distributed" , HALIGN_DISTRIBUTED },
		{ NULL, 0 }
	};

	static EnumVal const valigns[] = {
		{ "top", VALIGN_TOP },
		{ "center", VALIGN_CENTER },
		{ "bottom", VALIGN_BOTTOM },
		{ "justify", VALIGN_JUSTIFY },
		{ "distributed", VALIGN_DISTRIBUTED },
		{ NULL, 0 }
	};

	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	int halign = HALIGN_GENERAL;
	int valign = VALIGN_BOTTOM;
	int rotation = 0, indent = 0;
	int wrapText = FALSE, justifyLastLine = FALSE, shrinkToFit = FALSE;

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_enum (xin, attrs, "horizontal", haligns, &halign)) ;
		else if (attr_enum (xin, attrs, "vertical", valigns, &valign)) ;
		else if (attr_int (xin, attrs, "textRotation", &rotation));
		else if (attr_bool (xin, attrs, "wrapText", &wrapText)) ;
		else if (attr_int (xin, attrs, "indent", &indent)) ;
		else if (attr_bool (xin, attrs, "justifyLastLine", &justifyLastLine)) ;
		else if (attr_bool (xin, attrs, "shrinkToFit", &shrinkToFit)) ;
		/* "mergeCell" type="xs:boolean" use="optional" default="false" */
		/* "readingOrder" type="xs:unsignedInt" use="optional" default="0" */

		gnm_style_set_align_h	   (state->style_accum, halign);
		gnm_style_set_align_v	   (state->style_accum, valign);
		gnm_style_set_rotation	   (state->style_accum,
			(rotation == 0xff) ? -1 : ((rotation > 90) ? (360 + 90 - rotation) : rotation));
		gnm_style_set_wrap_text   (state->style_accum, wrapText);
		gnm_style_set_indent	   (state->style_accum, indent);
		gnm_style_set_shrink_to_fit (state->style_accum, shrinkToFit);
}
static void
xlsx_xf_protect (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	int locked = TRUE;
	int hidden = TRUE;

	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_bool (xin, attrs, "locked", &locked)) ;
		else if (attr_bool (xin, attrs, "hidden", &hidden)) ;
	gnm_style_set_contents_locked (state->style_accum, locked);
	gnm_style_set_contents_hidden (state->style_accum, hidden);
}

static void
xlsx_cell_style (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	xmlChar const *name = NULL;
	xmlChar const *id = NULL;
	GnmStyle *style = NULL;
	int tmp;

	/* cellStyle name="Normal" xfId="0" builtinId="0" */
	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_int (xin, attrs, "xfId", &tmp))
			style = xlsx_get_style_xf (xin, tmp);
		else if (0 == strcmp (attrs[0], "name"))
			name = attrs[1];
		else if (0 == strcmp (attrs[0], "builtinId"))
			id = attrs[1];

	if (NULL != style && NULL != id) {
		gnm_style_ref (style);
		g_hash_table_replace (state->cell_styles, g_strdup (id), style);
	}
}

static void
xlsx_dxf_begin (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	state->style_accum_partial = TRUE;
	state->style_accum = gnm_style_new ();
}
static void
xlsx_dxf_end (GsfXMLIn *xin, GsfXMLBlob *blob)
{
	XLSXReadState *state = (XLSXReadState *)xin->user_state;
	state->style_accum_partial = FALSE;
	xlsx_col_elem_end (xin, blob);
}

static GsfXMLInNode const xlsx_styles_dtd[] = {
GSF_XML_IN_NODE_FULL (START, START, -1, NULL, GSF_XML_NO_CONTENT, FALSE, TRUE, NULL, NULL, 0),
GSF_XML_IN_NODE_FULL (START, STYLE_INFO, XL_NS_SS, "styleSheet", GSF_XML_NO_CONTENT, FALSE, TRUE, NULL, NULL, 0),

  GSF_XML_IN_NODE (STYLE_INFO, NUM_FMTS, XL_NS_SS, "numFmts", GSF_XML_NO_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (NUM_FMTS, NUM_FMT, XL_NS_SS, "numFmt", GSF_XML_NO_CONTENT, &xlsx_style_numfmt, NULL),

  GSF_XML_IN_NODE_FULL (STYLE_INFO, FONTS, XL_NS_SS, "fonts", GSF_XML_NO_CONTENT,
			FALSE, FALSE, &xlsx_collection_begin, &xlsx_collection_end, XLSX_COLLECT_FONT),
    GSF_XML_IN_NODE (FONTS, FONT, XL_NS_SS, "font", GSF_XML_NO_CONTENT, &xlsx_col_elem_begin, &xlsx_col_elem_end),
      GSF_XML_IN_NODE (FONT, FONT_NAME,	     XL_NS_SS, "name",	    GSF_XML_NO_CONTENT, &xlsx_font_name, NULL),
      GSF_XML_IN_NODE (FONT, FONT_CHARSET,   XL_NS_SS, "charset",   GSF_XML_NO_CONTENT, NULL, NULL),
      GSF_XML_IN_NODE (FONT, FONT_FAMILY,    XL_NS_SS, "family",    GSF_XML_NO_CONTENT, NULL, NULL),
      GSF_XML_IN_NODE (FONT, FONT_BOLD,	     XL_NS_SS, "b",	    GSF_XML_NO_CONTENT, &xlsx_font_bold, NULL),
      GSF_XML_IN_NODE (FONT, FONT_ITALIC,    XL_NS_SS, "i",	    GSF_XML_NO_CONTENT, &xlsx_font_italic, NULL),
      GSF_XML_IN_NODE (FONT, FONT_STRIKE,    XL_NS_SS, "strike",    GSF_XML_NO_CONTENT, &xlsx_font_strike, NULL),
      GSF_XML_IN_NODE (FONT, FONT_OUTLINE,   XL_NS_SS, "outline",   GSF_XML_NO_CONTENT, NULL, NULL),
      GSF_XML_IN_NODE (FONT, FONT_SHADOW,    XL_NS_SS, "shadow",    GSF_XML_NO_CONTENT, NULL, NULL),
      GSF_XML_IN_NODE (FONT, FONT_CONDENSE,  XL_NS_SS, "condense",  GSF_XML_NO_CONTENT, NULL, NULL),
      GSF_XML_IN_NODE (FONT, FONT_EXTEND,    XL_NS_SS, "extend",    GSF_XML_NO_CONTENT, NULL, NULL),
      GSF_XML_IN_NODE (FONT, FONT_COLOR,     XL_NS_SS, "color",     GSF_XML_NO_CONTENT, &xlsx_font_color, NULL),
      GSF_XML_IN_NODE (FONT, FONT_SZ,	     XL_NS_SS, "sz",	    GSF_XML_NO_CONTENT,	&xlsx_CT_FontSize, NULL),
      GSF_XML_IN_NODE (FONT, FONT_ULINE,     XL_NS_SS, "u",	    GSF_XML_NO_CONTENT,	&xlsx_font_uline, NULL),
      GSF_XML_IN_NODE (FONT, FONT_VERTALIGN, XL_NS_SS, "vertAlign", GSF_XML_NO_CONTENT, &xlsx_font_valign, NULL),
      GSF_XML_IN_NODE (FONT, FONT_SCHEME,    XL_NS_SS, "scheme",    GSF_XML_NO_CONTENT, NULL, NULL),

  GSF_XML_IN_NODE_FULL (STYLE_INFO, FILLS, XL_NS_SS, "fills", GSF_XML_NO_CONTENT,
			FALSE, FALSE, &xlsx_collection_begin, &xlsx_collection_end, XLSX_COLLECT_FILLS),
    GSF_XML_IN_NODE (FILLS, FILL, XL_NS_SS, "fill", GSF_XML_NO_CONTENT, &xlsx_col_elem_begin, &xlsx_col_elem_end),
      GSF_XML_IN_NODE (FILL, PATTERN_FILL, XL_NS_SS, "patternFill", GSF_XML_NO_CONTENT, &xlsx_pattern, NULL),
	GSF_XML_IN_NODE_FULL (PATTERN_FILL, PATTERN_FILL_FG,  XL_NS_SS, "fgColor", GSF_XML_NO_CONTENT,
			      FALSE, FALSE, &xlsx_pattern_fg_bg, NULL, TRUE),
	GSF_XML_IN_NODE_FULL (PATTERN_FILL, PATTERN_FILL_BG,  XL_NS_SS, "bgColor", GSF_XML_NO_CONTENT,
			      FALSE, FALSE, &xlsx_pattern_fg_bg, NULL, FALSE),
      GSF_XML_IN_NODE (FILL, IMAGE_FILL, XL_NS_SS, "image", GSF_XML_NO_CONTENT, NULL, NULL),
      GSF_XML_IN_NODE (FILL, GRADIENT_FILL, XL_NS_SS, "gradientFill", GSF_XML_NO_CONTENT, &xlsx_CT_GradientFill, NULL),
	GSF_XML_IN_NODE (GRADIENT_FILL, GRADIENT_STOPS, XL_NS_SS, "stop", GSF_XML_NO_CONTENT, NULL, NULL),
	  GSF_XML_IN_NODE_FULL (GRADIENT_STOPS, GRADIENT_COLOR, XL_NS_SS, "color", GSF_XML_NO_CONTENT, FALSE, FALSE, &xlsx_pattern_fg_bg, NULL, TRUE),

  GSF_XML_IN_NODE_FULL (STYLE_INFO, BORDERS, XL_NS_SS, "borders", GSF_XML_NO_CONTENT,
			FALSE, FALSE, &xlsx_collection_begin, &xlsx_collection_end, XLSX_COLLECT_BORDERS),
    GSF_XML_IN_NODE (BORDERS, BORDER, XL_NS_SS, "border", GSF_XML_NO_CONTENT, &xlsx_col_border_begin, &xlsx_col_elem_end),
      GSF_XML_IN_NODE_FULL (BORDER, LEFT_B, XL_NS_SS, "left", GSF_XML_NO_CONTENT, FALSE, FALSE,
			    &xlsx_border_begin, &xlsx_border_end, GNM_STYLE_BORDER_LEFT),
        GSF_XML_IN_NODE (LEFT_B, LEFT_COLOR, XL_NS_SS, "color", GSF_XML_NO_CONTENT, &xlsx_border_color, NULL),
      GSF_XML_IN_NODE_FULL (BORDER, START_B, XL_NS_SS, "start", GSF_XML_NO_CONTENT, FALSE, FALSE,
			    &xlsx_border_begin, &xlsx_border_end, GNM_STYLE_BORDER_LEFT),
        GSF_XML_IN_NODE (START_B, START_COLOR, XL_NS_SS, "color", GSF_XML_NO_CONTENT, &xlsx_border_color, NULL),
      GSF_XML_IN_NODE_FULL (BORDER, RIGHT_B, XL_NS_SS, "right", GSF_XML_NO_CONTENT, FALSE, FALSE,
			    &xlsx_border_begin, &xlsx_border_end, GNM_STYLE_BORDER_RIGHT),
        GSF_XML_IN_NODE (RIGHT_B, RIGHT_COLOR, XL_NS_SS, "color", GSF_XML_NO_CONTENT, &xlsx_border_color, NULL),
      GSF_XML_IN_NODE_FULL (BORDER, END_B, XL_NS_SS, "end", GSF_XML_NO_CONTENT, FALSE, FALSE,
			    &xlsx_border_begin, &xlsx_border_end, GNM_STYLE_BORDER_RIGHT),
        GSF_XML_IN_NODE (END_B, END_COLOR, XL_NS_SS, "color", GSF_XML_NO_CONTENT, &xlsx_border_color, NULL),
       GSF_XML_IN_NODE_FULL (BORDER, TOP_B, XL_NS_SS,	"top", GSF_XML_NO_CONTENT, FALSE, FALSE,
			    &xlsx_border_begin, &xlsx_border_end, GNM_STYLE_BORDER_TOP),
        GSF_XML_IN_NODE (TOP_B, TOP_COLOR, XL_NS_SS, "color", GSF_XML_NO_CONTENT, &xlsx_border_color, NULL),
      GSF_XML_IN_NODE_FULL (BORDER, BOTTOM_B, XL_NS_SS, "bottom", GSF_XML_NO_CONTENT, FALSE, FALSE,
			    &xlsx_border_begin, &xlsx_border_end, GNM_STYLE_BORDER_BOTTOM),
        GSF_XML_IN_NODE (BOTTOM_B, BOTTOM_COLOR, XL_NS_SS, "color", GSF_XML_NO_CONTENT, &xlsx_border_color, NULL),
      GSF_XML_IN_NODE (BORDER, DIAG_B, XL_NS_SS, "diagonal", GSF_XML_NO_CONTENT,
			    &xlsx_border_begin, &xlsx_border_diagonal_end),
        GSF_XML_IN_NODE (DIAG_B, DIAG_COLOR, XL_NS_SS, "color", GSF_XML_NO_CONTENT, &xlsx_border_color, NULL),

      GSF_XML_IN_NODE (BORDER, BORDER_VERT, XL_NS_SS,	"vertical", GSF_XML_NO_CONTENT, NULL, NULL),
        GSF_XML_IN_NODE (BORDER_VERT, VERT_COLOR, XL_NS_SS, "color", GSF_XML_NO_CONTENT, NULL, NULL),
      GSF_XML_IN_NODE (BORDER, BORDER_HORIZ, XL_NS_SS,	"horizontal", GSF_XML_NO_CONTENT, NULL, NULL),
        GSF_XML_IN_NODE (BORDER_HORIZ, HORIZ_COLOR, XL_NS_SS, "color", GSF_XML_NO_CONTENT, NULL, NULL),

  GSF_XML_IN_NODE_FULL (STYLE_INFO, XFS, XL_NS_SS, "cellXfs", GSF_XML_NO_CONTENT,
			FALSE, FALSE, &xlsx_collection_begin, &xlsx_collection_end, XLSX_COLLECT_XFS),
    GSF_XML_IN_NODE (XFS, XF, XL_NS_SS, "xf", GSF_XML_NO_CONTENT, &xlsx_xf_begin, &xlsx_xf_end),
      GSF_XML_IN_NODE (XF, ALIGNMENT, XL_NS_SS, "alignment", GSF_XML_NO_CONTENT, &xlsx_xf_align, NULL),
      GSF_XML_IN_NODE (XF, PROTECTION, XL_NS_SS, "protection", GSF_XML_NO_CONTENT, &xlsx_xf_protect, NULL),

  GSF_XML_IN_NODE_FULL (STYLE_INFO, STYLE_XFS, XL_NS_SS, "cellStyleXfs", GSF_XML_NO_CONTENT,
		   FALSE, FALSE, &xlsx_collection_begin, &xlsx_collection_end, XLSX_COLLECT_STYLE_XFS),
    GSF_XML_IN_NODE (STYLE_XFS, STYLE_XF, XL_NS_SS, "xf", GSF_XML_NO_CONTENT, &xlsx_xf_begin, &xlsx_xf_end),
      GSF_XML_IN_NODE (STYLE_XF, STYLE_ALIGNMENT, XL_NS_SS, "alignment", GSF_XML_NO_CONTENT, &xlsx_xf_align, NULL),
      GSF_XML_IN_NODE (STYLE_XF, STYLE_PROTECTION, XL_NS_SS, "protection", GSF_XML_NO_CONTENT, &xlsx_xf_protect, NULL),

  GSF_XML_IN_NODE (STYLE_INFO, STYLE_NAMES, XL_NS_SS, "cellStyles", GSF_XML_NO_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (STYLE_NAMES, STYLE_NAME, XL_NS_SS, "cellStyle", GSF_XML_NO_CONTENT, &xlsx_cell_style, NULL),

  GSF_XML_IN_NODE_FULL (STYLE_INFO, PARTIAL_XFS, XL_NS_SS, "dxfs", GSF_XML_NO_CONTENT,
			FALSE, FALSE, &xlsx_collection_begin, &xlsx_collection_end, XLSX_COLLECT_DXFS),
    GSF_XML_IN_NODE (PARTIAL_XFS, PARTIAL_XF, XL_NS_SS, "dxf", GSF_XML_NO_CONTENT, &xlsx_dxf_begin, &xlsx_dxf_end),
      GSF_XML_IN_NODE (PARTIAL_XF, NUM_FMT, XL_NS_SS, "numFmt", GSF_XML_NO_CONTENT, NULL, NULL),		/* 2nd Def */
      GSF_XML_IN_NODE (PARTIAL_XF, FONT,    XL_NS_SS, "font", GSF_XML_NO_CONTENT, NULL, NULL),			/* 2nd Def */
      GSF_XML_IN_NODE (PARTIAL_XF, FILL,    XL_NS_SS, "fill", GSF_XML_NO_CONTENT, NULL, NULL),			/* 2nd Def */
      GSF_XML_IN_NODE (PARTIAL_XF, BORDER,  XL_NS_SS, "border", GSF_XML_NO_CONTENT, NULL, NULL),		/* 2nd Def */
      GSF_XML_IN_NODE (PARTIAL_XF, DXF_ALIGNMENT, XL_NS_SS, "alignment", GSF_XML_NO_CONTENT, NULL, NULL),
      GSF_XML_IN_NODE (PARTIAL_XF, DXF_PROTECTION, XL_NS_SS, "protection", GSF_XML_NO_CONTENT, NULL, NULL),
      GSF_XML_IN_NODE (PARTIAL_XF, DXF_FSB, XL_NS_SS, "extLst", GSF_XML_NO_CONTENT, NULL, NULL),

  GSF_XML_IN_NODE_FULL (STYLE_INFO, TABLE_STYLES, XL_NS_SS, "tableStyles", GSF_XML_NO_CONTENT,
			FALSE, FALSE, &xlsx_collection_begin, &xlsx_collection_end, XLSX_COLLECT_TABLE_STYLES),
    GSF_XML_IN_NODE (TABLE_STYLES, TABLE_STYLE, XL_NS_SS, "tableStyle", GSF_XML_NO_CONTENT, NULL, NULL),

  GSF_XML_IN_NODE (STYLE_INFO, COLORS, XL_NS_SS, "colors", GSF_XML_NO_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (COLORS, INDEXED_COLORS, XL_NS_SS, "indexedColors", GSF_XML_NO_CONTENT, NULL, NULL),
      GSF_XML_IN_NODE (INDEXED_COLORS, INDEXED_RGB, XL_NS_SS, "rgbColor", GSF_XML_NO_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (COLORS, THEME_COLORS, XL_NS_SS, "themeColors", GSF_XML_NO_CONTENT, NULL, NULL),
      GSF_XML_IN_NODE (THEME_COLORS, THEMED_RGB, XL_NS_SS, "rgbColor", GSF_XML_NO_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (COLORS, MRU_COLORS, XL_NS_SS, "mruColors", GSF_XML_NO_CONTENT, NULL, NULL),
      GSF_XML_IN_NODE (MRU_COLORS, MRU_COLOR, XL_NS_SS, "color", GSF_XML_NO_CONTENT, NULL, NULL),

GSF_XML_IN_NODE_END
};

/****************************************************************************/

static void
xlsx_theme_color_sys (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState	*state = (XLSXReadState *)xin->user_state;
	GOColor c;
	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_gocolor (xin, attrs, "lastClr", &c)) {
			g_hash_table_replace (state->theme_colors_by_name,
				g_strdup (((GsfXMLInNode *)xin->node_stack->data)->name),
				GUINT_TO_POINTER (c));
		}
}
static void
xlsx_theme_color_rgb (GsfXMLIn *xin, xmlChar const **attrs)
{
	XLSXReadState	*state = (XLSXReadState *)xin->user_state;
	GOColor c;
	for (; attrs != NULL && attrs[0] && attrs[1] ; attrs += 2)
		if (attr_gocolor (xin, attrs, "val", &c)) {
			g_hash_table_replace (state->theme_colors_by_name,
				g_strdup (((GsfXMLInNode *)xin->node_stack->data)->name),
				GUINT_TO_POINTER (c));
		}
}

static GsfXMLInNode const xlsx_theme_dtd[] = {
GSF_XML_IN_NODE_FULL (START, START, -1, NULL, GSF_XML_NO_CONTENT, FALSE, TRUE, NULL, NULL, 0),
GSF_XML_IN_NODE_FULL (START, THEME, XL_NS_DRAW, "theme", GSF_XML_NO_CONTENT, FALSE, TRUE, NULL, NULL, 0),
  GSF_XML_IN_NODE (THEME, ELEMENTS, XL_NS_DRAW, "themeElements", GSF_XML_NO_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (ELEMENTS, COLOR_SCHEME, XL_NS_DRAW, "clrScheme", GSF_XML_NO_CONTENT, NULL, NULL),
      GSF_XML_IN_NODE (COLOR_SCHEME, dk1, XL_NS_DRAW, "dk1", GSF_XML_NO_CONTENT, NULL, NULL),
        GSF_XML_IN_NODE (dk1, SYS_COLOR, XL_NS_DRAW, "sysClr", GSF_XML_NO_CONTENT, &xlsx_theme_color_sys, NULL),
        GSF_XML_IN_NODE (dk1, RGB_COLOR, XL_NS_DRAW, "srgbClr", GSF_XML_NO_CONTENT, &xlsx_theme_color_rgb, NULL),
          GSF_XML_IN_NODE (RGB_COLOR, COLOR_ALPHA, XL_NS_DRAW, "alpha", GSF_XML_NO_CONTENT, NULL, NULL),
      GSF_XML_IN_NODE (COLOR_SCHEME, lt1, XL_NS_DRAW, "lt1", GSF_XML_NO_CONTENT, NULL, NULL),
        GSF_XML_IN_NODE (lt1, SYS_COLOR, XL_NS_DRAW, "sysClr", GSF_XML_NO_CONTENT, NULL, NULL),	/* 2nd Def */
        GSF_XML_IN_NODE (lt1, RGB_COLOR, XL_NS_DRAW, "srgbClr", GSF_XML_NO_CONTENT, NULL, NULL),	/* 2nd Def */
      GSF_XML_IN_NODE (COLOR_SCHEME, lt2, XL_NS_DRAW, "lt2", GSF_XML_NO_CONTENT, NULL, NULL),
        GSF_XML_IN_NODE (lt2, SYS_COLOR, XL_NS_DRAW, "sysClr", GSF_XML_NO_CONTENT, NULL, NULL),	/* 2nd Def */
        GSF_XML_IN_NODE (lt2, RGB_COLOR, XL_NS_DRAW, "srgbClr", GSF_XML_NO_CONTENT, NULL, NULL),	/* 2nd Def */
      GSF_XML_IN_NODE (COLOR_SCHEME, dk2, XL_NS_DRAW, "dk2", GSF_XML_NO_CONTENT, NULL, NULL),
        GSF_XML_IN_NODE (dk2, SYS_COLOR, XL_NS_DRAW, "sysClr", GSF_XML_NO_CONTENT, NULL, NULL),	/* 2nd Def */
        GSF_XML_IN_NODE (dk2, RGB_COLOR, XL_NS_DRAW, "srgbClr", GSF_XML_NO_CONTENT, NULL, NULL),	/* 2nd Def */
      GSF_XML_IN_NODE (COLOR_SCHEME, accent1, XL_NS_DRAW, "accent1", GSF_XML_NO_CONTENT, NULL, NULL),
        GSF_XML_IN_NODE (accent1, SYS_COLOR, XL_NS_DRAW, "sysClr", GSF_XML_NO_CONTENT, NULL, NULL),	/* 2nd Def */
        GSF_XML_IN_NODE (accent1, RGB_COLOR, XL_NS_DRAW, "srgbClr", GSF_XML_NO_CONTENT, NULL, NULL),	/* 2nd Def */
      GSF_XML_IN_NODE (COLOR_SCHEME, accent2, XL_NS_DRAW, "accent2", GSF_XML_NO_CONTENT, NULL, NULL),
        GSF_XML_IN_NODE (accent2, SYS_COLOR, XL_NS_DRAW, "sysClr", GSF_XML_NO_CONTENT, NULL, NULL),	/* 2nd Def */
        GSF_XML_IN_NODE (accent2, RGB_COLOR, XL_NS_DRAW, "srgbClr", GSF_XML_NO_CONTENT, NULL, NULL),	/* 2nd Def */
      GSF_XML_IN_NODE (COLOR_SCHEME, accent3, XL_NS_DRAW, "accent3", GSF_XML_NO_CONTENT, NULL, NULL),
        GSF_XML_IN_NODE (accent3, SYS_COLOR, XL_NS_DRAW, "sysClr", GSF_XML_NO_CONTENT, NULL, NULL),	/* 2nd Def */
        GSF_XML_IN_NODE (accent3, RGB_COLOR, XL_NS_DRAW, "srgbClr", GSF_XML_NO_CONTENT, NULL, NULL),	/* 2nd Def */
      GSF_XML_IN_NODE (COLOR_SCHEME, accent4, XL_NS_DRAW, "accent4", GSF_XML_NO_CONTENT, NULL, NULL),
        GSF_XML_IN_NODE (accent4, SYS_COLOR, XL_NS_DRAW, "sysClr", GSF_XML_NO_CONTENT, NULL, NULL),	/* 2nd Def */
        GSF_XML_IN_NODE (accent4, RGB_COLOR, XL_NS_DRAW, "srgbClr", GSF_XML_NO_CONTENT, NULL, NULL),	/* 2nd Def */
      GSF_XML_IN_NODE (COLOR_SCHEME, accent5, XL_NS_DRAW, "accent5", GSF_XML_NO_CONTENT, NULL, NULL),
        GSF_XML_IN_NODE (accent5, SYS_COLOR, XL_NS_DRAW, "sysClr", GSF_XML_NO_CONTENT, NULL, NULL),	/* 2nd Def */
        GSF_XML_IN_NODE (accent5, RGB_COLOR, XL_NS_DRAW, "srgbClr", GSF_XML_NO_CONTENT, NULL, NULL),	/* 2nd Def */
      GSF_XML_IN_NODE (COLOR_SCHEME, accent6, XL_NS_DRAW, "accent6", GSF_XML_NO_CONTENT, NULL, NULL),
        GSF_XML_IN_NODE (accent6, SYS_COLOR, XL_NS_DRAW, "sysClr", GSF_XML_NO_CONTENT, NULL, NULL),	/* 2nd Def */
        GSF_XML_IN_NODE (accent6, RGB_COLOR, XL_NS_DRAW, "srgbClr", GSF_XML_NO_CONTENT, NULL, NULL),	/* 2nd Def */
      GSF_XML_IN_NODE (COLOR_SCHEME, hlink, XL_NS_DRAW, "hlink", GSF_XML_NO_CONTENT, NULL, NULL),
        GSF_XML_IN_NODE (hlink, SYS_COLOR, XL_NS_DRAW, "sysClr", GSF_XML_NO_CONTENT, NULL, NULL),	/* 2nd Def */
        GSF_XML_IN_NODE (hlink, RGB_COLOR, XL_NS_DRAW, "srgbClr", GSF_XML_NO_CONTENT, NULL, NULL),	/* 2nd Def */
      GSF_XML_IN_NODE (COLOR_SCHEME, folHlink, XL_NS_DRAW, "folHlink", GSF_XML_NO_CONTENT, NULL, NULL),
        GSF_XML_IN_NODE (folHlink, SYS_COLOR, XL_NS_DRAW, "sysClr", GSF_XML_NO_CONTENT, NULL, NULL),	/* 2nd Def */
        GSF_XML_IN_NODE (folHlink, RGB_COLOR, XL_NS_DRAW, "srgbClr", GSF_XML_NO_CONTENT, NULL, NULL),/* 2nd Def */

    GSF_XML_IN_NODE (ELEMENTS, FONT_SCHEME, XL_NS_DRAW, "fontScheme", GSF_XML_NO_CONTENT, NULL, NULL),
      GSF_XML_IN_NODE (FONT_SCHEME, MAJOR_FONT, XL_NS_DRAW, "majorFont", GSF_XML_NO_CONTENT, NULL, NULL),
        GSF_XML_IN_NODE (MAJOR_FONT, FONT_CS, XL_NS_DRAW, "cs", GSF_XML_NO_CONTENT, NULL, NULL),
        GSF_XML_IN_NODE (MAJOR_FONT, FONT_EA, XL_NS_DRAW, "ea", GSF_XML_NO_CONTENT, NULL, NULL),
        GSF_XML_IN_NODE (MAJOR_FONT, FONT_FONT, XL_NS_DRAW, "font", GSF_XML_NO_CONTENT, NULL, NULL),
        GSF_XML_IN_NODE (MAJOR_FONT, FONT_LATIN, XL_NS_DRAW, "latin", GSF_XML_NO_CONTENT, NULL, NULL),
      GSF_XML_IN_NODE (FONT_SCHEME, MINOR_FONT, XL_NS_DRAW, "minorFont", GSF_XML_NO_CONTENT, NULL, NULL),
        GSF_XML_IN_NODE (MINOR_FONT, FONT_CS, XL_NS_DRAW, "cs", GSF_XML_NO_CONTENT, NULL, NULL),
        GSF_XML_IN_NODE (MINOR_FONT, FONT_EA, XL_NS_DRAW, "ea", GSF_XML_NO_CONTENT, NULL, NULL),
        GSF_XML_IN_NODE (MINOR_FONT, FONT_FONT, XL_NS_DRAW, "font", GSF_XML_NO_CONTENT, NULL, NULL),
        GSF_XML_IN_NODE (MINOR_FONT, FONT_LATIN, XL_NS_DRAW, "latin", GSF_XML_NO_CONTENT, NULL, NULL),

    GSF_XML_IN_NODE (ELEMENTS, FORMAT_SCHEME, XL_NS_DRAW, "fmtScheme", GSF_XML_NO_CONTENT, NULL, NULL),
      GSF_XML_IN_NODE (FORMAT_SCHEME, FILL_STYLE_LIST,	XL_NS_DRAW, "fillStyleLst", GSF_XML_NO_CONTENT, NULL, NULL),
        GSF_XML_IN_NODE (FILL_STYLE_LIST,  SOLID_FILL, XL_NS_DRAW, "solidFill", GSF_XML_NO_CONTENT, NULL, NULL),
          GSF_XML_IN_NODE (SOLID_FILL, SCHEME_COLOR, XL_NS_DRAW, "schemeClr", GSF_XML_NO_CONTENT, NULL, NULL),
           GSF_XML_IN_NODE (SCHEME_COLOR, COLOR_TINT, XL_NS_DRAW, "tint", GSF_XML_NO_CONTENT, NULL, NULL),
           GSF_XML_IN_NODE (SCHEME_COLOR, COLOR_LUM, XL_NS_DRAW, "lumMod", GSF_XML_NO_CONTENT, NULL, NULL),
           GSF_XML_IN_NODE (SCHEME_COLOR, COLOR_SAT, XL_NS_DRAW, "satMod", GSF_XML_NO_CONTENT, NULL, NULL),
           GSF_XML_IN_NODE (SCHEME_COLOR, COLOR_SHADE, XL_NS_DRAW, "shade", GSF_XML_NO_CONTENT, NULL, NULL),
        GSF_XML_IN_NODE (FILL_STYLE_LIST,  GRAD_FILL, XL_NS_DRAW, "gradFill", GSF_XML_NO_CONTENT, NULL, NULL),
          GSF_XML_IN_NODE (GRAD_FILL, GRAD_PATH, XL_NS_DRAW, "path", GSF_XML_NO_CONTENT, NULL, NULL),
            GSF_XML_IN_NODE (GRAD_PATH, GRAD_PATH_RECT, XL_NS_DRAW, "fillToRect", GSF_XML_NO_CONTENT, NULL, NULL),
	  GSF_XML_IN_NODE (GRAD_FILL, GRAD_LIST, XL_NS_DRAW, "gsLst", GSF_XML_NO_CONTENT, NULL, NULL),
	   GSF_XML_IN_NODE (GRAD_LIST, GRAD_LIST_ITEM, XL_NS_DRAW, "gs", GSF_XML_NO_CONTENT, NULL, NULL),
	     GSF_XML_IN_NODE (GRAD_LIST_ITEM, RGB_COLOR, XL_NS_DRAW, "srgbClr", GSF_XML_NO_CONTENT, NULL, NULL),	/* 2nd Def */
             GSF_XML_IN_NODE (GRAD_LIST_ITEM, SCHEME_COLOR, XL_NS_DRAW, "schemeClr", GSF_XML_NO_CONTENT, NULL, NULL),	/* 2nd Def */
	  GSF_XML_IN_NODE (GRAD_FILL, GRAD_LINE,	XL_NS_DRAW, "lin", GSF_XML_NO_CONTENT, NULL, NULL),

      GSF_XML_IN_NODE (FORMAT_SCHEME, BG_FILL_STYLE_LIST,	XL_NS_DRAW, "bgFillStyleLst", GSF_XML_NO_CONTENT, NULL, NULL),
        GSF_XML_IN_NODE (BG_FILL_STYLE_LIST, GRAD_FILL, XL_NS_DRAW, "gradFill", GSF_XML_NO_CONTENT, NULL, NULL),	/* 2nd Def */
        GSF_XML_IN_NODE (BG_FILL_STYLE_LIST, SOLID_FILL, XL_NS_DRAW, "solidFill", GSF_XML_NO_CONTENT, NULL, NULL),	/* 2nd Def */
      GSF_XML_IN_NODE (FORMAT_SCHEME, LINE_STYLE_LIST,	XL_NS_DRAW, "lnStyleLst", GSF_XML_NO_CONTENT, NULL, NULL),
        GSF_XML_IN_NODE (LINE_STYLE_LIST, LINE_STYLE, XL_NS_DRAW, "ln", GSF_XML_NO_CONTENT, NULL, NULL),
	  GSF_XML_IN_NODE (LINE_STYLE, LN_NOFILL, XL_NS_DRAW, "noFill", GSF_XML_NO_CONTENT, NULL, NULL),
	  GSF_XML_IN_NODE (LINE_STYLE, LN_DASH, XL_NS_DRAW, "prstDash", GSF_XML_NO_CONTENT, NULL, NULL),		/* 2nd Def */
	  GSF_XML_IN_NODE (LINE_STYLE, SOLID_FILL, XL_NS_DRAW, "solidFill", GSF_XML_NO_CONTENT, NULL, NULL),		/* 2nd Def */
	  GSF_XML_IN_NODE (LINE_STYLE, FILL_PATT,	XL_NS_DRAW, "pattFill", GSF_XML_NO_CONTENT, NULL, NULL),
	  GSF_XML_IN_NODE (FORMAT_SCHEME, EFFECT_STYLE_LIST,	XL_NS_DRAW, "effectStyleLst", GSF_XML_NO_CONTENT, NULL, NULL),
            GSF_XML_IN_NODE (EFFECT_STYLE_LIST, EFFECT_STYLE,	XL_NS_DRAW, "effectStyle", GSF_XML_NO_CONTENT, NULL, NULL),
	      GSF_XML_IN_NODE (EFFECT_STYLE, EFFECT_PROP, XL_NS_DRAW, "sp3d", GSF_XML_NO_CONTENT, NULL, NULL),
		GSF_XML_IN_NODE (EFFECT_PROP, PROP_BEVEL, XL_NS_DRAW, "bevelT", GSF_XML_NO_CONTENT, NULL, NULL),
	      GSF_XML_IN_NODE (EFFECT_STYLE, EFFECT_LIST, XL_NS_DRAW, "effectLst", GSF_XML_NO_CONTENT, NULL, NULL),
		GSF_XML_IN_NODE (EFFECT_LIST, OUTER_SHADOW, XL_NS_DRAW, "outerShdw", GSF_XML_NO_CONTENT, NULL, NULL),
		  GSF_XML_IN_NODE (OUTER_SHADOW, RGB_COLOR, XL_NS_DRAW, "srgbClr", GSF_XML_NO_CONTENT, NULL, NULL),	/* 2nd Def */
	      GSF_XML_IN_NODE (EFFECT_STYLE, EFFECT_SCENE_3D, XL_NS_DRAW, "scene3d", GSF_XML_NO_CONTENT, NULL, NULL),
		GSF_XML_IN_NODE (EFFECT_SCENE_3D, 3D_CAMERA, XL_NS_DRAW, "camera", GSF_XML_NO_CONTENT, NULL, NULL),
		  GSF_XML_IN_NODE (3D_CAMERA, 3D_ROT, XL_NS_DRAW, "rot", GSF_XML_NO_CONTENT, NULL, NULL),
		GSF_XML_IN_NODE (EFFECT_SCENE_3D, 3D_LIGHT, XL_NS_DRAW, "lightRig", GSF_XML_NO_CONTENT, NULL, NULL),
		  GSF_XML_IN_NODE (3D_LIGHT, 3D_ROT, XL_NS_DRAW, "rot", GSF_XML_NO_CONTENT, NULL, NULL),

  GSF_XML_IN_NODE (THEME, OBJ_DEFAULTS, XL_NS_DRAW, "objectDefaults", GSF_XML_NO_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (OBJ_DEFAULTS, SP_DEF, XL_NS_DRAW, "spDef", GSF_XML_NO_CONTENT, NULL, NULL),
      GSF_XML_IN_NODE (SP_DEF, SHAPE_PR,  XL_NS_DRAW, "spPr", GSF_XML_NO_CONTENT, NULL, NULL),
      GSF_XML_IN_NODE (SP_DEF, BODY_PR,   XL_NS_DRAW, "bodyPr", GSF_XML_NO_CONTENT, NULL, NULL),
      GSF_XML_IN_NODE (SP_DEF, LST_STYLE, XL_NS_DRAW, "lstStyle", GSF_XML_NO_CONTENT, NULL, NULL),
      GSF_XML_IN_NODE (SP_DEF, LN_DEF,	  XL_NS_DRAW, "lnDef", GSF_XML_NO_CONTENT, NULL, NULL),
  GSF_XML_IN_NODE (THEME, EXTRA_COLOR_SCHEME, XL_NS_DRAW, "extraClrSchemeLst", GSF_XML_NO_CONTENT, NULL, NULL),

  GSF_XML_IN_NODE_END
};

/****************************************************************************/

G_MODULE_EXPORT gboolean
xlsx_file_probe (GOFileOpener const *fo, GsfInput *input, GOFileProbeLevel pl);

gboolean
xlsx_file_probe (GOFileOpener const *fo, GsfInput *input, GOFileProbeLevel pl)
{
	GsfInfile *zip;
	GsfInput  *stream;
	gboolean   res = FALSE;

	if (NULL != (zip = gsf_infile_zip_new (input, NULL))) {
		if (NULL != (stream = gsf_infile_child_by_vname (zip, "xl", "workbook.xml", NULL))) {
			g_object_unref (G_OBJECT (stream));
			res = TRUE;
		}
		g_object_unref (G_OBJECT (zip));
	}
	return res;
}

static void
xlsx_style_array_free (GPtrArray *styles)
{
	if (styles != NULL) {
		unsigned i = styles->len;
		GnmStyle *style;
		while (i-- > 0)
			if (NULL != (style = g_ptr_array_index (styles, i)))
				gnm_style_unref (style);

		g_ptr_array_free (styles, TRUE);
	}
}

#include "xlsx-read-docprops.c"

G_MODULE_EXPORT void
xlsx_file_open (GOFileOpener const *fo, GOIOContext *context,
		WorkbookView *wb_view, GsfInput *input);

void
xlsx_file_open (GOFileOpener const *fo, GOIOContext *context,
		WorkbookView *wb_view, GsfInput *input)
{
	XLSXReadState	 state;
	GnmLocale       *locale;

	memset (&state, 0, sizeof (XLSXReadState));
	state.context	= context;
	state.wb_view	= wb_view;
	state.wb	= wb_view_get_workbook (wb_view);
	state.sheet	= NULL;
	state.run_attrs	= NULL;
	state.rich_attrs = NULL;
	state.sst = g_array_new (FALSE, TRUE, sizeof (XLSXStr));
	state.shared_exprs = g_hash_table_new_full (g_str_hash, g_str_equal,
		(GDestroyNotify)g_free, (GDestroyNotify) gnm_expr_top_unref);
	state.cell_styles = g_hash_table_new_full (g_str_hash, g_str_equal,
		(GDestroyNotify)g_free, (GDestroyNotify) gnm_style_unref);
	state.num_fmts = g_hash_table_new_full (g_str_hash, g_str_equal,
		(GDestroyNotify)g_free, (GDestroyNotify) go_format_unref);
	state.date_fmt = xlsx_pivot_date_fmt ();
	state.convs = xlsx_conventions_new ();
	state.theme_colors_by_name = g_hash_table_new_full (g_str_hash, g_str_equal,
		(GDestroyNotify)g_free, NULL);
	/* fill in some default colors (when theme is absent */
	g_hash_table_replace (state.theme_colors_by_name, g_strdup ("bg1"), GUINT_TO_POINTER (GO_COLOR_WHITE));
	state.pivot.cache_by_id = g_hash_table_new_full (g_str_hash, g_str_equal,
		(GDestroyNotify)g_free, (GDestroyNotify) g_object_unref);

	locale = gnm_push_C_locale ();

	if (NULL != (state.zip = gsf_infile_zip_new (input, NULL))) {
		/* optional */
		GsfInput *wb_part = gsf_open_pkg_open_rel_by_type (GSF_INPUT (state.zip),
			"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument", NULL);

		if (NULL != wb_part) {
			GsfInput *in;

			in = gsf_open_pkg_open_rel_by_type (wb_part,
				"http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings", NULL);
			xlsx_parse_stream (&state, in, xlsx_shared_strings_dtd);

			in = gsf_open_pkg_open_rel_by_type (wb_part,
				"http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme", NULL);
			xlsx_parse_stream (&state, in, xlsx_theme_dtd);

			in = gsf_open_pkg_open_rel_by_type (wb_part,
				"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles", NULL);
			xlsx_parse_stream (&state, in, xlsx_styles_dtd);

			xlsx_parse_stream (&state, wb_part, xlsx_workbook_dtd);

			xlsx_read_docprops (&state);
		} else
			go_cmd_context_error_import (GO_CMD_CONTEXT (context),
				_("No workbook stream found."));
		g_object_unref (G_OBJECT (state.zip));
	}

	gnm_pop_C_locale (locale);

	if (NULL != state.sst) {
		unsigned i = state.sst->len;
		XLSXStr *entry;
		while (i-- > 0) {
			entry = &g_array_index (state.sst, XLSXStr, i);
			go_string_unref (entry->str);
			go_format_unref (entry->markup);
		}
		g_array_free (state.sst, TRUE);
	}
	g_hash_table_destroy (state.pivot.cache_by_id);
	xlsx_conventions_free (state.convs);
	go_format_unref (state.date_fmt);
	g_hash_table_destroy (state.num_fmts);
	g_hash_table_destroy (state.cell_styles);
	g_hash_table_destroy (state.shared_exprs);
	xlsx_style_array_free (state.fonts);
	xlsx_style_array_free (state.fills);
	xlsx_style_array_free (state.borders);
	xlsx_style_array_free (state.xfs);
	xlsx_style_array_free (state.style_xfs);
	xlsx_style_array_free (state.dxfs);
	xlsx_style_array_free (state.table_styles);
	g_hash_table_destroy (state.theme_colors_by_name);

	workbook_set_saveinfo (state.wb, GO_FILE_FL_AUTO,
		go_file_saver_for_id ("Gnumeric_Excel:xlsx"));
}

/* TODO * TODO * TODO
 *
 * IMPROVE
 *	- column widths : Don't use hard coded font size
 *	- share colours
 *	- conditional formats
 *		: other condition types
 *		: check binary operators
 *
 * ".xlam",	"application/vnd.ms-excel.addin.macroEnabled.12" ,
 * ".xlsb",	"application/vnd.ms-excel.sheet.binary.macroEnabled.12" ,
 * ".xlsm",	"application/vnd.ms-excel.sheet.macroEnabled.12" ,
 * ".xlsx",	"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet" ,
 * ".xltm",	"application/vnd.ms-excel.template.macroEnabled.12" ,
 * ".xltx",	"application/vnd.openxmlformats-officedocument.spreadsheetml.template"
**/
