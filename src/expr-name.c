/* vim: set sw=8: -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * expr-name.c: Supported named expressions
 *
 * Author:
 *    Jody Goldberg <jody@gnome.org>
 *
 * Based on work by:
 *    Michael Meeks <michael@ximian.com>
 */

#include <gnumeric-config.h>
#include <glib/gi18n-lib.h>
#include <string.h>
#include "gnumeric.h"
#include "expr-name.h"

#include "dependent.h"
#include "value.h"
#include "workbook-priv.h"
#include "expr.h"
#include "expr-impl.h"
#include "sheet.h"
#include "ranges.h"
#include "gutils.h"
#include "sheet-style.h"

#include <goffice/goffice.h>


static gboolean
expr_name_validate_r1c1 (const char *name)
{
	const char *p = name;
	gint i;

	if (p[0] != 'R' && p[0] != 'r')
		return TRUE;
	p++;
	/* no need to worry about [] since they are not alphanumeric */
	for (i = 0; p[0] && g_ascii_isdigit (p[0]); p = g_utf8_next_char (p))
		i++;
	if (i==0)
		return TRUE;
	if (p[0] != 'C' && p[0] != 'c')
		return TRUE;
	p++;
	for (i = 0; p[0] && g_ascii_isdigit (p[0]); p = g_utf8_next_char (p))
		i++;
	if (i==0)
		return TRUE;
	return (p[0] != '\0');
}

static gboolean
expr_name_validate_a1 (const char *name)
{
	const char *p = name;
	gint i;

	for (i = 0; *p && g_ascii_isalpha(p[0]);
	     p = g_utf8_next_char (p))
		i++;
	if (i==0 || i>4) /* We want to allow "total2010" and it   */
		         /* is unlikely to have more than  456976 */
		         /* columns  atm */
		return TRUE;
	for (i = 0; *p && g_ascii_isdigit (p[0]);
	     p = g_utf8_next_char (p))
		i++;
	if (i==0)
		return TRUE;
	return (*p != '\0');
}

/**
 * expr_name_validate:
 * @name: tentative name
 *
 * returns TRUE if the given name is valid, FALSE otherwise.
 */
gboolean
expr_name_validate (const char *name)
{
	const char *p;
	GnmValue *v;

	g_return_val_if_fail (name != NULL, FALSE);

	if (name[0] == 0)
		return FALSE;

	v = value_new_from_string (VALUE_BOOLEAN, name, NULL, TRUE);
	if (!v)
		v = value_new_from_string (VALUE_BOOLEAN, name, NULL, FALSE);
	if (v) {
		value_release (v);
		return FALSE;
	}

	/* Hmm...   Now what?  */
	if (!g_unichar_isalpha (g_utf8_get_char (name)) &&
	    name[0] != '_')
		return FALSE;

	for (p = name; *p; p = g_utf8_next_char (p)) {
		if (!g_unichar_isalnum (g_utf8_get_char (p)) &&
		    p[0] != '_')
			return FALSE;
	}

	/* Make sure it's not A1 etc.*/
	/* Note that we can't use our regular parsers */
	/* since we also have to avoid names that may become */
	/* sensible when the sheet size changes. */
	if (!expr_name_validate_a1 (name))
		return FALSE;

	/* What about R1C1?  */
	if (!expr_name_validate_r1c1 (name))
		return FALSE;

	return TRUE;
}


static void
cb_nexpr_remove (GnmNamedExpr *nexpr)
{
	g_return_if_fail (nexpr->scope != NULL);

	nexpr->scope = NULL;
	expr_name_set_expr (nexpr, NULL);
	expr_name_unref (nexpr);
}

static void
cb_collect_name_deps (gpointer key, G_GNUC_UNUSED gpointer value,
		      gpointer user_data)
{
	GSList **list = user_data;
	*list = g_slist_prepend (*list, key);
}

static GSList *
expr_name_unlink_deps (GnmNamedExpr *nexpr)
{
	GSList *ptr, *deps = NULL;

	if (nexpr->dependents == NULL)
		return NULL;

	g_hash_table_foreach (nexpr->dependents, cb_collect_name_deps, &deps);

	/* pull them out */
	for (ptr = deps ; ptr != NULL ; ptr = ptr->next) {
		GnmDependent *dep = ptr->data;
		if (dependent_is_linked (dep))
			dependent_unlink (dep);
	}
	return deps;
}

static void
expr_name_relink_deps (GnmNamedExpr *nexpr)
{
	GSList *deps = NULL;

	if (nexpr->dependents == NULL)
		return;

	g_hash_table_foreach (nexpr->dependents, cb_collect_name_deps, &deps);
	dependents_link (deps);
	g_slist_free (deps);
}

GnmNamedExprCollection *
gnm_named_expr_collection_new (void)
{
	GnmNamedExprCollection *res = g_new (GnmNamedExprCollection, 1);

	res->names = g_hash_table_new_full (g_str_hash, g_str_equal,
		NULL, (GDestroyNotify) cb_nexpr_remove);
	res->placeholders = g_hash_table_new_full (g_str_hash, g_str_equal,
		NULL, (GDestroyNotify) cb_nexpr_remove);

	return res;
}

/**
 * gnm_named_expr_collection_free :
 * @names : The collection of names
 *
 * Frees names defined in the local scope.
 * NOTE : THIS DOES NOT INVALIDATE NAMES THAT REFER
 *        TO THIS SCOPE.
 *        eg
 *           in scope sheet2 we have a name that refers
 *           to sheet1.  That will remain!
 **/
void
gnm_named_expr_collection_free (GnmNamedExprCollection *names)
{
	if (names != NULL) {
		g_hash_table_destroy (names->names);
		g_hash_table_destroy (names->placeholders);
		g_free (names);
	}
}

static void
cb_unlink_all_names (G_GNUC_UNUSED gpointer key,
		     gpointer value,
		     G_GNUC_UNUSED gpointer user_data)
{
	GnmNamedExpr *nexpr = value;
	GSList *deps = expr_name_unlink_deps (nexpr);
	g_slist_free (deps);
}

void
gnm_named_expr_collection_unlink (GnmNamedExprCollection *names)
{
	if (!names)
		return;

	g_hash_table_foreach (names->names,
			      cb_unlink_all_names,
			      NULL);
}

static void
cb_relink_all_names (G_GNUC_UNUSED gpointer key,
		     gpointer value,
		     G_GNUC_UNUSED gpointer user_data)
{
	GnmNamedExpr *nexpr = value;
	expr_name_relink_deps (nexpr);
}

void
gnm_named_expr_collection_relink (GnmNamedExprCollection *names)
{
	if (!names)
		return;

	g_hash_table_foreach (names->names,
			      cb_relink_all_names,
			      NULL);
}

GnmNamedExpr *
gnm_named_expr_collection_lookup (GnmNamedExprCollection const *scope,
				  char const *name)
{
	if (scope != NULL) {
		GnmNamedExpr *nexpr = g_hash_table_lookup (scope->names, name);
		if (nexpr == NULL)
			nexpr = g_hash_table_lookup (scope->placeholders, name);
		return nexpr;
	} else
		return NULL;
}

static void
cb_list_names (G_GNUC_UNUSED gpointer key,
	       gpointer value,
	       gpointer user_data)
{
	GSList **pres = user_data;
	GO_SLIST_PREPEND (*pres, value);
}

GSList *
gnm_named_expr_collection_list (GnmNamedExprCollection const *scope)
{
	GSList *res = NULL;
	if (scope) {
		g_hash_table_foreach (scope->names,
				      cb_list_names,
				      &res);
	}
	return res;
}

static void
gnm_named_expr_collection_insert (GnmNamedExprCollection *scope,
				  GnmNamedExpr *nexpr)
{
	if (gnm_debug_flag ("names")) {
		char *scope_name = nexpr->pos.sheet
			? g_strdup_printf ("sheet %s", nexpr->pos.sheet->name_quoted)
			: g_strdup ("workbook");
		g_printerr ("Inserting name %s into its %s container%s\n",
			    nexpr->name->str,
			    scope_name,
			    nexpr->is_placeholder ? " as a placeholder" : "");
		g_free (scope_name);
	}

	/* name can be active at this point, eg we are converting a
	 * placeholder, or changing a scope */
	nexpr->scope = scope;
	g_hash_table_replace (nexpr->is_placeholder
	      ? scope->placeholders : scope->names, (gpointer)nexpr->name->str, nexpr);
}

typedef struct {
	Sheet const *sheet;
	GnmRange const *r;
	GnmNamedExpr *res;
} CheckName;

static void
cb_check_name (G_GNUC_UNUSED gpointer key, GnmNamedExpr *nexpr,
	       CheckName *user)
{
	GnmValue *v;

	if (nexpr->scope == NULL || nexpr->is_hidden || !nexpr->texpr)
		return;

	v = gnm_expr_top_get_range (nexpr->texpr);
	if (v != NULL) {
		if (v->type == VALUE_CELLRANGE) {
			GnmRangeRef const *ref = &v->v_range.cell;
			if (!ref->a.col_relative &&
			    !ref->b.col_relative &&
			    !ref->a.row_relative &&
			    !ref->b.row_relative &&
			    eval_sheet (ref->a.sheet, user->sheet) == user->sheet &&
			    eval_sheet (ref->b.sheet, user->sheet) == user->sheet &&
			    MIN (ref->a.col, ref->b.col) == user->r->start.col &&
			    MAX (ref->a.col, ref->b.col) == user->r->end.col &&
			    MIN (ref->a.row, ref->b.row) == user->r->start.row &&
			    MAX (ref->a.row, ref->b.row) == user->r->end.row)
				user->res = nexpr;
		}
		value_release (v);
	}
}

static GnmNamedExpr *
gnm_named_expr_collection_check (GnmNamedExprCollection *scope,
				 Sheet const *sheet, GnmRange const *r)
{
	CheckName user;

	if (scope == NULL)
		return NULL;

	user.sheet = sheet;
	user.r	   = r;
	user.res   = NULL;

	g_hash_table_foreach (scope->names, (GHFunc)cb_check_name, &user);
	return user.res;
}

/* Iterate over all names, including placeholders.  */
void
gnm_named_expr_collection_foreach (GnmNamedExprCollection *names,
				   GHFunc func,
				   gpointer data)
{
	g_hash_table_foreach (names->names, func, data);
	g_hash_table_foreach (names->placeholders, func, data);
}

/******************************************************************************/

/**
 * expr_name_handle_references : register or unregister a name with
 *    all of the sheets it explicitly references.  This is necessary
 *    because names are not dependents, and if they reference a deleted
 *    sheet we will not notice.
 */
static void
expr_name_handle_references (GnmNamedExpr *nexpr, gboolean add)
{
	GSList *sheets, *ptr;

	sheets = gnm_expr_top_referenced_sheets (nexpr->texpr);

	for (ptr = sheets ; ptr != NULL ; ptr = ptr->next) {
		Sheet *sheet = ptr->data;
		GnmNamedExpr *found;

		/* Implicit reference.  */
		if (!sheet)
			continue;

		/* No need to do anything during destruction */
		if (sheet->deps == NULL)
			continue;

		found = g_hash_table_lookup (sheet->deps->referencing_names, nexpr);
		if (add) {
			if (found == NULL)  {
				g_hash_table_insert (sheet->deps->referencing_names, nexpr, nexpr);
			} else {
				g_warning ("Name being registered multiple times ?");
			}
		} else {
			if (found == NULL)  {
				g_warning ("Unregistered name being removed?");
			} else {
				g_hash_table_remove (sheet->deps->referencing_names, nexpr);
			}
		}
	}
	g_slist_free (sheets);
}


/**
 * expr_name_lookup:
 * @pp :
 * @name :
 *
 * lookup but do not reference a named expression.
 */
GnmNamedExpr *
expr_name_lookup (GnmParsePos const *pp, char const *name)
{
	GnmNamedExpr *res = NULL;
	Sheet const *sheet = NULL;
	Workbook const *wb = NULL;

	g_return_val_if_fail (name != NULL, NULL);

	if (pp != NULL) {
		sheet = pp->sheet;
		wb = (sheet != NULL) ? sheet->workbook : pp->wb;
	}

	if (sheet != NULL && sheet->names != NULL)
		res = gnm_named_expr_collection_lookup (sheet->names, name);
	if (res == NULL && wb != NULL && wb->names != NULL)
		res = gnm_named_expr_collection_lookup (wb->names, name);
	return res;
}

/**
 * expr_name_new :
 *
 * Creates a new name without linking it into any container.
 **/
GnmNamedExpr *
expr_name_new (char const *name)
{
	GnmNamedExpr *nexpr;

	g_return_val_if_fail (name != NULL, NULL);

	nexpr = g_new0 (GnmNamedExpr,1);

	nexpr->ref_count	= 1;
	nexpr->name		= go_string_new (name);
	nexpr->texpr		= NULL;
	nexpr->dependents	= NULL;
	nexpr->is_placeholder	= TRUE;
	nexpr->is_hidden	= FALSE;
	nexpr->is_permanent	= FALSE;
	nexpr->is_editable	= TRUE;
	nexpr->scope = NULL;

	if (gnm_debug_flag ("names"))
		g_printerr ("Created new name %s\n", name);

	return nexpr;
}

/*
 * NB. if we already have a circular reference in addition
 * to this one we are checking we will come to serious grief.
 */

/* Note: for a loopcheck stop_at_name must be FALSE. */
/*       stop_at_name = TRUE is used when we check all names anyways. */
static gboolean
do_expr_name_loop_check (char const *name, GnmNamedExpr *nexpr,  /* One of these */
			 GnmExpr const *expr,
			 gboolean stop_at_name)
{
	switch (GNM_EXPR_GET_OPER (expr)) {
	case GNM_EXPR_OP_RANGE_CTOR:
	case GNM_EXPR_OP_INTERSECT:
	case GNM_EXPR_OP_ANY_BINARY:
		return (do_expr_name_loop_check (name, nexpr,
						 expr->binary.value_a,
						 stop_at_name) ||
			do_expr_name_loop_check (name, nexpr,
						 expr->binary.value_b,
						 stop_at_name));
	case GNM_EXPR_OP_ANY_UNARY:
		return do_expr_name_loop_check (name, nexpr,
						expr->unary.value,
						stop_at_name);
	case GNM_EXPR_OP_NAME: {
		GnmNamedExpr const *nexpr2 = expr->name.name;
		if (name && !strcmp (nexpr2->name->str, name))
			return TRUE;
		if (nexpr == nexpr2)
			return TRUE;
		if (!stop_at_name && nexpr2->texpr != NULL) /* look inside this name tree too */
			return expr_name_check_for_loop (name, nexpr2->texpr);
		return FALSE;
	}
	case GNM_EXPR_OP_FUNCALL: {
		int i;
		for (i = 0; i < expr->func.argc; i++)
			if (do_expr_name_loop_check (name, nexpr,
						     expr->func.argv[i],
						     stop_at_name))
				return TRUE;
		break;
	}
	case GNM_EXPR_OP_CONSTANT:
	case GNM_EXPR_OP_CELLREF:
	case GNM_EXPR_OP_ARRAY_CORNER:
	case GNM_EXPR_OP_ARRAY_ELEM:
		break;
	case GNM_EXPR_OP_SET: {
		int i;
		for (i = 0; i < expr->set.argc; i++)
			if (do_expr_name_loop_check (name, nexpr,
						     expr->set.argv[i],
						     stop_at_name))
				return TRUE;
		break;
	}
	}
	return FALSE;
}

gboolean
expr_name_check_for_loop (char const *name, GnmExprTop const *texpr)
{
	g_return_val_if_fail (texpr != NULL, TRUE);

	return do_expr_name_loop_check (name, NULL, texpr->expr, FALSE);
}

static void
expr_name_queue_deps (GnmNamedExpr *nexpr)
{
	if (nexpr->dependents)
		g_hash_table_foreach (nexpr->dependents,
				      (GHFunc)dependent_queue_recalc,
				      NULL);
}

/**
 * expr_name_add:
 * @pp:
 * @name:
 * @texpr: if texpr == NULL then create a placeholder with value #NAME?
 * @error_msg:
 * @link_to_container:
 *
 * Absorbs the reference to @texpr.
 * If @error_msg is non NULL it may hold a pointer to a translated descriptive
 * string.  NOTE : caller is responsible for freeing the error message.
 *
 * The reference semantics of the new expression are
 * 1) new names with @link_to_container TRUE are referenced by the container.
 *    The caller DOES NOT OWN a reference to the result, and needs to add their
 *    own.
 * 2) if @link_to_container is FALSE the caller DOES OWN a reference, and
 *    can free the result by unrefing the name.
 **/
GnmNamedExpr *
expr_name_add (GnmParsePos const *pp, char const *name,
	       GnmExprTop const *texpr, char **error_msg,
	       gboolean link_to_container,
	       GnmNamedExpr *stub)
{
	GnmNamedExpr *nexpr = NULL;
	GnmNamedExprCollection *scope = NULL;

	g_return_val_if_fail (pp != NULL, NULL);
	g_return_val_if_fail (pp->sheet != NULL || pp->wb != NULL, NULL);
	g_return_val_if_fail (name != NULL, NULL);
	g_return_val_if_fail (stub == NULL || stub->is_placeholder, NULL);

	if (texpr != NULL && expr_name_check_for_loop (name, texpr)) {
		gnm_expr_top_unref (texpr);
		if (error_msg)
			*error_msg = g_strdup_printf (_("'%s' has a circular reference"), name);
		return NULL;
	}

	scope = (pp->sheet != NULL) ? pp->sheet->names : pp->wb->names;
	/* see if there was a place holder */
	nexpr = g_hash_table_lookup (scope->placeholders, name);
	if (nexpr != NULL) {
		if (texpr == NULL) {
			/* there was already a placeholder for this */
			expr_name_ref (nexpr);
			return nexpr;
		}

		/* convert the placeholder into a real name */
		g_hash_table_steal (scope->placeholders, name);
		nexpr->is_placeholder = FALSE;
	} else {
		nexpr = g_hash_table_lookup (scope->names, name);
		/* If this is a permanent name, we may be adding it */
		/* on opening of a file, although */
		/* the name is already in place. */
		if (nexpr != NULL) {
			if (nexpr->is_permanent)
				link_to_container = FALSE;
			else {
				if (error_msg != NULL)
					*error_msg = (pp->sheet != NULL)
						? g_strdup_printf (_("'%s' is already defined in sheet"), name)
						: g_strdup_printf (_("'%s' is already defined in workbook"), name);

				gnm_expr_top_unref (texpr);
				return NULL;
			}
		}
	}

	if (error_msg)
		*error_msg = NULL;

	if (nexpr == NULL) {
		if (stub != NULL) {
			nexpr = stub;
			stub->is_placeholder = FALSE;
			go_string_unref (stub->name);
			stub->name = go_string_new (name);
		} else {
			nexpr = expr_name_new (name);
			nexpr->is_placeholder = (texpr == NULL);
		}
	}
	parse_pos_init (&nexpr->pos,
		pp->wb, pp->sheet, pp->eval.col, pp->eval.row);
	if (texpr == NULL)
		texpr = gnm_expr_top_new_constant
			(value_new_error_NAME (NULL));
	expr_name_set_expr (nexpr, texpr);
	if (link_to_container)
		gnm_named_expr_collection_insert (scope, nexpr);

	return nexpr;
}

void
expr_name_ref (GnmNamedExpr *nexpr)
{
	g_return_if_fail (nexpr != NULL);

	nexpr->ref_count++;
}

void
expr_name_unref (GnmNamedExpr *nexpr)
{
	g_return_if_fail (nexpr != NULL);

	if (nexpr->ref_count-- > 1)
		return;

	if (gnm_debug_flag ("names"))
		g_printerr ("Finalizing name %s\n", nexpr->name->str);

	g_return_if_fail (nexpr->scope == NULL);

	if (nexpr->name) {
		go_string_unref (nexpr->name);
		nexpr->name = NULL;
	}

	if (nexpr->texpr != NULL)
		expr_name_set_expr (nexpr, NULL);

	if (nexpr->dependents != NULL) {
		g_hash_table_destroy (nexpr->dependents);
		nexpr->dependents  = NULL;
	}

	nexpr->pos.wb      = NULL;
	nexpr->pos.sheet   = NULL;

	g_free (nexpr);
}

/**
 * expr_name_remove :
 * @nexpr :
 *
 * Remove a @nexpr from its container and deactivate it.
 * NOTE : @nexpr may continue to exist if things still have references to it,
 * but they will evaluate to #REF!
 **/
void
expr_name_remove (GnmNamedExpr *nexpr)
{
	g_return_if_fail (nexpr != NULL);
	g_return_if_fail (nexpr->scope != NULL);

	if (gnm_debug_flag ("names")) {
		g_printerr ("Removing name %s from its container%s\n",
			    nexpr->name->str,
			    nexpr->is_placeholder ? " as a placeholder" : "");
	}

	g_hash_table_remove (
		nexpr->is_placeholder ? nexpr->scope->placeholders : nexpr->scope->names,
		nexpr->name->str);
}

const char *
expr_name_name (GnmNamedExpr const *nexpr)
{
	g_return_val_if_fail (nexpr != NULL, NULL);
	return nexpr->name->str;
}

/**
 * expr_name_set_name :
 * @nexpr : the named expression
 * @new_name : the new name of the expression
 *
 * returns: TRUE on error.
 */
gboolean
expr_name_set_name (GnmNamedExpr *nexpr,
		    const char *new_name)
{
	const char *old_name;
	GHashTable *h;

	g_return_val_if_fail (nexpr != NULL, TRUE);
	g_return_val_if_fail (nexpr->scope == NULL || new_name, TRUE);

	old_name = nexpr->name->str;
	if (go_str_compare (new_name, old_name) == 0)
		return FALSE;

#if 0
	g_printerr ("Renaming %s to %s\n", old_name, new_name);
#endif
	h = nexpr->scope
		? (nexpr->is_placeholder
		   ? nexpr->scope->placeholders
		   : nexpr->scope->names)
		: NULL;
	if (h) {
		if (new_name &&
		    (g_hash_table_lookup (nexpr->scope->placeholders, new_name) ||
		     g_hash_table_lookup (nexpr->scope->names, new_name))) {
			/* The only error not to be blamed on the programmer is
			   already-in-use.  */
			return TRUE;
		}

		g_hash_table_steal (h, old_name);
	}

	go_string_unref (nexpr->name);
	nexpr->name = go_string_new (new_name);

	if (h)
		g_hash_table_insert (h, (gpointer)nexpr->name->str, nexpr);

	return FALSE;
}


/**
 * expr_name_as_string :
 * @nexpr :
 * @pp : optionally null.
 *
 * returns a string that the caller needs to free.
 */
char *
expr_name_as_string (GnmNamedExpr const *nexpr, GnmParsePos const *pp,
		     GnmConventions const *fmt)
{
	if (pp == NULL)
		pp = &nexpr->pos;
	return gnm_expr_top_as_string (nexpr->texpr, pp, fmt);
}

GnmValue *
expr_name_eval (GnmNamedExpr const *nexpr, GnmEvalPos const *pos,
		GnmExprEvalFlags flags)
{
	g_return_val_if_fail (pos, NULL);

	if (!nexpr)
		return value_new_error_NAME (pos);

	return gnm_expr_top_eval (nexpr->texpr, pos, flags);
}

/**
 * expr_name_downgrade_to_placeholder:
 * @nexpr:
 *
 * Takes a real non-placeholder name and converts it to being a placeholder.
 * unrefing its expression
 **/
void
expr_name_downgrade_to_placeholder (GnmNamedExpr *nexpr)
{
	g_return_if_fail (nexpr != NULL);

	expr_name_set_is_placeholder (nexpr, TRUE);
	expr_name_set_expr
		(nexpr,
		 gnm_expr_top_new_constant (value_new_error_NAME (NULL)));
}

/*******************************************************************
 * Manage things that depend on named expressions.
 */
/**
 * expr_name_set_pos:
 * @nexpr : the named expression
 * @pp: the new position
 *
 * Returns a translated error string which the caller must free if something
 * goes wrong.
 **/
char *
expr_name_set_pos (GnmNamedExpr *nexpr, GnmParsePos const *pp)
{
	GnmNamedExprCollection *old_scope, *new_scope;
	const char *name;

	g_return_val_if_fail (nexpr != NULL, NULL);
	g_return_val_if_fail (nexpr->scope != NULL, NULL);
	g_return_val_if_fail (pp != NULL, NULL);

	old_scope = nexpr->scope;
	new_scope = pp->sheet ? pp->sheet->names : pp->wb->names;

	name = nexpr->name->str;
	if (old_scope != new_scope &&
	    (g_hash_table_lookup (new_scope->placeholders, name) ||
	     g_hash_table_lookup (new_scope->names, name))) {
		const char *fmt = pp->sheet
			? _("'%s' is already defined in sheet")
			: _("'%s' is already defined in workbook");
		return g_strdup_printf (fmt, name);
	}

	g_hash_table_steal (
		nexpr->is_placeholder ? old_scope->placeholders : old_scope->names,
		name);

	nexpr->pos = *pp;
	gnm_named_expr_collection_insert (new_scope, nexpr);
	return NULL;
}

/**
 * expr_name_set_expr :
 * @nexpr : the named expression
 * @new_expr : the new content
 * @rwinfo : optional.
 *
 * Unrefs the current content of @nexpr and absorbs a ref to @new_expr.
 **/
void
expr_name_set_expr (GnmNamedExpr *nexpr, GnmExprTop const *texpr)
{
	GSList *good = NULL;

	g_return_if_fail (nexpr != NULL);

	if (texpr == nexpr->texpr)
		return;
	if (nexpr->texpr != NULL) {
		GSList *deps = NULL, *junk = NULL;

		deps = expr_name_unlink_deps (nexpr);
		expr_name_handle_references (nexpr, FALSE);
		gnm_expr_top_unref (nexpr->texpr);

		/*
		 * We do not want to relink deps for sheets that are going
		 * away.  This speeds up exit for workbooks with lots of
		 * names defined.
		 */
		while (deps) {
			GSList *next = deps->next;
			GnmDependent *dep = deps->data;

			if (dep->sheet && dep->sheet->being_invalidated)
				deps->next = junk, junk = deps;
			else
				deps->next = good, good = deps;

			deps = next;
		}

		g_slist_free (junk);
	}
	nexpr->texpr = texpr;
	dependents_link (good);
	g_slist_free (good);

	if (texpr != NULL)
		expr_name_handle_references (nexpr, TRUE);

	expr_name_queue_deps (nexpr);
}

void
expr_name_add_dep (GnmNamedExpr *nexpr, GnmDependent *dep)
{
	if (nexpr->dependents == NULL)
		nexpr->dependents = g_hash_table_new (g_direct_hash,
						      g_direct_equal);

	g_hash_table_insert (nexpr->dependents, dep, dep);
}

void
expr_name_remove_dep (GnmNamedExpr *nexpr, GnmDependent *dep)
{
	g_return_if_fail (nexpr->dependents != NULL);

	g_hash_table_remove (nexpr->dependents, dep);
}

/**
 * expr_name_is_placeholder :
 * @ne :
 *
 * Returns TRUE if @ne is a placeholder for an unknown name
 **/
gboolean
expr_name_is_placeholder (GnmNamedExpr const *nexpr)
{
	g_return_val_if_fail (nexpr != NULL, FALSE);

	return (nexpr->texpr &&
		gnm_expr_top_is_err (nexpr->texpr, GNM_ERROR_NAME));
}

void
expr_name_set_is_placeholder (GnmNamedExpr *nexpr, gboolean is_placeholder)
{
	const char *name;

	g_return_if_fail (nexpr != NULL);

	name = expr_name_name (nexpr);

	is_placeholder = !!is_placeholder;
	if (nexpr->is_placeholder == is_placeholder)
		return;
	nexpr->is_placeholder = is_placeholder;

	if (nexpr->scope) {
		g_hash_table_steal (is_placeholder
				    ? nexpr->scope->names
				    : nexpr->scope->placeholders,
				    name);
		gnm_named_expr_collection_insert (nexpr->scope, nexpr);
	}
}

gboolean
expr_name_is_active (GnmNamedExpr const *nexpr)
{
	g_return_val_if_fail (nexpr != NULL, FALSE);
	return nexpr->scope != NULL;
}

struct cb_expr_name_in_use {
	GnmNamedExpr *nexpr;
	gboolean in_use;
};

static void
cb_expr_name_in_use (G_GNUC_UNUSED const char *name,
		     GnmNamedExpr *nexpr,
		     struct cb_expr_name_in_use *pdata)
{
	if (pdata->in_use)
		return;

	pdata->in_use =
		do_expr_name_loop_check (NULL, pdata->nexpr,
					 nexpr->texpr->expr, TRUE);
}

/**
 * expr_name_in_use :
 * @nexpr: A named expression.
 *
 * Returns: TRUE, if the named expression appears to be in use.  This is an
 * approximation only, as we only look at the workbook in which the name is
 * defined.
 */

gboolean
expr_name_in_use (GnmNamedExpr *nexpr)
{
	Workbook *wb;
	struct cb_expr_name_in_use data;

	if (nexpr->dependents != NULL &&
	    g_hash_table_size (nexpr->dependents) != 0)
		return TRUE;

	data.nexpr = nexpr;
	data.in_use = FALSE;

	wb = nexpr->pos.sheet ? nexpr->pos.sheet->workbook : nexpr->pos.wb;
	workbook_foreach_name (wb, FALSE,
			       (GHFunc)cb_expr_name_in_use,
			       &data);

	return data.in_use;
}


int
expr_name_cmp_by_name (GnmNamedExpr const *a, GnmNamedExpr const *b)
{
	Sheet const *sheeta = a->pos.sheet;
	Sheet const *sheetb = b->pos.sheet;
	int res = 0;

	if (sheeta != sheetb) {
		/* Locals after non-locals.  */
		if (!sheeta || !sheetb)
			return (!sheeta) - (!sheetb);

		/* By non-local sheet order.  */
		res = g_utf8_collate (sheeta->name_case_insensitive,
				      sheetb->name_case_insensitive);
	}

	if (res == 0)	/* By name.  */
		res = go_utf8_collate_casefold (a->name->str, b->name->str);

	return res;
}

/**
 * sheet_names_check :
 * @sheet :
 * @r :
 *
 * Returns a constant string if @sheet!@r is the target of a named range.
 * Preference is given to workbook scope over sheet.
 **/
char const *
sheet_names_check (Sheet const *sheet, GnmRange const *r)
{
	GnmNamedExpr *nexpr;
	GnmRange tmp;

	g_return_val_if_fail (IS_SHEET (sheet), NULL);
	g_return_val_if_fail (r != NULL, NULL);

	tmp = *r;
	range_normalize (&tmp);
	nexpr = gnm_named_expr_collection_check (sheet->names, sheet, &tmp);
	if (nexpr == NULL) {
		nexpr = gnm_named_expr_collection_check (sheet->workbook->names, sheet, &tmp);
		/* The global name is not accessible if there is a local name (#306685) */
		if (nexpr != NULL &&
		    gnm_named_expr_collection_lookup (sheet->names, nexpr->name->str) != NULL)
			return NULL;
	}

	return (nexpr != NULL) ? nexpr->name->str : NULL;
}


/**
 * expr_name_perm_add:
 * @name:               name
 * @texpr:              string to be the value of the name
 * @is_editable:        whether this is a predefined action
 *
 * This is a wrapper around expr_name_add to set this as permanent name.
 *
 *
 **/
void
expr_name_perm_add (Sheet *sheet, char const *name,
		    GnmExprTop const *value,
		    gboolean is_editable)
{
	GnmNamedExpr *res;
	GnmParsePos pp;

	parse_pos_init_sheet (&pp, sheet);
	res = expr_name_add (&pp, name, value, NULL, TRUE, NULL);
	if (res) {
		res->is_permanent = TRUE;
		res->is_editable = is_editable;
	}
}

/* ------------------------------------------------------------------------- */

static void
expr_name_set_expr_ref (GnmNamedExpr *nexpr, GnmExprTop const *texpr)
{
	gnm_expr_top_ref (texpr);
	expr_name_set_expr (nexpr, texpr);
}


GOUndo *
expr_name_set_expr_undo_new (GnmNamedExpr *ne)
{
	expr_name_ref (ne);
	gnm_expr_top_ref (ne->texpr);

	return go_undo_binary_new (ne, (gpointer)ne->texpr,
				   (GOUndoBinaryFunc)expr_name_set_expr_ref,
				   (GFreeFunc)expr_name_unref,
				   (GFreeFunc)gnm_expr_top_unref);
}

/* ------------------------------------------------------------------------- */
