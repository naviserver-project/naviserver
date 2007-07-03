# Upload test page with progress statistics
#
# Vlad Seryakov vlad@crystalballinc.com
#

switch -- [ns_queryget cmd] {
  stats {
     # Discover absolute path to the script
     set url [ns_normalizepath [file dirname [ns_conn url]]/upload.tcl]
     set stats [ns_upload_stats $url]
     ns_log Notice upload.tcl: $url: $stats
     # Calculate percentage
     if { $stats != "" } {
       foreach { len size } $stats {}
       set stats [expr round($len.0*100/$size.0)]
     } else {
       set stats -1
     }
     ns_return 200 text/html $stats
  }
  
  upload {
     ns_return 200 text/html "Upload completed"
  }

  form {
     ns_return 200 text/html {

     <FORM ACTION=upload.tcl METHOD=POST ENCTYPE=multipart/form-data>
     <INPUT TYPE=HIDDEN NAME=cmd VALUE=upload>

     File: <INPUT TYPE=FILE NAME=file>

     <INPUT TYPE=BUTTON VALUE=Submit onClick="parent.setTimeout('progress()',2000);this.form.submit();">
     </FORM>
     }
  }

  default {
     ns_return 200 text/html {

     <SCRIPT>

     function progress()
     {
        var now = new Date();
        try { req = new ActiveXObject('Msxml2.XMLHTTP'); } catch (e) {
           try { req = new ActiveXObject('Microsoft.XMLHTTP'); } catch (e) {
              if(typeof XMLHttpRequest != 'undefined') req = new XMLHttpRequest();
           }
        }
        req.open('GET','upload.tcl?cmd=stats&t='+now.getTime(),false);
        req.send(null);
        var rc = parseInt(req.responseText);
        var obj = document.getElementById('Progress');
        if(!isNaN(rc) && rc >= 0) {
          obj.innerHTML = 'Progress: ' + rc + '%';
          setTimeout('progress()',1000);
        }
     }
     </SCRIPT>

     <HEAD><TITLE>Upload Test</TITLE></HEAD>   

     <BODY>

     Upload test page with progress statistics<P>

     <UL>
     <LI>To make test file:<P>
         dd if=/dev/zero of=test.dat count=500000<P>

     <LI>In nsd.tcl adjust parameters to enable statistics and big uploads<P>

         ns_section ns/parameters<BR>
         ns_param progressminsize   [expr 1024*1024]<P>

         ns_section ns/server/servername/adp<BR>
         ns_param enabletclpages  true

         ns_section ns/server/default/module/nssock<BR>
         ns_param maxinput        [expr 1024*1024*500]<BR>
         ns_param spoolerthreads  1<BR>
     </UL>
     <P>
     <TABLE WIDTH=100% BGCOLOR=#EEEEEE BORDER=0>
     <TR><TD>
         <IFRAME NAME=Form SRC=upload.tcl?cmd=form BORDER=0 FRAMEBORDER=0 WIDTH=100% HEIGHT=100></IFRAME>
         </TD>
         <TD WIDTH=50% BGCOLOR=#FFFFFF ID=Progress></TD>
     </TR>
     </TABLE>
     </BODY>
     
     }
  }
}
