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
 * tclimg.c --
 *
 *  Commands for image files.
 */

#include "nsd.h"

/*
 * Image types recognized and processed here
 */
enum imgtype {
    unknown,
    jpeg,
    gif,
    png
};

/*
 * For parsing JPEG stream
 */

#define M_SOI 0xD8 /* Start Of Image (beginning of datastream) */
#define M_EOI 0xD9 /* End Of Image (end of datastream) */
#define M_SOS 0xDA /* Start Of Scan (begins compressed data) */

/*
 * Local functions defined in this file
 */

static int JpegSize(Tcl_Channel chan, uint32_t *wPtr, uint32_t *hPtr) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);
static int PngSize (Tcl_Channel chan, uint32_t *wPtr, uint32_t *hPtr) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);
static int GifSize (Tcl_Channel chan, uint32_t *wPtr, uint32_t *hPtr) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2) NS_GNUC_NONNULL(3);

static bool JpegRead2Bytes(Tcl_Channel chan, uint32_t *value) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);
static int JpegNextMarker(Tcl_Channel chan) NS_GNUC_NONNULL(1);

static enum imgtype GetImageType(Tcl_Channel chan) NS_GNUC_NONNULL(1);

static void SetResultObjDims(Tcl_Interp *interp, uint32_t w, uint32_t h) NS_GNUC_NONNULL(1);

static int ChanGetc(Tcl_Channel chan) NS_GNUC_NONNULL(1);
static Tcl_Channel GetFileChan(Tcl_Interp *interp, const char *path) NS_GNUC_NONNULL(1) NS_GNUC_NONNULL(2);


/*
 *----------------------------------------------------------------------
 *
 * NsTclImgTypeObjCmd --
 *
 *      Implements ns_imgtype as obj command.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */

int
NsTclImgTypeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const char  *file;
    int          result;
    Ns_ObjvSpec  args[] = {
        {"file",  Ns_ObjvString, &file, NULL},
        {NULL, NULL, NULL, NULL}
    };
    
    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;
        
    } else {
        const char  *type = "unknown";
        Tcl_Channel  chan = GetFileChan(interp, file);
        
        if (chan == NULL) {
            result = TCL_ERROR;

        } else {
            switch (GetImageType(chan)) {
            case jpeg:    type = "jpeg";    break;
            case png:     type = "png";     break;
            case gif:     type = "gif";     break;
            case unknown: type = "unknown"; break;
            default: /*should not happen */ assert(0); break;
            }
            result = Tcl_Close(interp, chan);
            Tcl_SetObjResult(interp, Tcl_NewStringObj(type, -1));
        }
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclImgMimeObjCmd --
 *
 *      Implements ns_imgmime as obj command.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */

int
NsTclImgMimeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const char *file;
    int         result;
    Ns_ObjvSpec args[] = {
        {"file",  Ns_ObjvString, &file, NULL},
        {NULL, NULL, NULL, NULL}
    };
    
    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;
        
    } else {
        const char  *mime = "image/unknown";
        Tcl_Channel  chan = GetFileChan(interp, file);
        
        if (chan == NULL) {
            result = TCL_ERROR;
        } else {

            switch (GetImageType(chan)) {
            case jpeg:    mime = "image/jpeg";    break;
            case png:     mime = "image/png";     break;
            case gif:     mime = "image/gif";     break;
            case unknown: mime = "image/unknown"; break;
            default: /*should not happen */ assert(0); break;
            }

            result = Tcl_Close(interp, chan);
            Tcl_SetObjResult(interp, Tcl_NewStringObj(mime, -1));
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclImgSizeObjCmd --
 *
 *      Implements ns_imgsize as obj command.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */

int
NsTclImgSizeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const char *file;
    int         result;
    Ns_ObjvSpec args[] = {
        {"file",  Ns_ObjvString, &file, NULL},
        {NULL, NULL, NULL, NULL}
    };
    
    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;
        
    } else {
        Tcl_Channel  chan = GetFileChan(interp, file);

        if (chan == NULL) {
            result = TCL_ERROR;
        } else {
            uint32_t    w = 0u, h = 0u;

            switch (GetImageType(chan)) {
            case jpeg:    result = JpegSize(chan, &w, &h); break;
            case png:     result = PngSize (chan, &w, &h); break;
            case gif:     result = GifSize (chan, &w, &h); break;
            case unknown: result = TCL_ERROR; break;
            default: /*should not happen */ result = TCL_ERROR; assert(0); break;
            }
            
            if (Tcl_Close(interp, chan) != TCL_OK) {
                result = TCL_ERROR;
            } else {
            
                if (result != TCL_OK) {
                    SetResultObjDims(interp, 0u, 0u);
                    result = TCL_OK;
                } else {
                    SetResultObjDims(interp, w, h);
                }
            }
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclGifSizeObjCmd --
 *
 *      Implements ns_gifsize as obj command.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */

int
NsTclGifSizeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const char *file;
    int         result;
    Ns_ObjvSpec args[] = {
        {"gif_file", Ns_ObjvString, &file, NULL},
        {NULL, NULL, NULL, NULL}
    };
    
    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;
        
    } else {
        uint32_t    w = 0u, h = 0u;
        Tcl_Channel chan = GetFileChan(interp, file);
    
        if (chan == NULL) {
            result = TCL_ERROR;

        } else if (GetImageType(chan) != gif || GifSize(chan, &w, &h) != TCL_OK) {
            (void)Tcl_Close(interp, chan);
            Tcl_AppendResult(interp, "invalid GIF file \"", file, "\"", NULL);
            result = TCL_ERROR;

        } else {
            result = Tcl_Close(interp, chan);
            SetResultObjDims(interp, w, h);            
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclPngSizeObjCmd --
 *
 *      Implements ns_pngsize as obj command.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */

int
NsTclPngSizeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const char *file;
    int         result;
    Ns_ObjvSpec args[] = {
        {"png_file", Ns_ObjvString, &file, NULL},
        {NULL, NULL, NULL, NULL}
    };
    
    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;
        
    } else {
        uint32_t    w, h;
        Tcl_Channel chan = GetFileChan(interp, file);

        if (chan == NULL) {
            result = TCL_ERROR;

        } else if (GetImageType(chan) != png || PngSize(chan, &w, &h) != TCL_OK) {
            (void)Tcl_Close(interp, chan);
            Tcl_AppendResult(interp, "invalid PNG file \"", file, "\"", NULL);
            result = TCL_ERROR;

        } else {
            result = Tcl_Close(interp, chan);
            SetResultObjDims(interp, w, h);
        }
    }
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclJpegSizeObjCmd --
 *
 *      Implements ns_jpegsize as obj command.
 *
 * Results:
 *      Tcl result.
 *
 * Side effects:
 *      See docs.
 *
 *----------------------------------------------------------------------
 */

int
NsTclJpegSizeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    const char *file;
    int         result;
    Ns_ObjvSpec args[] = {
        {"jpeg_file", Ns_ObjvString, &file, NULL},
        {NULL, NULL, NULL, NULL}
    };
    
    if (Ns_ParseObjv(NULL, args, interp, 1, objc, objv) != NS_OK) {
        result = TCL_ERROR;
        
    } else {
        uint32_t    w = 0u, h = 0u;
        Tcl_Channel chan = GetFileChan(interp, file);

        if (chan == NULL) {
            result = TCL_ERROR;

        } else if (GetImageType(chan) != jpeg || JpegSize(chan, &w, &h) != TCL_OK) {
            (void) Tcl_Close(interp, chan);
            Tcl_AppendResult(interp, "invalid JPEG file \"", file, "\"", NULL);
            result = TCL_ERROR;

        } else {
            result = Tcl_Close(interp, chan);
            SetResultObjDims(interp, w, h);
        }
    }        
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * GifSize --
 *
 *      Parses out the size of the GIF image.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      Seeks the channel back and forth.
 *
 *----------------------------------------------------------------------
 */

static int
GifSize(Tcl_Channel chan, uint32_t *wPtr, uint32_t *hPtr)
{
    unsigned char count, buf[0x300];
    unsigned int  depth, colormap;

    NS_NONNULL_ASSERT(chan != NULL);
    NS_NONNULL_ASSERT(wPtr != NULL);
    NS_NONNULL_ASSERT(hPtr != NULL);

    /*
     * Skip the magic as caller has already
     * checked it allright.
     */

    if (Tcl_Read(chan, (char*)buf, 6) != 6) {
        return TCL_ERROR;
    }

    if (Tcl_Read(chan, (char*)buf, 7) != 7) {
        return TCL_ERROR;
    }

    depth = (unsigned int)(1u << ((buf[4] & 0x7u) + 1u));
    colormap = (((buf[4] & 0x80u) != 0u) ? 1u : 0u);

    if (colormap != 0u) {
        int bytesToRead = 3 * (int)depth;
        if (Tcl_Read(chan, (char *)buf, bytesToRead) != bytesToRead) {
            return TCL_ERROR;
        }
    }

  outerloop:
    if (Tcl_Read(chan, (char *)buf, 1) != 1) {
        return TCL_ERROR;
    }

    if (buf[0] == UCHAR('!')) {
        if (Tcl_Read(chan, (char *)buf, 1) != 1) {
            return TCL_ERROR;
        }
    innerloop:
        if (Tcl_Read(chan, (char *) &count, 1) != 1) {
            return TCL_ERROR;
        }
        if (count == 0u) {
            goto outerloop;
        }
        if (Tcl_Read(chan, (char *)buf, (int)count) != (int)count) {
            return TCL_ERROR;
        }
        goto innerloop;
    } else if (buf[0] != UCHAR(',')) {
        return TCL_ERROR;
    }

    if (Tcl_Read(chan, (char*)buf, 9) != 9) {
        return TCL_ERROR;
    }

    *wPtr = (uint32_t)(0x100u * buf[5] + buf[4]);
    *hPtr = (uint32_t)(0x100u * buf[7] + buf[6]);

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * PngSize --
 *
 *      Parses out the size of PNG image.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      Seeks the channel back and forth.
 *
 *----------------------------------------------------------------------
 */

static int
PngSize(Tcl_Channel chan, uint32_t *wPtr, uint32_t *hPtr)
{
    unsigned int w = 0u, h = 0u;
    int          result = TCL_OK;

    NS_NONNULL_ASSERT(chan != NULL);
    NS_NONNULL_ASSERT(wPtr != NULL);
    NS_NONNULL_ASSERT(hPtr != NULL);

    if (
        (Tcl_Seek(chan, 16LL, SEEK_SET) == -1)
        || (Tcl_Eof(chan) == 1)
        || (Tcl_Read(chan, (char *)&w, 4) != 4)
        || (Tcl_Read(chan, (char *)&h, 4) != 4)
        ) {
        result = TCL_ERROR;

    } else {
        *wPtr = htonl(w);
        *hPtr = htonl(h);
    }
    
    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * JpegSize --
 *
 *      Parses out the size of JPEG image out of JPEG stream.
 *
 * Results:
 *      Standard Tcl result.
 *
 * Side effects:
 *      Seeks the channel back and forth.
 *
 *----------------------------------------------------------------------
 */

static int
JpegSize(Tcl_Channel chan, uint32_t *wPtr, uint32_t *hPtr)
{
    int result = TCL_ERROR;
    
    NS_NONNULL_ASSERT(chan != NULL);
    NS_NONNULL_ASSERT(wPtr != NULL);
    NS_NONNULL_ASSERT(hPtr != NULL);
    
    if (ChanGetc(chan) == 0xFF && ChanGetc(chan) == M_SOI) {
        for (;;) {
	    int          i;
	    uint32_t     numBytes = 0u;

            i = JpegNextMarker(chan);
            if (i == EOF || i == M_SOS || i == M_EOI) {
                break;
            }
            if (0xC0 <= i && i <= 0xC3) {
	        uint32_t first;
                
                if (JpegRead2Bytes(chan, &first) && ChanGetc(chan) != EOF
                    && JpegRead2Bytes(chan, hPtr)
                    && JpegRead2Bytes(chan, wPtr)
                    ) {
                    result = TCL_OK;
                }
                break;
            }
            (void) JpegRead2Bytes(chan, &numBytes);
            if (numBytes < 2u || Tcl_Seek(chan, (Tcl_WideInt)numBytes - 2, SEEK_CUR) == -1) {
                break;
            }
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * JpegRead2Bytes --
 *
 *  Read 2 bytes, convert to unsigned int. All 2-byte quantities
 *  in JPEG markers are MSB first.
 *
 * Results:
 *  The two byte value in arg2, success as return value
 *
 * Side effects:
 *  Advances file pointer.
 *
 *----------------------------------------------------------------------
 */

static bool
JpegRead2Bytes(Tcl_Channel chan, uint32_t *value)
{
    int c1, c2;
    bool success;

    NS_NONNULL_ASSERT(chan != NULL);
    NS_NONNULL_ASSERT(value != NULL);

    c1 = ChanGetc(chan);
    c2 = ChanGetc(chan);
    if (c1 == EOF || c2 == EOF) {
        success = NS_FALSE;
    } else {
        *value = (((unsigned int) c1) << 8) + ((unsigned int) c2);
        success = NS_TRUE;
    }
    return success;
}


/*
 *----------------------------------------------------------------------
 *
 * JpegNextMarker --
 *
 *  Find the next JPEG marker and return its marker code. We
 *  expect at least one FF byte, possibly more if the compressor
 *  used FFs to pad the file. There could also be non-FF garbage
 *  between markers. The treatment of such garbage is
 *  unspecified; we choose to skip over it but emit a warning
 *  msg. This routine must not be used after seeing SOS marker,
 *  since it will not deal correctly with FF/00 sequences in the
 *  compressed image data...
 *
 * Results:
 *  The next marker code.
 *
 * Side effects:
 *  Will eat up any duplicate FF bytes.
 *
 *----------------------------------------------------------------------
 */

static int
JpegNextMarker(Tcl_Channel chan)
{
    int c;

    NS_NONNULL_ASSERT(chan != NULL);
    
    /*
     * Find 0xFF byte; count and skip any non-FFs.
     */

    c = ChanGetc(chan);
    while (c != EOF && c != 0xFF) {
        c = ChanGetc(chan);
    }
    if (c != EOF) {
        /*
         * Get marker code byte, swallowing any duplicate FF bytes.
         */
        do {
            c = ChanGetc(chan);
        } while (c == 0xFF);
    }

    return c;
}


/*
 *----------------------------------------------------------------------
 *
 * GetImageType --
 *
 *      Examines image type by looking up some magic numbers.
 *
 * Results:
 *      Image type.
 *
 * Side effects:
 *      Seeks the channel back and forth.
 *
 *----------------------------------------------------------------------
 */

static enum imgtype
GetImageType(Tcl_Channel chan)
{
    unsigned char buf[8];
    int           toRead;
    enum imgtype  type = unknown;
    static const unsigned char jpeg_magic  [] = {0xffu, 0xd8u};
    static const          char gif87_magic [] = {'G','I','F','8','7','a'};
    static const          char gif89_magic [] = {'G','I','F','8','9','a'};
    static const unsigned char png_magic   [] = {0x89u,0x50u,0x4eu,0x47u,0xdu,0x0au,0x1au,0x0au};

    NS_NONNULL_ASSERT(chan != NULL);
    
    (void)Tcl_Seek(chan, 0LL, SEEK_SET);
    toRead = (int)sizeof(buf);
    
    if (Tcl_Read(chan, (char*)buf, toRead) != toRead) {
        (void)Tcl_Seek(chan, 0LL, SEEK_SET);

    } else if (memcmp(buf, jpeg_magic, sizeof(jpeg_magic)) == 0) {
        unsigned char trail[] = {0x00u, 0x00u};
	static const unsigned char jpeg_trail  [] = {0xffu, 0xd9u};

        (void)Tcl_Seek(chan,  0LL, SEEK_END);
        (void)Tcl_Seek(chan, -2LL, SEEK_CUR);
        (void)Tcl_Read(chan, (char*)trail, 2);
        if (memcmp(trail, jpeg_trail, 2u) == 0) {
            type = jpeg;
        }
        
    } else if (   memcmp(gif87_magic, buf, sizeof(gif87_magic)) == 0
               || memcmp(gif89_magic, buf, sizeof(gif89_magic)) == 0) {
        type = gif;
        
    } else if (memcmp(png_magic, buf, sizeof(png_magic)) == 0) {
        type = png;
    }

    (void)Tcl_Seek(chan, 0LL, SEEK_SET);

    return type;
}


/*
 *----------------------------------------------------------------------
 *
 * ChanGetc --
 *
 *  Read a single unsigned char from a channel.
 *
 * Results:
 *  Character or EOF.
 *
 * Side effects:
 *  None.
 *
 *----------------------------------------------------------------------
 */

static int
ChanGetc(Tcl_Channel chan)
{
    unsigned char buf[1];
    int           resultChar;

    NS_NONNULL_ASSERT(chan != NULL);

    if (Tcl_Read(chan, (char *) buf, 1) != 1) {
        resultChar = EOF;
    } else {
        resultChar = (int) buf[0];
    }
    return resultChar;
}


/*
 *----------------------------------------------------------------------
 *
 * SetResultObjDims --
 *
 *      Set width and height dimensions as a two element list.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static void
SetResultObjDims(Tcl_Interp *interp, uint32_t w, uint32_t h)
{
    Tcl_Obj *objv[2];

    NS_NONNULL_ASSERT(interp != NULL);
    
    objv[0] = Tcl_NewIntObj((int)w);
    objv[1] = Tcl_NewIntObj((int)h);
    Tcl_SetObjResult(interp, Tcl_NewListObj(2, objv));
}


/*
 *----------------------------------------------------------------------
 *
 * GetFileChan --
 *
 *      Opens the (binary) channel to a file.
 *
 * Results:
 *      Tcl_Channel or NULL on error.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static Tcl_Channel
GetFileChan(Tcl_Interp *interp, const char *path)
{
    Tcl_Channel chan;

    NS_NONNULL_ASSERT(interp != NULL);
    NS_NONNULL_ASSERT(path != NULL);
    
    chan = Tcl_OpenFileChannel(interp, path, "r", 0);
    if (chan != NULL) {
        if (Tcl_SetChannelOption(interp, chan, "-translation", "binary") != TCL_OK) {
            (void)Tcl_Close(interp, chan);
            chan = NULL;
        }
    }

    return chan;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
