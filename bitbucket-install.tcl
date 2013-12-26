set pageName [ns_queryget file ""]
switch -exact $pageName {
  nsconf.tcl -
  nsstats.tcl {
    set source https://bitbucket.org/naviserver/[file root $pageName]/get/tip.tar.gz
    set page [ns_info pagedir]/$pageName
    if {![file readable $page]} {
      exec wget --quiet --no-check-certificate -O /tmp/nsstats.tar.gz $source
      if {[catch {exec tar Ozxvf /tmp/nsstats.tar.gz *$pageName > $page} errorMsg]} {
	if {[string match "x *" $errorMsg]} { 
	  ns_log notice "error: $errorMsg"
	  ns_return 200 text/html \
	      "<html><body>error while downloading $pageName: <b>$errorMsg</b>. <a href='/'>return</a>" 
	}
      }
      ns_return 200 text/html \
	  "<html><body>$pageName successfully downloaded and installed. <a href='/'>return</a>" 
    }
    ns_return 200 text/html \
	"<html><body>$pageName already installed. <a href='/'>return</a>" 
  }
  default {
    set pageName [string map [list < "&lt;" > "&gt;"] $pageName]
    ns_return 200 text/html "<html><body>page $pageName unknown. <a href='/'>return</a>" 
  }
}




