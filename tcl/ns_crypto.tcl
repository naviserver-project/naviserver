# -*- Tcl -*-
# Crypto support based on OpenSSL and nsf
#
# Author: Gustaf Neumann
# Date:   August 2016

#catch {package require nx}

if {[info commands ::nx::Class] eq ""} {
    ns_log warning "ns_md, ns_hmac, ns_hotp and ns_totp are not available"
    return
}

###########################################################################
# class ::ns_crypto::HashFunctions
#
#     Define common behavior for ns_crypto::* functionality
#
nx::Class create ::ns_crypto::HashFunctions {
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

###########################################################################
# class ns_md
#
#     Provide an oo interface to the OpenSSL Message Digest
#     functionality.
#
nx::Class create ns_md -superclass ::ns_crypto::HashFunctions {
   
    :public object method string {-digest message} {
	::ns_crypto::md string -digest $digest $message
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
	set :ctx [::ns_crypto::md new ${:digest}]
    }
    :public method destroy {} {
	::ns_crypto::md free ${:ctx}
	next
    }
   
    :public method add {message} {
	::ns_crypto::md add ${:ctx} $message
    }
    :public method get {} {
	::ns_crypto::md get ${:ctx}
    }
}


###########################################################################
# class ns_hmac
#
#     Provide an oo interface to the OpenSSL Hash Based Message
#     authentication Code (HMAC). In essence, this is a password
#     secured hash code.
#

nx::Class create ns_hmac -superclass ::ns_crypto::HashFunctions {
    :property key:required
   
    :public object method string {-digest key message} {
	::ns_crypto::hmac string -digest $digest $key $message
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
	set :ctx [::ns_crypto::hmac new ${:digest} ${:key}]
    }
    :public method destroy {} {
	::ns_crypto::hmac free ${:ctx}
	next
    }
   
    :public method add {message} {
	::ns_crypto::hmac add ${:ctx} $message
    }
    :public method get {} {
	::ns_crypto::hmac get ${:ctx}
    }
}

###########################################################################
# HOTP: Implementation of One-Time Passwords as described in RFC 4226
# based on a HMAC function.  RFC 4226 defines HOTP as:
#
#     HOTP(K,C) = Truncate(HMAC-SHA-1(K,C))
#
#    K: key
#    C: counter (moving factor for one time passwd)
#    
# The function below allows to choose the digest algorithm, the number
# of digits. C is provided as data. 

nsf::proc ns_hotp {
    {-digest sha256}
    {-digits:integer 6}
    {-key ""}
    data
} {
    set hmac [::ns_crypto::hmac string -digest $digest $key $data]
    return [::ns_crypto::hotp_truncate -digits $digits $hmac]
}

#
# The RFC 4226 truncate function returns from a hexadecimal input a
# decimal number with $digits digits.
#
nsf::proc ::ns_crypto::hotp_truncate {
    -digits:integer
    input
} {
    #
    # offset is value of last byte
    #
    set offset  [expr {("0x[string range $input end-1 end]" & 0x0f) * 2}]

    #
    # Get the value of 4 bytes starting from offset, mask most significant bit
    #
    set dbc1 [expr {"0x[string range $input $offset $offset+7]" & 0x7fffffff}]
    #
    # DBC1 (stands for "dynamic binary code" in RFC 4226) is assumed to
    # be equal to DBC2. The same is done in the refence implementation
    # RFC 4226.
    #
    # Finally return last $digits digits of $dbc1
    return [string range $dbc1 end-[incr digits -1] end]
}



###########################################################################
# TOTP: Implementation of a Time-Based One-Time Password Algorithm as
# defined in RFC 6238 based on HOTP. TOTP is defined as
#
#     TOTP = HOTP(K, T), where T is an integer
#
#     K: key
#     T: time slice (moving factor for one time passwd)
#
# The RFC examples use 8 digits for output, the moving factor is there
# an 64 bit value.  Of course, in general the value can be different,
# but these are used here to obtain the same results as in test
# vectors in the RFC.

nsf::proc ns_totp {
    {-digest sha256}
    {-digits:integer 8}
    {-interval:integer 30}
    {-user_id:integer 0}
    {-key}
    {-time}
} {
    #
    # If no key is provided, get configured secret and personalize
    # this for the given user_id
    #
    if {![info exists key]} {
	set secret [ns_config "ns/server/[ns_info server]" serversecret ""]; 
	set key [binary format H* [::ns_crypto::md string -digest sha224 $secret-$user_id]]
    }
    #
    # If no time is provided, take the current time
    #
    if {![info exists time]} {set time [clock seconds]}
    #
    # Call the HTOP implementation for the time slice (return the same
    # value for interval seconds)
    #
    set totp [ns_hotp \
		  -digest $digest \
		  -digits $digits \
		  -key $key \
		  [binary format W [expr {$time / $interval}]]]
    return $totp
}



