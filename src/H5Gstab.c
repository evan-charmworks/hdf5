/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by the Board of Trustees of the University of Illinois.         *
 * All rights reserved.                                                      *
 *                                                                           *
 * This file is part of HDF5.  The full HDF5 copyright notice, including     *
 * terms governing use, modification, and redistribution, is contained in    *
 * the files COPYING and Copyright.html.  COPYING can be found at the root   *
 * of the source code distribution tree; Copyright.html can be found at the  *
 * root level of an installed copy of the electronic HDF5 document set and   *
 * is linked from the top-level documents page.  It can also be found at     *
 * http://hdf.ncsa.uiuc.edu/HDF5/doc/Copyright.html.  If you do not have     *
 * access to either file, you may request a copy from hdfhelp@ncsa.uiuc.edu. *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* Programmer: Robb Matzke <matzke@llnl.gov>
 *	       Friday, September 19, 1997
 *
 */
#define H5F_PACKAGE		/*suppress error about including H5Fpkg	  */
#define H5G_PACKAGE		/*suppress error about including H5Gpkg	  */

/* Pablo information */
/* (Put before include files to avoid problems with inline functions) */
#define PABLO_MASK	H5G_stab_mask

#include "H5private.h"
#include "H5Eprivate.h"
#include "H5Fpkg.h"         /*file access                             */
#include "H5FLprivate.h"	/*Free Lists	  */
#include "H5Gpkg.h"
#include "H5HLprivate.h"
#include "H5MMprivate.h"
#include "H5Oprivate.h"

/* Declare extern the PQ free list for the wrapped strings */
H5FL_BLK_EXTERN(str_buf);

/* Private prototypes */
static herr_t H5G_insert_name(H5G_entry_t *loc, H5G_entry_t *obj,
        const char *name);


/*-------------------------------------------------------------------------
 * Function:	H5G_stab_create
 *
 * Purpose:	Creates a new empty symbol table (object header, name heap,
 *		and B-tree).  The caller can specify an initial size for the
 *		name heap.  The object header of the group is opened for
 *		write access.
 *
 *		In order for the B-tree to operate correctly, the first
 *		item in the heap is the empty string, and must appear at
 *		heap offset zero.
 *
 * Errors:
 *
 * Return:	Non-negative on success/Negative on failure
 *
 * Programmer:	Robb Matzke
 *		matzke@llnl.gov
 *		Aug  1 1997
 *
 * Modifications:
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5G_stab_create(H5F_t *f, hid_t dxpl_id, size_t init, H5G_entry_t *self/*out*/)
{
    size_t		    name;	/*offset of "" name	*/
    H5O_stab_t		    stab;	/*symbol table message	*/
    herr_t      ret_value=SUCCEED;       /* Return value */

    FUNC_ENTER_NOAPI(H5G_stab_create, FAIL);

    /*
     * Check arguments.
     */
    assert(f);
    assert(self);
    init = MAX(init, H5HL_SIZEOF_FREE(f) + 2);

    /* Create symbol table private heap */
    if (H5HL_create(f, dxpl_id, init, &(stab.heap_addr)/*out*/)<0)
	HGOTO_ERROR(H5E_SYM, H5E_CANTINIT, FAIL, "can't create heap");
    name = H5HL_insert(f, dxpl_id, stab.heap_addr, 1, "");
    if ((size_t)(-1)==name)
	HGOTO_ERROR(H5E_SYM, H5E_CANTINIT, FAIL, "can't initialize heap");

    /*
     * B-tree's won't work if the first name isn't at the beginning
     * of the heap.
     */
    assert(0 == name);

    /* Create the B-tree */
    if (H5B_create(f, dxpl_id, H5B_SNODE, NULL, &(stab.btree_addr)/*out*/) < 0)
	HGOTO_ERROR(H5E_SYM, H5E_CANTINIT, FAIL, "can't create B-tree");

    /*
     * Create symbol table object header.  It has a zero link count
     * since nothing refers to it yet.	The link count will be
     * incremented if the object is added to the group directed graph.
     */
    if (H5O_create(f, dxpl_id, 4 + 2 * H5F_SIZEOF_ADDR(f), self/*out*/) < 0)
	HGOTO_ERROR(H5E_SYM, H5E_CANTINIT, FAIL, "can't create header");

    /*
     * Insert the symbol table message into the object header and the symbol
     * table entry.
     */
    if (H5O_modify(self, H5O_STAB_ID, H5O_NEW_MESG, H5O_FLAG_CONSTANT, 1, &stab, dxpl_id)<0) {
	H5O_close(self);
	HGOTO_ERROR(H5E_SYM, H5E_CANTINIT, FAIL, "can't create message");
    }
    self->cache.stab.btree_addr = stab.btree_addr;
    self->cache.stab.heap_addr = stab.heap_addr;
    self->type = H5G_CACHED_STAB;

done:
    FUNC_LEAVE_NOAPI(ret_value);
}


/*-------------------------------------------------------------------------
 * Function:	H5G_stab_find
 *
 * Purpose:	Finds a symbol named NAME in the symbol table whose
 *		description is stored in GRP_ENT in file F and returns its
 *		symbol table entry through OBJ_ENT (which is optional).
 *
 * Errors:
 *
 * Return:	Non-negative on success/Negative on failure
 *
 * Programmer:	Robb Matzke
 *		matzke@llnl.gov
 *		Aug  1 1997
 *
 * Modifications:
 *
 *      Pedro Vicente, <pvn@ncsa.uiuc.edu> 22 Aug 2002
 *      Added `id to name' support.
 *      Added a deep copy of the symbol table entry
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5G_stab_find(H5G_entry_t *grp_ent, const char *name,
	      H5G_entry_t *obj_ent/*out*/, hid_t dxpl_id)
{
    H5G_bt_ud1_t	udata;		/*data to pass through B-tree	*/
    H5O_stab_t		stab;		/*symbol table message		*/
    herr_t      ret_value=SUCCEED;       /* Return value */

    FUNC_ENTER_NOAPI(H5G_stab_find, FAIL);

    /* Check arguments */
    assert(grp_ent);
    assert(grp_ent->file);
    assert(name && *name);

    /* set up the udata */
    if (NULL == H5O_read(grp_ent, H5O_STAB_ID, 0, &stab, dxpl_id))
	HGOTO_ERROR(H5E_SYM, H5E_BADMESG, FAIL, "can't read message");
    udata.operation = H5G_OPER_FIND;
    udata.name = name;
    udata.heap_addr = stab.heap_addr;

    /* search the B-tree */
    if (H5B_find(grp_ent->file, dxpl_id, H5B_SNODE, stab.btree_addr, &udata) < 0) {
        HGOTO_ERROR(H5E_SYM, H5E_NOTFOUND, FAIL, "not found");
    } /* end if */
    /* change OBJ_ENT only if found */
    else {
        if (obj_ent) {
            /* do a NULL copy, since the obj_ent name will be constructed in H5G_insert_name() */
            if (H5G_ent_copy(obj_ent, &(udata.ent),H5G_COPY_NULL)<0)
                HGOTO_ERROR(H5E_DATATYPE, H5E_CANTOPENOBJ, FAIL, "unable to copy entry");

            /* insert the name into the symbol entry OBJ_ENT */
            if (H5G_insert_name( grp_ent, obj_ent, name ) < 0)
                HGOTO_ERROR(H5E_SYM, H5E_CANTINIT, FAIL, "cannot insert name");
        } /* end if */
    } /* end else */

done:
    FUNC_LEAVE_NOAPI(ret_value);
}


/*-------------------------------------------------------------------------
 * Function:	H5G_stab_insert
 *
 * Purpose:	Insert a new symbol into the table described by GRP_ENT in
 *		file F.	 The name of the new symbol is NAME and its symbol
 *		table entry is OBJ_ENT.
 *
 * Errors:
 *
 * Return:	Non-negative on success/Negative on failure
 *
 * Programmer:	Robb Matzke
 *		matzke@llnl.gov
 *		Aug  1 1997
 *
 * Modifications:
 *
 *      Pedro Vicente, <pvn@ncsa.uiuc.edu> 22 Aug 2002
 *      Added `id to name' support.
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5G_stab_insert(H5G_entry_t *grp_ent, const char *name, H5G_entry_t *obj_ent, hid_t dxpl_id)
{
    H5O_stab_t		stab;		/*symbol table message		*/
    H5G_bt_ud1_t	udata;		/*data to pass through B-tree	*/
    herr_t      ret_value=SUCCEED;       /* Return value */

    FUNC_ENTER_NOAPI(H5G_stab_insert, FAIL);

    /* check arguments */
    assert(grp_ent && grp_ent->file);
    assert(name && *name);
    assert(obj_ent && obj_ent->file);
    if (grp_ent->file->shared != obj_ent->file->shared)
	HGOTO_ERROR(H5E_SYM, H5E_LINK, FAIL, "interfile hard links are not allowed");

    /* insert the name into the symbol entry OBJ_ENT */
    if(H5G_insert_name(grp_ent, obj_ent, name) < 0)
       HGOTO_ERROR(H5E_SYM, H5E_CANTINIT, FAIL, "cannot insert name");

    /* initialize data to pass through B-tree */
    if (NULL == H5O_read(grp_ent, H5O_STAB_ID, 0, &stab, dxpl_id))
	HGOTO_ERROR(H5E_SYM, H5E_BADMESG, FAIL, "not a symbol table");

    udata.operation = H5G_OPER_INSERT;
    udata.name = name;
    udata.heap_addr = stab.heap_addr;
    H5G_ent_copy(&(udata.ent),obj_ent,H5G_COPY_NULL); /* NULL copy here, no copies happens in H5G_node_insert() callback() */

    /* insert */
    if (H5B_insert(grp_ent->file, dxpl_id, H5B_SNODE, stab.btree_addr, &udata) < 0)
	HGOTO_ERROR(H5E_SYM, H5E_CANTINSERT, FAIL, "unable to insert entry");
    
    /* update the name offset in the entry */
    obj_ent->name_off = udata.ent.name_off;

done:
    FUNC_LEAVE_NOAPI(ret_value);
}


/*-------------------------------------------------------------------------
 * Function:	H5G_stab_remove
 *
 * Purpose:	Remove NAME from a symbol table.
 *
 * Return:	Non-negative on success/Negative on failure
 *
 * Programmer:	Robb Matzke
 *              Thursday, September 17, 1998
 *
 * Modifications:
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5G_stab_remove(H5G_entry_t *grp_ent, const char *name, hid_t dxpl_id)
{
    H5O_stab_t		stab;		/*symbol table message		*/
    H5G_bt_ud1_t	udata;		/*data to pass through B-tree	*/
    herr_t      ret_value=SUCCEED;       /* Return value */
    
    FUNC_ENTER_NOAPI(H5G_stab_remove, FAIL);

    assert(grp_ent && grp_ent->file);
    assert(name && *name);

    /* initialize data to pass through B-tree */
    if (NULL==H5O_read(grp_ent, H5O_STAB_ID, 0, &stab, dxpl_id))
	HGOTO_ERROR(H5E_SYM, H5E_BADMESG, FAIL, "not a symbol table");
    udata.operation = H5G_OPER_REMOVE;
    udata.name = name;
    udata.heap_addr = stab.heap_addr;
    HDmemset(&(udata.ent), 0, sizeof(udata.ent));

    /* remove */
    if (H5B_remove(grp_ent->file, dxpl_id, H5B_SNODE, stab.btree_addr, &udata)<0)
	HGOTO_ERROR(H5E_SYM, H5E_CANTINIT, FAIL, "unable to remove entry");

done:
    FUNC_LEAVE_NOAPI(ret_value);
}


/*-------------------------------------------------------------------------
 * Function: H5G_insert_name
 *
 * Purpose: Insert a name into the symbol entry OBJ, located at LOC
 *
 * Return: Success: 0, Failure: -1
 *
 * Programmer: Pedro Vicente, pvn@ncsa.uiuc.edu
 *
 * Date: August 22, 2002
 *
 * Comments: The allocated memory (H5MM_malloc) is freed in H5O_close
 *
 * Modifications:
 *
 *-------------------------------------------------------------------------
 */
static herr_t 
H5G_insert_name(H5G_entry_t *loc, H5G_entry_t *obj, const char *name)
{
    char *new_user_path;        /* Pointer to new user path */
    char *new_canon_path;       /* Pointer to new canonical path */
    size_t  name_len;           /* Length of name to append */
    size_t  user_path_len;      /* Length of location's user path name */
    size_t  canon_path_len;     /* Length of location's canonical path name */
    herr_t  ret_value = SUCCEED;       
 
    FUNC_ENTER_NOAPI(H5G_insert_name, FAIL);

    assert(loc);
    assert(obj);
    assert(name);
 
    /* Only attempt to build a new name if the location's name exists */
    if(loc->canon_path_r) {
        const char *loc_user_path;      /* Pointer to raw string for user path */
        const char *loc_canon_path;     /* Pointer to raw string for canonical path */

        /* Reset the object's previous names, if they exist */
        if(obj->user_path_r) {
            H5RS_decr(obj->user_path_r);
            obj->user_path_r=NULL;
        } /* end if */
        if(obj->canon_path_r) {
            H5RS_decr(obj->canon_path_r);
            obj->canon_path_r=NULL;
        } /* end if */
        obj->user_path_hidden=0;

        /* Get the length of the strings involved */
        user_path_len = H5RS_len(loc->user_path_r);
        canon_path_len = H5RS_len(loc->canon_path_r);
        name_len = HDstrlen(name);

        /* Modify the object's user path */

        /* Get the raw string for the user path */
        loc_user_path=H5RS_get_str(loc->user_path_r);
        assert(loc_user_path);

        /* The location's user path already ends in a '/' separator */
        if ('/'==loc_user_path[user_path_len-1]) {
            if (NULL==(new_user_path = H5FL_BLK_MALLOC(str_buf,user_path_len+name_len+1)))
                HGOTO_ERROR (H5E_RESOURCE, H5E_NOSPACE, FAIL, "memory allocation failed");
            HDstrcpy(new_user_path, loc_user_path);
        } /* end if */
        /* The location's user path needs a separator */
        else {
            if (NULL==(new_user_path = H5FL_BLK_MALLOC(str_buf,user_path_len+1+name_len+1)))
                HGOTO_ERROR (H5E_RESOURCE, H5E_NOSPACE, FAIL, "memory allocation failed");
            HDstrcpy(new_user_path, loc_user_path);
            HDstrcat(new_user_path, "/");
        } /* end else */

        /* Append the component's name */
        HDstrcat(new_user_path, name);

        /* Give ownership of the user path to the entry */
        obj->user_path_r=H5RS_own(new_user_path);
        assert(obj->user_path_r);

        /* Modify the object's canonical path */

        /* Get the raw string for the canonical path */
        loc_canon_path=H5RS_get_str(loc->canon_path_r);
        assert(loc_canon_path);

        /* The location's canonical path already ends in a '/' separator */
        if ('/'==loc_canon_path[canon_path_len-1]) {
            if (NULL==(new_canon_path = H5FL_BLK_MALLOC(str_buf,canon_path_len+name_len+1)))
                HGOTO_ERROR (H5E_RESOURCE, H5E_NOSPACE, FAIL, "memory allocation failed");
            HDstrcpy(new_canon_path, loc_canon_path);
        } /* end if */
        /* The location's canonical path needs a separator */
        else {
            if (NULL==(new_canon_path = H5FL_BLK_MALLOC(str_buf,canon_path_len+1+name_len+1)))
                HGOTO_ERROR (H5E_RESOURCE, H5E_NOSPACE, FAIL, "memory allocation failed");
            HDstrcpy(new_canon_path, loc_canon_path);
            HDstrcat(new_canon_path, "/");
        } /* end else */

        /* Append the component's name */
        HDstrcat(new_canon_path, name);

        /* Give ownership of the canonical path to the entry */
        obj->canon_path_r=H5RS_own(new_canon_path);
        assert(obj->canon_path_r);
    } /* end if */

done:
    FUNC_LEAVE_NOAPI(ret_value);
}


/*-------------------------------------------------------------------------
 * Function:	H5G_stab_delete
 *
 * Purpose:	Delete entire symbol table information from file
 *
 * Return:	Non-negative on success/Negative on failure
 *
 * Programmer:	Quincey Koziol
 *              Thursday, March 20, 2003
 *
 * Modifications:
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5G_stab_delete(H5F_t *f, hid_t dxpl_id, haddr_t btree_addr, haddr_t heap_addr)
{
    H5G_bt_ud1_t	udata;		/*data to pass through B-tree	*/
    herr_t  ret_value = SUCCEED;       
 
    FUNC_ENTER_NOAPI(H5G_stab_delete, FAIL);

    assert(f);
    assert(H5F_addr_defined(btree_addr));
    assert(H5F_addr_defined(heap_addr));

    /* Set up user data for B-tree deletion */
    HDmemset(&udata, 0, sizeof udata);
    udata.operation = H5G_OPER_REMOVE;
    udata.name = NULL;
    udata.heap_addr = heap_addr;

    /* Delete entire B-tree */
    if(H5B_delete(f, dxpl_id, H5B_SNODE, btree_addr, &udata)<0)
        HGOTO_ERROR(H5E_SYM, H5E_CANTDELETE, FAIL, "unable to delete symbol table B-tree");

    /* Delete local heap for names */
    if(H5HL_delete(f, dxpl_id, heap_addr)<0)
        HGOTO_ERROR(H5E_SYM, H5E_CANTDELETE, FAIL, "unable to delete symbol table heap");

done:
    FUNC_LEAVE_NOAPI(ret_value);
} /* end H5G_stab_delete() */

