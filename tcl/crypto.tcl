if {[info commands ::nx::Class] eq ""} {
    ns_log warning "ns_md and ns_hmac are not available; use crypto::md and crypto::hmac instead"
    return
}

#
# class ::crypto::HashFunctions
#
#     Define common behavior for crypto::* functionality
#
nx::Class create ::crypto::HashFunctions {
    :property {digest sha256}
    :variable ctx

    :public method readfile {filename} {
	#
	# Read a file blockwisw and call the incremental crypo
	# function on every block.
	#
	set F [open $filename]
	fconfigure $F -encoding binary -translation binary
	while (1) {
	    set block [read $F 32768]
	    :add $block
	    if {[string length $block] < 32768} {
		break
	    }
	}
	close $F
	#
	# Return the hash sum
	#
	return [:get]
    }
}

#
# class ns_md
#
#     Provide an oo interface to the OpenSSL Message Digest
#     functionality.
#
nx::Class create ns_md -superclass ::crypto::HashFunctions {
   
    :public object method string {-digest message} {
	::crypto::md string -digest $digest $message
    }
    
    :public object method file {-digest filename} {
	if {![file readable $filename]} {
	    return -code error "file $filename is not readable"
	}
	set m [:new -digest $digest]
	set r [$m readfile $filename]
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


#
# class ns_hmac
#
#     Provide an oo interface to the OpenSSL Hash Based Message
#     authentication Code (HMAC). In essence, this is a password
#     secured hash code.
#

nx::Class create ns_hmac -superclass ::crypto::HashFunctions {
    :property key:required
   
    :public object method string {-digest key message} {
	::crypto::hmac string -digest $digest $key $message
    }

    :public object method file {-digest key filename} {
	if {![file readable $filename]} {
	    return -code error "file $filename is not readable"
	}
	set m [:new -digest $digest -key $key]
	set r [$m readfile $filename]
	$m destroy
	return $r
    }
    
    :method init {} {
	set :ctx [::crypto::hmac new ${:digest} ${:key}]
    }
    :public method destroy {} {
	::crypto::hmac free ${:ctx}
	next
    }
   
    :public method add {message} {
	::crypto::hmac add ${:ctx} $message
    }
    :public method get {} {
	::crypto::hmac get ${:ctx}
    }
}
