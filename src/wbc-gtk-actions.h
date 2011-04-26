/* vim: set sw=8: -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
#ifndef _GNM_WBC_GTK_ACTIONS_H_
# define _GNM_WBC_GTK_ACTIONS_H_

#include "gnumeric.h"
#include "gui-gnumeric.h"
#include "workbook-control.h"
#include "widgets/gnumeric-expr-entry.h"

#include <gtk/gtk.h>

G_BEGIN_DECLS

	GtkButton *standard_tb;
	GtkButton *format_tb;
	GtkButton *object_tb;
	gboolean standard_tb_b;
	gboolean format_tb_b;
	gboolean object_tb_b;

	typedef struct _OpenLocation {
		WBCGtk *app;
		char *file;
	} OpenLocation;

	HildonAppMenu* create_hildon_main_menu(WBCGtk *wbcg);
	void open_launchPage();
	void wbc_gtk_set_toggle_action_state (WBCGtk const *wbcg, char const *action, gboolean state);

G_END_DECLS

#endif /* _GNM_WBC_GTK_H_ */
