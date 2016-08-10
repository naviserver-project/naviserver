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
 * mimetypes.c --
 *
 *      Defines standard default mime types.
 */

#include "nsd.h"

#define TYPE_DEFAULT "*/*"


/*
 * Local functions defined in this file.
 */

static void AddType(CONST char *ext, CONST char *type);
static char *LowerDString(Ns_DString *dsPtr, CONST char *ext);

/*
 * Static variables defined in this file.
 */

static Tcl_HashTable    types;
static const char      *defaultType = TYPE_DEFAULT;
static const char      *noextType = TYPE_DEFAULT;
/*
 * The default extension matching table.  This should be kept up to date with
 * the client.  Case in the extension is ignored.
 */

static const struct exttype {
    const char     *ext;
    const char     *type;
} typetab[] = {

    /*
     * Basic text/html types.
     */
    { ".adp",   NSD_TEXTHTML},
    { ".dci",   NSD_TEXTHTML},
    { ".htm",   NSD_TEXTHTML},
    { ".html",  NSD_TEXTHTML},
    { ".sht",   NSD_TEXTHTML},
    { ".shtml", NSD_TEXTHTML},

    /*
     * All other types.
     */
    
    { ".123",     "application/vnd.lotus-1-2-3"},            
    { ".1905.1",  "application/vnd.ieee.1905"},              
    { ".323",     "text/h323" },
    { ".3dm",     "x-world/x-3dmf" },
    { ".3dmf",    "x-world/x-3dmf" },
    { ".3dml",    "model/vnd.flatland.3dml"},                
    { ".3g2",     "video/3gpp2" },
    { ".3gp",     "video/3gpp"},                             /* http://www.iana.org/go/rfc6381 */
    { ".3mf",     "application/vnd.ms-3mfdocument"},         
    { ".726",     "audio/32kadpcm"},                         /* http://www.iana.org/go/rfc2421 */
    { ".7z",      "application/x-7z-compressed" },
    { ".a",       "text/vnd-a"},                             
    { ".a2l",     "application/a2l"},                        
    { ".aab",     "application/x-authorware-bin" },
    { ".aac",     "audio/x-aac"},                            /* wikipedia: aac */
    { ".aal",     "audio/atrac-advanced-lossless"},          /* http://www.iana.org/go/rfc5584 */
    { ".aam",     "application/x-authorware-map" },
    { ".aas",     "application/x-authorware-seg" },
    { ".abc",     "text/vnd.abc"},                           
    { ".ac",      "application/pkix-attr-cert"},             /* http://www.iana.org/go/rfc5877 */
    { ".acc",     "application/vnd.americandynamics.acc"},   
    { ".acgi",    "text/html" },
    { ".acu",     "application/vnd-acucobol"},               
    { ".acx",     "application/internet-property-stream" },
    { ".aep",     "application/vnd.audiograph"},             
    { ".afl",     "video/animaflex" },
    { ".ahead",   "application/vnd.ahead.space"},            
    { ".ai",      "application/postscript" },
    { ".aif",     "audio/aiff" },
    { ".aifc",    "audio/aiff" },
    { ".aiff",    "audio/aiff" },
    { ".aim",     "application/x-aim" },
    { ".aip",     "text/x-audiosoft-intra" },
    { ".ait",     "application/vnd.dvb.ait"},                
    { ".ami",     "application/vnd.amiga.ami"},              
    { ".aml",     "application/aml"},                        
    { ".ani",     "application/x-navi-animation" },
    { ".anx",     "application/annodex" },
    { ".any",     "application/vnd.mitsubishi.misty-guard.trustweb"}, 
    { ".aos",     "application/x-nokia-9000-communicator-add-on-software" },
    { ".apkg",    "application/vnd.anki"},                   
    { ".apng",    "image/vnd.mozilla.apng"},                 
    { ".appcache", "text/cache-manifest"},                    
    { ".apr",     "application/vnd.lotus-approach"},         
    { ".aps",     "application/mime" },
    { ".arc",     "application/octet-stream" },
    { ".arj",     "application/arj" },
    { ".art",     "image/x-art" },
    { ".asc",     "application/pgp-signature"},              /* http://www.iana.org/go/rfc3156 */
    { ".asf",     "application/vnd.ms-asf"},                 
    { ".asice",   "application/vnd.etsi.asic-e+zip"},        
    { ".asics",   "application/vnd.etsi.asic-s+zip"},        
    { ".asm",     "text/x-asm" },
    { ".aso",     "application/vnd.accpac.simply.aso"},      
    { ".asp",     "text/asp" },
    { ".asr",     "video/x-ms-asf" },
    { ".asx",     "video/x-ms-asf" },
    { ".at3",     "audio/atrac3"},                           /* http://www.iana.org/go/rfc5584 */
    { ".atc",     "application/vnd.acucorp"},                
    { ".atf",     "application/atf"},                        
    { ".atfx",    "application/atfx"},                       
    { ".atom",    "application/atom+xml" },
    { ".atomcat", "application/atomcat+xml"},                /* rfc 5023 */
    { ".atomdeleted", "application/atomdeleted+xml"},        /* http://www.iana.org/go/rfc6721 */
    { ".atomsvc", "application/atomsvc+xml"},                /* rfc 5023 */
    { ".atx",     "audio/atrac-x"},                          /* http://www.iana.org/go/rfc5584 */
    { ".atxml",   "application/atxml"},                      
    { ".au",      "audio/basic" },
    { ".auc",     "application/tamp-apex-update-confirm"},   /* http://www.iana.org/go/rfc5934 */
    { ".avi",     "video/x-msvideo" },
    { ".avs",     "video/avs-video" },
    { ".axa",     "audio/annodex" },
    { ".axs",     "application/olescript" },
    { ".axv",     "video/annodex" },
    { ".azf",     "application/vnd.airzip.filesecure.azf"},  
    { ".azs",     "application/vnd.airzip.filesecure.azs"},  
    { ".azv",     "image/vnd.airzip.accelerator.azv"},       
    { ".bar",     "application/vnd.qualcomm.brew-app-res"},  
    { ".bas",     "text/plain" },
    { ".bcpio",   "application/x-bcpio" },
    { ".bdm",     "application/vnd.syncml.dm+wbxml"},        
    { ".bed",     "application/vnd.realvnc.bed"},            
    { ".bh2",     "application/vnd.fujitsu.oasysprs"},       
    { ".bik",     "video/vnd.radgamettools.bink"},           
    { ".bin",     "application/x-macbinary" },
    { ".bm",      "image/bmp" },
    { ".bmi",     "application/vnd.bmi"},                    
    { ".bmml",    "application/vnd.balsamiq.bmml+xml"},      
    { ".bmp",     "image/bmp" },
    { ".bmpr",    "application/vnd.balsamiq.bmpr"},          
    { ".boo",     "application/book" },
    { ".book",    "application/book" },
    { ".boz",     "application/x-bzip2" },
    { ".bsh",     "application/x-bsh" },
    { ".bsp",     "model/vnd.valve.source.compiled-map"},    
    { ".btif",    "image/prs.btif"},                          /* iana: btif */
    { ".bz",      "application/x-bzip" },
    { ".bz2",     "application/x-bzip2" },
    { ".c",       "text/plain" },
    { ".c++",     "text/plain" },
    { ".c11amc",  "application/vnd.cluetrust.cartomobile-config"}, 
    { ".c11amz",  "application/vnd.cluetrust.cartomobile-config-pkg"}, 
    { ".c4g",     "application/vnd.clonk.c4group"},          
    { ".cab",     "application/vnd.ubisoft.webplayer"},      
    { ".cat",     "application/vnd.ms-pki.secret" },
    { ".cbor",    "application/cbor"},                       /* http://www.iana.org/go/rfc7049 */
    { ".cc",      "text/plain" },
    { ".ccad",    "application/clariscad" },
    { ".ccc",     "text/vnd.net2phone.commcenter.command"},  
    { ".ccmp",    "application/ccmp+xml"},                   /* http://www.iana.org/go/rfc6503 */
    { ".cco",     "application/x-cocoa" },
    { ".cdbcmsg", "application/vnd.contact.cmsg"},           
    { ".cdf",     "application/x-cdf" },
    { ".cdfx",    "application/cdfx+xml"},                   
    { ".cdkey",   "application/vnd.mediastation.cdkey"},     
    { ".cdmia",   "application/cdmi-capability"},            /* http://www.iana.org/go/rfc6208 */
    { ".cdmic",   "application/cdmi-container"},             /* http://www.iana.org/go/rfc6208 */
    { ".cdmid",   "application/cdmi-domain"},                /* http://www.iana.org/go/rfc6208 */
    { ".cdmio",   "application/cdmi-object"},                /* http://www.iana.org/go/rfc6208 */
    { ".cdmiq",   "application/cdmi-queue"},                 /* http://www.iana.org/go/rfc6208 */
    { ".cdxml",   "application/vnd.chemdraw+xml"},           
    { ".cdy",     "application/vnd.cinderella"},             
    { ".cea",     "application/cea"},                        
    { ".cer",     "application/pkix-cert"},                  /* http://www.iana.org/go/rfc2585 */
    { ".cha",     "application/x-chat" },
    { ".chat",    "application/x-chat" },
    { ".chm",     "application/vnd.ms-htmlhelp"},            
    { ".cif",     "application/vnd.multiad.creator.cif"},    
    { ".cii",     "application/vnd.anser-web-certificate-issue-initiation"}, 
    { ".cla",     "application/vnd.claymore"},               
    { ".class",   "application/octet-stream" },
    { ".clkk",    "application/vnd.crick.clicker.keyboard"}, 
    { ".clkp",    "application/vnd.crick.clicker.palette"},  
    { ".clkt",    "application/vnd.crick.clicker.template"}, 
    { ".clkw",    "application/vnd.crick.clicker.wordbank"}, 
    { ".clkx",    "application/vnd.crick.clicker"},          
    { ".clp",     "application/x-msclip" },
    { ".cmc",     "application/vnd.cosmocaller"},            
    { ".cmsc",    "application/cms"},                        /* http://www.iana.org/go/rfc7193 */
    { ".cnd",     "text/jcr-cnd"},                           
    { ".coffee",  "application/vnd.coffeescript"},           
    { ".com",     "application/octet-stream" },
    { ".conf",    "text/plain" },
    { ".cpio",    "application/x-cpio" },
    { ".cpkg",    "application/vnd.xmpie.cpkg"},             
    { ".cpp",     "text/plain" },
    { ".cpt",     "application/x-cpt" },
    { ".crl",     "application/pkix-crl"},                   /* http://www.iana.org/go/rfc2585 */
    { ".crt",     "application/x-x509-ca-cert" },
    { ".crtr",    "application/vnd.multiad.creator"},        
    { ".cryptonote", "application/vnd.rig.cryptonote"},         
    { ".csh",     "application/x-csh" },
    { ".csl",     "application/vnd.citationstyles.style+xml"}, 
    { ".csp",     "application/vnd.commonspace"},            
    { ".css",     "text/css"},                               /* http://www.iana.org/go/rfc2318 */
    { ".csv",     "text/csv"},                               /* http://www.iana.org/go/rfc7111 */
    { ".csvs",    "text/csv-schema"},                        
    { ".cuc",     "application/tamp-community-update-confirm"}, /* http://www.iana.org/go/rfc5934 */
    { ".curl",    "application/vnd-curl"},                   
    { ".cw",      "application/prs.cww"},                    
    { ".cxx",     "text/plain" },
    { ".dae",     "model/vnd.collada+xml"},                  
    { ".daf",     "application/vnd.mobius.daf"},             
    { ".dart",    "application/vnd-dart"},                   
    { ".davmount","application/davmount+xml"},            /* rfc 4918 */
    { ".dcd",     "application/dcd"},                        
    { ".dcm",     "application/dicom"},                      /* http://www.iana.org/go/rfc3240 */
    { ".dcr",     "application/x-director" },
    { ".ddd",     "application/vnd.fujixerox.ddd"},          
    { ".ddf",     "application/vnd.syncml.dmddf+xml"},       
    { ".deb",     "application/vnd.debian.binary-package"},  
    { ".deepv",   "application/x-deepv" },
    { ".def",     "text/plain" },
    { ".der",     "application/x-x509-ca-cert" },
    { ".dfac",    "application/vnd.dreamfactory"},           
    { ".dia",     "application/x-dia" },
    { ".dif",     "video/x-dv" },
    { ".dii",     "application/dii"},                        
    { ".dim",     "application/vnd.fastcopy-disk-image"},    
    { ".dir",     "application/x-director" },
    { ".dis",     "application/vnd.mobius.dis"},             
    { ".dist",    "application/vnd.apple.installer+xml"},    
    { ".dit",     "application/dit"},                        
    { ".djvu",    "image/vnd-djvu"},                         
    { ".dl",      "video/x-dl" },
    { ".dll",     "application/x-msdownload" },
    { ".dls",     "audio/dls"},                              /* http://www.iana.org/go/rfc4613 */
    { ".dms",     "text/vnd.dmclientscript"},                
    { ".dna",     "application/vnd.dna"},                    
    { ".doc",     "application/msword" },
    { ".docjson", "application/vnd.document+json"},          
    { ".docm",    "application/vnd.ms-word.document.macroenabled.12"}, 
    { ".docx",    "application/vnd.openxmlformats-officedocument.wordprocessingml.document"}, 
    { ".dot",     "application/msword" },
    { ".dotm",    "application/vnd.ms-word.template.macroenabled.12"}, 
    { ".dotx",    "application/vnd.openxmlformats-officedocument.wordprocessingml-template"}, 
    { ".dp",      "application/vnd.osgi.dp"},                
    { ".dpg",     "application/vnd.dpgraph"},                
    { ".dpkg",    "application/vnd.xmpie.dpkg"},             
    { ".drw",     "application/drafting" },
    { ".dsc",     "text/prs.lines.tag"},                     /* iana: prs lines tag */
    { ".dssc",    "application/dssc+der"},                   /* http://www.iana.org/go/rfc5698 */
    { ".dtd",     "application/xml-dtd"},                    /* http://www.iana.org/go/rfc7303 */
    { ".dtshd",   "audio/vnd.dts.hd"},                       
    { ".dump",    "application/octet-stream" },
    { ".dv",      "video/x-dv" },
    { ".dvb",     "video/vnd.dvb.file"},                     
    { ".dvc",     "application/dvcs"},                       /* http://www.iana.org/go/rfc3029 */
    { ".dvi",     "application/x-dvi" },
    { ".dwf",     "model/vnd-dwf"},                          
    { ".dwg",     "image/vnd.dwg" },
    { ".dxp",     "application/vnd.spotfire.dxp"},           
    { ".dxr",     "application/vnd-dxr"},                    
    { ".dzr",     "application/vnd.dzr"},                    
    { ".e",       "application/vnd.picsel"},                 
    { ".ecelp",   "audio/vnd.nuera.ecelp9600"},              
    { ".edm",     "application/vnd.novadigm.edm"},           
    { ".edx",     "application/vnd.novadigm.edx"},           
    { ".el",      "text/x-script.elisp" },
    { ".elc",     "application/x-elc" },
    { ".elm",     "text/plain" },
    { ".eml",     "text/plain" },
    { ".emm",     "application/vnd.ibm.electronic-media"},   
    { ".emotionml", "application/emotionml+xml"},              
    { ".ent",     "application/vnd.nervana"},                
    { ".env",     "application/x-envoy" },
    { ".eol",     "audio/vnd.digital-winds"},                
    { ".ep",      "application/vnd.bluetooth.ep.oob"},       
    { ".eps",     "application/postscript" },
    { ".epub",    "application/epub+zip"},                   
    { ".es",      "application/ecmascript"},                 /* http://www.iana.org/go/rfc4329 */
    { ".esa",     "application/vnd.osgi.subsystem"},         
    { ".esf",     "application/vnd.epson.esf"},              
    { ".etx",     "text/x-setext" },
    { ".evy",     "application/x-envoy" },
    { ".exe",     "application/vnd.microsoft.portable-executable"}, 
    { ".ext",     "application/vnd.novadigm.ext"},           
    { ".ez2",     "application/vnd.ezpix-album"},            
    { ".ez3",     "application/vnd.ezpix-package"},          
    { ".f",       "text/plain" },
    { ".f77",     "text/x-fortran" },
    { ".f90",     "text/plain" },
    { ".fbs",     "image/vnd.fastbidsheet"},                 
    { ".fcdt",    "application/vnd.adobe.formscentral.fcdt"}, 
    { ".fcs",     "application/vnd.isac.fcs"},               
    { ".fdf",     "application/vnd.fdf" },
    { ".fdt",     "application/fdt+xml"},                    /* http://www.iana.org/go/rfc6726 */
    { ".fe",      "application/vnd.denovo.fcselayout-link"}, 
    { ".fg5",     "application/vnd.fujitsu.oasysgp"},        
    { ".fif",     "application/fractals" },
    { ".fits",    "application/fits"},                       /* http://www.iana.org/go/rfc4047 */
    { ".fla",     "application/vnd.dtg.local.flash"},        
    { ".flac",    "audio/flac" },
    { ".fli",     "video/x-fli" },
    { ".flo",     "application/vnd.micrografx.flo"},         
    { ".flr",     "x-world/x-vrml" },
    { ".flv",     "video/x-flv" },
    { ".flx",     "text/vnd.fmi.flexstor" },
    { ".fly",     "text/vnd.fly"},                           
    { ".fmf",     "video/x-atomic3d-feature" },
    { ".fnc",     "application/vnd.frogans.fnc"},            
    { ".fo",      "application/vnd.software602.filler.form+xml"}, 
    { ".for",     "text/plain" },
    { ".fpx",     "image/vnd.fpx"},                          
    { ".frl",     "application/freeloader" },
    { ".fsc",     "application/vnd.fsc.weblaunch"},          
    { ".fst",     "image/vnd.fst"},                          
    { ".ftc",     "application/vnd.fluxtime.clip"},          
    { ".funk",    "audio/make" },
    { ".fvt",     "video/vnd.fvt"},                          
    { ".fxp",     "application/vnd.adobe.fxp"},              
    { ".fzs",     "application/vnd.fuzzysheet"},             
    { ".g",       "text/plain" },
    { ".g2w",     "application/vnd.geoplan"},                
    { ".g3",      "application/vnd.geocube+xml"},            
    { ".g3w",     "application/vnd.geospace"},               
    { ".gac",     "application/vnd.groove-account"},         
    { ".gbr",     "application/rpki-ghostbusters"},          /* http://www.iana.org/go/rfc6493 */
    { ".gbt",     "text/plain" },
    { ".gdl",     "model/vnd.gdl"},                          
    { ".geo",     "application/vnd.dynageo"},                
    { ".gex",     "application/vnd.geometry-explorer"},      
    { ".ggb",     "application/vnd.geogebra.file"},          
    { ".ggt",     "application/vnd.geogebra.tool"},          
    { ".ghf",     "application/vnd.groove-help"},            
    { ".gif",     "image/gif" },
    { ".gim",     "application/vnd.groove-identity-message"}, 
    { ".gl",      "video/gl" },
    { ".gmx",     "application/vnd.gmx"},                    
    { ".gph",     "application/vnd.flographit"},             
    { ".gqf",     "application/vnd.grafeq"},                 
    { ".gram",    "application/srgs"},                     /* w3c speech grammar */
    { ".grv",     "application/vnd.groove-injector"},        
    { ".grxml",   "application/srgs+xml"},                 /* w3c speech grammar */
    { ".gsd",     "audio/x-gsm" },
    { ".gsheet",  "application/urc-grpsheet+xml"},           
    { ".gsm",     "audio/x-gsm" },
    { ".gsp",     "application/x-gsp" },
    { ".gss",     "application/x-gss" },
    { ".gtar",    "application/x-gtar" },
    { ".gtm",     "application/vnd.groove-tool-message"},    
    { ".gtw",     "model/vnd.gtw"},                          
    { ".gv",      "text/vnd.graphviz"},                      
    { ".gxt",     "application/vnd.geonext"},                
    { ".gz",      "application/gzip"},                       /* http://www.iana.org/go/rfc6713 */
    { ".gzip",    "application/gzip" },
    { ".h",       "text/plain" },
    { ".hal",     "application/vnd.hal+xml"},                
    { ".hbci",    "application/vnd.hbci"},                   
    { ".hdf",     "application/x-hdf" },
    { ".hdt",     "application/vnd.hdt"},                    
    { ".heldxml", "application/held+xml"},                   /* http://www.iana.org/go/rfc5985 */
    { ".help",    "application/x-helpfile" },
    { ".hgl",     "application/vnd.hp-hpgl" },
    { ".hh",      "text/plain" },
    { ".hlb",     "text/x-script" },
    { ".hlp",     "application/winhlp" },
    { ".hpg",     "application/vnd.hp-hpgl" },
    { ".hpgl",    "application/vnd.hp-hpgl" },
    { ".hps",     "application/vnd.hp-hps"},                 
    { ".hpub",    "application/prs.hpub+zip"},               
    { ".hqx",     "application/mac-binhex40" },
    { ".hta",     "application/hta" },
    { ".htc",     "text/x-component" },
    { ".htke",    "application/vnd.kenameaapp"},             
    { ".htt",     "text/webviewhtml" },
    { ".htx",     "text/html" },
    { ".hvd",     "application/vnd.yamaha.hv-dic"},          
    { ".hvp",     "application/vnd.yamaha.hv-voice"},        
    { ".hvs",     "application/vnd.yamaha.hv-script"},       
    { ".i2g",     "application/vnd.intergeo"},               
    { ".ica",     "application/vnd.commerce-battelle"},      
    { ".icc",     "application/vnd.iccprofile"},             
    { ".ice",     "x-conference/x-cooltalk" },
    { ".ico",     "image/x-icon" },
    { ".ics",     "text/calendar"},                          /* http://www.iana.org/go/rfc5545 */
    { ".idc",     "text/plain" },
    { ".ief",     "image/ief" },
    { ".iefs",    "image/ief" },
    { ".ifb",     "text/calendar" },
    { ".iges",    "application/iges" },
    { ".igl",     "application/vnd.igloader"},               
    { ".igm",     "application/vnd.insors.igm"},             
    { ".igs",     "application/iges" },
    { ".igx",     "application/vnd.micrografx-igx"},         
    { ".iii",     "application/x-iphone" },
    { ".ima",     "application/x-ima" },
    { ".imap",    "application/x-httpd-imap" },
    { ".imgcal",  "application/vnd.3lightssoftware.imagescal"}, 
    { ".imp",     "application/vnd.accpac.simply.imp"},      
    { ".ims",     "application/vnd.ms-ims"},                 
    { ".imscc",   "application/vnd.ims.imsccv1p1"},          
    { ".inf",     "application/inf" },
    { ".ins",     "application/x-internet-signup" },
    { ".iota",    "application/vnd.astraea-software.iota"},  
    { ".ip",      "application/x-ip2" },
    { ".ipfix",   "application/ipfix"},                      /* http://www.iana.org/go/rfc5655 */
    { ".irm",     "application/vnd.ibm.rights-management"},  
    { ".irp",     "application/vnd.irepository.package+xml"}, 
    { ".isp",     "application/x-internet-signup" },
    { ".isu",     "video/x-isvideo" },
    { ".it",      "audio/it" },
    { ".its",     "application/its+xml"},                    
    { ".iv",      "application/x-inventor" },
    { ".ivp",     "application/vnd.immervision-ivp"},        
    { ".ivr",     "i-world/i-vrml" },
    { ".ivu",     "application/vnd.immervision-ivu"},        
    { ".ivy",     "application/x-livescreen" },
    { ".jad",     "text/vnd.sun.j2me.app-descriptor"},       
    { ".jam",     "application/vnd.jam"},                    
    { ".jar",     "application/x-java-archive" },
    { ".jav",     "text/plain" },
    { ".java",    "text/plain" },
    { ".jcm",     "application/x-java-commerce" },
    { ".jfif",    "image/jpeg" },
    { ".jfif-tbnl", "image/jpeg" },
    { ".jisp",    "application/vnd.jisp"},                   
    { ".jlt",     "application/vnd.hp-jlyt"},                
    { ".jng",     "image/x-jng" },
    { ".joda",    "application/vnd.joost.joda-archive"},     
    { ".jp2",     "image/jp2"},                              /* http://www.iana.org/go/rfc3745 */
    { ".jpe",     "image/jpeg" },
    { ".jpeg",    "image/jpeg" },
    { ".jpf",     "image/jpx"},                              /* http://www.iana.org/go/rfc3745 */
    { ".jpg",     "image/jpeg" },
    { ".jpgv",    "video/jpeg"},                             /* rfc 3555 */
    { ".jpm",     "image/jpm"},                              /* http://www.iana.org/go/rfc3745 */
    { ".jps",     "image/x-jps" },
    { ".jrd",     "application/jrd+json"},                   /* http://www.iana.org/go/rfc7033 */
    { ".js",      "application/javascript"},                 /* http://www.iana.org/go/rfc4329 */
    { ".json",    "application/json"},                       /* http://www.iana.org/go/rfc7158 */
    { ".jsonld",  "application/vnd.ims.lis.v2.result+json"}, 
    { ".jtd",     "text/vnd.esmertec.theme-descriptor"},     
    { ".jut",     "image/jutvision" },
    { ".kar",     "audio/midi" },
    { ".kia",     "application/vnd.kidspiration"},           
    { ".kml",     "application/vnd.google-earth.kml+xml"},   
    { ".kmz",     "application/vnd.google-earth.kmz"},       
    { ".kne",     "application/vnd.kinar"},                  
    { ".koz",     "audio/vnd.audiokoz"},                     
    { ".ksh",     "application/x-ksh" },
    { ".ktz",     "application/vnd.kahootz"},                
    { ".la",      "audio/nspaudio" },
    { ".lam",     "audio/x-liveaudio" },
    { ".lasxml",  "application/vnd.las.las+xml"},            
    { ".latex",   "application/x-latex" },
    { ".lbd",     "application/vnd.llamagraphics.life-balance.desktop"}, 
    { ".lbe",     "application/vnd.llamagraphics.life-balance.exchange+xml"}, 
    { ".le",      "application/vnd.bluetooth.le.oob"},       
    { ".les",     "application/vnd.hhe.lesson-player"},      
    { ".lha",     "application/octet-stream" },
    { ".lhx",     "application/octet-stream" },
    { ".link66",  "application/vnd.route66.link66+xml"},     
    { ".list",    "text/plain" },
    { ".list3820", "application/vnd.ibm.modcap"},             
    { ".lma",     "audio/nspaudio" },
    { ".log",     "text/plain" },
    { ".lostsyncxml", "application/lostsync+xml"},               /* http://www.iana.org/go/rfc6739 */
    { ".ls",      "application/x-javascript" },
    { ".lsf",     "video/x-la-asf" },
    { ".lsp",     "application/x-lisp" },
    { ".lst",     "text/plain" },
    { ".lsx",     "video/x-la-asf" },
    { ".ltf",     "application/vnd.frogans.ltf"},            
    { ".ltx",     "application/x-latex" },
    { ".lvp",     "audio/vnd.lucent.voice"},                 
    { ".lwp",     "application/vnd.lotus-wordpro"},          
    { ".lxf",     "application/lxf"},                        
    { ".lzh",     "application/x-lzh" },
    { ".lzx",     "application/lzx" },
    { ".m",       "application/vnd.wolfram.mathematica.package"}, 
    { ".m13",     "application/x-msmediaview" },
    { ".m14",     "application/x-msmediaview" },
    { ".m1v",     "video/mpeg" },
    { ".m21",     "application/mp21"},                       
    { ".m2a",     "audio/mpeg" },
    { ".m2v",     "video/mpeg" },
    { ".m3u",     "application/vnd.apple.mpegurl"},          
    { ".m4a",     "audio/mp4" },
    { ".m4p",     "audio/mp4" },
    { ".m4s",     "video/iso.segment"},                      
    { ".m4v",     "video/x-m4v"},                          /* wikipedia: m4v */
    { ".ma",      "application/mathematica"},              /* iana - mathematica */
    { ".mads",    "application/mads+xml"},                   /* http://www.iana.org/go/rfc6207 */
    { ".mag",     "application/vnd.ecowin.chart"},           
    { ".man",     "application/x-troff-man" },
    { ".map",     "application/x-navimap" },
    { ".mar",     "text/plain" },
    { ".mathml",  "application/mathml+xml"},               /* w3c math home */
    { ".mbd",     "application/mbedlet" },
    { ".mbk",     "application/vnd.mobius.mbk"},             
    { ".mbox",    "application/mbox"},                       /* http://www.iana.org/go/rfc4155 */
    { ".mc$",     "application/x-magic-cap-package-1.0" },
    { ".mc1",     "application/vnd.medcalcdata"},            
    { ".mcd",     "application/vnd.mcd"},                    
    { ".mcf",     "text/mcf" },
    { ".mcp",     "application/netmc" },
    { ".md",      "text/markdown"},                          /* http://www.iana.org/go/draft-ietf-appsawg-text-markdown-12 */
    { ".mdb",     "application/x-msaccess" },
    { ".mdc",     "application/vnd.marlin.drm.mdcf"},        
    { ".mdi",     "image/vnd.ms-modi"},                      
    { ".me",      "application/x-troff-me" },
    { ".mets",    "application/mets+xml"},                   /* http://www.iana.org/go/rfc6207 */
    { ".mf4",     "application/mf4"},                        
    { ".mfm",     "application/vnd.mfmp"},                   
    { ".mft",     "application/rpki-manifest"},              /* http://www.iana.org/go/rfc6481 */
    { ".mgp",     "application/vnd.osgeo.mapguide.package"}, 
    { ".mgz",     "application/vnd.proteus.magazine"},       
    { ".mht",     "message/rfc822" },
    { ".mhtml",   "message/rfc822" },
    { ".mid",     "audio/sp-midi"},                          
    { ".midi",    "audio/x-midi" },
    { ".mif",     "application/vnd.mif" },
    { ".mime",    "message/rfc822" },
    { ".miz",     "text/mizar"},                             
    { ".mj2",     "video/mj2"},                              /* http://www.iana.org/go/rfc3745 */
    { ".mjf",     "audio/x-vnd.audioexplosion.mjuicemediafile" },
    { ".mjpg",    "video/x-motion-jpeg" },
    { ".mlp",     "audio/vnd.dolby.mlp"},                    
    { ".mm",      "application/base64" },
    { ".mmd",     "application/vnd.chipnuts.karaoke-mmd"},   
    { ".mmdb",    "application/vnd.maxmind.maxmind-db"},     
    { ".mme",     "application/base64" },
    { ".mmf",     "application/vnd.smaf"},                   
    { ".mmr",     "image/vnd.fujixerox.edmics-mmr"},         
    { ".mng",     "image/x-mng" },
    { ".mny",     "application/x-msmoney" },
    { ".mocha",   "application/x-javascript" },
    { ".mod",     "audio/mod" },
    { ".mods",    "application/mods+xml"},                   /* http://www.iana.org/go/rfc6207 */
    { ".moov",    "video/quicktime" },
    { ".mov",     "video/quicktime" },
    { ".movie",   "video/x-sgi-movie" },
    { ".mp",      "audio/mpeg"},                             /* http://www.iana.org/go/rfc3003 */
    { ".mp2",     "audio/mpeg" },
    { ".mp3",     "audio/mpeg" },
    { ".mp4",     "video/mp4"},                              /* http://www.iana.org/go/rfc6381 */
    { ".mp4a",    "audio/mp4" },
    { ".mpa",     "video/mpeg" },
    { ".mpc",     "application/vnd.mophun.certificate"},     
    { ".mpd",     "application/dash+xml"},                   
    { ".mpdd",    "application/dashdelta"},                  
    { ".mpe",     "video/mpeg" },
    { ".mpeg",    "video/mpeg" },
    { ".mpf",     "text/vnd.ms-mediapackage"},               
    { ".mpg",     "video/mpeg" },
    { ".mpga",    "audio/mpeg" },
    { ".mpm",     "application/vnd.blueice.multipass"},      
    { ".mpn",     "application/vnd.mophun.application"},     
    { ".mpp",     "application/vnd.ms-project" },
    { ".mpt",     "application/x-project" },
    { ".mpv",     "application/x-project" },
    { ".mpv2",    "video/mpeg" },
    { ".mpx",     "application/x-project" },
    { ".mpy",     "application/vnd.ibm.minipay"},            
    { ".mqy",     "application/vnd.mobius.mqy"},             
    { ".mrc",     "application/marc" },
    { ".mrcx",    "application/marcxml+xml"},                /* http://www.iana.org/go/rfc6207 */
    { ".ms",      "application/x-troff-ms" },
    { ".msa",     "application/vnd.msa-disk-image"},         
    { ".mscml",   "application/mediaservercontrol+xml"},   /* rfc 5022 */
    { ".msd",     "application/vnd.fdsn.mseed"},             
    { ".mseq",    "application/vnd.mseq"},                   
    { ".msf",     "application/vnd.epson.msf"},              
    { ".msh",     "model/mesh"},                           /* rfc 2077 */
    { ".msl",     "application/vnd.mobius.msl"},             
    { ".msty",    "application/vnd.muvee.style"},            
    { ".mts",     "model/vnd.mts"},                          
    { ".mv",      "video/x-sgi-movie" },
    { ".mvb",     "application/x-msmediaview" },
    { ".mvt",     "application/vnd.mapbox-vector-tile"},     
    { ".mwf",     "application/vnd.mfer"},                   
    { ".mxf",     "application/mxf"},                        /* http://www.iana.org/go/rfc4539 */
    { ".mxl",     "application/vnd.recordare.musicxml"},     
    { ".mxmf",    "audio/mobile-xmf"},                       /* http://www.iana.org/go/rfc4723 */
    { ".mxml",    "application/xv+xml"},                     /* http://www.iana.org/go/rfc4374 */
    { ".mxs",     "application/vnd.triscape.mxs"},           
    { ".mxu",     "video/vnd-mpegurl"},                      
    { ".my",      "audio/make" },
    { ".mzz",     "application/x-vnd.audioexplosion.mzz" },
    { ".n-gage",  "application/vnd.nokia.n-gage.symbian.install"}, 
    { ".nap",     "image/naplps" },
    { ".naplps",  "image/naplps" },
    { ".nb",      "application/mathematica"},                
    { ".nbp",     "application/vnd.wolfram.player"},         
    { ".nc",      "application/x-netcdf" },
    { ".ncm",     "applicaction/vnd.nokia.configuration-message" },
    { ".nds",     "application/vnd.nintendo.nitro.rom"},     
    { ".ngdat",   "application/vnd.nokia.n-gage.data"},      
    { ".nif",     "image/x-niff" },
    { ".niff",    "image/x-niff" },
    { ".nim",     "video/vnd.nokia.interleaved-multimedia"}, 
    { ".nix",     "application/x-mix-transfer" },
    { ".nlu",     "application/vnd.neurolanguage.nlu"},      
    { ".nml",     "application/vnd.enliven"},                
    { ".nnd",     "application/vnd.noblenet-directory"},     
    { ".nns",     "application/vnd.noblenet-sealer"},        
    { ".nnw",     "application/vnd.noblenet-web"},           
    { ".notebook", "application/vnd.smart.notebook"},         
    { ".nsc",     "application/x-conference" },
    { ".nsf",     "application/vnd.lotus-notes"},            
    { ".ntf",     "application/vnd.nitf"},                   
    { ".nvd",     "application/x-navidoc" },
    { ".nvm",     "application/x-navimap" },
    { ".nws",     "message/rfc822" },
    { ".o",       "application/octet-stream" },
    { ".oa2",     "application/vnd.fujitsu.oasys2"},         
    { ".oa3",     "application/vnd.fujitsu.oasys3"},         
    { ".oas",     "application/vnd.fujitsu.oasys"},          
    { ".obg",     "application/vnd.openblox.game-binary"},   
    { ".obgx",    "application/vnd.openblox.game+xml"},      
    { ".oda",     "application/oda" },
    { ".odb",     "application/vnd.oasis.opendocument.database"}, 
    { ".odc",     "application/vnd.oasis.opendocument.chart"}, 
    { ".odf",     "application/vnd.oasis.opendocument.formula"}, 
    { ".odg",     "application/vnd.oasis.opendocument.graphics"}, 
    { ".odi",     "application/vnd.oasis.opendocument.image"}, 
    { ".odm",     "application/vnd.oasis.opendocument.text-master"}, 
    { ".odp",     "application/vnd.oasis.opendocument.presentation"}, 
    { ".ods",     "application/vnd.oasis.opendocument.spreadsheet"}, 
    { ".odt",     "application/vnd.oasis.opendocument.text"}, 
    { ".odx",     "application/odx"},                        
    { ".oeb",     "application/vnd.openeye.oeb"},            
    { ".oga",     "audio/ogg"},                              /* http://www.iana.org/go/draft-ietf-codec-oggopus-14 */
    { ".ogex",    "model/vnd.opengex"},                      
    { ".ogg",     "audio/ogg" },
    { ".ogv",     "video/ogg"},                              /* http://www.iana.org/go/draft-ietf-codec-oggopus-14 */
    { ".ogx",     "application/ogg"},                        /* http://www.iana.org/go/draft-ietf-codec-oggopus-14 */
    { ".omc",     "application/x-omc" },
    { ".omcd",    "application/x-omcdatamaker" },
    { ".omcr",    "application/x-omcregerator" },
    { ".opf",     "application/oebps-package+xml"},          /* http://www.iana.org/go/rfc4839 */
    { ".or3",     "application/vnd.lotus-organizer"},        
    { ".orq",     "application/ocsp-request"},               /* http://www.iana.org/go/rfc6960 */
    { ".ors",     "application/ocsp-response"},              /* http://www.iana.org/go/rfc6960 */
    { ".osf",     "application/vnd.yamaha.openscoreformat"}, 
    { ".otc",     "application/vnd.oasis.opendocument.chart-template"}, 
    { ".otf",     "application/vnd.oasis.opendocument.formula-template"}, 
    { ".otg",     "application/vnd.oasis.opendocument.graphics-template"}, 
    { ".oth",     "application/vnd.oasis.opendocument.text-web"}, 
    { ".oti",     "application/vnd.oasis.opendocument.image-template"}, 
    { ".otp",     "application/vnd.oasis.opendocument.presentation-template"}, 
    { ".ots",     "application/vnd.oasis.opendocument.spreadsheet-template"}, 
    { ".ott",     "application/vnd.oasis.opendocument.text-template"}, 
    { ".owl",     "application/vnd.biopax.rdf+xml"},         
    { ".oxlicg",  "application/vnd.oxli.countgraph"},        
    { ".oxps",    "application/oxps"},                       
    { ".p",       "application/pkcs10"},                     /* http://www.iana.org/go/rfc5967 */
    { ".p10",     "application/pkcs10" },
    { ".p12",     "application/x-pkcs-12" },
    { ".p2p",     "application/vnd.wfa.p2p"},                
    { ".p7a",     "application/x-pkcs7-signature" },
    { ".p7b",     "application/x-pkcs7-certificates" },
    { ".p7c",     "application/x-pkcs7-mime" },
    { ".p7m",     "application/x-pkcs7-mime" },
    { ".p7r",     "application/x-pkcs7-certreqresp" },
    { ".p7s",     "application/x-pkcs7-signature" },
    { ".p8",      "application/pkcs8"},                    /* rfc 5208 */
    { ".pac",     "application/x-ns-proxy-autoconfig" },
    { ".package", "application/vnd.autopackage"},            
    { ".part",    "application/pro_eng" },
    { ".paw",     "application/vnd.pawaafile"},              
    { ".pbd",     "application/vnd.powerbuilder6"},          
    { ".pbm",     "image/x-portable-bitmap" },
    { ".pcap",    "application/vnd.tcpdump.pcap"},           
    { ".pcl",     "application/vnd.hp-pcl" },
    { ".pct",     "image/x-pict" },
    { ".pcx",     "image/vnd.zbrush.pcx"},                   
    { ".pdb",     "chemical/x-pdb" },
    { ".pdf",     "application/pdf" },
    { ".pdx",     "application/pdx"},                        
    { ".pfr",     "application/font-tdpfr"},                 /* http://www.iana.org/go/rfc3073 */
    { ".pfunk",   "audio/make" },
    { ".pfx",     "application/x-pkcs12" },
    { ".pgb",     "image/vnd.globalgraphics.pgb"},           
    { ".pgm",     "image/x-portable-graymap" },
    { ".pgp",     "application/pgp-signature"},            /* rfc 2015 */
    { ".pic",     "image/vnd.radiance"},                     
    { ".pict",    "image/pict" },
    { ".pil",     "application/vnd.piaccess.application-licence"}, 
    { ".pkg",     "application/x-newton-compatible-pkg" },
    { ".pki",     "application/pkixcmp"},                  /* rfc 2585 */
    { ".pkipath", "application/pkix-pkipath"},               /* http://www.iana.org/go/rfc6066 */
    { ".pko",     "application/vnd.ms-pki.pko" },
    { ".pl",      "text/plain" },
    { ".plb",     "application/vnd.3gpp.pic-bw-large"},      
    { ".plc",     "application/vnd.mobius.plc"},             
    { ".plf",     "application/vnd.pocketlearn"},            
    { ".plj",     "audio/vnd.everad.plj"},                   
    { ".plp",     "application/vnd.panoply"},                
    { ".pls",     "application/pls+xml"},                  /* rfc 4267 */
    { ".plx",     "application/x-pixclscript" },
    { ".pm",      "image/x-xpixmap" },
    { ".pm4",     "application/x-pagemaker" },
    { ".pm5",     "application/x-pagemaker" },
    { ".pma",     "application/x-perfmon" },
    { ".pmc",     "application/x-perfmon" },
    { ".pml",     "application/vnd.ctc-posml"},              
    { ".pmr",     "application/x-perfmon" },
    { ".pmw",     "application/x-perfmon" },
    { ".png",     "image/png" },
    { ".pnm",     "image/x-portable-anymap" },
    { ".portpkg", "application/vnd.macports.portpkg"},       
    { ".pot",     "application/vnd.ms-powerpoint" },
    { ".potm",    "application/vnd.ms-powerpoint.template.macroenabled.12"}, 
    { ".potx",    "application/vnd.openxmlformats-officedocument.presentationml-template"}, 
    { ".pov",     "model/x-pov" },
    { ".ppa",     "application/vnd.ms-powerpoint" },
    { ".ppam",    "application/vnd.ms-powerpoint.addin.macroenabled.12"}, 
    { ".ppd",     "application/vnd.cups-ppd"},               
    { ".ppkg",    "application/vnd.xmpie.ppkg"},             
    { ".ppm",     "image/x-portable-pixmap" },
    { ".pps",     "application/vnd.ms-powerpoint" },
    { ".ppsm",    "application/vnd.ms-powerpoint.slideshow.macroenabled.12"}, 
    { ".ppsx",    "application/vnd.openxmlformats-officedocument.presentationml.slideshow"}, 
    { ".ppt",     "application/vnd.ms-powerpoint" },
    { ".pptm",    "application/vnd.ms-powerpoint.presentation.macroenabled.12"}, 
    { ".pptx",    "application/vnd.openxmlformats-officedocument.presentationml.presentation"}, 
    { ".ppz",     "application/vnd.ms-powerpoint" },
    { ".prc",     "application/vnd.palm"},                   
    { ".pre",     "application/vnd.lotus-freelance" },
    { ".preminet", "application/vnd.preminet"},               
    { ".prf",     "application/pics-rules" },
    { ".provx",   "application/provenance+xml"},             
    { ".prt",     "application/pro_eng" },
    { ".prz",     "application/vnd.lotus-freelance"},        
    { ".ps",      "application/postscript" },
    { ".psb",     "application/vnd.3gpp.pic-bw-small"},      
    { ".psd",     "image/vnd.adobe.photoshop" },
    { ".pskcxml", "application/pskc+xml"},                 /* rfc 6030 */
    { ".pti",     "image/prs.pti"},                          
    { ".pub",     "application/x-mspublisher" },
    { ".pvb",     "application/vnd.3gpp.pic-bw-var"},        
    { ".pvu",     "paleovu/x-pv" },
    { ".pwn",     "application/vnd.3m.post-it-notes"},       
    { ".pwz",     "application/vnd.ms-powerpoint" },
    { ".py",      "text/x-script.python" },
    { ".pya",     "audio/vnd.ms-playready.media.pya"},       
    { ".pyc",     "application/x-bytecode.python" },
    { ".pyv",     "video/vnd.ms-playready.media.pyv"},       
    { ".qam",     "application/vnd.epson.quickanime"},       
    { ".qbo",     "application/vnd.intu.qbo"},               
    { ".qcall",   "application/vnd.ericsson.quickcall"},     
    { ".qcp",     "audio/vnd.qcelp" },
    { ".qd3",     "x-world/x-3dmf" },
    { ".qd3d",    "x-world/x-3dmf" },
    { ".qfx",     "application/vnd.intu.qfx"},               
    { ".qif",     "image/x-quicktime" },
    { ".qps",     "application/vnd.publishare-delta-tree"},  
    { ".qt",      "video/quicktime" },
    { ".qtc",     "video/x-qtc" },
    { ".qti",     "image/x-quicktime" },
    { ".qtif",    "image/quicktime" },
    { ".quox",    "application/vnd.quobject-quoxdocument"},  
    { ".qxd",     "application/vnd.quark.quarkxpress"},      
    { ".ra",      "audio/x-pn-realaudio" },
    { ".ram",     "audio/x-pn-realaudio" },
    { ".rar",     "application/x-rar-compressed" },
    { ".ras",     "image/x-cmu-raster" },
    { ".rast",    "image/x-cmu-raster" },
    { ".rcprofile", "application/vnd.ipunplugged.rcprofile"},  
    { ".rdf",     "application/rdf+xml"},                    /* http://www.iana.org/go/rfc3870 */
    { ".rdf-crypt", "application/prs.rdf-xml-crypt"},          
    { ".rdz",     "application/vnd.data-vision.rdz"},        
    { ".relo",    "application/p2p-overlay+xml"},            /* http://www.iana.org/go/rfc6940 */
    { ".rep",     "application/vnd.businessobjects"},        
    { ".rexx",    "text/x-script.rexx" },
    { ".rf",      "image/vnd.rn-realflash" },
    { ".rgb",     "image/x-rgb" },
    { ".rip",     "audio/vnd.rip"},                          
    { ".rl",      "application/resource-lists+xml"},       /* rfc 4826 */
    { ".rlc",     "image/vnd.fujixerox.edmics-rlc"},         
    { ".rld",     "application/resource-lists-diff+xml"},  /* rfc 4826 */
    { ".rm",      "audio/vnd.hns.audio"},                    
    { ".rmi",     "audio/midi" },
    { ".rmm",     "audio/x-pn-realaudio" },
    { ".rmp",     "audio/x-pn-realaudio-plugin" },
    { ".rms",     "application/vnd.jcp.javame.midlet-rms"},  
    { ".rnc",     "application/relax-ng-compact-syntax"},    /* http://www.jtc1sc34.org/repository/0661.pdf */
    { ".rnd",     "application/prs.nprend"},                 
    { ".rng",     "application/vnd.nokia.ringing-tone" },
    { ".rnx",     "application/vnd.rn-realplayer" },
    { ".roa",     "application/rpki-roa"},                   /* http://www.iana.org/go/rfc6481 */
    { ".roff",    "application/x-roff" },
    { ".rp",      "image/vnd.rn-realpix" },
    { ".rp9",     "application/vnd.cloanto.rp9"},            
    { ".rpm",     "application/octet-stream" },
    { ".rpss",    "application/vnd.nokia.radio-presets"},    
    { ".rpst",    "application/vnd.nokia.radio-preset"},     
    { ".rq",      "application/sparql-query"},             /* w3c sparql */
    { ".rs",      "application/rls-services+xml"},         /* rfc 4826 */
    { ".rsheet",  "application/urc-ressheet+xml"},           
    { ".rss",     "application/rss+xml" },
    { ".rt",      "text/richtext" },
    { ".rtf",     "application/rtf"},                        
    { ".rtx",     "text/richtext" },
    { ".rv",      "video/vnd.rn-realvideo" },
    { ".s",       "application/vnd.sealed.3df"},             
    { ".s3m",     "audio/s3m" },
    { ".sac",     "application/tamp-sequence-adjust-confirm"}, /* http://www.iana.org/go/rfc5934 */
    { ".saf",     "application/vnd.yamaha.smaf-audio"},      
    { ".saveme",  "application/octet-stream" },
    { ".sbk",     "application/x-tbook" },
    { ".sbml",    "application/sbml+xml"},                 /* rfc 3823 */
    { ".sc",      "application/vnd.ibm.secure-container"},   
    { ".scd",     "application/vnd.scribus"},                
    { ".scim",    "application/scim+json"},                  /* http://www.iana.org/go/rfc7644 */
    { ".scld",    "application/vnd.doremir.scorecloud-binary-document"}, 
    { ".scm",     "application/vnd.lotus-screencam"},        
    { ".scq",     "application/scvp-cv-request"},            /* http://www.iana.org/go/rfc5055 */
    { ".scs",     "application/scvp-cv-response"},           /* http://www.iana.org/go/rfc5055 */
    { ".scsf",    "application/vnd.sealed.csf"},             
    { ".sct",     "text/scriptlet" },
    { ".sdkm",    "application/vnd.solent.sdkm+xml"},        
    { ".sdml",    "text/plain" },
    { ".sdoc",    "application/vnd.sealed-doc"},             
    { ".sdp",     "application/sdp"},                        /* http://www.iana.org/go/rfc4566 */
    { ".sdr",     "application/sounder" },
    { ".sea",     "application/sea" },
    { ".seed",    "application/vnd.fdsn.seed"},              
    { ".semd",    "application/vnd.semd"},                   
    { ".semf",    "application/vnd.semf"},                   
    { ".seml",    "application/vnd.sealed-eml"},             
    { ".set",     "application/set" },
    { ".setpay",  "application/set-payment-initiation" },
    { ".setreg",  "application/set-registration-initiation" },
    { ".sfc",     "application/vnd.nintendo.snes.rom"},      
    { ".sfd",     "application/vnd.font-fontforge-sfd"},     
    { ".sfs",     "application/vnd.spotfire.sfs"},           
    { ".sgif",    "image/vnd.sealedmedia.softseal-gif"},     
    { ".sgm",     "text/sgml" },
    { ".sgml",    "text/sgml" },
    { ".sh",      "application/x-sh" },
    { ".shar",    "application/x-shar" },
    { ".shf",     "application/shf+xml"},                  /* rfc 4194 */
    { ".si",      "text/vnd.wap.si"},                        
    { ".sic",     "application/vnd.wap.sic"},                
    { ".sid",     "audio/prs.sid"},                          
    { ".sit",     "application/x-stuffit" },
    { ".siv",     "application/sieve"},                      /* http://www.iana.org/go/rfc5228 */
    { ".sjpg",    "image/vnd.sealedmedia.softseal-jpg"},     
    { ".skd",     "application/x-koan" },
    { ".skm",     "application/x-koan" },
    { ".skp",     "application/x-koan" },
    { ".skt",     "application/x-koan" },
    { ".sl",      "text/vnd.wap.sl"},                        
    { ".slc",     "application/vnd.wap-slc"},                
    { ".sldm",    "application/vnd.ms-powerpoint.slide.macroenabled.12"}, 
    { ".sldx",    "application/vnd.openxmlformats-officedocument.presentationml.slide"}, 
    { ".slt",     "application/vnd.epson.salt"},             
    { ".sm",      "application/vnd.stepmania.stepchart"},    
    { ".smht",    "application/vnd.sealed-mht"},             
    { ".smi",     "application/smil+xml" },
    { ".smil",    "application/smil"},                       /* http://www.iana.org/go/rfc4536 */
    { ".smk",     "video/vnd.radgamettools.smacker"},        
    { ".smov",    "video/vnd.sealedmedia.softseal-mov"},     
    { ".smp",     "audio/vnd.sealedmedia.softseal-mpeg"},    
    { ".smpg",    "video/vnd.sealed.mpeg4"},                 
    { ".sms",     "application/vnd.3gpp.sms"},               
    { ".smzip",   "application/vnd.stepmania.package"},      
    { ".snd",     "audio/basic" },
    { ".soa",     "text/dns"},                               /* http://www.iana.org/go/rfc4027 */
    { ".sol",     "application/solids" },
    { ".spc",     "application/x-pkcs7-certificate" },
    { ".spdf",    "application/vnd.sealedmedia.softseal-pdf"}, 
    { ".spf",     "application/vnd.yamaha.smaf-phrase"},     
    { ".spl",     "application/futuresplash" },
    { ".spng",    "image/vnd.sealed-png"},                   
    { ".spot",    "text/vnd.in3d.spot"},                     
    { ".spp",     "application/scvp-vp-response"},           /* http://www.iana.org/go/rfc5055 */
    { ".sppt",    "application/vnd.sealed-ppt"},             
    { ".spq",     "application/scvp-vp-request"},            /* http://www.iana.org/go/rfc5055 */
    { ".spr",     "application/x-sprite" },
    { ".sprite",  "application/x-sprite" },
    { ".spx",     "audio/ogg" },
    { ".sql",     "application/sql"},                        /* http://www.iana.org/go/rfc6922 */
    { ".src",     "application/x-wais-source" },
    { ".sru",     "application/sru+xml"},                    /* http://www.iana.org/go/rfc6207 */
    { ".srx",     "application/sparql-results+xml"},       /* w3c sparql */
    { ".sse",     "application/vnd.kodak-descriptor"},       
    { ".ssf",     "application/vnd.epson.ssf"},              
    { ".ssm",     "application/streamingmedia" },
    { ".ssml",    "application/ssml+xml"},                 /* w3c speech synthesis */
    { ".sst",     "application/vnd.ms-pki.certstore" },
    { ".sswf",    "video/vnd.sealed-swf"},                   
    { ".st",      "application/vnd.sailingtracker.track"},   
    { ".stc",     "application/vnd.sun.xml.calc.template" },
    { ".std",     "application/vnd.sun.xml.draw.template" },
    { ".step",    "application/step" },
    { ".stf",     "application/vnd.wt.stf"},                 
    { ".sti",     "application/vnd.sun.xml.impress.template" },
    { ".stif",    "application/vnd.sealed-tiff"},            
    { ".stk",     "application/hyperstudio"},              /* iana - hyperstudio */
    { ".stl",     "application/x-navistyle" },
    { ".stm",     "text/html" },
    { ".stml",    "application/vnd.sealedmedia.softseal-html"}, 
    { ".stp",     "application/step" },
    { ".stw",     "application/vnd.sun.xml.writer.template" },
    { ".sub",     "image/vnd.dvb.subtitle"},                 
    { ".sus",     "application/vnd.sus-calendar"},           
    { ".sv4cpio", "application/x-sv4cpio" },
    { ".sv4crc",  "application/x-sv4crc" },
    { ".svc",     "application/vnd.dvb_service"},            
    { ".svf",     "image/vnd.dwg" },
    { ".svg",     "image/svg+xml" },
    { ".svgz",    "image/x-svgz" },
    { ".svr",     "application/x-world" },
    { ".swf",     "application/vnd.adobe.flash-movie"},      
    { ".swi",     "application/vnd.arastra.swi"},            
    { ".sxc",     "application/vnd.sun.xml.calc" },
    { ".sxd",     "application/vnd.sun.xml.draw" },
    { ".sxg",     "application/vnd.sun.xml.writer.global" },
    { ".sxi",     "application/vnd.vd-study"},               
    { ".sxls",    "application/vnd.sealed-xls"},             
    { ".sxm",     "application/vnd.sun.xml.math" },
    { ".sxw",     "application/vnd.sun.xml.writer" },
    { ".t",       "application/x-troff" },
    { ".t38",     "image/t38"},                              /* http://www.iana.org/go/rfc3362 */
    { ".taglet",  "application/vnd.mynfc"},                  
    { ".talk",    "text/x-speech" },
    { ".tao",     "application/vnd.tao.intent-module-archive"}, 
    { ".tap",     "image/vnd.tencent.tap"},                  
    { ".tar",     "application/x-tar" },
    { ".tau",     "application/tamp-apex-update"},           /* http://www.iana.org/go/rfc5934 */
    { ".tbk",     "application/toolbook" },
    { ".tcap",    "application/vnd.3gpp2.tcap"},             
    { ".tcl",     "text/plain" },
    { ".tcsh",    "text/x-script.tcsh" },
    { ".tcu",     "application/tamp-community-update"},      /* http://www.iana.org/go/rfc5934 */
    { ".td",      "application/urc-targetdesc+xml"},         
    { ".teacher", "application/vnd.smart.teacher"},          
    { ".tei",     "application/tei+xml"},                  /* rfc 6129 */
    { ".ter",     "application/tamp-error"},                 /* http://www.iana.org/go/rfc5934 */
    { ".tex",     "application/x-tex" },
    { ".texi",    "application/x-texinfo" },
    { ".texinfo", "application/x-texinfo" },
    { ".text",    "text/plain" },
    { ".tfi",     "application/thraud+xml"},               /* rfc 5941 */
    { ".tfx",     "image/tiff-fx"},                          /* http://www.iana.org/go/rfc3950 */
    { ".tgz",     "application/gzip" },
    { ".thmx",    "application/vnd.ms-officetheme"},         
    { ".tif",     "image/tiff"},                             /* http://www.iana.org/go/rfc3302 */
    { ".tiff",    "image/tiff" },
    { ".tlclient", "application/vnd.cendio.thinlinc.clientconf"}, 
    { ".tmo",     "application/vnd.tmobile-livetv"},         
    { ".torrent", "application/x-bittorrent" },
    { ".tpl",     "application/vnd.groove-tool-template"},   
    { ".tpt",     "application/vnd.trid.tpt"},               
    { ".tr",      "application/x-troff" },
    { ".tra",     "application/vnd.trueapp"},                
    { ".tree",    "application/vnd.rainstor.data"},          
    { ".ts",      "text/vnd.trolltech.linguist"},            
    { ".tsa",     "application/tamp-sequence-adjust"},       /* http://www.iana.org/go/rfc5934 */
    { ".tsd",     "application/timestamped-data"},           /* http://www.iana.org/go/rfc5955 */
    { ".tsi",     "audio/tsp-audio" },
    { ".tsp",     "audio/tsplayer" },
    { ".tsq",     "application/tamp-status-query"},          /* http://www.iana.org/go/rfc5934 */
    { ".tsr",     "application/tamp-status-response"},       /* http://www.iana.org/go/rfc5934 */
    { ".tst",     "application/vnd.etsi.timestamp-token"},   
    { ".tsv",     "text/tab-separated-values" },
    { ".ttml",    "application/ttml+xml"},                   
    { ".tuc",     "application/tamp-update-confirm"},        /* http://www.iana.org/go/rfc5934 */
    { ".tur",     "application/tamp-update"},                /* http://www.iana.org/go/rfc5934 */
    { ".turbot",  "image/florian" },
    { ".twd",     "application/vnd.simtech-mindmapper"},     
    { ".txd",     "application/vnd.genomatix.tuxedo"},       
    { ".txf",     "application/vnd.mobius.txf"},             
    { ".txt",     "text/plain" },
    { ".u8dsn",   "message/global-delivery-status"},         /* http://www.iana.org/go/rfc6533 */
    { ".u8mdn",   "message/global-disposition-notification"}, /* http://www.iana.org/go/rfc6533 */
    { ".u8msg",   "message/global"},                         /* http://www.iana.org/go/rfc6532 */
    { ".ufdl",    "application/vnd.ufdl"},                   
    { ".uil",     "text/x-uil" },
    { ".uis",     "application/urc-uisocketdesc+xml"},       
    { ".uls",     "text/iuls" },
    { ".umj",     "application/vnd.umajin"},                 
    { ".uni",     "text/uri-list" },
    { ".unis",    "text/uri-list" },
    { ".unityweb", "application/vnd.unity"},                  
    { ".unknown", "application/dns"},                        /* http://www.iana.org/go/rfc4027 */
    { ".unv",     "application/i-deas" },
    { ".uoml",    "application/vnd.uoml+xml"},               
    { ".uri",     "text/uri-list" },
    { ".uric",    "text/vnd.si.uricatalogue"},               
    { ".urim",    "application/vnd.uri-map"},                
    { ".uris",    "text/uri-list"},                          /* http://www.iana.org/go/rfc2483 */
    { ".ustar",   "application/x-ustar" },
    { ".utz",     "application/vnd.uiq.theme"},              
    { ".uu",      "text/x-uuencode" },
    { ".uue",     "text/x-uuencode" },
    { ".uva",     "audio/vnd.dece.audio"},                   
    { ".uvf",     "application/vnd.dece.data"},              
    { ".uvh",     "video/vnd.dece.hd"},                      
    { ".uvi",     "image/vnd.dece.graphic"},                 
    { ".uvm",     "video/vnd.dece.mobile"},                  
    { ".uvp",     "video/vnd.dece.pd"},                      
    { ".uvs",     "video/vnd.dece.sd"},                      
    { ".uvt",     "application/vnd.dece.ttml+xml"},          
    { ".uvu",     "video/vnd.uvvu-mp4"},                     
    { ".uvv",     "video/vnd.dece.video"},                   
    { ".uvx",     "application/vnd.dece.unspecified"},       
    { ".uvz",     "application/vnd.dece-zip"},               
    { ".vbk",     "audio/vnd.nortel.vbk"},                   
    { ".vcd",     "application/x-cdlink" },
    { ".vcf",     "text/vcard"},                             /* http://www.iana.org/go/rfc6350 */
    { ".vcg",     "application/vnd.groove-vcard"},           
    { ".vcs",     "text/x-vcalendar" },
    { ".vcx",     "application/vnd.vcx"},                    
    { ".vda",     "application/vda" },
    { ".vdo",     "video/vdo" },
    { ".vew",     "application/groupwise" },
    { ".vfr",     "application/vnd.tml"},                    
    { ".vis",     "application/vnd.visionary"},              
    { ".viv",     "video/vnd.vivo" },
    { ".vivo",    "video/vnd.vivo" },
    { ".vmd",     "application/vocaltec-media-desc" },
    { ".vmf",     "application/vocaltect-media-file" },
    { ".vmt",     "application/vnd.valve.source.material"},  
    { ".voc",     "audio/voc" },
    { ".vos",     "video/vosaic" },
    { ".vox",     "audio/voxware" },
    { ".vpm",     "multipart/voice-message"},                /* http://www.iana.org/go/rfc2423 */
    { ".vqe",     "audio/x-twinvq-plugin" },
    { ".vqf",     "audio/x-twinvq" },
    { ".vql",     "audio/x-twinvq-plugin" },
    { ".vrml",    "x-world/x-vrml" },
    { ".vrt",     "x-world/x-vrt" },
    { ".vsc",     "application/vnd.vidsoft.vidconference"},  
    { ".vsd",     "application/vnd.visio"},                  
    { ".vsf",     "application/vnd.vsf"},                    
    { ".vst",     "application/x-visio" },
    { ".vsw",     "application/x-visio" },
    { ".vtf",     "image/vnd.valve.source.texture"},         
    { ".vtu",     "model/vnd.vtu"},                          
    { ".vxml",    "application/voicexml+xml"},             /* rfc 4267 */
    { ".w60",     "application/wordperfect6.0" },
    { ".w61",     "application/wordperfect6.1" },
    { ".w6w",     "application/msword" },
    { ".wadl",    "application/vnd.sun.wadl+xml"},           
    { ".wav",     "audio/l16"},                              /* http://www.iana.org/go/rfc4856 */
    { ".wb1",     "application/x-qpro" },
    { ".wbmp",    "image/vnd-wap-wbmp"},                     
    { ".wbs",     "application/vnd.criticaltools.wbs+xml"},  
    { ".wcm",     "application/vnd.ms-works" },
    { ".wdb",     "application/vnd.ms-works" },
    { ".web",     "application/vnd.xara" },
    { ".weba",    "audio/webm"},                           /* webm project */
    { ".webm",    "video/webm"},                           /* webm project */
    { ".webp",    "image/webp"},                           /* wikipedia: webp */
    { ".wg",      "application/vnd.pmi.widget"},             
    { ".wgt",     "application/widget"},                   /* w3c widget packaging and xml configuration */
    { ".wiz",     "application/msword" },
    { ".wk1",     "application/x-123" },
    { ".wks",     "application/vnd.ms-works" },
    { ".wlnk",    "application/link-format"},                /* http://www.iana.org/go/rfc6690 */
    { ".wma",     "audio/x-ms-wma" },
    { ".wmc",     "application/vnd.wmc"},                    
    { ".wmf",     "application/x-msmetafile" },
    { ".wml",     "text/vnd.wap-wml"},                       
    { ".wmlc",    "application/vnd-wap-wmlc"},               
    { ".wmls",    "text/vnd.wap.wmlscript"},                 
    { ".wmlsc",   "application/vnd.wap.wmlscript" },
    { ".woff",    "application/x-font-woff"},              /* wikipedia: web open font format */
    { ".word",    "application/msword" },
    { ".wp",      "application/wordperfect" },
    { ".wp5",     "application/wordperfect" },
    { ".wp6",     "application/wordperfect" },
    { ".wpd",     "application/wordperfect" },
    { ".wpl",     "application/vnd.ms-wpl"},                 
    { ".wps",     "application/vnd.ms-works" },
    { ".wq1",     "application/x-lotus" },
    { ".wqd",     "application/vnd.wqd"},                    
    { ".wri",     "application/x-mswrite" },
    { ".wrl",     "x-world/x-vrml" },
    { ".wrz",     "x-world/x-vrml" },
    { ".wsc",     "application/vnd.wfa.wsc"},                
    { ".wsdl",    "application/wsdl+xml"},                 /* w3c web service description language */
    { ".wspolicy","application/wspolicy+xml"},            /* w3c web services policy */
    { ".wsrc",    "application/x-wais-source" },
    { ".wtk",     "application/x-wintalk" },
    { ".wv",      "application/vnd.wv.csp+wbxml"},           
    { ".x",       "application/vnd.hzn-3d-crossword"},       
    { ".x-png",   "image/png" },
    { ".x3d",     "model/x3d+xml"},                          
    { ".x3db",    "model/x3d+fastinfoset"},                  
    { ".x3dv",    "model/x3d-vrml"},                         
    { ".x_b",     "model/vnd.parasolid.transmit-binary"},    
    { ".x_t",     "model/vnd.parasolid.transmit-text"},      
    { ".xaf",     "x-world/x-vrml" },
    { ".xar",     "application/vnd.xara"},                   
    { ".xbd",     "application/vnd.fujixerox.docuworks.binder"}, 
    { ".xbm",     "image/x-xbitmap" },
    { ".xcs",     "application/calendar+xml"},               /* http://www.iana.org/go/rfc6321 */
    { ".xct",     "application/vnd.fujixerox.docuworks.container"}, 
    { ".xdd",     "application/bacnet-xdd+zip"},             
    { ".xdm",     "application/vnd.syncml.dm+xml"},          
    { ".xdp",     "application/vnd.adobe.xdp+xml"},          
    { ".xdr",     "video/x-amt-demorun" },
    { ".xdssc",   "application/dssc+xml"},                   /* http://www.iana.org/go/rfc5698 */
    { ".xdw",     "application/vnd.fujixerox.docuworks"},    
    { ".xenc",    "application/xenc+xml"},                 /* w3c xml encryption syntax and processing */
    { ".xer",     "application/patch-ops-error+xml"},      /* rfc 5261 */
    { ".xfdf",    "application/vnd.adobe.xfdf"},             
    { ".xfdl",    "application/vnd.xfdl"},                   
    { ".xgz",     "xgl/drawing" },
    { ".xht",     "application/xhtml+xml" },
    { ".xhtml",   "application/xhtml+xml"},                  
    { ".xif",     "image/vnd.xiff"},                         
    { ".xl",      "application/excel" },
    { ".xla",     "application/vnd.ms-excel" },
    { ".xlam",    "application/vnd.ms-excel.addin.macroenabled.12"}, 
    { ".xlb",     "application/vnd.ms-excel" },
    { ".xlc",     "application/vnd.ms-excel" },
    { ".xld",     "application/vnd.ms-excel" },
    { ".xlim",    "application/vnd.xmpie.xlim"},             
    { ".xlk",     "application/vnd.ms-excel" },
    { ".xll",     "application/vnd.ms-excel" },
    { ".xlm",     "application/vnd.ms-excel" },
    { ".xls",     "application/vnd.ms-excel" },
    { ".xlsb",    "application/vnd.ms-excel.sheet.binary.macroenabled.12"}, 
    { ".xlsm",    "application/vnd.ms-excel.sheet.macroenabled.12"}, 
    { ".xlsx",    "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},    
    { ".xlt",     "application/vnd.ms-excel" },
    { ".xltm",    "application/vnd.ms-excel.template.macroenabled.12"}, 
    { ".xltx",    "application/vnd.openxmlformats-officedocument.spreadsheetml.template"},
    { ".xlv",     "application/vnd.ms-excel" },
    { ".xlw",     "application/vnd.ms-excel" },
    { ".xm",      "audio/xm" },
    { ".xmi",     "application/vnd.xmi+xml"},                
    { ".xml",     "text/xml"},                               /* http://www.iana.org/go/rfc7303 */
    { ".xmls",    "application/dskpp+xml"},                  /* http://www.iana.org/go/rfc6063 */
    { ".xmz",     "xgl/movie" },
    { ".xo",      "application/vnd.olpc-sugar"},             
    { ".xof",     "x-world/x-vrml" },
    { ".xop",     "application/xop+xml"},                  /* w3c xop */
    { ".xpix",    "application/x-vnd.ls-xpix" },
    { ".xpm",     "image/x-xpixmap" },
    { ".xpr",     "application/vnd.is-xpr"},                 
    { ".xps",     "application/vnd.ms-xpsdocument"},         
    { ".xsf",     "application/prs.xsf+xml"},                
    { ".xsl",     "application/xml" },
    { ".xslt",    "application/xslt+xml" },
    { ".xsm",     "application/vnd.syncml+xml"},             
    { ".xspf",    "application/xspf+xml" },
    { ".xsr",     "video/x-amt-showrun" },
    { ".xul",     "application/vnd.mozilla.xul+xml"},        
    { ".xwd",     "image/x-xwindowdump" },
    { ".xyz",     "chemical/x-xyz" },
    { ".yme",     "application/vnd.yaoweme"},                
    { ".z",       "application/x-compressed" },
    { ".zaz",     "application/vnd.zzazz.deck+xml"},         
    { ".zfc",     "application/vnd.filmit.zfc"},             
    { ".zfo",     "application/vnd.software602.filler.form-xml-zip"}, 
    { ".zip",     "application/zip" },
    { ".zir",     "application/vnd.zul"},                    
    { ".zmm",     "application/vnd.handheld-entertainment+xml"}, 
    { ".zoo",     "application/octet-stream" },
    { ".zsh",     "text/x-script.zsh" },

    { NULL,     NULL }
};


/*
 *----------------------------------------------------------------------
 *
 * NsConfigMimeTypes --
 *
 *      Add compiled-in and configured mime types.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
NsConfigMimeTypes(void)
{
    const Ns_Set *set;
    size_t        i;
    static int    once = 0;

    if (once == 0) {
        once = 1;

        /*
         * Initialize hash table of file extensions.
         */

        Tcl_InitHashTable(&types, TCL_STRING_KEYS);

        /*
         * Add default system types first from above
         */

        for (i = 0u; typetab[i].ext != NULL; ++i) {
            AddType(typetab[i].ext, typetab[i].type);
        }
    }

    set = Ns_ConfigGetSection("ns/mimetypes");
    if (likely(set != NULL)) {

        defaultType = Ns_SetIGet(set, "default");
        if (defaultType == NULL) {
            defaultType = TYPE_DEFAULT;
        }

        noextType = Ns_SetIGet(set, "noextension");
        if (noextType == NULL) {
            noextType = defaultType;
        }

        for (i = 0u; i < Ns_SetSize(set); i++) {
            AddType(Ns_SetKey(set, i), Ns_SetValue(set, i));
        }
    }
}


/*
 *----------------------------------------------------------------------
 *
 * Ns_GetMimeType --
 *
 *      Guess the mime type based on filename extension. Case is
 *      ignored.
 *
 * Results:
 *      A mime type.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

const char *
Ns_GetMimeType(const char *file)
{
    const char          *start, *ext, *result = defaultType;

    NS_NONNULL_ASSERT(file != NULL);
    
    start = strrchr(file, INTCHAR('/'));
    if (start == NULL) {
        start = file;
    }
    ext = strrchr(start, INTCHAR('.'));
    if (ext == NULL) {
        result = noextType;

    } else {
        Ns_DString           ds;
        const Tcl_HashEntry *hPtr;
            
        Ns_DStringInit(&ds);
        ext = LowerDString(&ds, ext);
        hPtr = Tcl_FindHashEntry(&types, ext);
        Ns_DStringFree(&ds);
        if (hPtr != NULL) {
            result = Tcl_GetHashValue(hPtr);
        }
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsTclGuessTypeObjCmd --
 *
 *      Implements ns_guesstype.
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
NsTclGuessTypeObjCmd(ClientData UNUSED(clientData), Tcl_Interp *interp, int objc, Tcl_Obj *CONST* objv)
{
    int         result = TCL_OK;

    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "filename");
        result = TCL_ERROR;
    } else {
        const char *type = Ns_GetMimeType(Tcl_GetString(objv[1]));
         
        Tcl_SetObjResult(interp, Tcl_NewStringObj(type, -1));
    }

    return result;
}


/*
 *----------------------------------------------------------------------
 *
 * NsGetMimeTypesNs_IsBinaryMimeType --
 *
 *      Check, if the provided mimetime is a "binary" mimetype.
 *
 * Results:
 *      boolean
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */
bool
Ns_IsBinaryMimeType(const char *contentType) {
    
    NS_NONNULL_ASSERT(contentType != NULL);
    
    return (strncmp("text/", contentType, 5u) != 0);
}

/*
 *----------------------------------------------------------------------
 *
 * NsGetMimeTypes --
 *
 *      Append list of configured extension / mime-type mappings to
 *      given dstring.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
NsGetMimeTypes(Ns_DString *dsPtr)
{
    Tcl_HashSearch       search;
    const Tcl_HashEntry *hPtr;

    NS_NONNULL_ASSERT(dsPtr != NULL);
    
    hPtr = Tcl_FirstHashEntry(&types, &search);
    while (hPtr != NULL) {
        Tcl_DStringAppendElement(dsPtr, Tcl_GetHashKey(&types, hPtr));
        Tcl_DStringAppendElement(dsPtr, Tcl_GetHashValue(hPtr));
        hPtr = Tcl_NextHashEntry(&search);
    }
}


/*
 *----------------------------------------------------------------------
 *
 * AddType --
 *
 *      Add a mime type to the global hash table.
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
AddType(CONST char *ext, CONST char *type)
{
    Ns_DString      ds;
    Tcl_HashEntry  *he;
    int             isNew;

    Ns_DStringInit(&ds);
    ext = LowerDString(&ds, ext);
    he = Tcl_CreateHashEntry(&types, ext, &isNew);
    if (isNew == 0) {
	char *oldType = Tcl_GetHashValue(he);

	if (STREQ(oldType, type)) {
	    Ns_Log(Warning, 
		   "config mimtypes: redefine mime type for %s with identical value (%s); statement useless",
		   ext, oldType);
	} else {
	    Ns_Log(Warning, 
		   "config mimtypes: redefine predefined mime type for %s value '%s' with different value: %s",
		   ext, oldType, type);
	}

        ns_free(oldType);
    }
    Tcl_SetHashValue(he, ns_strdup(type));
    Ns_DStringFree(&ds);
}


/*
 *----------------------------------------------------------------------
 *
 * LowerDString --
 *
 *      Append a string to the dstring, converting all alphabetic
 *      characeters to lowercase.
 *
 * Results:
 *      dsPtr->string
 *
 * Side effects:
 *      Appends to dstring.
 *
 *----------------------------------------------------------------------
 */

static char *
LowerDString(Ns_DString *dsPtr, CONST char *ext)
{
    char *p;

    Ns_DStringAppend(dsPtr, ext);
    p = dsPtr->string;
    while (*p != '\0') {
        if (CHARTYPE(upper, *p) != 0) {
            *p = CHARCONV(lower, *p);
        }
        ++p;
    }

    return dsPtr->string;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * indent-tabs-mode: nil
 * End:
 */
