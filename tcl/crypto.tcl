if {[info commands ::nx::Class] eq ""} {
    ns_log warning "ns_md is not available; use crypto::md instead"
    return
}

nx::Class create ns_md {
    :property digest
    :variable ctx
   
    :public object method string {-digest message} {
	::crypto::md string -digest $digest $message
    }
    
    :public object method file {-digest filename} {
	if {![file readable $filename]} {
	    return -code error "file $filename is not readable"
	}
	set F [open $filename]
	fconfigure $F -encoding binary -translation binary
	set m [:new -digest $digest]
	while (1) {
	    set block [read $F 32768]
	    $m add $block
	    if {[string length $block] < 32768} {
		break
	    }
	}
	set r [$m get]
	$m destroy
	return $r
    }
    
    :method init {} {
	set :ctx [::crypto::md new ${:digest}]
    }
    :public method destroy {} {
	::crypto::md free ${:ctx}
	next
    }
   
    :public method add {message} {
	::crypto::md add ${:ctx} $message
    }
    :public method get {} {
	::crypto::md get ${:ctx}
    }
}
