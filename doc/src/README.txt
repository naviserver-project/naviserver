
What is this?
-------------

This directory holds NaviServer documentation sources in doctools format.
To write doctools source files, you only need a text editor of your choice.
To actually format the files into some other format, you'd need the 
doctools package which is the part of the tcllib package you can find at:

    http://sourceforge.net/projects/tcllib

After downloading the package, make sure you do "make install" as this will
arrange for all utilities to be installed in the binaries directory. 
Also, be sure to have the latest release of Tcl installed on your system. 


Doc Howto
---------

In this directory you will find a file called "template.man".
Copy this file to the name of command you are documenting:

   cp template.man ns_info.man

and start adding content in doctool format. The reference documentation
about doctools can be found on:

   http://aspn.activestate.com/ASPN/docs/ActiveTcl/8.4/tcllib/doctools/doctools_fmt.html

(Interestingly, the above documentation is also written in doctools format
and converted to HTML by the doctools converters)

You can always peek at the existing doctools man files as distributed by the
NaviServer or any other module documentation found in the tcllib package. 

When you're done, be sure to "cvs add" and "cvs commit" the file.

The resulting documentation is automatically generated in nroff and html
format when you invoke "make build-doc" in the top-level distribution directory. 
The output files are stored under "doc/man" and "doc/html" for the 
corresponding format(s). 

During the writing you may want to check how the output of your file will
look like when converted to html and/or nroff. This can be easily achieved
with the "dtplite" utility:

   % dtplite -o - html  ns_sendmail.man >ns_sendmail.html
   % dtplite -o - nroff ns_sendmail.man >ns_sendmail.n
   % nroff -man ns_sendmail.n

On Linux, "man" supports also compressed pages:

   % dtplite -o - nroff ns_sendmail.man | gzip >ns_sendmail.n.gz
   % man -l ns_sendmail.n.gz

