# Upload test page with progress statistics
#
# Vlad Seryakov vlad@crystalballinc.com
#

# Command to perform
set cmd [ns_queryget cmd]

# Name of current server
set server [ns_info server]

# In case of very large files and if server set maxupload limit, this
# will return temporary file name where all content is spooled
set file [ns_conn contentfile]

# No query and not empty file means we need to deal with situation when
# all content is in the temp file
if { $cmd == "" && $file != "" } {
  ns_log notice uploaded into $file, content type = [ns_set iget [ns_conn headers] content-type]
  file rename -force -- $file /tmp/test
  set cmd upload
}


switch -- $cmd {
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

     <form action="upload.tcl" method="post" enctype="multipart/form-data">
     <input type="hidden" name="cmd" value="upload">

     File: <input type="file" name="file">

     <input type="button" value="Submit" onClick="parent.setTimeout('progress()',2000);this.form.submit();">
     </form>
     }
  }

  default {
      ns_return 200 text/html [subst {

     <script>
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
     </script>

     <head><title>Upload Test</title></head>   

     <body>

     <h2>Upload test page with progress statistics</h2>

     <ul>
     <li>To make test file:
     <pre>
     dd if=/dev/zero of=test.dat count=500000
     </pre>
     <li>Adjust parameters to enable statistics and big uploads in the used <br>
     configuration file (<code>[ns_info config]</code>) like the following if necessary:
     <pre>
     ns_section ns/parameters
     ns_param progressminsize   [expr 1024*1024]     ;# configured value: [ns_config ns/parameters progressminsize]; show progress for files larger than this value

     ns_section ns/server/$server/adp
     ns_param enabletclpages  true          ;# configured value: [ns_config ns/server/$server/adp enabletclpages]

     ns_section ns/server/$server/module/nssock
     ns_param maxinput       1000000000     ;# configured value: [ns_config ns/server/$server/module/nssock maxinput]; max accepted size of upload
     ns_param maxupload       700000000     ;# configured value: [ns_config ns/server/$server/module/nssock maxupload]; spool files larger than this to disk
     ns_param spoolerthreads          1     ;# configured value: [ns_config ns/server/$server/module/nssock spoolerthreads]; number of spooler threads
     </pre>
     </ul>
     <p>
     <table width="100%" bgcolor="#eeeeee" border="0">
     <tr><td>
         <iframe name="form" src="upload.tcl?cmd=form" border="0" frameborder="0" width="100%" height="100"></iframe>
         </td>
         <td width="50%" bgcolor="#ffffff" id="progress"></td>
     </tr>
     </table>
     Back to <a href='.'>example page</a>.<br>
     </body>
     
     }]
  }
}
