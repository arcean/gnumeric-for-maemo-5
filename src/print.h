/* vim: set sw=8: -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
#ifndef _GNM_PRINT_H_
# define _GNM_PRINT_H_

#include "gnumeric.h"
#include <gsf/gsf-output.h>
#include <cairo.h>

G_BEGIN_DECLS

#define GNUMERIC_PRINT_SETTING_PRINTRANGE_KEY		"GnumericPrintRange"
#define GNUMERIC_PRINT_SETTING_PRINT_FROM_SHEET_KEY	"GnumericPrintFromSheet"
#define GNUMERIC_PRINT_SETTING_PRINT_TO_SHEET_KEY	"GnumericPrintToSheet"
#define GNUMERIC_PRINT_SETTING_IGNORE_PAGE_BREAKS_KEY   "GnumericPrintIgnorePageBreaks"

typedef enum { /* These numbers are saved in .gnuemric files */
	PRINT_SAVED_INFO = -1,
	PRINT_ACTIVE_SHEET = 0,
	PRINT_ALL_SHEETS = 1,
	PRINT_ALL_SHEETS_INCLUDING_HIDDEN = 2,
	PRINT_SHEET_RANGE = 3,
	PRINT_SHEET_SELECTION = 4,
	PRINT_IGNORE_PRINTAREA = 5,
	PRINT_SHEET_SELECTION_IGNORE_PRINTAREA = 6
} PrintRange;

void gnm_print_sheet (WorkbookControl *wbc, Sheet *sheet,
		      gboolean preview, PrintRange default_range,
		      GsfOutput *export_dst);

void gnm_print_sheet_objects (cairo_t *cr,
			      Sheet const *sheet,
			      GnmRange *range,
			      double base_x, double base_y);

/* Internal */
extern gboolean gnm_print_debug;

G_END_DECLS

#endif /* _GNM_PRINT_H_ */
