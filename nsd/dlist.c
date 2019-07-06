/*
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://mozilla.org/.
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
 * the License for the specific language governing rights and limitations
 * under the License.
 *
 * The Original Code is AOLserver Code and related documentation
 * distributed by AOL.
 *
 * The Initial Developer of the Original Code is America Online,
 * Inc. Portions created by AOL are Copyright (C) 1999 America Online,
 * Inc. All Rights Reserved.
 *
 * Alternatively, the contents of this file may be used under the terms
 * of the GNU General Public License (the "GPL"), in which case the
 * provisions of GPL are applicable instead of those above.  If you wish
 * to allow use of your version of this file only under the terms of the
 * GPL and not to allow others to use your version of this file under the
 * License, indicate your decision by deleting the provisions above and
 * replace them with the notice and other provisions required by the GPL.
 * If you do not delete the provisions above, a recipient may use your
 * version of this file under either the License or the GPL.
 */


/*
 * dlist.c --
 *
 *      Functions that deal with dynamic lists, similar to DStrings,
 *      but the basic unint is a void pointer.
 *
 *        typedef struct Ns_DList {
 *            void   *data;
 *            size_t  size;
 *            size_t  avail;
 *            void   *static_data[30];
 *        } Ns_DList;
 *
 */

#include "nsd.h"

/*
 *----------------------------------------------------------------------
 *
 * Ns_DListInit, Ns_DListAppend, Ns_DListFree   --
 *
 *      Functions similar to Tcl_DString, but working on (void*) elements
 *      instead of chars. The Ns_DList operations work on static data as long
 *      the space is sufficient, and doubles in size afterwards. In the
 *      worst case, half of the data is unused, but that is the same size of
 *      overhead like for a single linked list.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Potentially allocating/reallocating memory.
 *
 *----------------------------------------------------------------------
 */
void
Ns_DListInit(Ns_DList *dlPtr)
{
  dlPtr->data = &dlPtr->static_data[0];
  dlPtr->avail = Ns_NrElements(dlPtr->static_data);
  dlPtr->size = 0u;
}

void
Ns_DListAppend(Ns_DList *dlPtr, void *element)
{
  if (dlPtr->avail < 1) {
    size_t requiredSize = dlPtr->size * 2u;

    if (dlPtr->data != &dlPtr->static_data[0]) {
      dlPtr->data = (void *)ckrealloc((char *)dlPtr->data, sizeof(dlPtr->data[0]) * requiredSize);
    } else {
      dlPtr->data = (void *)ckalloc(sizeof(dlPtr->data[0]) * requiredSize);
      memcpy(dlPtr->data, &dlPtr->static_data[0], dlPtr->size * sizeof(dlPtr->data[0]));
    }
    dlPtr->avail = requiredSize - dlPtr->size;
  }
  dlPtr->avail --;
  dlPtr->data[dlPtr->size] = element;
  dlPtr->size ++;
}

void
Ns_DListFree(Ns_DList *dlPtr)
{
  if (dlPtr->data != &dlPtr->static_data[0]) {
    ckfree((char*)dlPtr->data);
  }
  Ns_DListInit(dlPtr);
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
